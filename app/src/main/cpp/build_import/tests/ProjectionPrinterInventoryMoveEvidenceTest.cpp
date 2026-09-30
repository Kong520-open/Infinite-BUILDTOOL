#include "ProjectionPrinterInventoryMoveEvidence.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {
using build_import::ProjectionPrinterInventorySlot;
using build_import::ProjectionPrinterInventorySnapshot;
using namespace build_import::projection_inventory_evidence;

ProjectionPrinterInventorySlot item(const char* name, int32_t net_id, uint16_t count,
                                    int32_t runtime_id, uint16_t aux = 0U) {
    ProjectionPrinterInventorySlot result;
    result.occupied = true;
    result.has_network_stack_id = true;
    result.network_stack_id = net_id;
    result.count = count;
    result.runtime_item_id = runtime_id;
    result.aux = aux;
    result.name = name;
    return result;
}

SubmittedMove submittedMove(bool swap = false) {
    SubmittedMove move;
    move.source_slot = 14;
    move.destination_slot = 2;
    move.session_generation = 7U;
    move.remote_revision_before_send = 10U;
    move.source_before = item("spruce_planks", 101, 64U, 55, 1U);
    if (swap) move.destination_before = item("cobblestone", 202, 23U, 4);
    move.remote_source_known = true;
    move.remote_source_before_send = move.source_before;
    return move;
}

ProjectionPrinterInventorySnapshot beforeMove(const SubmittedMove& move) {
    ProjectionPrinterInventorySnapshot remote;
    remote.ready = true;
    remote.session_generation = move.session_generation;
    remote.revision = move.remote_revision_before_send;
    remote.slots[static_cast<size_t>(move.source_slot)] = move.source_before;
    remote.slots[static_cast<size_t>(move.destination_slot)] = move.destination_before;
    return remote;
}

ProjectionPrinterInventorySnapshot afterMove(const SubmittedMove& move) {
    auto remote = beforeMove(move);
    ++remote.revision;
    remote.slots[static_cast<size_t>(move.source_slot)] = move.destination_before;
    remote.slots[static_cast<size_t>(move.destination_slot)] = move.source_before;
    // A server may assign new IDs when it commits the move. Verification must
    // observe valid resulting IDs rather than require the old source ID.
    remote.slots[static_cast<size_t>(move.destination_slot)].network_stack_id = 303;
    if (move.destination_before.occupied) {
        remote.slots[static_cast<size_t>(move.source_slot)].network_stack_id = 404;
    }
    return remote;
}

LiveSource staleLive(const SubmittedMove& move) {
    LiveSource live;
    live.ready = true;
    live.count_known = true;
    live.slot = move.source_before;
    return live;
}

bool confirms(const SubmittedMove& move, const ProjectionPrinterInventorySnapshot& remote) {
    return remoteInventoryConfirmsMove(move, remote,
        [](const std::string& actual, const std::string& expected) {
            return actual == expected;
        });
}

void testRepeatedChecksNeverReleaseSameStack() {
    const auto move = submittedMove();
    const auto remote = beforeMove(move);
    const auto live = staleLive(move);
    // Retiring pending after a timeout does not expire the stored evidence.
    for (int retry_tick = 0; retry_tick < 10000; ++retry_tick) {
        assert(!sourceChangedSinceSubmission(move, live, remote));
    }
}

void testUnrelatedRemoteUpdatesDoNotReleaseSource() {
    const auto move = submittedMove();
    auto remote = beforeMove(move);
    remote.slots[6] = item("dirt", 999, 64U, 3);
    for (int revision = 0; revision < 50; ++revision) {
        ++remote.revision;
        --remote.slots[6].count;
        assert(!sourceChangedSinceSubmission(move, staleLive(move), remote));
    }
}

void testRemoteMovedButLiveStaleStaysIsolated() {
    const auto move = submittedMove();
    auto remote = afterMove(move);
    assert(confirms(move, remote));
    assert(!sourceChangedSinceSubmission(move, staleLive(move), remote));
    remote.revision += 1000U;
    assert(!sourceChangedSinceSubmission(move, staleLive(move), remote));
}

void testLiveSourceChangeReleasesRestriction() {
    const auto move = submittedMove();
    const auto remote = beforeMove(move);
    auto live = staleLive(move);
    live.slot = {};
    assert(sourceChangedSinceSubmission(move, live, remote));
    live = staleLive(move);
    ++live.slot.network_stack_id;
    assert(sourceChangedSinceSubmission(move, live, remote));
    live = staleLive(move);
    --live.slot.count;
    assert(sourceChangedSinceSubmission(move, live, remote));
    live.count_known = false;
    assert(!sourceChangedSinceSubmission(move, live, remote));
    live.slot = {};
    live.ready = false;
    assert(!sourceChangedSinceSubmission(move, live, remote));
}

