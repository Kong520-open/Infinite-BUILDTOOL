#include "PyRpcAckDecoder.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace build_import {
namespace {

constexpr size_t kMaxPayloadBytes = 64U * 1024U;
constexpr size_t kMaxStringBytes = 16U * 1024U;
constexpr size_t kMaxUuidBytes = 128U;
constexpr size_t kMaxReasonBytes = 4096U;
constexpr uint32_t kMaxContainerEntries = 256U;
constexpr size_t kMaxDepth = 8U;
constexpr size_t kMaxSkippedNodes = 4096U;

bool readVarUInt(std::string_view input, size_t* cursor, uint32_t* output) {
    if (!cursor || !output) return false;
    uint32_t value = 0;
    for (uint32_t byte_index = 0; byte_index < 5U && *cursor < input.size(); ++byte_index) {
        const uint8_t byte = static_cast<uint8_t>(input[(*cursor)++]);
        if (byte_index == 4U && (byte & 0xF0U) != 0U) return false;
        value |= static_cast<uint32_t>(byte & 0x7FU) << (byte_index * 7U);
        if ((byte & 0x80U) == 0U) {
            *output = value;
            return true;
        }
    }
    return false;
}

uint32_t readLittleEndian32(std::string_view input, size_t offset) {
    return static_cast<uint32_t>(static_cast<uint8_t>(input[offset])) |
        (static_cast<uint32_t>(static_cast<uint8_t>(input[offset + 1U])) << 8U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(input[offset + 2U])) << 16U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(input[offset + 3U])) << 24U);
}

bool extractPyRpcPayload(std::string_view packet, std::string_view* payload) {
    if (!payload) return false;
    *payload = {};
    size_t cursor = 0U;
    uint32_t wire_header = 0;
    uint32_t payload_length = 0;
    if (!readVarUInt(packet, &cursor, &wire_header) ||
        (wire_header & 0x3FFU) != kPyRpcPacketId ||
        !readVarUInt(packet, &cursor, &payload_length) ||
        payload_length == 0U || payload_length > kMaxPayloadBytes ||
        payload_length > packet.size() - cursor) {
        return false;
    }
    const std::string_view decoded = packet.substr(cursor, payload_length);
    cursor += payload_length;
    if (packet.size() - cursor != sizeof(uint32_t) ||
        readLittleEndian32(packet, cursor) != kPyRpcClientMessageId) {
        return false;
    }
    *payload = decoded;
    return true;
}

class MessagePackReader {
public:
    explicit MessagePackReader(std::string_view input) : input_(input) {}

    bool atEnd() const { return cursor_ == input_.size(); }

    bool readArrayHeader(uint32_t* count) {
        uint8_t code = 0;
        if (!readByte(&code) || !readContainerCount(code, 0x90U, 0x9FU, 0xDCU, 0xDDU, count)) {
            return false;
        }
        return *count <= kMaxContainerEntries;
    }

    bool readMapHeader(uint32_t* count) {
        uint8_t code = 0;
        if (!readByte(&code) || !readContainerCount(code, 0x80U, 0x8FU, 0xDEU, 0xDFU, count)) {
            return false;
        }
        return *count <= kMaxContainerEntries;
    }

    bool readText(std::string_view* value) {
        if (!value) return false;
        uint8_t code = 0;
        uint32_t length = 0;
        if (!readByte(&code) || !readTextLength(code, &length) || length > kMaxStringBytes) {
            return false;
        }
        if (!has(length)) return false;
        *value = input_.substr(cursor_, length);
        cursor_ += length;
        return true;
    }

    bool readBoolean(bool* value) {
        if (!value) return false;
        uint8_t code = 0;
        if (!readByte(&code)) return false;
        if (code == 0xC2U || code == 0xC3U) {
            *value = code == 0xC3U;
            return true;
        }
        if (code <= 0x7FU) {
            if (code > 1U) return false;
            *value = code != 0U;
            return true;
        }
        size_t width = 0;
        switch (code) {
            case 0xCCU: case 0xD0U: width = 1U; break;
            case 0xCDU: case 0xD1U: width = 2U; break;
            case 0xCEU: case 0xD2U: width = 4U; break;
            case 0xCFU: case 0xD3U: width = 8U; break;
            default: return false;
        }
        uint64_t encoded = 0;
        if (!readBigEndian(width, &encoded) || encoded > 1U) return false;
        *value = encoded != 0U;
        return true;
    }

