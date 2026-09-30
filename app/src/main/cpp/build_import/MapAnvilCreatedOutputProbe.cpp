#include "MapAnvilCreatedOutputProbe.h"

#include <cstring>

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "dobby.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#endif

namespace build_import {

bool DecodeMapAnvilCreatedOutputScalars(
    std::string_view header, std::string_view variant,
    MapAnvilCreatedOutputObservation* output) noexcept {
    if (!output || header.size() < 24U || variant.size() < 24U) return false;
    MapAnvilCreatedOutputObservation decoded;
    std::memcpy(&decoded.kind, header.data(), sizeof(decoded.kind));
    std::memcpy(&decoded.container_name, header.data() + 4U,
                sizeof(decoded.container_name));
    std::memcpy(&decoded.dynamic_id, header.data() + 8U,
                sizeof(decoded.dynamic_id));
    std::memcpy(&decoded.has_dynamic_id, header.data() + 12U,
                sizeof(decoded.has_dynamic_id));
    std::memcpy(&decoded.slot, header.data() + 16U, sizeof(decoded.slot));
    std::memcpy(&decoded.count, header.data() + 20U, sizeof(decoded.count));
    std::memcpy(&decoded.network_id, variant.data(),
                sizeof(decoded.network_id));
    std::memcpy(&decoded.secondary_id, variant.data() + 8U,
                sizeof(decoded.secondary_id));
    std::memcpy(&decoded.tag, variant.data() + 16U,
                sizeof(decoded.tag));
    *output = decoded;
    return true;
}

bool MapAnvilCreatedOutputIsTarget(
    const MapAnvilCreatedOutputObservation& record) noexcept {
    return record.kind == 4U && record.container_name == 61U &&
           record.slot == 50U;
}

bool MapAnvilCreatedOutputStorageIndex(
    uint32_t site, uint32_t site_ordinal, uint8_t* output) noexcept {
    if (!output) return false;
    if (site == 1U && site_ordinal < 8U) {
        *output = static_cast<uint8_t>(site_ordinal);
        return true;
    }
    if (site == 2U && site_ordinal < 24U) {
        *output = static_cast<uint8_t>(8U + site_ordinal);
        return true;
    }
    return false;
}

uint32_t MapAnvilCreatedOutputNewRecordBits(
    const MapAnvilCreatedOutputSnapshot& snapshot,
    uint32_t logged_mask) noexcept {
    uint32_t bits = 0U;
    const uint32_t count = snapshot.captured_count <= snapshot.records.size()
                               ? snapshot.captured_count
                               : static_cast<uint32_t>(snapshot.records.size());
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t callback_index = snapshot.records[index].callback_index;
        if (callback_index < 32U) bits |= 1U << callback_index;
    }
    return bits & ~logged_mask;
}

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
namespace {

// libminecraftpe.so Build ID ccf9c31f3121a46e89d868dcedb239466b3ac726.
// At site 1 x22 is the post-copy CreatedOutput record; at site 2 x24 is the
// same record just before it is consumed. These exact bytes are checked before
// installing instrumentation, so a different game image fails closed.
constexpr uintptr_t kConstructedRva = 0x0DA7A40CULL;
constexpr uintptr_t kBeforeConsumeRva = 0x0DA7DAACULL;
constexpr uintptr_t kAnvilSimulationEntryRva = 0x0DA7E458ULL;
constexpr uintptr_t kAnvilType2EntryRva = 0x0DA7D388ULL;
constexpr uintptr_t kCraftOptionalConstructorRva = 0x0DA66EFCULL;
constexpr uint8_t kConstructedFingerprint[32] = {
    0xC0,0x62,0x04,0x91,0x1B,0x00,0x00,0x14,0x00,0xE4,0x00,0x6F,0xFF,0x53,0x00,0xF9,
    0xC0,0x1A,0x80,0x3D,0xE0,0x83,0x01,0xAD,0xE0,0x83,0x02,0xAD,0xE0,0x83,0x03,0xAD,
};
constexpr uint8_t kBeforeConsumeFingerprint[32] = {
    0x80,0x26,0x40,0xF9,0xA8,0x07,0x80,0x52,0x49,0x06,0x80,0x52,0x7F,0x13,0x0C,0xF8,
    0xA8,0x83,0x18,0x38,0xA9,0x43,0x19,0xB8,0xE8,0x03,0x03,0x91,0xA1,0xE3,0x01,0xD1,
};
constexpr uint8_t kAnvilSimulationEntryFingerprint[32] = {
    0xFF,0x83,0x01,0xD1,0xFD,0x7B,0x02,0xA9,0xF7,0x1B,0x00,0xF9,0xF6,0x57,0x04,0xA9,
    0xF4,0x4F,0x05,0xA9,0xFD,0x83,0x00,0x91,0x57,0xD0,0x3B,0xD5,0xF3,0x03,0x01,0xAA,
};
constexpr uint8_t kAnvilType2EntryFingerprint[32] = {
    0xFD,0x7B,0xBA,0xA9,0xFC,0x6F,0x01,0xA9,0xFA,0x67,0x02,0xA9,0xF8,0x5F,0x03,0xA9,
    0xF6,0x57,0x04,0xA9,0xF4,0x4F,0x05,0xA9,0xFD,0x03,0x00,0x91,0xFF,0x43,0x0A,0xD1,
};
// Identical to the optional-craft constructor fingerprint in
// MapManualOutboundTrace. The constructor receives action storage in x0;
// this hook only records x0 and lr, never the recipe wrapper or action bytes.
constexpr uint8_t kCraftOptionalConstructorFingerprint[32] = {
    0xFD,0x7B,0xBD,0xA9,0xF5,0x0B,0x00,0xF9,0xF4,0x4F,0x02,0xA9,0xFD,0x03,0x00,0x91,
    0xF5,0x03,0x01,0xAA,0xE1,0x01,0x80,0x52,0xF4,0x03,0x02,0x2A,0xF3,0x03,0x00,0xAA,
};
constexpr size_t kRecordHeaderBytes = 24U;
constexpr uintptr_t kVariantOffset = 0x100U;
constexpr size_t kVariantBytes = 24U;
constexpr uint32_t kMaximumCallbacks = 32U;
// Raw attempts are independently bounded so generic preview traffic cannot
// consume the anvil path's budget. Only verified 61:50 records occupy the
// much smaller 8/24 observation quotas.
constexpr uint32_t kMaximumGenericAttempts = 512U;
constexpr uint32_t kMaximumAnvilAttempts = 4096U;
constexpr uint32_t kMaximumEntryCalls = 4096U;
// Preview generation can construct CraftRecipeOptional actions each frame.
// Keep enough bounded history for a later click within the 30-second window.
constexpr uint32_t kMaximumCtorRecords = 1024U;

struct AtomicRecord {
    std::atomic<uint64_t> ticket{0};
    std::atomic<uint32_t> site{0};
    std::atomic<uint32_t> kind{0};
    std::atomic<uint8_t> container_name{0};
    std::atomic<uint32_t> dynamic_id{0};
    std::atomic<uint8_t> has_dynamic_id{0};
    std::atomic<uint8_t> slot{0};
    std::atomic<uint32_t> count{0};
    std::atomic<int32_t> network_id{0};
    std::atomic<int32_t> secondary_id{0};
    std::atomic<int32_t> tag{0};
    std::atomic<bool> ready{false};
};

struct AtomicEntrySample {
    std::atomic<uint32_t> calls{0};
    std::atomic<uintptr_t> first_receiver{0};
    std::atomic<uintptr_t> first_caller_pc{0};
    std::atomic<bool> first_ready{false};
};

struct AtomicCtorRecord {
    std::atomic<uintptr_t> action{0};
    std::atomic<uintptr_t> caller_pc{0};
    std::atomic<bool> ready{false};
};

std::mutex g_arm_mutex;
std::atomic<uint64_t> g_active_ticket{0};
std::atomic<int64_t> g_deadline_ns{0};
std::atomic<uint32_t> g_callback_count{0};
std::atomic<uint32_t> g_unreadable_count{0};
std::atomic<uint32_t> g_generic_attempts{0};
std::atomic<uint32_t> g_anvil_attempts{0};
std::atomic<uint32_t> g_generic_header_hits{0};
std::atomic<uint32_t> g_anvil_header_hits{0};
std::atomic<uint64_t> g_generic_first_header{0};
std::atomic<uint64_t> g_anvil_first_header{0};
std::atomic<uint32_t> g_generic_reserved{0};
std::atomic<uint32_t> g_anvil_reserved{0};
AtomicRecord g_records[kMaximumCallbacks];
AtomicEntrySample g_anvil_simulation_entry;
AtomicEntrySample g_anvil_type2_entry;
AtomicEntrySample g_craft_optional_constructor;
AtomicCtorRecord g_ctor_records[kMaximumCtorRecords];
std::atomic<uint32_t> g_ctor_recorded{0};
bool g_instrumentation_installed = false;
bool g_partial_install_failed = false;
bool g_armed_once = false;
bool g_anvil_simulation_entry_installed = false;
bool g_anvil_type2_entry_installed = false;
bool g_craft_optional_constructor_installed = false;

int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool matchesLiveAnvilOpen(const MapAnvilWindowEvidence& window) noexcept {
    if (!window.anvil_block_and_window_confirmed ||
        window.observed_container_type != 5U) return false;
    const std::string_view wire = window.container_open_packet;
    uint32_t header = 0;
    for (size_t index = 0; index < 5U && index < wire.size(); ++index) {
        const uint8_t next = static_cast<uint8_t>(wire[index]);
        if (index == 4U && (next & 0xF0U) != 0U) return false;
        header |= static_cast<uint32_t>(next & 0x7FU) << (index * 7U);
        if ((next & 0x80U) != 0U) continue;
        return (header & 0x3FFU) == 0x2EU &&
               wire.size() - (index + 1U) >= 2U &&
               static_cast<uint8_t>(wire[index + 1U]) == window.window_id &&
               static_cast<uint8_t>(wire[index + 2U]) == 5U;
    }
    return false;
}

bool matchSite(uintptr_t base, uintptr_t rva, const uint8_t (&bytes)[32],
               uintptr_t* address) noexcept {
    return address && ResolveMinecraftExecutableOffset(base, rva, 32U, address) &&
           IsMemoryReadable(reinterpret_cast<const void*>(*address), 32U) &&
           std::memcmp(reinterpret_cast<const void*>(*address), bytes, 32U) == 0;
}

bool reserveBelow(std::atomic<uint32_t>* counter, uint32_t limit,
                  uint32_t* ordinal) noexcept {
    if (!counter || !ordinal) return false;
    uint32_t current = counter->load(std::memory_order_relaxed);
    while (current < limit &&
           !counter->compare_exchange_weak(
               current, current + 1U, std::memory_order_acq_rel,
               std::memory_order_relaxed)) {}
    if (current >= limit) return false;
    *ordinal = current;
    return true;
}

// These three optional callbacks touch only atomics, Dobby's register
// context, and its just-written saved-LR stack slot. They never dereference
// a game pointer or invoke the expensive memory-readability test.
uintptr_t originalCallerPc(const DobbyRegisterContext* context) noexcept {
    if (!context || context->sp < sizeof(uintptr_t) ||
        (context->sp & 0xFU) != 0U) return 0U;
    // The bundled arm64 closure trampoline saves the incoming x30 at S-8.
    // Its bridge records the original entry stack pointer S in context->sp;
    // context->lr instead points back into Dobby's own BLR trampoline.
    uintptr_t caller_pc = 0U;
    const auto* saved_lr = reinterpret_cast<const void*>(
        static_cast<uintptr_t>(context->sp) - sizeof(uintptr_t));
    std::memcpy(&caller_pc, saved_lr, sizeof(caller_pc));
    return caller_pc;
}

bool observeEntry(AtomicEntrySample* sample, uintptr_t receiver,
                  uintptr_t caller_pc, uint32_t* ordinal) noexcept {
    if (!sample || !ordinal ||
        g_active_ticket.load(std::memory_order_acquire) == 0U ||
        steadyNowNs() >= g_deadline_ns.load(std::memory_order_acquire) ||
        !reserveBelow(&sample->calls, kMaximumEntryCalls, ordinal)) return false;
    if (*ordinal == 0U) {
        sample->first_receiver.store(receiver, std::memory_order_relaxed);
        sample->first_caller_pc.store(caller_pc, std::memory_order_relaxed);
        sample->first_ready.store(true, std::memory_order_release);
    }
    return true;
}

void AnvilSimulationEntryCallback(void*, DobbyRegisterContext* context) noexcept {
    if (!context) return;
    uint32_t unused = 0;
    (void)observeEntry(&g_anvil_simulation_entry,
                       context->general.regs.x0,
                       originalCallerPc(context), &unused);
}

void AnvilType2EntryCallback(void*, DobbyRegisterContext* context) noexcept {
    if (!context) return;
    uint32_t unused = 0;
    (void)observeEntry(&g_anvil_type2_entry,
                       context->general.regs.x0,
                       originalCallerPc(context), &unused);
}

void CraftOptionalConstructorCallback(
    void*, DobbyRegisterContext* context) noexcept {
    if (!context) return;
    uint32_t ordinal = 0;
    const uintptr_t caller_pc = originalCallerPc(context);
    if (!observeEntry(&g_craft_optional_constructor,
                      context->general.regs.x0, caller_pc, &ordinal) ||
        ordinal >= kMaximumCtorRecords) return;
    AtomicCtorRecord& record = g_ctor_records[ordinal];
    record.action.store(context->general.regs.x0, std::memory_order_relaxed);
    record.caller_pc.store(caller_pc, std::memory_order_relaxed);
    record.ready.store(true, std::memory_order_release);
    g_ctor_recorded.fetch_add(1U, std::memory_order_release);
}

void countUnreadable() noexcept {
    uint32_t unused = 0;
    (void)reserveBelow(&g_unreadable_count, 255U, &unused);
}

void observeRecord(uintptr_t record, uint32_t site) noexcept {
    const uint64_t ticket = g_active_ticket.load(std::memory_order_acquire);
    if (ticket == 0 || record == 0U ||
        steadyNowNs() >= g_deadline_ns.load(std::memory_order_acquire)) return;
    if ((site == 1U &&
         g_generic_reserved.load(std::memory_order_relaxed) >= 8U) ||
        (site == 2U &&
         g_anvil_reserved.load(std::memory_order_relaxed) >= 24U)) return;
    std::atomic<uint32_t>* attempts = site == 1U ? &g_generic_attempts :
                                      site == 2U ? &g_anvil_attempts : nullptr;
    const uint32_t attempt_limit = site == 1U ? kMaximumGenericAttempts :
                                   kMaximumAnvilAttempts;
    uint32_t attempt_ordinal = 0;
    if (!attempts || !reserveBelow(attempts, attempt_limit,
                                   &attempt_ordinal)) return;
    if (record > std::numeric_limits<uintptr_t>::max() - kVariantOffset ||
        !IsMemoryReadable(reinterpret_cast<const void*>(record),
                          kRecordHeaderBytes)) {
        countUnreadable();
        return;
    }
    char header[kRecordHeaderBytes];
    std::memcpy(header, reinterpret_cast<const void*>(record), sizeof(header));
    MapAnvilCreatedOutputObservation header_only;
    std::memcpy(&header_only.kind, header, sizeof(header_only.kind));
    std::memcpy(&header_only.container_name, header + 4U,
                sizeof(header_only.container_name));
    std::memcpy(&header_only.slot, header + 16U,
                sizeof(header_only.slot));
    const uint64_t packed_header = (1ULL << 63U) |
        static_cast<uint64_t>(header_only.kind) |
        (static_cast<uint64_t>(header_only.container_name) << 32U) |
        (static_cast<uint64_t>(header_only.slot) << 40U);
    uint64_t empty_header = 0;
    (site == 1U ? g_generic_first_header : g_anvil_first_header)
        .compare_exchange_strong(empty_header, packed_header,
                                 std::memory_order_release,
                                 std::memory_order_relaxed);
    if (!MapAnvilCreatedOutputIsTarget(header_only)) return;
    (site == 1U ? g_generic_header_hits : g_anvil_header_hits)
        .fetch_add(1U, std::memory_order_relaxed);
    if (!IsMemoryReadable(reinterpret_cast<const void*>(record + kVariantOffset),
                          kVariantBytes)) {
        countUnreadable();
        return;
    }
    char variant[kVariantBytes];
    std::memcpy(variant, reinterpret_cast<const void*>(record + kVariantOffset),
                sizeof(variant));
    MapAnvilCreatedOutputObservation decoded;
    if (!DecodeMapAnvilCreatedOutputScalars(
            std::string_view(header, sizeof(header)),
            std::string_view(variant, sizeof(variant)), &decoded)) return;
    std::atomic<uint32_t>* site_reserved = site == 1U ?
        &g_generic_reserved : &g_anvil_reserved;
    const uint32_t site_limit = site == 1U ? 8U : 24U;
    uint32_t site_ordinal = 0;
    if (!reserveBelow(site_reserved, site_limit, &site_ordinal)) return;
    uint8_t index = 0;
    if (!MapAnvilCreatedOutputStorageIndex(site, site_ordinal, &index)) return;
    g_callback_count.fetch_add(1U, std::memory_order_release);
    decoded.ticket = ticket;
    decoded.site = site;
    decoded.callback_index = index;
    AtomicRecord& target = g_records[index];
    target.ticket.store(decoded.ticket, std::memory_order_relaxed);
    target.site.store(decoded.site, std::memory_order_relaxed);
    target.kind.store(decoded.kind, std::memory_order_relaxed);
    target.container_name.store(decoded.container_name, std::memory_order_relaxed);
    target.dynamic_id.store(decoded.dynamic_id, std::memory_order_relaxed);
    target.has_dynamic_id.store(decoded.has_dynamic_id, std::memory_order_relaxed);
    target.slot.store(decoded.slot, std::memory_order_relaxed);
    target.count.store(decoded.count, std::memory_order_relaxed);
    target.network_id.store(decoded.network_id, std::memory_order_relaxed);
    target.secondary_id.store(decoded.secondary_id, std::memory_order_relaxed);
    target.tag.store(decoded.tag, std::memory_order_relaxed);
    target.ready.store(true, std::memory_order_release);
}

void ConstructedCallback(void*, DobbyRegisterContext* context) noexcept {
    if (context) observeRecord(context->general.regs.x22, 1U);
}

void BeforeConsumeCallback(void*, DobbyRegisterContext* context) noexcept {
    if (context) observeRecord(context->general.regs.x24, 2U);
}

bool installReadOnlyHooks(uintptr_t base) noexcept {
    if (g_instrumentation_installed) return true;
    if (g_partial_install_failed) return false;
    uintptr_t constructed = 0, before_consume = 0;
    if (!matchSite(base, kConstructedRva, kConstructedFingerprint,
                   &constructed) ||
        !matchSite(base, kBeforeConsumeRva, kBeforeConsumeFingerprint,
                   &before_consume)) return false;
    // A partial install is permanently inert; Dobby hooks are not assumed
    // removable, and this process never retries after a failed installation.
    g_partial_install_failed = true;
    dobby_set_near_trampoline(true);
    if (DobbyInstrument(reinterpret_cast<void*>(constructed),
                        ConstructedCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(before_consume),
                        BeforeConsumeCallback) != 0) return false;
    g_instrumentation_installed = true;
    g_partial_install_failed = false;
    // Each optional hook is independent. Failure of any fingerprint or Dobby
    // install cannot disable the already verified record probe.
    uintptr_t optional = 0;
    if (matchSite(base, kAnvilSimulationEntryRva,
                  kAnvilSimulationEntryFingerprint, &optional)) {
        g_anvil_simulation_entry_installed =
            DobbyInstrument(reinterpret_cast<void*>(optional),
                            AnvilSimulationEntryCallback) == 0;
    }
    if (matchSite(base, kAnvilType2EntryRva,
                  kAnvilType2EntryFingerprint, &optional)) {
        g_anvil_type2_entry_installed =
            DobbyInstrument(reinterpret_cast<void*>(optional),
                            AnvilType2EntryCallback) == 0;
    }
    if (matchSite(base, kCraftOptionalConstructorRva,
                  kCraftOptionalConstructorFingerprint, &optional)) {
        g_craft_optional_constructor_installed =
            DobbyInstrument(reinterpret_cast<void*>(optional),
                            CraftOptionalConstructorCallback) == 0;
    }
    return true;
}

} // namespace
#endif