void testRemoteOnlySourceChangeNeedsFreshSameWorldEvidence() {
    const auto move = submittedMove();
    auto remote = afterMove(move);
    assert(sourceChangedSinceSubmission(move, LiveSource{}, remote));
    remote.revision = move.remote_revision_before_send;
    assert(!sourceChangedSinceSubmission(move, LiveSource{}, remote));
    ++remote.revision;
    ++remote.session_generation;
    assert(!sourceChangedSinceSubmission(move, LiveSource{}, remote));
    remote.session_generation = move.session_generation;
    remote.ready = false;
    assert(!sourceChangedSinceSubmission(move, LiveSource{}, remote));
}

void testFirstRemoteBaselineDoesNotBlindlyReleaseSameSource() {
    auto move = submittedMove();
    move.remote_source_known = false;
    auto remote = beforeMove(move);
    ++remote.revision;
    assert(!sourceChangedSinceSubmission(move, LiveSource{}, remote));
    remote = afterMove(move);
    assert(sourceChangedSinceSubmission(move, LiveSource{}, remote));
    assert(!sourceChangedSinceSubmission(move, staleLive(move), remote));
}

void testRemoteMoveProofRequiresCompleteFreshSameWorldPair() {
    const auto move = submittedMove();
    auto remote = afterMove(move);
    assert(confirms(move, remote));
    remote.ready = false;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.revision = move.remote_revision_before_send;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    ++remote.session_generation;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.inventory_id = 2U;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.slots[14] = move.source_before;
    assert(!confirms(move, remote));
    remote = beforeMove(move);
    remote.slots[14] = {};
    ++remote.revision;
    assert(!confirms(move, remote));
}

void testRemoteProofRejectsWrongMaterialCountAuxOrMissingIdentity() {
    const auto move = submittedMove();
    auto remote = afterMove(move);
    remote.slots[2].name = "oak_planks";
    assert(!confirms(move, remote));
    remote = afterMove(move);
    --remote.slots[2].count;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    ++remote.slots[2].aux;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.slots[2].has_network_stack_id = false;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.slots[2].network_stack_id = 0;
    assert(!confirms(move, remote));
}

void testSwapNeedsBothMaterialsAndCounts() {
    const auto move = submittedMove(true);
    auto remote = afterMove(move);
    assert(confirms(move, remote));
    remote.slots[14] = {};
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.slots[14].name = "dirt";
    assert(!confirms(move, remote));
    remote = afterMove(move);
    ++remote.slots[14].count;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.slots[14].network_stack_id = 0;
    assert(!confirms(move, remote));
}

void testUnknownRemoteNameRequiresExactKnownRuntimeType() {
    auto move = submittedMove();
    auto remote = afterMove(move);
    remote.slots[2].name.clear();
    assert(confirms(move, remote));
    ++remote.slots[2].runtime_item_id;
    assert(!confirms(move, remote));
    remote = afterMove(move);
    remote.slots[2].name.clear();
    move.source_before.runtime_item_id = 0;
    assert(!confirms(move, remote));
}

void testCompletionStatusRequiresExactRequestAndCannotProveMove() {
    const auto completion_status = static_cast<uint8_t>(
        RequestStatus::ScreenHandlerEndRequestFailed);
    assert(completion_status == 4U);
    assert(isRequestCompletionFailure(true, completion_status));
    assert(!isRequestCompletionFailure(false, completion_status));
    assert(!isRequestCompletionFailure(true, static_cast<uint8_t>(RequestStatus::Ok)));
    assert(!isRequestCompletionFailure(true, 3U));
    const auto move = submittedMove();
    assert(!confirms(move, beforeMove(move)));
    assert(confirms(move, afterMove(move)));
}

}  // namespace

int main() {
    testRepeatedChecksNeverReleaseSameStack();
    testUnrelatedRemoteUpdatesDoNotReleaseSource();
    testRemoteMovedButLiveStaleStaysIsolated();
    testLiveSourceChangeReleasesRestriction();
    testRemoteOnlySourceChangeNeedsFreshSameWorldEvidence();
    testFirstRemoteBaselineDoesNotBlindlyReleaseSameSource();
    testRemoteMoveProofRequiresCompleteFreshSameWorldPair();
    testRemoteProofRejectsWrongMaterialCountAuxOrMissingIdentity();
    testSwapNeedsBothMaterialsAndCounts();
    testUnknownRemoteNameRequiresExactKnownRuntimeType();
    testCompletionStatusRequiresExactRequestAndCannotProveMove();
    std::puts("ProjectionPrinterInventoryMoveEvidenceTest: 11 cases passed");
}
