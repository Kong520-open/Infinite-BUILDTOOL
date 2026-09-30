#include "../PyRpcEnvelopeBridge.h"
#include "../PyRpcPointerBridge.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

using namespace build_import;

int main() {
    const std::string envelope_source = rpcReusableEnvelopePythonSource();
    assert(envelope_source.find(
        "_build_import_rpc_template_state = [None]") != std::string::npos);
    assert(envelope_source.find(
        "cmd_token = _pack_string(cmd_marker)\n") != std::string::npos);
    assert(envelope_source.find(
        "if probe.count(cmd_token) != 1 or probe.count(uuid_token) != 1:\n") !=
           std::string::npos);
    assert(envelope_source.find(
        "signature = (packed_cmd[1], packed_uuid[1])\n") !=
           std::string::npos);
    assert(envelope_source.find(
        "if optimized != legacy:\n") !=
           std::string::npos);
    assert(envelope_source.find(
        "_state[0] = False\n"
        "    return _legacy(player_id, cmd, command_uuid)\n") !=
           std::string::npos);
    assert(envelope_source.find("_struct.pack('>BB', 217, size)") !=
           std::string::npos);
    assert(envelope_source.find("_struct.pack('>BH', 218, size)") !=
           std::string::npos);
    assert(envelope_source.find("_struct.pack('>BB', 196, size)") !=
           std::string::npos);
    assert(envelope_source.find("_struct.pack('>BH', 197, size)") !=
           std::string::npos);

    std::string payload = "/setblock 1 2 3 stone";
    payload.push_back('\0');
    payload += "uuid";
    const std::string address = std::to_string(static_cast<unsigned long long>(
        reinterpret_cast<uintptr_t>(payload.data())));
    const std::string size = std::to_string(payload.size());

    const std::string untracked = makeRpcPointerCall(
        RpcPointerCall::SendUntracked, payload.data(), payload.size());
    assert(untracked == "import sys\nsys._build_import_rpc_ptr(" + address +
        "L," + size + ",0)\n");

    const std::string tracked = makeRpcPointerCall(
        RpcPointerCall::SendTracked, payload.data(), payload.size());
    assert(tracked == "import sys\nsys._build_import_rpc_ptr(" + address +
        "L," + size + ",1)\n");

    const std::string poll = makeRpcPointerCall(
        RpcPointerCall::Poll, payload.data(), payload.size());
    assert(poll == "import sys\nsys._build_import_rpc_poll_ptr(" + address +
        "L," + size + ")\n");

    const std::string probe = makeRpcPointerCall(
        RpcPointerCall::Probe, payload.data(), payload.size());
    assert(probe == "import sys\nsys._build_import_rpc_probe_ptr(" + address +
        "L," + size + ")\n");

    assert(makeRpcPointerCall(RpcPointerCall::SendUntracked, nullptr, 1).empty());
    assert(makeRpcPointerCall(RpcPointerCall::SendUntracked,
                              payload.data(), 0).empty());
    assert(makeRpcPointerCall(RpcPointerCall::SendUntracked, payload.data(),
                              kMaximumRpcPointerPayloadBytes + 1U).empty());
    return 0;
}