    bool readNil() {
        uint8_t code = 0;
        return readByte(&code) && code == 0xC0U;
    }

    bool skipValue(size_t depth = 0U) {
        if (depth > kMaxDepth || ++skipped_nodes_ > kMaxSkippedNodes) return false;
        uint8_t code = 0;
        if (!readByte(&code)) return false;

        if (code <= 0x7FU || code >= 0xE0U || code == 0xC0U ||
            code == 0xC2U || code == 0xC3U) {
            return true;
        }
        if ((code & 0xE0U) == 0xA0U) {
            return skipBytes(code & 0x1FU);
        }
        if ((code & 0xF0U) == 0x90U) {
            return skipArray(code & 0x0FU, depth);
        }
        if ((code & 0xF0U) == 0x80U) {
            return skipMap(code & 0x0FU, depth);
        }

        uint64_t length = 0;
        switch (code) {
            case 0xC1U: return false;
            case 0xC4U:
                if (!readBigEndian(1U, &length)) return false;
                return skipSizedBytes(length);
            case 0xC5U:
                if (!readBigEndian(2U, &length)) return false;
                return skipSizedBytes(length);
            case 0xC6U:
                if (!readBigEndian(4U, &length)) return false;
                return skipSizedBytes(length);
            case 0xC7U:
                if (!readBigEndian(1U, &length)) return false;
                return skipExtension(length);
            case 0xC8U:
                if (!readBigEndian(2U, &length)) return false;
                return skipExtension(length);
            case 0xC9U:
                if (!readBigEndian(4U, &length)) return false;
                return skipExtension(length);
            case 0xCAU: return skipBytes(4U);
            case 0xCBU: return skipBytes(8U);
            case 0xCCU: case 0xD0U: return skipBytes(1U);
            case 0xCDU: case 0xD1U: return skipBytes(2U);
            case 0xCEU: case 0xD2U: return skipBytes(4U);
            case 0xCFU: case 0xD3U: return skipBytes(8U);
            case 0xD4U: return skipBytes(2U);
            case 0xD5U: return skipBytes(3U);
            case 0xD6U: return skipBytes(5U);
            case 0xD7U: return skipBytes(9U);
            case 0xD8U: return skipBytes(17U);
            case 0xD9U:
                if (!readBigEndian(1U, &length)) return false;
                return skipSizedBytes(length);
            case 0xDAU:
                if (!readBigEndian(2U, &length)) return false;
                return skipSizedBytes(length);
            case 0xDBU:
                if (!readBigEndian(4U, &length)) return false;
                return skipSizedBytes(length);
            case 0xDCU:
                if (!readBigEndian(2U, &length)) return false;
                return length <= kMaxContainerEntries && skipArray(static_cast<uint32_t>(length), depth);
            case 0xDDU:
                if (!readBigEndian(4U, &length)) return false;
                return length <= kMaxContainerEntries && skipArray(static_cast<uint32_t>(length), depth);
            case 0xDEU:
                if (!readBigEndian(2U, &length)) return false;
                return length <= kMaxContainerEntries && skipMap(static_cast<uint32_t>(length), depth);
            case 0xDFU:
                if (!readBigEndian(4U, &length)) return false;
                return length <= kMaxContainerEntries && skipMap(static_cast<uint32_t>(length), depth);
            default: return false;
        }
    }

private:
    bool has(size_t length) const {
        return length <= input_.size() - cursor_;
    }

    bool readByte(uint8_t* value) {
        if (!value || !has(1U)) return false;
        *value = static_cast<uint8_t>(input_[cursor_++]);
        return true;
    }

