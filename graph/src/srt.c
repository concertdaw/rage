#include "srt.h"
#include "macros.h"
#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <semaphore.h>

struct rage_SupportTruck {
    rage_SupportConvoy * convoy;  // Not totally convinced by the backref
    rage_Element * elem;
    rage_InterpolatedView ** prep_view;
    int64_t frames_prepared;
    rage_InterpolatedView ** clean_view;
    int64_t frames_to_clean;
};

typedef RAGE_ARRAY(rage_SupportTruck *) rage_Trucks;
static RAGE_POINTER_ARRAY_APPEND_FUNC_DEF(rage_Trucks, rage_SupportTruck, truck_append)
static RAGE_POINTER_ARRAY_REMOVE_FUNC_DEF(rage_Trucks, rage_SupportTruck, truck_remove)

struct rage_SupportConvoy {
    bool running;
    pthread_t worker_thread;
    uint32_t period_size;
    rage_FrameNo invalid_after;
    rage_Countdown * countdown;
    // mutex to prevent truck array freeing during iteration:
    pthread_mutex_t active;
    rage_Trucks * prep_trucks;
    rage_Trucks * clean_trucks;
    int counts_skipped;
    bool triggered_tick;
    sem_t triggered_tick_sem;
    rage_TransportState transport_state;
};

rage_SupportConvoy * rage_support_convoy_new(uint32_t period_size, rage_Countdown * countdown) {
    rage_SupportConvoy * convoy = malloc(sizeof(rage_SupportConvoy));
    convoy->running = false;
    convoy->period_size = period_size;
    convoy->invalid_after = UINT64_MAX;
    convoy->countdown = countdown;
    // FIXME: can in theory fail
    pthread_mutex_init(&convoy->active, NULL);
    convoy->prep_trucks = malloc(sizeof(rage_Trucks));
    convoy->prep_trucks->len = 0;
    convoy->prep_trucks->items = NULL;
    convoy->clean_trucks = malloc(sizeof(rage_Trucks));
    convoy->clean_trucks->len = 0;
    convoy->clean_trucks->items = NULL;
    convoy->counts_skipped = 0;
    convoy->triggered_tick = false;
    sem_init(&convoy->triggered_tick_sem, 0, 0);
    return convoy;
}

void rage_support_convoy_free(rage_SupportConvoy * convoy) {
    pthread_mutex_destroy(&convoy->active);
    assert(convoy->prep_trucks->len == 0);
    assert(convoy->clean_trucks->len == 0);
    assert(!convoy->running);
    RAGE_ARRAY_FREE(convoy->prep_trucks);
    RAGE_ARRAY_FREE(convoy->clean_trucks);
    free(convoy);
}

static rage_Error prep_truck(rage_SupportTruck * truck) {
    rage_FrameNo const prep_began = rage_interpolated_view_get_pos(truck->prep_view[0]);
    rage_Error rv = RAGE_ELEM_PREP(truck->elem, truck->prep_view);
    if (!RAGE_FAILED(rv)) {
        rage_FrameNo const prep_ended = rage_interpolated_view_get_pos(truck->prep_view[0]);
        truck->frames_prepared += prep_ended - prep_began;
    }
    return rv;
}

static rage_Error clean_truck(rage_SupportTruck * truck) {
    rage_FrameNo const clean_began = rage_interpolated_view_get_pos(truck->clean_view[0]);
    rage_Error rv = RAGE_ELEM_CLEAN(truck->elem, truck->clean_view);
    if (!RAGE_FAILED(rv)) {
        rage_FrameNo const clean_ended = rage_interpolated_view_get_pos(truck->clean_view[0]);
        truck->frames_to_clean -= clean_ended - clean_began;
    }
    return rv;
}

static rage_Error apply_to_trucks(
        rage_Trucks * trucks, rage_Error (*op)(rage_SupportTruck *)) {
    for (unsigned i = 0; i < trucks->len; i++) {
        rage_Error rv = op(trucks->items[i]);
        if (RAGE_FAILED(rv))
            return rv;
    }
    return RAGE_OK;
}

static rage_Error clear(rage_Trucks * trucks, rage_FrameNo invalid_after) {
    rage_SupportTruck * truck;
    rage_FrameNo newest_prep, oldest_prep;
    for (unsigned i = 0; i < trucks->len; i++) {
        truck = trucks->items[i];
        newest_prep = rage_interpolated_view_get_pos(truck->prep_view[0]);
        if (newest_prep <= invalid_after)
            continue;
        oldest_prep = newest_prep - truck->frames_prepared;
        if (invalid_after < oldest_prep) {
            return RAGE_ERROR("Cannot clear that far back");
        }
        for (uint32_t j = 0; j < truck->elem->cet->controls.len; j++) {
            rage_interpolated_view_seek(truck->prep_view[j], invalid_after);
        }
        rage_Error err = RAGE_ELEM_CLEAR(truck->elem, truck->prep_view, newest_prep - invalid_after);
        if (RAGE_FAILED(err))
            return err;
        truck->frames_prepared = invalid_after - oldest_prep;
    }
    return RAGE_OK;
}

