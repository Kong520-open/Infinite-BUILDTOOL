#include "common_bridge_handler.h"

extern "C" void common_closure_bridge_handler(DobbyRegisterContext *ctx, ClosureTrampoline *tramp) {
  typedef void (*routing_handler_t)(Interceptor::Entry *, DobbyRegisterContext *);
  auto routing_handler = (routing_handler_t)features::apple::arm64e_pac_strip_and_sign(tramp->carry_handler);
  routing_handler((Interceptor::Entry *)tramp->carry_data, ctx);
  return;
}
