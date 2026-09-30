#include "MapManualOutboundTrace.h"

#include "MapAnvilCreatedOutputProbe.h"
#include "MapAnvilDebugBridge.h"
#include "ProjectionPrinterInventoryMailbox.h"
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"

#include <android/log.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace build_import {
namespace {

// libminecraftpe.so ccf9c31f3121a46e89d868dcedb239466b3ac726.
// The offsets below are corroborated by the current native inventory mover
// and by the request/action constructors in this exact game image. They are
// read-only; a different game image simply produces no trace.
constexpr uintptr_t kPacketVtableRva = 0x12951140ULL;
constexpr uintptr_t kPacketConstructorRva = 0x0B4C4A04ULL;
constexpr uintptr_t kRequestDataConstructorRva = 0x0DAA4018ULL;
constexpr uintptr_t kPlaceConstructorRva = 0x0DAA2390ULL;
constexpr uintptr_t kSwapConstructorRva = 0x0DAA2468ULL;
constexpr uintptr_t kConsumeConstructorRva = 0x0DAA2D04ULL;
constexpr uintptr_t kCraftOptionalConstructorRva = 0x0DA66EFCULL;
constexpr uintptr_t kPlaceVtableRva = 0x12AA8130ULL;
constexpr uintptr_t kSwapVtableRva = 0x12AA8178ULL;
constexpr uintptr_t kConsumeVtableRva = 0x12AA8250ULL;
constexpr uintptr_t kCraftOptionalVtableRva = 0x12AA4978ULL;
// Tag-1 NetIdVariant embeds a predicted-stack request object. Its first word
// is this process-relative vtable pointer, not a negative network stack ID.
constexpr uintptr_t kPredictedStackIdVtableRva = 0x12AAA500ULL;
constexpr size_t kPacketBatchOffset = 0x30U;
constexpr size_t kRequestIdOffset = 0x08U;
// RequestDataAddAction at 0x0DAA5A18 writes an 8-byte action owner into the
// vector at +0x30. The 24-byte native-string vector starts at +0x10.
constexpr size_t kRequestActionsOffset = 0x30U;
constexpr size_t kRequestNamesOffset = 0x10U;
constexpr size_t kActionTypeOffset = 0x08U;
constexpr uint32_t kMaximumTraceLines = 128U;
constexpr auto kMaximumTraceTime = std::chrono::minutes(20);
constexpr size_t kMaximumRequestsPerPacket = 8U;
constexpr size_t kMaximumActionsPerRequest = 16U;
constexpr size_t kMaximumNamesPerRequest = 16U;

constexpr std::array<uint8_t, 32U> kPacketConstructorFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF4,0x4F,0x01,0xA9,0xFD,0x03,0x00,0x91,0xF3,0x03,0x01,0xAA,
    0xF4,0x03,0x00,0xAA,0xBC,0xDE,0xC9,0x97,0x68,0xA4,0x03,0xB0,0x08,0x01,0x05,0x91,
}};
constexpr std::array<uint8_t, 32U> kRequestDataConstructorFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0xF3,0x03,0x00,0xAA,
    0x67,0xA3,0x00,0x94,0x08,0x00,0x80,0x12,0x7F,0x7E,0x01,0xA9,0x7F,0x12,0x00,0xF9,
}};
constexpr std::array<uint8_t, 32U> kPlaceConstructorFingerprint{{
    0xFD,0x7B,0xBD,0xA9,0xF6,0x57,0x01,0xA9,0xF4,0x4F,0x02,0xA9,0xFD,0x03,0x00,0x91,
    0x28,0x20,0x80,0x52,0x01,0x2C,0x00,0x39,0xF3,0x03,0x00,0xAA,0x08,0x10,0x00,0x79,
}};
constexpr std::array<uint8_t, 32U> kSwapConstructorFingerprint{{
    0xFD,0x7B,0xBD,0xA9,0xF6,0x57,0x01,0xA9,0xF4,0x4F,0x02,0xA9,0xFD,0x03,0x00,0x91,
    0xE8,0x6B,0xFA,0xB0,0xF3,0x03,0x00,0xAA,0xF4,0x03,0x00,0xAA,0x00,0x7D,0x40,0xFD,
}};
constexpr std::array<uint8_t, 32U> kConsumeConstructorFingerprint{{
    0xFF,0x03,0x02,0xD1,0xFD,0x7B,0x04,0xA9,0xF7,0x2B,0x00,0xF9,0xF6,0x57,0x06,0xA9,
    0xF4,0x4F,0x07,0xA9,0xFD,0x03,0x01,0x91,0x56,0xD0,0x3B,0xD5,0xF7,0x43,0x00,0x91,
}};
constexpr std::array<uint8_t, 32U> kCraftOptionalConstructorFingerprint{{
    0xFD,0x7B,0xBD,0xA9,0xF5,0x0B,0x00,0xF9,0xF4,0x4F,0x02,0xA9,0xFD,0x03,0x00,0x91,
    0xF5,0x03,0x01,0xAA,0xE1,0x01,0x80,0x52,0xF4,0x03,0x02,0x2A,0xF3,0x03,0x00,0xAA,
}};

