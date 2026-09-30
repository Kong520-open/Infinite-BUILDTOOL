#if defined(__aarch64__)
    .text
    .p2align 2
    .hidden BuildToolCreateItemUseTransaction
    .global BuildToolCreateItemUseTransaction
    .type BuildToolCreateItemUseTransaction, %function

// C ABI: void bridge(uintptr_t factory, int32_t type, void** output)
// The engine factory returns its unique_ptr through the AAPCS64 x8 indirect
// result register. Tail-calling it keeps the original caller's return address.
BuildToolCreateItemUseTransaction:
    mov x9, x0
    mov w0, w1
    mov x8, x2
    br x9

    .size BuildToolCreateItemUseTransaction, .-BuildToolCreateItemUseTransaction
#endif