#define RAGE_MIN(a, b) (a < b) ? a : b

static uint32_t n_frames_grace(rage_Trucks * trucks) {
    uint32_t min_frames_until_clean = UINT32_MAX, min_prepared = UINT32_MAX;
    for (unsigned i = 0; i < trucks->len; i++) {
        rage_SupportTruck * truck = trucks->items[i];
        min_prepared = RAGE_MIN(truck->frames_prepared, min_prepared);
        int64_t frames_until_clean = truck->elem->cet->spec.max_uncleaned_frames - truck->frames_to_clean;
        min_frames_until_clean = RAGE_MIN(min_frames_until_clean, frames_until_clean);
    }
    return RAGE_MIN(min_prepared, min_frames_until_clean);
}

#undef RAGE_MIN

static void account_for_rt_frames(rage_Trucks * trucks, uint32_t frames_passed) {
    for (unsigned i = 0; i < trucks->len; i++) {
        rage_SupportTruck * truck = trucks->items[i];
        truck->frames_prepared -= frames_passed;
        truck->frames_to_clean += frames_passed;
    }
}

static void * rage_support_convoy_worker(void * ptr) {
    rage_SupportConvoy * convoy = ptr;
    char const * err = NULL;
    rage_Error op_result;
    uint32_t min_frames_wait;
    int64_t counts_waited;
    uint32_t counts_to_wait = 0;
    pthread_mutex_lock(&convoy->active);
    #define RAGE_BAIL_ON_FAIL \
        if (RAGE_FAILED(op_result)) { \
            err = RAGE_FAILURE_VALUE(op_result); \
            break; \
        }
    while (convoy->running) {
        if (convoy->invalid_after != UINT64_MAX) {
             op_result = clear(convoy->prep_trucks, convoy->invalid_after);
             RAGE_BAIL_ON_FAIL
             convoy->invalid_after = UINT64_MAX;
        }
        op_result = apply_to_trucks(convoy->prep_trucks, prep_truck);
        RAGE_BAIL_ON_FAIL
        counts_waited = counts_to_wait - convoy->counts_skipped;
        if (counts_waited < 0) {
            err = "SupportConvoy waited for negative counts?!?";
            break;
        }
        convoy->counts_skipped = 0;
        // FIXME: Should be all trucks not just prep
        account_for_rt_frames(convoy->prep_trucks, counts_waited * convoy->period_size);
        min_frames_wait = n_frames_grace(convoy->prep_trucks);
        counts_to_wait = min_frames_wait / convoy->period_size;
        if (0 > rage_countdown_add(convoy->countdown, counts_to_wait)) {
            err = "SupportConvoy failed to keep up";
            break;
        }
        if (convoy->triggered_tick) {
            sem_post(&convoy->triggered_tick_sem);
            convoy->triggered_tick = false;
        }
        pthread_mutex_unlock(&convoy->active);
        rage_countdown_timed_wait(convoy->countdown, UINT32_MAX);
        pthread_mutex_lock(&convoy->active);
        op_result = apply_to_trucks(convoy->clean_trucks, clean_truck);
        RAGE_BAIL_ON_FAIL
    }
    #undef RAGE_BAIL_ON_FAIL
    pthread_mutex_unlock(&convoy->active);
    return (void *) err;
}

static void unlock_and_await_tick(rage_SupportConvoy * convoy) {
    bool trigger = convoy->running;
    if (trigger) {
        convoy->counts_skipped = rage_countdown_unblock_wait(convoy->countdown);
        convoy->triggered_tick = true;
    }
    pthread_mutex_unlock(&convoy->active);
    if (trigger) {
        sem_wait(&convoy->triggered_tick_sem);
    }
}

rage_Error rage_support_convoy_start(rage_SupportConvoy * convoy) {
    assert(!convoy->running);
    pthread_mutex_lock(&convoy->active);
    convoy->running = true;
    if (pthread_create(
            &convoy->worker_thread, NULL,
            rage_support_convoy_worker, convoy)) {
        // This makes a mess if the thread doesn't start
        return RAGE_ERROR("Failed to create worker thread");
    }
    unlock_and_await_tick(convoy);
    return RAGE_OK;
}

rage_Error rage_support_convoy_stop(rage_SupportConvoy * convoy) {
    assert(convoy->running);
    pthread_mutex_lock(&convoy->active);
    convoy->running = false;
    rage_countdown_unblock_wait(convoy->countdown);
    pthread_mutex_unlock(&convoy->active);
    char const * err;
    if (pthread_join(convoy->worker_thread, (void **) &err)) {
        return RAGE_ERROR("Failed to join worker thread");
    }
    if (err != NULL) {
        return RAGE_ERROR(err);
    }
    return RAGE_OK;
}

