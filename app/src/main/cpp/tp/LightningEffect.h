#pragma once

#include <cstdint>

class LightningEffect {
public:
    // Idempotent and retryable. Returns true only when the network receive
    // hook required for command acknowledgements is ready.
    static bool init(uintptr_t baseAddr);
    static bool isReceiveHookReady();
    static void revoke() noexcept;
};