bool ArmMapAnvilCreatedOutputProbe(
    uintptr_t minecraft_base, uint64_t ticket, int64_t expected_map_uuid,
    const MapAnvilWindowEvidence& window) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    if (!minecraft_base || !ticket || expected_map_uuid == -1 ||
        !matchesLiveAnvilOpen(window)) return false;
    std::lock_guard<std::mutex> lock(g_arm_mutex);
    if (g_armed_once || g_active_ticket.load(std::memory_order_acquire) != 0 ||
        !installReadOnlyHooks(minecraft_base)) return false;
    g_armed_once = true;
    g_callback_count.store(0U, std::memory_order_relaxed);
    g_unreadable_count.store(0U, std::memory_order_relaxed);
    g_generic_attempts.store(0U, std::memory_order_relaxed);
    g_anvil_attempts.store(0U, std::memory_order_relaxed);
    g_generic_header_hits.store(0U, std::memory_order_relaxed);
    g_anvil_header_hits.store(0U, std::memory_order_relaxed);
    g_generic_first_header.store(0U, std::memory_order_relaxed);
    g_anvil_first_header.store(0U, std::memory_order_relaxed);
    g_generic_reserved.store(0U, std::memory_order_relaxed);
    g_anvil_reserved.store(0U, std::memory_order_relaxed);
    const auto reset_entry = [](AtomicEntrySample* entry) noexcept {
        entry->calls.store(0U, std::memory_order_relaxed);
        entry->first_receiver.store(0U, std::memory_order_relaxed);
        entry->first_caller_pc.store(0U, std::memory_order_relaxed);
        entry->first_ready.store(false, std::memory_order_relaxed);
    };
    reset_entry(&g_anvil_simulation_entry);
    reset_entry(&g_anvil_type2_entry);
    reset_entry(&g_craft_optional_constructor);
    g_ctor_recorded.store(0U, std::memory_order_relaxed);
    for (auto& record : g_records) {
        record.ready.store(false, std::memory_order_relaxed);
    }
    for (auto& record : g_ctor_records) {
        record.ready.store(false, std::memory_order_relaxed);
    }
    g_deadline_ns.store(steadyNowNs() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(90)).count(), std::memory_order_release);
    g_active_ticket.store(ticket, std::memory_order_release);
    return true;