    bool readBigEndian(size_t width, uint64_t* value) {
        if (!value || width == 0U || width > sizeof(uint64_t) || !has(width)) return false;
        uint64_t result = 0;
        for (size_t index = 0; index < width; ++index) {
            result = (result << 8U) | static_cast<uint8_t>(input_[cursor_++]);
        }
        *value = result;
        return true;
    }

    bool readContainerCount(uint8_t code, uint8_t fix_min, uint8_t fix_max,
                            uint8_t code16, uint8_t code32, uint32_t* count) {
        if (!count) return false;
        if (code >= fix_min && code <= fix_max) {
            *count = code & 0x0FU;
            return true;
        }
        size_t width = 0;
        if (code == code16) width = 2U;
        else if (code == code32) width = 4U;
        else return false;
        uint64_t decoded = 0;
        if (!readBigEndian(width, &decoded) || decoded > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *count = static_cast<uint32_t>(decoded);
        return true;
    }

    bool readTextLength(uint8_t code, uint32_t* length) {
        if (!length) return false;
        if ((code & 0xE0U) == 0xA0U) {
            *length = code & 0x1FU;
            return true;
        }
        size_t width = 0;
        switch (code) {
            case 0xC4U: case 0xD9U: width = 1U; break;
            case 0xC5U: case 0xDAU: width = 2U; break;
            case 0xC6U: case 0xDBU: width = 4U; break;
            default: return false;
        }
        uint64_t decoded = 0;
        if (!readBigEndian(width, &decoded) || decoded > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *length = static_cast<uint32_t>(decoded);
        return true;
    }

    bool skipBytes(size_t length) {
        if (!has(length)) return false;
        cursor_ += length;
        return true;
    }

    bool skipSizedBytes(uint64_t length) {
        return length <= kMaxStringBytes &&
            length <= std::numeric_limits<size_t>::max() &&
            skipBytes(static_cast<size_t>(length));
    }

    bool skipExtension(uint64_t length) {
        return length <= kMaxStringBytes && length < std::numeric_limits<size_t>::max() &&
            skipBytes(static_cast<size_t>(length) + 1U);
    }

    bool skipArray(uint32_t count, size_t depth) {
        if (depth >= kMaxDepth) return false;
        for (uint32_t index = 0; index < count; ++index) {
            if (!skipValue(depth + 1U)) return false;
        }
        return true;
    }

    bool skipMap(uint32_t count, size_t depth) {
        if (depth >= kMaxDepth) return false;
        for (uint32_t index = 0; index < count; ++index) {
            if (!skipValue(depth + 1U) || !skipValue(depth + 1U)) return false;
        }
        return true;
    }

    std::string_view input_;
    size_t cursor_ = 0U;
    size_t skipped_nodes_ = 0U;
};

bool decodePayload(std::string_view payload, PyRpcAckEvent* event) {
    MessagePackReader reader(payload);
    uint32_t root_count = 0;
    if (!reader.readArrayHeader(&root_count) || root_count != 3U) return false;

    std::string_view method;
    if (!reader.readText(&method) || method != "ModEventS2C") return false;

    uint32_t argument_count = 0;
    if (!reader.readArrayHeader(&argument_count) || argument_count != 4U) return false;
    std::string_view name_space;
    std::string_view system;
    std::string_view event_name;
    if (!reader.readText(&name_space) || name_space != "Minecraft" ||
        !reader.readText(&system) || system != "aiCommand" ||
        !reader.readText(&event_name)) {
        return false;
    }

    const bool is_after_execute = event_name == "AfterExecuteCommandEvent";
    const bool is_command_output = event_name == "ExecuteCommandOutputEvent";
    const bool is_available_failure = event_name == "AvailableCheckFailed";
    if (!is_after_execute && !is_command_output && !is_available_failure) return false;

    uint32_t field_count = 0;
    if (!reader.readMapHeader(&field_count)) return false;
    PyRpcAckEvent decoded;
    decoded.kind = is_after_execute
        ? PyRpcAckEventKind::AfterExecuteCommand
        : (is_command_output ? PyRpcAckEventKind::ExecuteCommandOutput
                             : PyRpcAckEventKind::AvailableCheckFailed);
    bool has_uuid = false;
    bool has_execute_result = false;
    bool has_reason = false;

    for (uint32_t field_index = 0; field_index < field_count; ++field_index) {
        std::string_view key;
        if (!reader.readText(&key)) return false;
        if ((is_after_execute || is_command_output) && key == "uuid") {
            if (has_uuid) return false;
            std::string_view uuid;
            if (!reader.readText(&uuid) || uuid.empty() || uuid.size() > kMaxUuidBytes) return false;
            decoded.uuid.assign(uuid.data(), uuid.size());
            has_uuid = true;
        } else if (is_after_execute && key == "executeResult") {
            if (has_execute_result || !reader.readBoolean(&decoded.execute_result)) return false;
            has_execute_result = true;
        } else if (is_available_failure && key == "reason") {
            if (has_reason) return false;
            std::string_view reason;
            if (!reader.readText(&reason) || reason.empty() || reason.size() > kMaxReasonBytes) {
                return false;
            }
            decoded.reason.assign(reason.data(), reason.size());
            has_reason = true;
        } else if (!reader.skipValue(2U)) {
            return false;
        }
    }

    if (!reader.readNil() || !reader.atEnd()) return false;
    if ((is_after_execute && (!has_uuid || !has_execute_result)) ||
        (is_command_output && !has_uuid) ||
        (is_available_failure && !has_reason)) {
        return false;
    }
    *event = std::move(decoded);
    return true;
}

bool isSilentPayload(std::string_view payload) {
    MessagePackReader reader(payload);
    uint32_t root_count = 0;
    if (!reader.readArrayHeader(&root_count) || root_count != 3U) return false;

    std::string_view method;
    if (!reader.readText(&method) || method != "ModEventS2C") return false;

    uint32_t argument_count = 0;
    std::string_view name_space;
    std::string_view system;
    std::string_view event_name;
    if (!reader.readArrayHeader(&argument_count) || argument_count != 4U ||
        !reader.readText(&name_space) || name_space != "Minecraft" ||
        !reader.readText(&system) || system != "aiCommand" ||
        !reader.readText(&event_name)) {
        return false;
    }
    const bool is_after_execute = event_name == "AfterExecuteCommandEvent";
    const bool is_command_output = event_name == "ExecuteCommandOutputEvent";
    if (!is_after_execute && !is_command_output) return false;

    uint32_t field_count = 0;
    if (!reader.readMapHeader(&field_count)) return false;
    bool has_uuid = false;
    bool silent_uuid = false;
    bool has_execute_result = false;
    for (uint32_t field_index = 0; field_index < field_count; ++field_index) {
        std::string_view key;
        if (!reader.readText(&key)) return false;
        if (key == "uuid") {
            if (has_uuid) return false;
            std::string_view uuid;
            if (!reader.readText(&uuid) || uuid.empty() || uuid.size() > kMaxUuidBytes) {
                return false;
            }
            has_uuid = true;
            silent_uuid = uuid == kBuildImportSilentRpcUuid;
        } else if (is_after_execute && key == "executeResult") {
            bool ignored = false;
            if (has_execute_result || !reader.readBoolean(&ignored)) return false;
            has_execute_result = true;
        } else if (!reader.skipValue(2U)) {
            return false;
        }
    }
    if (!reader.readNil() || !reader.atEnd()) return false;
    return has_uuid && silent_uuid && (!is_after_execute || has_execute_result);
}

}  // namespace

bool decodePyRpcAckPacket(std::string_view packet, PyRpcAckEvent* event) {
    if (!event) return false;
    *event = {};
    std::string_view payload;
    if (!extractPyRpcPayload(packet, &payload)) return false;
    return decodePayload(payload, event);
}

bool isSilentBuildImportRpcPacket(std::string_view packet) {
    std::string_view payload;
    return extractPyRpcPayload(packet, &payload) && isSilentPayload(payload);
}

}  // namespace build_import
