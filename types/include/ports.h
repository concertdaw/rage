#pragma once

#include "atoms.h"
#include "time_series.h"

typedef enum {
    RAGE_STREAM_AUDIO
} rage_StreamDef;

typedef struct {
    RAGE_ARRAY(rage_TupleDef const) controls;
    RAGE_ARRAY(rage_StreamDef const) inputs;
    RAGE_ARRAY(rage_StreamDef const) outputs;
} rage_ProcessRequirements;

typedef float const * rage_InStream;
typedef float * rage_OutStream;

typedef struct {
    RAGE_ARRAY(rage_TimeSeries) controls;
    rage_InStream * inputs;
    rage_OutStream * outputs;
} rage_Ports;

rage_Ports rage_ports_new(rage_ProcessRequirements const * const requirements);
void rage_ports_free(rage_Ports ports);