std::atomic<uint8_t> g_profile_state{0U}; // 0 unchecked, 1 valid, 2 incompatible
std::atomic<bool> g_trace_finished{false};
std::atomic<int64_t> g_trace_deadline_ns{0};
std::mutex g_trace_mutex;
bool g_trace_started = false;
std::chrono::steady_clock::time_point g_trace_deadline;
uint32_t g_trace_lines = 0U;

template <typename T>
bool readValue(const void* base, size_t offset, T* output) noexcept {
    if (!base || !output) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    if (address > std::numeric_limits<uintptr_t>::max() - offset) return false;
    const void* source = reinterpret_cast<const void*>(address + offset);
    if (!IsMemoryReadable(source, sizeof(T))) return false;
    std::memcpy(output, source, sizeof(T));
    return true;
}

template <size_t N>
bool fingerprintMatches(uintptr_t base, uintptr_t rva,
                        const std::array<uint8_t, N>& fingerprint) noexcept {
    uintptr_t address = 0;
    return ResolveMinecraftExecutableOffset(base, rva, N, &address) &&
           IsMemoryReadable(reinterpret_cast<const void*>(address), N) &&
           std::memcmp(reinterpret_cast<const void*>(address),
                       fingerprint.data(), N) == 0;
}

bool profileMatches(uintptr_t base) noexcept {
    if (base == 0U || base > std::numeric_limits<uintptr_t>::max() - kPacketVtableRva) {
        return false;
    }
    const uint8_t state = g_profile_state.load(std::memory_order_acquire);
    if (state != 0U) return state == 1U;
    const bool matches =
        fingerprintMatches(base, kPacketConstructorRva, kPacketConstructorFingerprint) &&
        fingerprintMatches(base, kRequestDataConstructorRva,
                           kRequestDataConstructorFingerprint) &&
        fingerprintMatches(base, kPlaceConstructorRva, kPlaceConstructorFingerprint) &&
        fingerprintMatches(base, kSwapConstructorRva, kSwapConstructorFingerprint) &&
        fingerprintMatches(base, kConsumeConstructorRva,
                           kConsumeConstructorFingerprint) &&
        fingerprintMatches(base, kCraftOptionalConstructorRva,
                           kCraftOptionalConstructorFingerprint);
    uint8_t unchecked = 0U;
    const uint8_t verified = matches ? 1U : 2U;
    if (g_profile_state.compare_exchange_strong(
            unchecked, verified, std::memory_order_acq_rel,
            std::memory_order_acquire)) return matches;
    // Another thread may have completed verification while this one was
    // reading bytes. A stale post-hook failure must not overwrite a profile
    // that was verified before instrumentation.
    return unchecked == 1U;
}

struct PointerVector {
    uintptr_t begin = 0;
    uintptr_t end = 0;
    uintptr_t capacity = 0;
};

