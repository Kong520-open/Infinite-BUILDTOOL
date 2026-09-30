#pragma once

#include "dobby/dobby_internal.h"

#include "Interceptor.h"
#include "TrampolineBridge/ClosureTrampolineBridge/ClosureTrampoline.h"

inline asm_func_t closure_bridge_addr = nullptr;

void closure_bridge_init();

void get_routing_bridge_next_hop(DobbyRegisterContext *ctx, void *address);

void set_routing_bridge_next_hop(DobbyRegisterContext *ctx, void *address);

extern "C" void common_closure_bridge_handler(DobbyRegisterContext *ctx, ClosureTrampoline *tramp);
