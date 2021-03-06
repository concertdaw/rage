#pragma once

#include "chronology.h"
#include "atoms.h"
#include "macros.h"

/*
 * The selected interpolation type of a rage_TimePoint.
 */
typedef enum {
    RAGE_INTERPOLATION_CONST,
    RAGE_INTERPOLATION_LINEAR
} rage_InterpolationMode;

/*
 * A point in a session time interpolated view of an atom tuple.
 * A series of these points form a rage_TimeSeries.
 */
typedef struct {
    rage_Time time;
    rage_Atom * value;
    rage_InterpolationMode mode;
    // rage_Atom * interpolation_args;
} rage_TimePoint;

/*
 * An array of rage_TimePoint instances that cover all time in the session.
 */
typedef RAGE_ARRAY(rage_TimePoint) rage_TimeSeries;