bool readVector(const void* object, size_t offset, size_t stride,
                size_t maximum, PointerVector* output, size_t* count) noexcept {
    if (!output || !count || stride == 0 ||
        !readValue(object, offset, output)) return false;
    const auto& v = *output;
    if (v.begin == 0U && v.end == 0U && v.capacity == 0U) {
        *count = 0U;
        return true;
    }
    if (v.begin == 0U || v.begin > v.end || v.end > v.capacity ||
        (v.end - v.begin) % stride != 0U ||
        (v.end - v.begin) / stride > maximum ||
        !IsMemoryReadable(reinterpret_cast<const void*>(v.begin), v.end - v.begin)) {
        return false;
    }
    *count = (v.end - v.begin) / stride;
    return true;
}

bool readNativeStringLength(const void* item, size_t* length) noexcept {
    if (!item || !length || !IsMemoryReadable(item, 24U)) return false;
    const auto* bytes = static_cast<const uint8_t*>(item);
    if ((bytes[0] & 1U) == 0U) {
        *length = static_cast<size_t>(bytes[0] >> 1U);
        return *length <= 22U;
    }
    size_t long_length = 0;
    std::memcpy(&long_length, bytes + 8U, sizeof(long_length));
    if (long_length > 1024U) return false;
    *length = long_length;
    return true;
}

struct SlotSummary {
    uint8_t container = 0;
    uint8_t dynamic = 0;
    uint32_t dynamic_id = 0;
    uint8_t slot = 0;
    int32_t network_id = 0;
};

bool readSlot(const void* action, size_t offset, SlotSummary* output) noexcept {
    if (!output) return false;
    return readValue(action, offset, &output->container) &&
        readValue(action, offset + 4U, &output->dynamic_id) &&
        readValue(action, offset + 8U, &output->dynamic) &&
        output->dynamic <= 1U &&
        readValue(action, offset + 0xCU, &output->slot) &&
        readValue(action, offset + 0x10U, &output->network_id);
}

void logLine(const char* format, ...) noexcept __attribute__((format(printf, 1, 2)));
void logLine(const char* format, ...) noexcept {
    if (g_trace_lines >= kMaximumTraceLines ||
        std::chrono::steady_clock::now() >= g_trace_deadline) return;
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "Infinitecz_MapManualTrace", format, args);
    va_end(args);
    ++g_trace_lines;
}

