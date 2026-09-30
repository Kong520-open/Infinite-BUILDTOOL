#include "ProjectionPrinterSilentRotation.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace build_import {
namespace {

// Bedrock 1.21.120 / protocol 859.  All packet writes below are guarded by
// both this packet vtable profile and the field-validator instruction profile.
constexpr uintptr_t kPlayerAuthInputVtableRva = 0x129465F0ULL;
constexpr uintptr_t kPlayerAuthInputCompleteDestructorRva = 0x0B2C4A88ULL;
constexpr uintptr_t kPlayerAuthInputDeletingDestructorRva = 0x0B2C4B00ULL;
constexpr uintptr_t kPlayerAuthInputPacketIdFunctionRva = 0x0B2C4E0CULL;
constexpr uintptr_t kPlayerAuthInputValidatorRva = 0x0B2CB2CCULL;
constexpr size_t kRotationPitchOffset = 0x2CU;
constexpr size_t kRotationYawOffset = 0x30U;
constexpr size_t kHeadYawOffset = 0x40U;
constexpr size_t kRequiredPacketBytes = kHeadYawOffset + sizeof(float);

constexpr std::array<uint8_t, 40U> kPlayerAuthInputValidatorFingerprint{{
    0xFFU, 0x03U, 0x01U, 0xD1U, 0xFDU, 0x7BU, 0x02U, 0xA9U,
    0xF4U, 0x4FU, 0x03U, 0xA9U, 0xFDU, 0x83U, 0x00U, 0x91U,
    0x54U, 0xD0U, 0x3BU, 0xD5U, 0xF3U, 0x03U, 0x00U, 0xAAU,
    0x41U, 0xC1U, 0xFBU, 0x90U, 0x21U, 0xD4U, 0x09U, 0x91U,
    0x88U, 0x16U, 0x40U, 0xF9U, 0x00U, 0xB0U, 0x00U, 0x91U,
}};
constexpr std::array<uint8_t, 8U> kPlayerAuthInputPacketIdFingerprint{{
    0x00U, 0x12U, 0x80U, 0x52U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
}};

enum class TicketPhase : uint8_t {
    Idle = 0,
    Armed = 1,
    Synchronized = 2,
};

std::atomic<uint64_t> g_next_ticket{1U};
std::atomic<uint64_t> g_active_ticket{0U};
std::atomic<uint32_t> g_yaw_bits{0U};
std::atomic<TicketPhase> g_phase{TicketPhase::Idle};
std::mutex g_control_mutex;

struct AppliedPacketOverride {
    ProjectionPrinterSilentRotation::Ticket ticket = 0U;
    void* packet = nullptr;
    float original_yaw = 0.0F;
    float original_head_yaw = 0.0F;
};

// sendToServer is normally not nested, but keeping a small fixed stack makes
// restoration safe even if the original sender emits another packet inline.
thread_local std::array<AppliedPacketOverride, 4U> g_applied_packets{};
thread_local size_t g_applied_packet_count = 0U;

uint32_t bitsForFloat(float value) noexcept {
    uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float floatForBits(uint32_t bits) noexcept {
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool isVerifiedPlayerAuthInputProfile() noexcept {
#if !defined(__aarch64__)
    return false;
#else
    const uintptr_t module_base = Main::getBaseAddress();
    if (module_base == 0U) return false;

    uintptr_t validator = 0U;
    if (!ResolveMinecraftExecutableOffset(module_base, kPlayerAuthInputValidatorRva,
                                          kPlayerAuthInputValidatorFingerprint.size(),
                                          &validator) ||
        std::memcmp(reinterpret_cast<const void*>(validator),
                    kPlayerAuthInputValidatorFingerprint.data(),
                    kPlayerAuthInputValidatorFingerprint.size()) != 0) {
        return false;
    }

    if (module_base > UINTPTR_MAX - kPlayerAuthInputVtableRva) return false;
    const uintptr_t vtable_address = module_base + kPlayerAuthInputVtableRva;
    const auto* const vtable = reinterpret_cast<const uintptr_t*>(vtable_address);
    if (!IsMemoryReadable(vtable, 3U * sizeof(uintptr_t)) ||
        vtable[0] != module_base + kPlayerAuthInputCompleteDestructorRva ||
        vtable[1] != module_base + kPlayerAuthInputDeletingDestructorRva ||
        vtable[2] != module_base + kPlayerAuthInputPacketIdFunctionRva) {
        return false;
    }

    uintptr_t packet_id_function = 0U;
    return ResolveMinecraftExecutableOffset(module_base,
                                             kPlayerAuthInputPacketIdFunctionRva,
                                             kPlayerAuthInputPacketIdFingerprint.size(),
                                             &packet_id_function) &&
        packet_id_function == vtable[2] &&
        std::memcmp(reinterpret_cast<const void*>(packet_id_function),
                    kPlayerAuthInputPacketIdFingerprint.data(),
                    kPlayerAuthInputPacketIdFingerprint.size()) == 0;
#endif
}

bool isVerifiedPlayerAuthInputPacket(const void* packet) noexcept {
#if !defined(__aarch64__)
    (void)packet;
    return false;
#else
    if (!packet || !IsMemoryReadable(packet, kRequiredPacketBytes)) return false;
    const uintptr_t module_base = Main::getBaseAddress();
    if (module_base == 0U || module_base > UINTPTR_MAX - kPlayerAuthInputVtableRva) {
        return false;
    }
    uintptr_t vtable = 0U;
    std::memcpy(&vtable, packet, sizeof(vtable));
    return vtable == module_base + kPlayerAuthInputVtableRva;
#endif
}

bool ticketIsActive(ProjectionPrinterSilentRotation::Ticket ticket) noexcept {
    return ticket != 0U &&
        g_active_ticket.load(std::memory_order_acquire) == ticket &&
        g_phase.load(std::memory_order_acquire) == TicketPhase::Armed;
}

}  // namespace

bool ProjectionPrinterSilentRotation::armYaw(float yaw, Ticket* ticket,
                                             std::string* error) {
    if (ticket) *ticket = 0U;
    if (!std::isfinite(yaw) || yaw < -360.0F || yaw > 360.0F) {
        if (error) *error = "silent rotation yaw is outside the valid range";
        return false;
    }
    if (!isVerifiedPlayerAuthInputProfile()) {
        if (error) *error = "silent rotation ABI profile does not match this game version";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_control_mutex);
    if (g_phase.load(std::memory_order_acquire) != TicketPhase::Idle) {
        if (error) *error = "another silent rotation request is still active";
        return false;
    }
    Ticket next = g_next_ticket.fetch_add(1U, std::memory_order_relaxed);
    if (next == 0U) next = g_next_ticket.fetch_add(1U, std::memory_order_relaxed);
    if (next == 0U) {
        if (error) *error = "could not allocate a silent rotation ticket";
        return false;
    }
    g_yaw_bits.store(bitsForFloat(yaw), std::memory_order_relaxed);
    g_active_ticket.store(next, std::memory_order_release);
    g_phase.store(TicketPhase::Armed, std::memory_order_release);
    if (ticket) *ticket = next;
    if (error) error->clear();
    return true;
}

bool ProjectionPrinterSilentRotation::isSynchronized(Ticket ticket) noexcept {
    return ticket != 0U &&
        g_active_ticket.load(std::memory_order_acquire) == ticket &&
        g_phase.load(std::memory_order_acquire) == TicketPhase::Synchronized;
}

void ProjectionPrinterSilentRotation::finish(Ticket ticket) noexcept {
    if (ticket == 0U) return;
    std::lock_guard<std::mutex> lock(g_control_mutex);
    if (g_active_ticket.load(std::memory_order_acquire) != ticket) return;
    g_phase.store(TicketPhase::Idle, std::memory_order_release);
    g_active_ticket.store(0U, std::memory_order_release);
}

void ProjectionPrinterSilentRotation::cancel(Ticket ticket) noexcept {
    finish(ticket);
}

void ProjectionPrinterSilentRotation::cancelAll() noexcept {
    std::lock_guard<std::mutex> lock(g_control_mutex);
    g_phase.store(TicketPhase::Idle, std::memory_order_release);
    g_active_ticket.store(0U, std::memory_order_release);
}

ProjectionPrinterSilentRotation::Ticket
ProjectionPrinterSilentRotation::onBeforeOutgoingPacket(void* packet) noexcept {
    try {
        const Ticket ticket = g_active_ticket.load(std::memory_order_acquire);
        if (!ticketIsActive(ticket) || !isVerifiedPlayerAuthInputPacket(packet)) return 0U;

        if (g_applied_packet_count >= g_applied_packets.size()) return 0U;
        const float yaw = floatForBits(g_yaw_bits.load(std::memory_order_relaxed));
        if (!std::isfinite(yaw)) return 0U;
        auto* const bytes = static_cast<uint8_t*>(packet);
        AppliedPacketOverride& applied = g_applied_packets[g_applied_packet_count];
        applied.ticket = ticket;
        applied.packet = packet;
        std::memcpy(&applied.original_yaw, bytes + kRotationYawOffset,
                    sizeof(applied.original_yaw));
        std::memcpy(&applied.original_head_yaw, bytes + kHeadYawOffset,
                    sizeof(applied.original_head_yaw));
        // Preserve the natural packet's pitch.  The stair half is determined
        // by the ItemUse click face/height, not by moving the local camera.
        std::memcpy(bytes + kRotationYawOffset, &yaw, sizeof(yaw));
        std::memcpy(bytes + kHeadYawOffset, &yaw, sizeof(yaw));
        ++g_applied_packet_count;
        return ticket;
    } catch (...) {
        // A sender hook must always fail closed and forward the original packet.
        return 0U;
    }
}

void ProjectionPrinterSilentRotation::onAfterOutgoingPacket(Ticket ticket) noexcept {
    if (!ticket) return;
    // The original sender has already received the temporary yaw by the time
    // this callback runs.  Its packet object may legitimately have been
    // consumed or released while serializing, so restoring the in-memory
    // object is best-effort only.  Do not mistake an unavailable object here
    // for a failed send: that would leave the printer waiting for a
    // synchronization which the server has already seen.
    bool applied_to_matching_packet = false;
    if (g_applied_packet_count != 0U) {
        AppliedPacketOverride& applied = g_applied_packets[g_applied_packet_count - 1U];
        if (applied.ticket == ticket) {
            applied_to_matching_packet = true;
            if (applied.packet && IsMemoryReadable(applied.packet, kRequiredPacketBytes)) {
                auto* const bytes = static_cast<uint8_t*>(applied.packet);
                std::memcpy(bytes + kRotationYawOffset, &applied.original_yaw,
                            sizeof(applied.original_yaw));
                std::memcpy(bytes + kHeadYawOffset, &applied.original_head_yaw,
                            sizeof(applied.original_head_yaw));
            }
            applied = AppliedPacketOverride{};
            --g_applied_packet_count;
        }
    }
    if (!applied_to_matching_packet) return;
    const TicketPhase expected = TicketPhase::Armed;
    if (g_active_ticket.load(std::memory_order_acquire) == ticket) {
        TicketPhase mutable_expected = expected;
        g_phase.compare_exchange_strong(mutable_expected, TicketPhase::Synchronized,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
    }
}

}  // namespace build_import
