#include "../PyRpcAckDecoder.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

using namespace build_import;

namespace {

enum class ResultEncoding { TrueValue, FalseValue, IntegerOne, IntegerZero, IntegerTwo };

void appendVarUInt(std::string* output, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        output->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void appendBigEndian(std::string* output, uint64_t value, size_t width) {
    for (size_t index = width; index > 0U; --index) {
        output->push_back(static_cast<char>((value >> ((index - 1U) * 8U)) & 0xFFU));
    }
}

void appendLittleEndian32(std::string* output, uint32_t value) {
    for (size_t index = 0; index < 4U; ++index) {
        output->push_back(static_cast<char>((value >> (index * 8U)) & 0xFFU));
    }
}

void appendArrayHeader(std::string* output, uint32_t count) {
    if (count <= 15U) {
        output->push_back(static_cast<char>(0x90U | count));
    } else if (count <= 0xFFFFU) {
        output->push_back(static_cast<char>(0xDCU));
        appendBigEndian(output, count, 2U);
    } else {
        output->push_back(static_cast<char>(0xDDU));
        appendBigEndian(output, count, 4U);
    }
}

void appendMapHeader(std::string* output, uint32_t count) {
    if (count <= 15U) {
        output->push_back(static_cast<char>(0x80U | count));
    } else if (count <= 0xFFFFU) {
        output->push_back(static_cast<char>(0xDEU));
        appendBigEndian(output, count, 2U);
    } else {
        output->push_back(static_cast<char>(0xDFU));
        appendBigEndian(output, count, 4U);
    }
}

void appendText(std::string* output, std::string_view value, bool binary = false) {
    if (binary) {
        if (value.size() <= 0xFFU) {
            output->push_back(static_cast<char>(0xC4U));
            appendBigEndian(output, value.size(), 1U);
        } else if (value.size() <= 0xFFFFU) {
            output->push_back(static_cast<char>(0xC5U));
            appendBigEndian(output, value.size(), 2U);
        } else {
            output->push_back(static_cast<char>(0xC6U));
            appendBigEndian(output, value.size(), 4U);
        }
    } else if (value.size() <= 31U) {
        output->push_back(static_cast<char>(0xA0U | value.size()));
    } else if (value.size() <= 0xFFU) {
        output->push_back(static_cast<char>(0xD9U));
        appendBigEndian(output, value.size(), 1U);
    } else if (value.size() <= 0xFFFFU) {
        output->push_back(static_cast<char>(0xDAU));
        appendBigEndian(output, value.size(), 2U);
    } else {
        output->push_back(static_cast<char>(0xDBU));
        appendBigEndian(output, value.size(), 4U);
    }
    output->append(value.data(), value.size());
}

void appendResult(std::string* output, ResultEncoding encoding) {
    switch (encoding) {
        case ResultEncoding::TrueValue: output->push_back(static_cast<char>(0xC3U)); break;
        case ResultEncoding::FalseValue: output->push_back(static_cast<char>(0xC2U)); break;
        case ResultEncoding::IntegerOne:
            output->push_back(static_cast<char>(0xCCU));
            output->push_back(1);
            break;
        case ResultEncoding::IntegerZero:
            output->push_back(static_cast<char>(0xD0U));
            output->push_back(0);
            break;
        case ResultEncoding::IntegerTwo: output->push_back(2); break;
    }
}

std::string envelope(std::string payload, uint32_t message_id = kPyRpcClientMessageId,
                     uint32_t wire_header = kPyRpcPacketId) {
    std::string packet;
    appendVarUInt(&packet, wire_header);
    appendVarUInt(&packet, static_cast<uint32_t>(payload.size()));
    packet += payload;
    appendLittleEndian32(&packet, message_id);
    return packet;
}

void appendRpcPrefix(std::string* payload, std::string_view event_name, bool binary) {
    appendArrayHeader(payload, 3U);
    appendText(payload, "ModEventS2C", binary);
    appendArrayHeader(payload, 4U);
    appendText(payload, "Minecraft", binary);
    appendText(payload, "aiCommand", binary);
    appendText(payload, event_name, binary);
}

std::string afterExecutePayload(std::string_view uuid, ResultEncoding result,
                                bool binary = false, bool add_unknown = false) {
    std::string payload;
    appendRpcPrefix(&payload, "AfterExecuteCommandEvent", binary);
    appendMapHeader(&payload, add_unknown ? 3U : 2U);
    appendText(&payload, "uuid", binary);
    appendText(&payload, uuid, binary);
    if (add_unknown) {
        appendText(&payload, "extra", binary);
        appendArrayHeader(&payload, 2U);
        payload.push_back(static_cast<char>(0xC0U));
        appendMapHeader(&payload, 1U);
        appendText(&payload, "value", binary);
        payload.push_back(7);
    }
    appendText(&payload, "executeResult", binary);
    appendResult(&payload, result);
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

std::string availableFailurePayload(std::string_view reason, bool binary = false) {
    std::string payload;
    appendRpcPrefix(&payload, "AvailableCheckFailed", binary);
    appendMapHeader(&payload, 1U);
    appendText(&payload, "reason", binary);
    appendText(&payload, reason, binary);
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

std::string commandOutputPayload(std::string_view uuid, bool binary = false) {
    std::string payload;
    appendRpcPrefix(&payload, "ExecuteCommandOutputEvent", binary);
    appendMapHeader(&payload, 5U);
    appendText(&payload, "msg", binary);
    appendText(&payload, "command output", binary);
    appendText(&payload, "cmd", binary);
    appendText(&payload, "/fill 0 0 0 1 1 1 minecraft:stone", binary);
    appendText(&payload, "uuid", binary);
    appendText(&payload, uuid, binary);
    appendText(&payload, "isReExecute", binary);
    appendResult(&payload, ResultEncoding::FalseValue);
    appendText(&payload, "extra", binary);
    appendArrayHeader(&payload, 2U);
    payload.push_back(static_cast<char>(0xC0U));
    payload.push_back(7);
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

std::string commandOutputMissingUuidPayload() {
    std::string payload;
    appendRpcPrefix(&payload, "ExecuteCommandOutputEvent", false);
    appendMapHeader(&payload, 2U);
    appendText(&payload, "msg");
    appendText(&payload, "command output");
    appendText(&payload, "cmd");
    appendText(&payload, "/fill 0 0 0 1 1 1 minecraft:stone");
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

std::string missingUuidPayload() {
    std::string payload;
    appendRpcPrefix(&payload, "AfterExecuteCommandEvent", false);
    appendMapHeader(&payload, 1U);
    appendText(&payload, "executeResult");
    appendResult(&payload, ResultEncoding::TrueValue);
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

std::string duplicateUuidPayload() {
    std::string payload;
    appendRpcPrefix(&payload, "AfterExecuteCommandEvent", false);
    appendMapHeader(&payload, 3U);
    appendText(&payload, "uuid");
    appendText(&payload, "first");
    appendText(&payload, "uuid");
    appendText(&payload, "second");
    appendText(&payload, "executeResult");
    appendResult(&payload, ResultEncoding::TrueValue);
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

std::string excessiveDepthPayload() {
    std::string payload;
    appendRpcPrefix(&payload, "AfterExecuteCommandEvent", false);
    appendMapHeader(&payload, 3U);
    appendText(&payload, "uuid");
    appendText(&payload, "depth-test");
    appendText(&payload, "executeResult");
    appendResult(&payload, ResultEncoding::TrueValue);
    appendText(&payload, "extra");
    for (size_t depth = 0; depth < 10U; ++depth) appendArrayHeader(&payload, 1U);
    payload.push_back(static_cast<char>(0xC0U));
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

std::string excessiveContainerPayload() {
    std::string payload;
    appendRpcPrefix(&payload, "AfterExecuteCommandEvent", false);
    appendMapHeader(&payload, 3U);
    appendText(&payload, "uuid");
    appendText(&payload, "container-test");
    appendText(&payload, "executeResult");
    appendResult(&payload, ResultEncoding::TrueValue);
    appendText(&payload, "extra");
    appendArrayHeader(&payload, 257U);
    payload.push_back(static_cast<char>(0xC0U));
    return payload;
}

}  // namespace

int main() {
    PyRpcAckEvent event;
    const std::string uuid = "123e4567-e89b-12d3-a456-426614174000";

    assert(decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::TrueValue)), &event));
    assert(event.kind == PyRpcAckEventKind::AfterExecuteCommand);
    assert(event.uuid == uuid && event.execute_result && event.reason.empty());

    assert(decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::IntegerOne, true, true)), &event));
    assert(event.kind == PyRpcAckEventKind::AfterExecuteCommand);
    assert(event.uuid == uuid && event.execute_result);

    assert(decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::IntegerZero)), &event));
    assert(!event.execute_result);
    assert(decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::FalseValue)), &event));
    assert(!event.execute_result);

    const std::string reason = "operator permission denied";
    assert(decodePyRpcAckPacket(envelope(availableFailurePayload(reason, true)), &event));
    assert(event.kind == PyRpcAckEventKind::AvailableCheckFailed);
    assert(event.reason == reason && event.uuid.empty());
    assert(shouldSuppressBuildImportRpcFeedback(event, true));
    assert(!shouldSuppressBuildImportRpcFeedback(event, false));

    assert(decodePyRpcAckPacket(
        envelope(commandOutputPayload(kBuildImportSilentRpcUuid, true)), &event));
    assert(event.kind == PyRpcAckEventKind::ExecuteCommandOutput);
    assert(event.uuid == kBuildImportSilentRpcUuid);
    assert(!event.execute_result && event.reason.empty());
    assert(shouldSuppressBuildImportRpcFeedback(event, false));
    assert(isBuildImportRpcUuid(event.uuid));
    assert(isSilentBuildImportRpcPacket(
        envelope(commandOutputPayload(kBuildImportSilentRpcUuid, true))));
    assert(isSilentBuildImportRpcPacket(envelope(afterExecutePayload(
        kBuildImportSilentRpcUuid, ResultEncoding::TrueValue, true, true))));
    assert(!isSilentBuildImportRpcPacket(
        envelope(commandOutputPayload("infinitecz_build_123_456"))));
    assert(!isSilentBuildImportRpcPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::TrueValue))));
    assert(isBuildImportRpcUuid("infinitecz_build_123_456"));
    assert(!isBuildImportRpcUuid("unrelated_build_123_456"));
    assert(isBuildExportTeleportProbeUuid("infinitecz_export_tp_probe_123_456"));
    assert(!isBuildExportTeleportProbeUuid("infinitecz_export_tp_123_456"));

    assert(decodePyRpcAckPacket(
        envelope(commandOutputPayload("infinitecz_build_123_456")), &event));
    assert(event.kind == PyRpcAckEventKind::ExecuteCommandOutput);
    assert(isBuildImportRpcUuid(event.uuid));

    const uint32_t subclient_header = kPyRpcPacketId | (2U << 10U) | (1U << 12U);
    assert(decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::TrueValue),
                 kPyRpcClientMessageId, subclient_header), &event));

    assert(!decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::TrueValue),
                 kPyRpcClientMessageId, 0x4FU), &event));
    assert(event.kind == PyRpcAckEventKind::None);
    assert(!decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::TrueValue), 98247598U), &event));

    std::string trailing = envelope(afterExecutePayload(uuid, ResultEncoding::TrueValue));
    trailing.push_back('x');
    assert(!decodePyRpcAckPacket(trailing, &event));
    assert(!isSilentBuildImportRpcPacket(trailing));
    trailing.pop_back();
    trailing.pop_back();
    assert(!decodePyRpcAckPacket(trailing, &event));

    assert(!decodePyRpcAckPacket(envelope(missingUuidPayload()), &event));
    assert(!decodePyRpcAckPacket(envelope(commandOutputMissingUuidPayload()), &event));
    assert(!decodePyRpcAckPacket(envelope(duplicateUuidPayload()), &event));
    assert(!decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::IntegerTwo)), &event));
    assert(!decodePyRpcAckPacket(
        envelope(afterExecutePayload(std::string(129U, 'u'), ResultEncoding::TrueValue)), &event));
    assert(!decodePyRpcAckPacket(
        envelope(availableFailurePayload(std::string(4097U, 'r'))), &event));
    assert(!decodePyRpcAckPacket(envelope(excessiveDepthPayload()), &event));
    assert(!decodePyRpcAckPacket(envelope(excessiveContainerPayload()), &event));
    assert(!decodePyRpcAckPacket(envelope(std::string(65537U, '\0')), &event));
    assert(!decodePyRpcAckPacket(std::string("\xC8\x80\x80\x80\x80\x10", 6U), &event));
    assert(!decodePyRpcAckPacket(
        envelope(afterExecutePayload(uuid, ResultEncoding::TrueValue)), nullptr));
    return 0;
}
