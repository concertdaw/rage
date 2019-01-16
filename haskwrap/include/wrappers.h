#pragma once
#define RAGE_DISABLE_ANON_UNIONS 1
#include "error.h"
#include "time_series.h"
#include "ports.h"
#include "binding_interface.h"
#include "element.h"

typedef struct rage_hs_ElementKind rage_hs_ElementKind;
typedef RAGE_OR_ERROR(rage_hs_ElementKind *) rage_hs_ElementKindLoadResult;

rage_hs_ElementKindLoadResult * rage_hs_element_loader_load(char * name);
void rage_hs_element_loader_unload(rage_hs_ElementKind * k);

typedef struct rage_hs_ConcreteElementType rage_hs_ConcreteElementType;
typedef RAGE_OR_ERROR(rage_hs_ConcreteElementType *) rage_hs_ConcreteElementTypeResult;

rage_hs_ConcreteElementTypeResult * rage_hs_element_type_specialise(rage_hs_ElementKind *k, rage_Atom ** params);
void rage_hs_concrete_element_type_free(rage_hs_ConcreteElementType * cet);

rage_InstanceSpec * rage_hs_cet_get_spec(rage_hs_ConcreteElementType * cetr);

rage_ParamDefList const * rage_hs_element_kind_parameters(rage_hs_ElementKind *k);

typedef struct rage_hs_Graph rage_hs_Graph;
typedef RAGE_OR_ERROR(rage_hs_Graph *) rage_hs_NewGraph;

rage_hs_NewGraph * rage_hs_graph_new(
    uint32_t sample_rate, rage_BackendDescriptions * inputs,
    rage_BackendDescriptions * outputs);

typedef struct rage_hs_GraphNode rage_hs_GraphNode;
typedef RAGE_OR_ERROR(rage_hs_GraphNode *) rage_hs_NewGraphNode;

rage_hs_NewGraphNode * rage_hs_graph_add_node(
    rage_hs_Graph * g, rage_hs_ConcreteElementType * cetp,
    rage_TimeSeries const * ts);
void rage_hs_graph_remove_node(rage_hs_GraphNode * hgn);