void observeRequest(const void* request, uintptr_t base, size_t request_index) noexcept {
    int32_t request_id = 0;
    PointerVector actions;
    size_t action_count = 0;
    PointerVector names;
    size_t name_count = 0;
    if (!readValue(request, kRequestIdOffset, &request_id) || request_id == 0 ||
        !readVector(request, kRequestActionsOffset, sizeof(uintptr_t),
                    kMaximumActionsPerRequest, &actions, &action_count) ||
        !readVector(request, kRequestNamesOffset, 24U,
                    kMaximumNamesPerRequest, &names, &name_count)) return;

    logLine("out request=%d index=%zu actions=%zu custom_names=%zu",
            request_id, request_index, action_count, name_count);
    for (size_t i = 0; i < name_count; ++i) {
        size_t length = 0;
        const void* name = reinterpret_cast<const void*>(names.begin + i * 24U);
        if (readNativeStringLength(name, &length)) {
            logLine("out request=%d custom_name_index=%zu byte_length=%zu",
                    request_id, i, length);
        }
    }
    for (size_t i = 0; i < action_count; ++i) {
        uintptr_t action_address = 0;
        if (!readValue(reinterpret_cast<const void*>(actions.begin),
                       i * sizeof(uintptr_t), &action_address) || action_address == 0U) continue;
        const void* action = reinterpret_cast<const void*>(action_address);
        uintptr_t vtable = 0;
        uint8_t type = 0;
        if (!readValue(action, 0U, &vtable) ||
            !readValue(action, kActionTypeOffset, &type)) continue;
        if ((type == 1U && vtable == base + kPlaceVtableRva) ||
            (type == 2U && vtable == base + kSwapVtableRva)) {
            SlotSummary source;
            SlotSummary target;
            if (readSlot(action, 0x10U, &source) && readSlot(action, 0x38U, &target)) {
                uint8_t amount = 0;
                if (type == 1U && !readValue(action, 0x0BU, &amount)) continue;
                logLine("out request=%d action=%zu type=%u count=%u src=%u:%u dyn=%u:%u net=%d dst=%u:%u dyn=%u:%u net=%d",
                        request_id, i, static_cast<unsigned>(type),
                        static_cast<unsigned>(amount),
                        static_cast<unsigned>(source.container),
                        static_cast<unsigned>(source.slot),
                        static_cast<unsigned>(source.dynamic), source.dynamic_id,
                        source.network_id,
                        static_cast<unsigned>(target.container),
                        static_cast<unsigned>(target.slot),
                        static_cast<unsigned>(target.dynamic), target.dynamic_id,
                        target.network_id);
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
                if (type == 1U && source.container == 61U && source.slot == 50U) {
                    // The tag-1 payload contains a predicted-stack object
                    // {vtable, sequence}. Source SlotInfo starts at
                    // action+0x10 and its variant at SlotInfo+0x10.
                    uintptr_t predicted_stack_vtable = 0;
                    int32_t secondary = 0;
                    int32_t tag = 0;
                    if (readValue(action, 0x20U, &predicted_stack_vtable) &&
                        readValue(action, 0x28U, &secondary) &&
                        readValue(action, 0x30U, &tag)) {
                        const bool vtable_matches =
                            base <= std::numeric_limits<uintptr_t>::max() -
                                        kPredictedStackIdVtableRva &&
                            predicted_stack_vtable == base +
                                kPredictedStackIdVtableRva;
                        logLine("out request=%d action=%zu created_output_variant predicted_sequence=%d tag=%d equals_request_id=%d vtable_matches=%d",
                                request_id, i, secondary, tag,
                                secondary == request_id ? 1 : 0,
                                vtable_matches ? 1 : 0);
                    }
                }
#endif
                continue;
            }
        } else if (type == 5U && vtable == base + kConsumeVtableRva) {
            // libminecraftpe.so Consume ctor at 0xDAA2D04 writes amount at
            // +0x0B and the consumed SlotInfo at +0x10. Read-only evidence.
            uint8_t amount = 0;
            SlotSummary source;
            if (readValue(action, 0x0BU, &amount) && amount != 0U &&
                readSlot(action, 0x10U, &source)) {
                logLine("out request=%d action=%zu type=5 count=%u src=%u:%u dyn=%u:%u net=%d",
                        request_id, i, static_cast<unsigned>(amount),
                        static_cast<unsigned>(source.container),
                        static_cast<unsigned>(source.slot),
                        static_cast<unsigned>(source.dynamic), source.dynamic_id,
                        source.network_id);
                continue;
            }
        } else if (type == 15U && vtable == base + kCraftOptionalVtableRva) {
            uint32_t recipe_id = 0;
            int32_t filtered_string_index = 0;
            if (readValue(action, 0x0CU, &recipe_id) &&
                readValue(action, 0x10U, &filtered_string_index)) {
                logLine("out request=%d action=%zu type=15 recipe=%u filtered_string_index=%d",
                        request_id, i, recipe_id, filtered_string_index);
                continue;
            }
        }
        // Unknown actions have no guessed layout. Their verified base type is
        // enough to correlate later with the response and companion actions.
        logLine("out request=%d action=%zu type=%u fields=unverified",
                request_id, i, static_cast<unsigned>(type));
    }
}