#else
    (void)minecraft_base;
    (void)ticket;
    (void)expected_map_uuid;
    (void)window;
    return false;
#endif
}

void DisarmMapAnvilCreatedOutputProbe(uint64_t ticket) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    std::lock_guard<std::mutex> lock(g_arm_mutex);
    if (ticket != 0 &&
        g_active_ticket.load(std::memory_order_acquire) == ticket) {
        g_active_ticket.store(0U, std::memory_order_release);
    }
#else
    (void)ticket;
#endif
}

bool ReadMapAnvilCreatedOutputProbe(
    uint64_t ticket, MapAnvilCreatedOutputSnapshot* output) noexcept {
    if (!output) return false;
    *output = {};
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    if (ticket == 0 ||
        g_active_ticket.load(std::memory_order_acquire) != ticket) return false;
    const uint32_t callbacks = g_callback_count.load(std::memory_order_acquire);
    output->generic_attempts = g_generic_attempts.load(std::memory_order_relaxed);
    output->anvil_attempts = g_anvil_attempts.load(std::memory_order_relaxed);
    output->generic_header_hits =
        g_generic_header_hits.load(std::memory_order_relaxed);
    output->anvil_header_hits =
        g_anvil_header_hits.load(std::memory_order_relaxed);
    const auto decode_header = [](uint64_t packed,
                                  MapAnvilCreatedOutputSnapshot::HeaderSample* sample) {
        if (!sample || (packed & (1ULL << 63U)) == 0U) return;
        sample->seen = true;
        sample->kind = static_cast<uint32_t>(packed);
        sample->container_name = static_cast<uint8_t>(packed >> 32U);
        sample->slot = static_cast<uint8_t>(packed >> 40U);
    };
    decode_header(g_generic_first_header.load(std::memory_order_acquire),
                  &output->generic_first_header);
    decode_header(g_anvil_first_header.load(std::memory_order_acquire),
                  &output->anvil_first_header);
    output->callback_count = callbacks;
    output->unreadable_count =
        g_unreadable_count.load(std::memory_order_relaxed);
    const auto read_entry = [](const AtomicEntrySample& source,
                               bool installed,
                               MapAnvilCreatedOutputSnapshot::EntrySample* target) noexcept {
        target->installed = installed;
        target->calls = source.calls.load(std::memory_order_acquire);
        target->first_ready = source.first_ready.load(std::memory_order_acquire);
        if (target->first_ready) {
            target->first_receiver =
                source.first_receiver.load(std::memory_order_relaxed);
            target->first_caller_pc =
                source.first_caller_pc.load(std::memory_order_relaxed);
        }
    };
    read_entry(g_anvil_simulation_entry, g_anvil_simulation_entry_installed,
               &output->anvil_simulation_entry);
    read_entry(g_anvil_type2_entry, g_anvil_type2_entry_installed,
               &output->anvil_type2_entry);
    read_entry(g_craft_optional_constructor,
               g_craft_optional_constructor_installed,
               &output->craft_optional_constructor);
    output->craft_constructor_recorded =
        g_ctor_recorded.load(std::memory_order_acquire);
    for (uint32_t index = 0; index < kMaximumCallbacks; ++index) {
        const AtomicRecord& source = g_records[index];
        if (!source.ready.load(std::memory_order_acquire) ||
            source.ticket.load(std::memory_order_relaxed) != ticket) continue;
        auto& target = output->records[output->captured_count++];
        target.ticket = ticket;
        target.site = source.site.load(std::memory_order_relaxed);
        target.callback_index = static_cast<uint8_t>(index);
        target.kind = source.kind.load(std::memory_order_relaxed);
        target.container_name = source.container_name.load(std::memory_order_relaxed);
        target.dynamic_id = source.dynamic_id.load(std::memory_order_relaxed);
        target.has_dynamic_id = source.has_dynamic_id.load(std::memory_order_relaxed);
        target.slot = source.slot.load(std::memory_order_relaxed);
        target.count = source.count.load(std::memory_order_relaxed);
        target.network_id = source.network_id.load(std::memory_order_relaxed);
        target.secondary_id = source.secondary_id.load(std::memory_order_relaxed);
        target.tag = source.tag.load(std::memory_order_relaxed);
    }
    return true;
#else
    (void)ticket;
    return false;
#endif
}

bool LookupMapAnvilCraftCtorCaller(
    uintptr_t action_address, uintptr_t* caller_pc) noexcept {
    if (!action_address || !caller_pc) return false;
    *caller_pc = 0U;
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    if (!g_craft_optional_constructor_installed ||
        g_active_ticket.load(std::memory_order_acquire) == 0U ||
        steadyNowNs() >= g_deadline_ns.load(std::memory_order_acquire)) return false;
    const uint32_t calls =
        g_craft_optional_constructor.calls.load(std::memory_order_acquire);
    const uint32_t limit = calls < kMaximumCtorRecords ? calls : kMaximumCtorRecords;
    // Reverse search selects the most recent constructor when native storage
    // has reused an address during preview generation.
    for (uint32_t index = limit; index > 0U; --index) {
        const AtomicCtorRecord& record = g_ctor_records[index - 1U];
        if (!record.ready.load(std::memory_order_acquire) ||
            record.action.load(std::memory_order_relaxed) != action_address) continue;
        *caller_pc = record.caller_pc.load(std::memory_order_relaxed);
        return true;
    }
#endif
    return false;
}

} // namespace build_import