static void change_trucks(
        rage_Trucks * (next_trucks)(rage_Trucks *, rage_SupportTruck * truck),
        rage_SupportConvoy * convoy,
        rage_SupportTruck * truck) {
    // FIXME: This is assuming a single control thread
    rage_Trucks
        * old_prep_trucks,
        * new_prep_trucks,
        * old_clean_trucks,
        * new_clean_trucks;
    if (truck->prep_view) {
        old_prep_trucks = convoy->prep_trucks;
        new_prep_trucks = next_trucks(old_prep_trucks, truck);
    } else {
        old_prep_trucks = NULL;
        new_prep_trucks = convoy->prep_trucks;
    }
    if (truck->clean_view) {
        old_clean_trucks = convoy->clean_trucks;
        new_clean_trucks = next_trucks(old_clean_trucks, truck);
    } else {
        old_clean_trucks = NULL;
        new_clean_trucks = convoy->clean_trucks;
    }
    pthread_mutex_lock(&convoy->active);
    convoy->prep_trucks = new_prep_trucks;
    convoy->clean_trucks = new_clean_trucks;
    unlock_and_await_tick(convoy);
    if (old_prep_trucks != NULL) {
        RAGE_ARRAY_FREE(old_prep_trucks);
    }
    if (old_clean_trucks != NULL) {
        RAGE_ARRAY_FREE(old_clean_trucks);
    }
}

rage_SupportTruck * rage_support_convoy_mount(
        rage_SupportConvoy * convoy, rage_Element * elem,
        rage_InterpolatedView ** prep_view,
        rage_InterpolatedView ** clean_view) {
    rage_SupportTruck * truck = malloc(sizeof(rage_SupportTruck));
    truck->elem = elem;
    truck->prep_view = prep_view;
    truck->frames_prepared = 0;
    truck->clean_view = clean_view;
    truck->frames_to_clean = 0;
    truck->convoy = convoy;
    change_trucks(truck_append, convoy, truck);
    return truck;
}

void rage_support_convoy_unmount(rage_SupportTruck * truck) {
    change_trucks(truck_remove, truck->convoy, truck);
    free(truck);
}

void rage_support_convoy_transport_state_changing(
        rage_SupportConvoy * convoy, rage_TransportState state) {
    convoy->transport_state = state;
    if (state == RAGE_TRANSPORT_ROLLING) {
        // Wait for any active prep work to finish (assumes single control thread)
        pthread_mutex_lock(&convoy->active);
        pthread_mutex_unlock(&convoy->active);
    }
}

void rage_support_convoy_transport_state_changed(
        rage_SupportConvoy * convoy, rage_TransportState state) {
    if (state == RAGE_TRANSPORT_STOPPED) {
        // Force an iteration to ensure cleanup completed
        pthread_mutex_lock(&convoy->active);
        unlock_and_await_tick(convoy);
    }
}

static void seek_views_to(rage_InterpolatedView ** views, uint32_t n_views, rage_FrameNo pos) {
    for (uint32_t j = 0; j < n_views; j++) {
        rage_interpolated_view_seek(views[j], pos);
    }
}

rage_Error rage_support_convoy_transport_seek(rage_SupportConvoy * convoy, rage_FrameNo target_frame) {
    rage_FrameNo clear_from = UINT64_MAX;
    rage_Error err = RAGE_OK;
    if (convoy->transport_state == RAGE_TRANSPORT_ROLLING) {
        return RAGE_ERROR("Transport seek whilst rolling not implemented");
    }
    pthread_mutex_lock(&convoy->active);
    for (unsigned i = 0; i < convoy->prep_trucks->len; i++) {
        rage_SupportTruck * truck = convoy->prep_trucks->items[i];
        clear_from = rage_interpolated_view_get_pos(truck->prep_view[0]) - truck->frames_prepared;
        seek_views_to(truck->prep_view, truck->elem->cet->controls.len, clear_from);
        // FIXME: When this fails end up in an awful mess
        err = RAGE_ELEM_CLEAR(truck->elem, truck->prep_view, truck->frames_prepared);
        if (RAGE_FAILED(err))
            break;
        seek_views_to(truck->prep_view, truck->elem->cet->controls.len, target_frame);
        truck->frames_prepared = 0;
    }
    for (unsigned i = 0; i < convoy->clean_trucks->len; i++) {
        rage_SupportTruck * truck = convoy->clean_trucks->items[i];
        assert(truck->frames_to_clean <= 0);
        seek_views_to(truck->clean_view, truck->elem->cet->controls.len, target_frame);
        truck->frames_to_clean = 0;
    }
    unlock_and_await_tick(convoy);
    return err;
}