void observeBoundedDebugRename(const void* request, uintptr_t base) noexcept {
    int32_t request_id = 0;
    PointerVector actions;
    size_t action_count = 0;
    PointerVector names;
    size_t name_count = 0;
    if (!readValue(request, kRequestIdOffset, &request_id) || request_id == 0 ||
        !readVector(request, kRequestActionsOffset, sizeof(uintptr_t),
                    kMaximumActionsPerRequest, &actions, &action_count) ||
        !readVector(request, kRequestNamesOffset, 24U,
                    kMaximumNamesPerRequest, &names, &name_count) ||
        name_count == 0U) return;
    uint32_t observed_recipe_id = 0;
    uintptr_t observed_craft_action = 0;
    for (size_t index = 0; index < action_count; ++index) {
        uintptr_t action_address = 0;
        if (!readValue(reinterpret_cast<const void*>(actions.begin),
                       index * sizeof(uintptr_t), &action_address) || !action_address) continue;
        const void* action = reinterpret_cast<const void*>(action_address);
        uintptr_t vtable = 0;
        uint8_t type = 0;
        uint32_t recipe_id = 0;
        if (readValue(action, 0U, &vtable) &&
            readValue(action, kActionTypeOffset, &type) &&
            type == 15U && vtable == base + kCraftOptionalVtableRva &&
            readValue(action, 0x0CU, &recipe_id) && recipe_id != 0U) {
            observed_recipe_id = recipe_id;
            observed_craft_action = action_address;
            break;
        }
    }
    if (observed_recipe_id == 0U) return;
    ObserveMapAnvilDebugOutboundRename(request_id, observed_recipe_id,
                                       static_cast<uint32_t>(name_count));
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    uintptr_t caller_pc = 0;
    const bool found_ctor = LookupMapAnvilCraftCtorCaller(
        observed_craft_action, &caller_pc);
    uintptr_t verified_call_instruction = 0;
    const bool caller_in_game = found_ctor && caller_pc >= base + 4U &&
        ResolveMinecraftExecutableOffset(
            base, caller_pc - base - 4U, 4U,
            &verified_call_instruction) &&
        verified_call_instruction == caller_pc - 4U;
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapAnvilProbe",
        "craft_ctor_origin request=%d found=%d game_rx=%d caller_rva=0x%llx",
        request_id, found_ctor ? 1 : 0, caller_in_game ? 1 : 0,
        caller_in_game ? static_cast<unsigned long long>(caller_pc - base) : 0ULL);
    // This correlation must not depend on the broad 128-line/20-minute
    // manual trace. A live anvil rename may happen after that log budget has
    // expired, while the narrow one-shot bridge is still collecting evidence.
    for (size_t index = 0; index < action_count; ++index) {
        uintptr_t action_address = 0;
        if (!readValue(reinterpret_cast<const void*>(actions.begin),
                       index * sizeof(uintptr_t), &action_address) ||
            action_address == 0U) continue;
        const void* action = reinterpret_cast<const void*>(action_address);
        uintptr_t vtable = 0;
        uint8_t type = 0;
        uint8_t amount = 0;
        SlotSummary source;
        uintptr_t predicted_stack_vtable = 0;
        int32_t secondary = 0;
        int32_t tag = 0;
        if (!readValue(action, 0U, &vtable) ||
            vtable != base + kPlaceVtableRva ||
            !readValue(action, kActionTypeOffset, &type) || type != 1U ||
            !readValue(action, 0x0BU, &amount) || amount != 1U ||
            !readSlot(action, 0x10U, &source) ||
            source.container != 61U || source.slot != 50U ||
            !readValue(action, 0x20U, &predicted_stack_vtable) ||
            !readValue(action, 0x28U, &secondary) ||
            !readValue(action, 0x30U, &tag) ||
            base > std::numeric_limits<uintptr_t>::max() -
                       kPredictedStackIdVtableRva ||
            predicted_stack_vtable != base + kPredictedStackIdVtableRva ||
            tag != 1) continue;
        ObserveMapAnvilDebugOutboundCreatedOutputVariant(
            request_id, source.network_id, secondary, tag);
    }
#endif
}

