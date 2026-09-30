#include "MapChestTransferJournal.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace build_import;

namespace {

constexpr const char* kFirstName = "map row1 col1";
constexpr const char* kSecondName = "map row1 col2";

MapChestTransferRecord prepared(uint64_t cursor, int64_t map_uuid,
                                uint64_t tile_count = 2) {
    MapChestTransferRecord record;
    record.world_id = "stable:v1:journal-test|0";
    record.dimension_id = 0;
    record.tile_cursor = cursor;
    record.tile_count = tile_count;
    record.chest_x = 235;
    record.chest_y = 22;
    record.chest_z = 126;
    record.chest_index = static_cast<uint16_t>(cursor / 27U);
    record.chest_slot = static_cast<uint8_t>(cursor % 27U);
    record.source_runtime_item_id = 358;
    record.source_network_stack_id = 76;
    record.source_map_uuid = map_uuid;
    record.pre_send_capture_token = 10;
    return record;
}

ContainerCaptureResult reopened(uint64_t token, uint16_t slot, int64_t map_uuid) {
    ContainerCaptureResult result;
    result.token = token;
    result.x = 235;
    result.y = 22;
    result.z = 126;
    result.container_id = 5;
    result.container_type = 0;
    result.container_opened = true;
    result.slot_count = 27;
    result.has_full_container_name = true;
    result.full_container_name = 0; // InventoryContent tail, NOT request target 7.
    result.has_dynamic_container_id = false;
    CapturedContainerItem item;
    item.slot = slot;
    item.numeric_id = 358;
    item.count = 1;
    item.has_network_stack_id = true;
    item.network_stack_id = 76;
    item.has_map_uuid = true;
    item.map_uuid = map_uuid;
    item.name_status = MapItemNameStatus::Present;
    item.name_source = MapItemNameSource::DisplayName;
    item.name_candidate = slot == 0 ? kFirstName : kSecondName;
    result.items.push_back(item);
    return result;
}

MapChestRecoveryAction recover(const MapChestTransferRecord& journal,
                               uint64_t checkpoint_cursor) {
    return ClassifyMapChestTransferRecovery(
        journal, "stable:v1:journal-test|0", 0, checkpoint_cursor, 2,
        235, 22, 126, journal.chest_slot);
}

void assertLoaded(const std::string& path, MapChestTransferRecord* output) {
    std::string error;
    assert(LoadMapChestTransferJournal(path, output, &error) ==
           MapChestJournalLoad::Loaded);
    assert(error.empty());
}

} // namespace

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("map_chest_journal_test_" + std::to_string(unique));
    assert(std::filesystem::create_directory(directory));
    const std::string state = (directory / "map_creation.state").string();
    const std::string sidecar = MapChestTransferJournalPath(state);
    std::string error;
    MapChestTransferRecord current;
    assert(LoadMapChestTransferJournal(state, &current, &error) ==
           MapChestJournalLoad::Missing);

    // Restart before send: persisted Prepared is not permission to send.
    const MapChestTransferRecord first = prepared(0, -532575944698LL);
    assert(BeginMapChestTransferJournal(state, first, &error));
    assert(!BeginMapChestTransferJournal(state, first, &error));
    assertLoaded(state, &current);
    assert(recover(current, 0) == MapChestRecoveryAction::FreshPreflightRequired);
    assert(recover(current, 1) == MapChestRecoveryAction::Unsafe);
    assert(ClassifyMapChestTransferRecovery(current, "wrong-world", 0, 0, 2,
                                            235, 22, 126, 0) ==
           MapChestRecoveryAction::Unsafe);
    assert(RefreshPreparedMapChestTransferJournal(state, current, 77, 11, &error));
    assertLoaded(state, &current);
    assert(current.source_network_stack_id == 77);
    assert(current.pre_send_capture_token == 11);

    // Sender callback must durably arm before its single sendToServer call.
    assert(!ArmMapChestTransferDispatch(state, current, 1, &error));
    assert(!ArmMapChestTransferDispatch(state, current, -16, &error));
    assert(ArmMapChestTransferDispatch(state, current, -17, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::DispatchArmed);
    assert(current.request_id == -17);
    assert(recover(current, 0) == MapChestRecoveryAction::ReopenChestNoResend);
    assert(!ArmMapChestTransferDispatch(state, first, -17, &error));

    // An accepted response still cannot advance the tile without a positive
    // chest or full native player-inventory proof.
    assert(NoteMapChestTransferAccepted(state, current, -17, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::ResponseAccepted);
    assert(recover(current, 0) == MapChestRecoveryAction::ReopenChestNoResend);
    assert(!ConfirmMapChestTransferReopen(state, current,
                                          reopened(11, 0, -532575944698LL),
                                          kFirstName, MapItemNameSource::DisplayName,
                                          &error));
    assert(!ConfirmMapChestTransferReopen(state, current,
                                          reopened(12, 1, -532575944698LL),
                                          kFirstName, MapItemNameSource::DisplayName,
                                          &error));
    assert(!ConfirmMapChestTransferReopen(state, current,
                                          reopened(12, 0, -123),
                                          kFirstName, MapItemNameSource::DisplayName,
                                          &error));
    assert(!ConfirmMapChestTransferReopen(state, current,
                                          reopened(12, 0, -532575944698LL),
                                          kSecondName, MapItemNameSource::DisplayName,
                                          &error));
    assert(!ConfirmMapChestTransferReopen(state, current,
                                          reopened(12, 0, -532575944698LL),
                                          kFirstName, MapItemNameSource::None,
                                          &error));
    assert(ConfirmMapChestTransferReopen(state, current,
                                         reopened(12, 0, -532575944698LL),
                                         kFirstName, MapItemNameSource::DisplayName,
                                         &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::ReopenConfirmed);
    assert(ConfirmMapChestTransferReopen(state, current,
                                         reopened(14, 0, -532575944698LL),
                                         kFirstName, MapItemNameSource::DisplayName,
                                         &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::ReopenConfirmed);
    assert(recover(current, 0) == MapChestRecoveryAction::CommitTileCursorNoResend);
    assert(recover(current, 1) == MapChestRecoveryAction::ClearJournalAfterCursorCommit);
    assert(!ClearCommittedMapChestTransferJournal(state, current, 0, &error));
    assert(ClearCommittedMapChestTransferJournal(state, current, 1, &error));
    assert(LoadMapChestTransferJournal(state, &current, &error) ==
           MapChestJournalLoad::Missing);

    // ACK lost after send: a NEW chest capture with the same UUID can still
    // reconcile, but no second send is ever authorized.
    const MapChestTransferRecord second = prepared(1, -42);
    assert(BeginMapChestTransferJournal(state, second, &error));
    assertLoaded(state, &current);
    assert(ArmMapChestTransferDispatch(state, current, -19, &error));
    assertLoaded(state, &current);
    assert(recover(current, 1) == MapChestRecoveryAction::ReopenChestNoResend);
    assert(ConfirmMapChestTransferReopen(state, current,
                                         reopened(13, 1, -42),
                                         kSecondName, MapItemNameSource::DisplayName,
                                         &error));
    assertLoaded(state, &current);
    assert(ClearCommittedMapChestTransferJournal(state, current, 2, &error));

    // New no-reopen path: a complete, stable native absence scan only
    // confirms a transfer AFTER its exact response is durably accepted.
    const MapChestTransferRecord inventory_only = prepared(0, -44);
    assert(BeginMapChestTransferJournal(state, inventory_only, &error));
    assertLoaded(state, &current);
    assert(ArmMapChestTransferDispatch(state, current, -21, &error));
    assertLoaded(state, &current);
    MapChestInventoryAbsenceProof absent;
    absent.accepted_request_id = -21;
    absent.absent_map_uuid = -44;
    absent.complete_native_inventory_scan = true;
    absent.expected_uuid_absent = true;
    assert(!ConfirmMapChestTransferInventoryAbsent(
        state, current, absent, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::DispatchArmed);
    assert(recover(current, 0) == MapChestRecoveryAction::ReopenChestNoResend);
    assert(NoteMapChestTransferAccepted(state, current, -21, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::ResponseAccepted);
    MapChestInventoryAbsenceProof wrong = absent;
    wrong.accepted_request_id = -23;
    assert(!ConfirmMapChestTransferInventoryAbsent(
        state, current, wrong, &error));
    wrong = absent;
    wrong.absent_map_uuid = -45;
    assert(!ConfirmMapChestTransferInventoryAbsent(
        state, current, wrong, &error));
    wrong = absent;
    wrong.complete_native_inventory_scan = false;
    assert(!ConfirmMapChestTransferInventoryAbsent(
        state, current, wrong, &error));
    wrong = absent;
    wrong.expected_uuid_absent = false;
    assert(!ConfirmMapChestTransferInventoryAbsent(
        state, current, wrong, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::ResponseAccepted);
    assert(!ClearCommittedMapChestTransferJournal(state, current, 1, &error));
    assert(ConfirmMapChestTransferInventoryAbsent(
        state, current, absent, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::InventoryConfirmed);
    assert(recover(current, 0) == MapChestRecoveryAction::CommitTileCursorNoResend);
    assert(recover(current, 1) == MapChestRecoveryAction::ClearJournalAfterCursorCommit);
    assert(!ConfirmMapChestTransferInventoryAbsent(
        state, current, absent, &error));
    assert(ClearCommittedMapChestTransferJournal(state, current, 1, &error));
    assert(LoadMapChestTransferJournal(state, &current, &error) ==
           MapChestJournalLoad::Missing);

    // No-reopen path: a matching accepted Place plus the original chest's
    // positive close signal can commit without scanning an unsynchronized
    // client inventory. Neither an armed send nor a mismatched transfer can.
    const MapChestTransferRecord close_only = prepared(0, -45);
    assert(BeginMapChestTransferJournal(state, close_only, &error));
    assertLoaded(state, &current);
    assert(ArmMapChestTransferDispatch(state, current, -23, &error));
    assertLoaded(state, &current);
    MapChestAcceptedCloseProof closed;
    closed.accepted_request_id = -23;
    closed.original_chest_window_settled = true;
    assert(!ConfirmMapChestTransferAcceptedClose(
        state, current, closed, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::DispatchArmed);
    assert(NoteMapChestTransferAccepted(state, current, -23, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::ResponseAccepted);
    MapChestAcceptedCloseProof wrong_close = closed;
    wrong_close.accepted_request_id = -25;
    assert(!ConfirmMapChestTransferAcceptedClose(
        state, current, wrong_close, &error));
    wrong_close = closed;
    wrong_close.original_chest_window_settled = false;
    assert(!ConfirmMapChestTransferAcceptedClose(
        state, current, wrong_close, &error));
    MapChestTransferRecord wrong_identity = current;
    wrong_identity.world_id = "another-world";
    assert(!ConfirmMapChestTransferAcceptedClose(
        state, wrong_identity, closed, &error));
    wrong_identity = current;
    wrong_identity.chest_x += 1;
    assert(!ConfirmMapChestTransferAcceptedClose(
        state, wrong_identity, closed, &error));
    wrong_identity = current;
    wrong_identity.tile_cursor = 1;
    wrong_identity.chest_slot = 1;
    assert(!ConfirmMapChestTransferAcceptedClose(
        state, wrong_identity, closed, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::ResponseAccepted);
    assert(recover(current, 0) == MapChestRecoveryAction::ReopenChestNoResend);
    assert(!ClearCommittedMapChestTransferJournal(state, current, 1, &error));
    assert(ConfirmMapChestTransferAcceptedClose(state, current, closed, &error));
    assertLoaded(state, &current);
    assert(current.phase == MapChestTransferPhase::AcceptedAndClosed);
    assert(recover(current, 0) == MapChestRecoveryAction::CommitTileCursorNoResend);
    assert(recover(current, 1) ==
           MapChestRecoveryAction::ClearJournalAfterCursorCommit);
    assert(!ConfirmMapChestTransferAcceptedClose(state, current, closed, &error));
    assert(!ClearCommittedMapChestTransferJournal(state, current, 0, &error));
    assert(ClearCommittedMapChestTransferJournal(state, current, 1, &error));
    assert(LoadMapChestTransferJournal(state, &current, &error) ==
           MapChestJournalLoad::Missing);

    // Tile 28 starts chest index 1 at slot 0, not an out-of-range slot 27.
    const MapChestTransferRecord third = prepared(27, -43, 28);
    MapChestTransferRecord invalid_third = third;
    invalid_third.chest_index = 0;
    assert(!BeginMapChestTransferJournal(state, invalid_third, &error));
    assert(BeginMapChestTransferJournal(state, third, &error));
    assertLoaded(state, &current);
    assert(current.tile_cursor == 27 && current.chest_index == 1 &&
           current.chest_slot == 0 && current.tile_count == 28);
    assert(ClassifyMapChestTransferRecovery(
               current, third.world_id, 0, 27, 28, 235, 22, 126, 0) ==
           MapChestRecoveryAction::FreshPreflightRequired);
    assert(std::filesystem::remove(sidecar));

    // Corruption and an orphan .tmp fail closed, never presenting Missing.
    assert(BeginMapChestTransferJournal(state, first, &error));
    {
        std::fstream file(sidecar, std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        file.seekp(64);
        file.put(static_cast<char>(0x7F));
        assert(file);
    }
    assert(LoadMapChestTransferJournal(state, &current, &error) ==
           MapChestJournalLoad::Unsafe);
    assert(std::filesystem::remove(sidecar));
    {
        std::ofstream orphan(sidecar + ".tmp", std::ios::binary);
        assert(orphan);
        orphan.put('x');
    }
    assert(LoadMapChestTransferJournal(state, &current, &error) ==
           MapChestJournalLoad::Unsafe);
    assert(std::filesystem::remove(sidecar + ".tmp"));
    assert(std::filesystem::remove(directory));
    return 0;
}
