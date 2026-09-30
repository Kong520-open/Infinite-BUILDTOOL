#ifndef INFINITE_TEXTURE_PY_RPC_POINTER_BRIDGE_H
#define INFINITE_TEXTURE_PY_RPC_POINTER_BRIDGE_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace build_import {

// The pointer bridge is synchronous: Python copies this many bytes before the
// C++ call returns. Larger payloads retain the literal fallback so an unusual
// command cannot make ctypes read an accidentally truncated buffer.
constexpr size_t kMaximumRpcPointerPayloadBytes = 2U * 1024U * 1024U;

enum class RpcPointerCall : uint8_t {
    SendUntracked,
    SendTracked,
    Poll,
    Probe,
};

inline std::string makeRpcPointerCall(RpcPointerCall call, const void* data,
                                      size_t size) {
    if (!data || size == 0 || size > kMaximumRpcPointerPayloadBytes) return {};
    const std::string address = std::to_string(
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(data)));
    const std::string length = std::to_string(static_cast<unsigned long long>(size));
    if (call == RpcPointerCall::Poll) {
        return "import sys\nsys._build_import_rpc_poll_ptr(" + address + "L," +
            length + ")\n";
    }
    if (call == RpcPointerCall::Probe) {
        return "import sys\nsys._build_import_rpc_probe_ptr(" + address + "L," +
            length + ")\n";
    }
    return "import sys\nsys._build_import_rpc_ptr(" + address + "L," + length +
        (call == RpcPointerCall::SendTracked ? ",1)\n" : ",0)\n");
}

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PY_RPC_POINTER_BRIDGE_H
