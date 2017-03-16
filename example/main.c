#include <stdio.h>
#include <unistd.h> // for sleep
#include "error.h"
#include "loader.h"
#include "proc_block.h"

int main() {
    printf("Example started\n");
    rage_NewProcBlock npb = rage_proc_block_new();
    if (RAGE_FAILED(npb)) {
        printf("Proc block creation failed: %s\n", RAGE_FAILURE_VALUE(npb));
        return 1;
    }
    rage_ProcBlock * const pb = RAGE_SUCCESS_VALUE(npb);
    printf("Proc block created\n");
    rage_ElementLoader * el = rage_element_loader_new();
    //rage_ElementTypes element_type_names = rage_element_loader_list(el);
    // FIXME: loading super busted
    rage_ElementTypeLoadResult et_ = rage_element_loader_load(
        el, "./build/libamp.so");
    if (RAGE_FAILED(et_)) {
        printf("Element type load failed: %s\n", RAGE_FAILURE_VALUE(et_));
        rage_proc_block_free(pb);
        return 2;
    }
    rage_ElementType * const et = RAGE_SUCCESS_VALUE(et_);
    printf("Element type loaded\n");
    // FIXME: Fixed stuff for amp
    rage_Atom tup[] = {
        {.i=1}
    };
    // FIXME: not working out frame size or anything
    rage_ElementNewResult elem_ = rage_element_new(et, 44100, 1024, tup);
    if (RAGE_FAILED(elem_)) {
        printf("Element load failed: %s\n", RAGE_FAILURE_VALUE(elem_));
        rage_proc_block_free(pb);
        rage_element_loader_free(el);
        return 3;
    }
    rage_Element * const elem = RAGE_SUCCESS_VALUE(elem_);
    printf("Element loaded\n");
    rage_Atom vals[] = {{.f = 1.0}};
    rage_TimePoint tps[] = {
        {
            .time = {.second = 0},
            .value = &(vals[0]),
            .mode = RAGE_INTERPOLATION_CONST
        }
    };
    rage_TimeSeries ts = {
        .len = 1,
        .items = tps
    };
    rage_MountResult mr = rage_proc_block_mount(pb, elem, &ts, "amp");
    if (RAGE_FAILED(mr)) {
        printf("Mount failed\n");
        rage_element_free(elem);
        rage_element_loader_unload(el, et);
        rage_element_loader_free(el);
        rage_proc_block_free(pb);
        return 4;
    }
    rage_Harness * const harness = RAGE_SUCCESS_VALUE(mr);
    //FIXME: handle errors (start/stop)
    printf("Starting engine...\n");
    rage_Error en_st = rage_proc_block_start(pb);
    if (RAGE_FAILED(en_st)) {
        printf("Start failed: %s\n", RAGE_FAILURE_VALUE(en_st));
    } else {
        printf("Sleeping...\n");
        sleep(90);
        rage_proc_block_stop(pb);
    }
    rage_proc_block_unmount(harness);
    printf("Unmounted\n");
    rage_element_free(elem);
    printf("Elem freed\n");
    rage_element_loader_unload(el, et);
    printf("Elem type freed\n");
    rage_element_loader_free(el);
    printf("Elem loader freed\n");
    rage_proc_block_free(pb);
}