void observeBoundedDebugAnvilInput(const void* request, uintptr_t base) noexcept {
    int32_t request_id = 0;
    PointerVector actions;
    size_t action_count = 0;
    PointerVector names;
    size_t name_count = 0;
    if (!readValue(request, kRequestIdOffset, &request_id) || request_id == 0 ||
        !readVector(request, kRequestActionsOffset, sizeof(uintptr_t),
                    kMaximumActionsPerRequest, &actions, &action_count) ||
        !readVector(request, kRequestNamesOffset, 24U,
                    kMaximumNamesPerRequest, &names, &name_count) ||
        action_count != 1U || name_count != 0U) return;
    uintptr_t action_address = 0;
    if (!readValue(reinterpret_cast<const void*>(actions.begin), 0U,
                   &action_address) || !action_address) return;
    const void* action = reinterpret_cast<const void*>(action_address);
    uintptr_t vtable = 0;
    uint8_t type = 0;
    uint8_t amount = 0;
    SlotSummary source;
    SlotSummary target;
    if (!readValue(action, 0U, &vtable) || vtable != base + kPlaceVtableRva ||
        !readValue(action, kActionTypeOffset, &type) || type != 1U ||
        !readValue(action, 0x0BU, &amount) || amount != 1U ||
        !readSlot(action, 0x10U, &source) ||
        !readSlot(action, 0x38U, &target) ||
        !((source.container == 29U && source.slot <= 8U) ||
          (source.container == 30U && source.slot >= 9U &&
           source.slot <= 35U)) ||
        source.dynamic != 0U || source.dynamic_id != 0U ||
        source.network_id <= 0 || target.container != 0U ||
        target.slot != 1U || target.dynamic != 0U ||
        target.dynamic_id != 0U || target.network_id != 0) return;
    ObserveMapAnvilDebugOutboundInput(request_id, source.network_id);
}

}  // namespace

bool PrimeMapManualOutboundTraceProfile() noexcept {
#if !defined(__aarch64__)
    return false;
#else
    return profileMatches(Main::getBaseAddress());
#endif
}

void ObserveMapManualOutboundPacket(const void* packet) noexcept {
#if !defined(__aarch64__)
    (void)packet;
#else
    if (!packet) return;
    const uintptr_t base = Main::getBaseAddress();
    if (!base || base > std::numeric_limits<uintptr_t>::max() - kPacketVtableRva) return;
    uintptr_t vtable = 0;
    if (!readValue(packet, 0U, &vtable) || vtable != base + kPacketVtableRva ||
        !profileMatches(base)) return;
    uintptr_t batch_address = 0;
    if (!readValue(packet, kPacketBatchOffset, &batch_address) || !batch_address) return;
    PointerVector requests;
    size_t request_count = 0;
    if (!readVector(reinterpret_cast<const void*>(batch_address), 0U,
                    sizeof(uintptr_t), kMaximumRequestsPerPacket,
                    &requests, &request_count) || request_count == 0U) return;
    for (size_t i = 0; i < request_count; ++i) {
        uintptr_t request_address = 0;
        if (readValue(reinterpret_cast<const void*>(requests.begin),
                      i * sizeof(uintptr_t), &request_address) && request_address != 0U) {
            observeBoundedDebugAnvilInput(
                reinterpret_cast<const void*>(request_address), base);
            observeBoundedDebugRename(reinterpret_cast<const void*>(request_address), base);
        }
    }
    if (g_trace_finished.load(std::memory_order_acquire)) return;
    const int64_t deadline_ns = g_trace_deadline_ns.load(std::memory_order_acquire);
    if (deadline_ns != 0 &&
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() >= deadline_ns) {
        if (!g_trace_finished.exchange(true, std::memory_order_acq_rel)) {
            EndMapManualTraceResponseSession();
        }
        return;
    }
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    if (!g_trace_started) {
        g_trace_started = true;
        g_trace_deadline = std::chrono::steady_clock::now() + kMaximumTraceTime;
        BeginMapManualTraceResponseSession();
        g_trace_deadline_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                g_trace_deadline.time_since_epoch()).count(),
            std::memory_order_release);
    }
    if (g_trace_lines >= kMaximumTraceLines ||
        std::chrono::steady_clock::now() >= g_trace_deadline) {
        g_trace_finished.store(true, std::memory_order_release);
        EndMapManualTraceResponseSession();
        return;
    }
    for (size_t i = 0; i < request_count; ++i) {
        uintptr_t request_address = 0;
        if (readValue(reinterpret_cast<const void*>(requests.begin),
                      i * sizeof(uintptr_t), &request_address) && request_address != 0U) {
            observeRequest(reinterpret_cast<const void*>(request_address), base, i);
        }
    }
    if (g_trace_lines >= kMaximumTraceLines ||
        std::chrono::steady_clock::now() >= g_trace_deadline) {
        g_trace_finished.store(true, std::memory_order_release);
        EndMapManualTraceResponseSession();
    }
#endif
}

}  // namespace build_import
