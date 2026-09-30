#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MOVE_EVIDENCE_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MOVE_EVIDENCE_H

#include "ProjectionPrinterInventoryMailbox.h"

#include <cstddef>
#include <cstdint>

namespace build_import::projection_inventory_evidence {

// Verified against the v859 server's action/EndRequest dispatch. Status 4
// replaces an otherwise successful action result when screen teardown fails.
// It is not a replacement for observing the resulting slot identities.
enum class RequestStatus : uint8_t {
    Ok = 0U,
    ScreenHandlerEndRequestFailed = 4U,
};

constexpr bool isRequestCompletionFailure(bool exact_request, uint8_t status) noexcept {
    return exact_request && status ==
        static_cast<uint8_t>(RequestStatus::ScreenHandlerEndRequestFailed);
}

struct SubmittedMove {
    int32_t source_slot = -1;
    int32_t destination_slot = -1;
    uint64_t session_generation = 0U;
    uint64_t remote_revision_before_send = 0U;
    ProjectionPrinterInventorySlot source_before;
    ProjectionPrinterInventorySlot destination_before;
    bool remote_source_known = false;
    ProjectionPrinterInventorySlot remote_source_before_send;
};

struct LiveSource {
    bool ready = false;
    bool count_known = false;
    ProjectionPrinterInventorySlot slot;
};

inline bool sameRemoteSlot(const ProjectionPrinterInventorySlot& left,
                           const ProjectionPrinterInventorySlot& right) noexcept {
    return left.occupied == right.occupied &&
        left.has_network_stack_id == right.has_network_stack_id &&
        left.network_stack_id == right.network_stack_id &&
        left.runtime_item_id == right.runtime_item_id &&
        left.aux == right.aux && left.count == right.count;
}

inline bool sourceChangedSinceSubmission(
    const SubmittedMove& submitted, const LiveSource& live,
    const ProjectionPrinterInventorySnapshot& remote) noexcept {
    if (submitted.source_slot < 9 || submitted.source_slot > 35) return false;
    const auto& original = submitted.source_before;
    if (live.ready) {
        if (!live.slot.occupied || (live.slot.has_network_stack_id &&
            live.slot.network_stack_id != original.network_stack_id)) {
            return true;
        }
        if (live.count_known && live.slot.has_network_stack_id &&
            live.slot.network_stack_id == original.network_stack_id &&
            live.slot.count != original.count) {
            return true;
        }
    }
    if (!remote.ready || remote.inventory_id != 0U ||
        submitted.session_generation == 0U ||
        remote.session_generation != submitted.session_generation ||
        remote.revision <= submitted.remote_revision_before_send) {
        return false;
    }
    const auto& source = remote.slots[static_cast<size_t>(submitted.source_slot)];
    if (live.ready && (!source.occupied ||
        source.network_stack_id != original.network_stack_id ||
        source.count != original.count)) {
        // The server moved it but live still exposes the old stack. Releasing
        // that source now would resend the exact stale request being guarded.
        return false;
    }
    if (submitted.remote_source_known) {
        // An unrelated slot can advance the global revision. Only a change
        // to this source is new evidence for retrying this source.
        return !sameRemoteSlot(source, submitted.remote_source_before_send);
    }
    return !source.occupied || source.network_stack_id != original.network_stack_id ||
        source.count != original.count || source.aux != original.aux;
}

template <typename NamesEqual>
bool remoteInventoryConfirmsMove(const SubmittedMove& submitted,
                                 const ProjectionPrinterInventorySnapshot& remote,
                                 NamesEqual names_equal) {
    if (!remote.ready || remote.inventory_id != 0U ||
        submitted.session_generation == 0U ||
        remote.session_generation != submitted.session_generation ||
        remote.revision <= submitted.remote_revision_before_send ||
        submitted.source_slot < 9 || submitted.source_slot > 35 ||
        submitted.destination_slot < 0 || submitted.destination_slot > 8 ||
        !submitted.source_before.occupied ||
        submitted.source_before.network_stack_id <= 0 ||
        submitted.source_before.count == 0U) {
        return false;
    }
    const auto matches_item = [&](const ProjectionPrinterInventorySlot& actual,
                                  const ProjectionPrinterInventorySlot& expected) {
        const bool type_matches = !actual.name.empty()
            ? names_equal(actual.name, expected.name)
            : expected.runtime_item_id > 0 &&
                actual.runtime_item_id == expected.runtime_item_id;
        return actual.occupied && actual.has_network_stack_id &&
            actual.network_stack_id > 0 && type_matches &&
            actual.aux == expected.aux && actual.count == expected.count;
    };
    const auto& destination = remote.slots[static_cast<size_t>(submitted.destination_slot)];
    if (!matches_item(destination, submitted.source_before)) return false;
    const auto& source = remote.slots[static_cast<size_t>(submitted.source_slot)];
    if (!submitted.destination_before.occupied) {
        return !source.occupied && source.count == 0U && source.network_stack_id == 0;
    }
    return matches_item(source, submitted.destination_before);
}

}  // namespace build_import::projection_inventory_evidence

#endif
