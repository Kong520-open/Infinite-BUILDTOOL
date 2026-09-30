#include "MapAnvilRenameJournal.h"
#include "MapTileNaming.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace build_import;

namespace {

MapAnvilRenameRecord prepared(uint64_t cursor = 0, uint32_t columns = 2,
                              uint32_t rows = 1) {
    MapAnvilRenameRecord record;
    record.world_id = "stable:v1:rename-test|0";
    record.dimension_id = 0;
    record.tile_cursor = cursor;
    record.tile_count = static_cast<uint64_t>(columns) * rows;
    record.columns = columns;
    record.rows = rows;
    record.anvil_x = 236;
    record.anvil_y = 22;
    record.anvil_z = 126;
    record.map_runtime_item_id = 358;
    record.map_uuid = -532575944698LL;
    record.input_source_network_stack_id = 71;
    assert(FormatMapTileName(cursor, columns, rows, &record.expected_title));
    return record;
}

MapAnvilInputProof inputProof(const MapAnvilRenameRecord& record) {
    MapAnvilInputProof proof;
    proof.world_id = record.world_id;
    proof.dimension_id = record.dimension_id;
    proof.anvil_x = record.anvil_x;
    proof.anvil_y = record.anvil_y;
    proof.anvil_z = record.anvil_z;
    proof.fresh_window_token = 10;
    proof.window_id = 3;
    proof.input_slot = 1;
    proof.count = 1;
    proof.runtime_item_id = record.map_runtime_item_id;
    proof.network_stack_id = record.input_source_network_stack_id + 10;
    proof.map_uuid = record.map_uuid;
    proof.native_readback = true;
    return proof;
}

MapAnvilResponseCorrelation inputCorrelation() {
    MapAnvilResponseCorrelation value;
    value.session_generation = 8;
    value.response_generation_before_send = 20;
    value.window_token = 10;
    value.window_id = 3;
    value.source_hotbar_slot = 6;
    return value;
}

MapAnvilResponseCorrelation craftCorrelation() {
    MapAnvilResponseCorrelation value = inputCorrelation();
    value.response_generation_before_send = 21;
    value.source_hotbar_slot = -1;
    return value;
}

MapAnvilOutputProof outputProof(const MapAnvilRenameRecord& record) {
    MapAnvilOutputProof proof;
    proof.world_id = record.world_id;
    proof.dimension_id = record.dimension_id;
    proof.count = 1;
    proof.runtime_item_id = record.map_runtime_item_id;
    proof.network_stack_id = 72;
    proof.map_uuid = record.map_uuid;
    proof.name_source = MapItemNameSource::DisplayName;
    proof.name = record.expected_title;
    proof.native_readback = true;
    return proof;
}

MapAnvilRenameRecord loaded(const std::string& state) {
    MapAnvilRenameRecord result;
    std::string error;
    assert(LoadMapAnvilRenameJournal(state, &result, &error) ==
           MapAnvilRenameLoad::Loaded);
    assert(error.empty());
    return result;
}

MapAnvilRenameRecovery recovery(const MapAnvilRenameRecord& record,
                                uint64_t checkpoint_cursor) {
    return ClassifyMapAnvilRenameRecovery(record, record.world_id,
                                          record.dimension_id,
                                          checkpoint_cursor, record.tile_count);
}

ContainerCaptureResult reopened(const MapAnvilRenameRecord& record) {
    ContainerCaptureResult capture;
    capture.token = 21;
    capture.x = 235;
    capture.y = 22;
    capture.z = 126;
    capture.container_id = 5;
    capture.container_type = 0;
    capture.container_opened = true;
    capture.slot_count = 27;
    capture.has_full_container_name = true;
    capture.full_container_name = 0;
    CapturedContainerItem item;
    item.slot = 0;
    item.numeric_id = record.map_runtime_item_id;
    item.count = 1;
    item.has_network_stack_id = true;
    item.network_stack_id = 72;
    item.has_map_uuid = true;
    item.map_uuid = record.map_uuid;
    item.name_status = MapItemNameStatus::Present;
    item.name_source = MapItemNameSource::DisplayName;
    item.name_candidate = record.expected_title;
    capture.items.push_back(item);
    return capture;
}

}  // namespace

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("map_anvil_journal_test_" + std::to_string(unique));
    assert(std::filesystem::create_directory(directory));
    const std::string state = (directory / "map_creation.state").string();
    const std::string sidecar = MapAnvilRenameJournalPath(state);
    std::string error;
    MapAnvilRenameRecord current;
    assert(LoadMapAnvilRenameJournal(state, &current, &error) ==
           MapAnvilRenameLoad::Missing);

    MapAnvilRenameRecord first = prepared();
    MapAnvilRenameRecord wrong_name = first;
    wrong_name.expected_title += " ";
    assert(!BeginMapAnvilRenameJournal(state, wrong_name, &error));
    assert(BeginMapAnvilRenameJournal(state, first, &error));
    assert(!BeginMapAnvilRenameJournal(state, first, &error));
    // A prior-version sidecar cannot prove whether a result click already
    // queued an asynchronous craft request; it must not be auto-upgraded.
    const std::string legacy_state = (directory / "legacy.state").string();
    const std::string legacy_sidecar = MapAnvilRenameJournalPath(legacy_state);
    assert(std::filesystem::copy_file(sidecar, legacy_sidecar));
    {
        std::fstream file(legacy_sidecar,
                          std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        file.seekp(6);
        file.put('3');
        file.seekp(8);
        file.put(static_cast<char>(3));
        assert(file);
    }
    MapAnvilRenameRecord legacy;
    assert(LoadMapAnvilRenameJournal(legacy_state, &legacy, &error) ==
           MapAnvilRenameLoad::Unsafe);
    assert(std::filesystem::remove(legacy_sidecar));
    current = loaded(state);
    assert(recovery(current, 0) == MapAnvilRenameRecovery::FreshInputPreflight);
    assert(recovery(current, 1) == MapAnvilRenameRecovery::Unsafe);
    assert(ClassifyMapAnvilRenameRecovery(current, "wrong-world", 0, 0, 2) ==
           MapAnvilRenameRecovery::Unsafe);

    MapAnvilResponseCorrelation invalid_correlation = inputCorrelation();
    invalid_correlation.session_generation = 0;
    assert(!ArmMapAnvilInputDispatch(state, current, -9,
                                      invalid_correlation, &error));
    invalid_correlation = inputCorrelation();
    invalid_correlation.window_id = 0xFFU;
    assert(!ArmMapAnvilInputDispatch(state, current, -9,
                                      invalid_correlation, &error));
    assert(!ArmMapAnvilInputDispatch(state, current, 2,
                                      inputCorrelation(), &error));
    assert(!ArmMapAnvilInputDispatch(state, current, -2,
                                      inputCorrelation(), &error));
    assert(ArmMapAnvilInputDispatch(state, current, -9,
                                     inputCorrelation(), &error));
    current = loaded(state);
    assert(current.phase == MapAnvilRenamePhase::InputDispatchArmed);
    assert(recovery(current, 0) == MapAnvilRenameRecovery::ReconcileInputNoResend);
    assert(!ArmMapAnvilInputDispatch(state, first, -11,
                                      inputCorrelation(), &error));
    assert(!ConfirmMapAnvilInput(state, current, inputProof(current), &error));
    assert(loaded(state).phase == MapAnvilRenamePhase::InputDispatchArmed);
    assert(!NoteMapAnvilInputAccepted(state, current, -11, &error));
    assert(NoteMapAnvilInputAccepted(state, current, -9, &error));
    current = loaded(state);
    MapAnvilInputProof input = inputProof(current);
    input.map_uuid = -123;
    assert(!ConfirmMapAnvilInput(state, current, input, &error));
    input = inputProof(current);
    input.network_stack_id = 0;
    assert(!ConfirmMapAnvilInput(state, current, input, &error));
    input = inputProof(current);
    assert(ConfirmMapAnvilInput(state, current, input, &error));
    current = loaded(state);
    assert(recovery(current, 0) == MapAnvilRenameRecovery::FreshCraftPreflight);
    assert(!ArmMapAnvilCraftDispatch(state, current, input, -9,
                                      craftCorrelation(), 1U, &error));
    input.fresh_window_token = 0;
    assert(!ArmMapAnvilCraftClick(state, current, input, &error));
    input = inputProof(current);
    assert(ArmMapAnvilCraftClick(state, current, input, &error));
    current = loaded(state);
    assert(current.phase == MapAnvilRenamePhase::CraftClickArmed);
    assert(current.craft_input_network_stack_id == input.network_stack_id);
    assert(current.craft_destination_hotbar_slot == -1);
    assert(recovery(current, 0) ==
           MapAnvilRenameRecovery::ReconcileCraftClickNoResend);
    assert(!ArmMapAnvilCraftClick(state, current, input, &error));
    assert(!ArmMapAnvilCraftDispatch(state, current, input, -11,
                                      craftCorrelation(), 9U, &error));
    MapAnvilInputProof wrong_input = input;
    wrong_input.network_stack_id += 1;
    assert(!ArmMapAnvilCraftDispatch(state, current, wrong_input, -11,
                                      craftCorrelation(), 1U, &error));
    MapAnvilResponseCorrelation wrong_craft_correlation = craftCorrelation();
    wrong_craft_correlation.window_token += 1U;
    assert(!ArmMapAnvilCraftDispatch(state, current, input, -11,
                                      wrong_craft_correlation, 1U, &error));
    assert(ArmMapAnvilCraftDispatch(state, current, input, -11,
                                     craftCorrelation(), 1U, &error));
    current = loaded(state);
    assert(current.craft_destination_hotbar_slot == 1);
    assert(recovery(current, 0) == MapAnvilRenameRecovery::ReconcileCraftNoResend);
    assert(!ArmMapAnvilCraftDispatch(state, current, input, -13,
                                      craftCorrelation(), 1U, &error));
    assert(!ConfirmMapAnvilRenamedOutput(state, current, outputProof(current), &error));
    assert(loaded(state).phase == MapAnvilRenamePhase::CraftDispatchArmed);
    assert(!NoteMapAnvilCraftAccepted(state, current, -13, 72, &error));
    assert(!NoteMapAnvilCraftAccepted(state, current, -11, 0, &error));
    assert(NoteMapAnvilCraftAccepted(state, current, -11, 72, &error));
    current = loaded(state);
    assert(current.craft_accepted_output_network_stack_id == 72);
    MapAnvilOutputProof output = outputProof(current);
    output.network_stack_id = 73;
    assert(!ConfirmMapAnvilRenamedOutput(state, current, output, &error));
    output = outputProof(current);
    output.name += " ";
    assert(!ConfirmMapAnvilRenamedOutput(state, current, output, &error));
    output = outputProof(current);
    output.name_source = MapItemNameSource::None;
    assert(!ConfirmMapAnvilRenamedOutput(state, current, output, &error));
    output = outputProof(current);
    assert(ConfirmMapAnvilRenamedOutput(state, current, output, &error));
    current = loaded(state);
    assert(current.renamed_network_stack_id == 72);
    assert(recovery(current, 0) == MapAnvilRenameRecovery::ProceedToChestStorage);
    assert(recovery(current, 1) == MapAnvilRenameRecovery::ClearAfterStorageCommit);
    assert(!ClearCommittedMapAnvilRenameJournal(state, current, {}, 0, &error));

    // A forged in-memory chest record is insufficient: the matching durable
    // ReopenConfirmed chest sidecar must also exist before rename cleanup.
    MapChestTransferRecord chest;
    chest.world_id = current.world_id;
    chest.dimension_id = current.dimension_id;
    chest.tile_cursor = current.tile_cursor;
    chest.tile_count = current.tile_count;
    chest.chest_x = 235;
    chest.chest_y = 22;
    chest.chest_z = 126;
    chest.chest_slot = 0;
    chest.source_runtime_item_id = current.map_runtime_item_id;
    chest.source_network_stack_id = current.renamed_network_stack_id;
    chest.source_map_uuid = current.map_uuid;
    chest.pre_send_capture_token = 20;
    // The new inventory-absence path also permits rename cleanup, but only
    // when the separate chest sidecar itself durably reached that phase.
    const std::string inventory_state =
        (directory / "inventory.state").string();
    assert(std::filesystem::copy_file(
        sidecar, MapAnvilRenameJournalPath(inventory_state)));
    assert(BeginMapChestTransferJournal(inventory_state, chest, &error));
    assert(ArmMapChestTransferDispatch(inventory_state, chest, -15, &error));
    MapChestTransferRecord inventory_chest = chest;
    inventory_chest.phase = MapChestTransferPhase::DispatchArmed;
    inventory_chest.request_id = -15;
    assert(NoteMapChestTransferAccepted(
        inventory_state, inventory_chest, -15, &error));
    inventory_chest.phase = MapChestTransferPhase::ResponseAccepted;
    MapChestInventoryAbsenceProof absent;
    absent.accepted_request_id = -15;
    absent.absent_map_uuid = current.map_uuid;
    absent.complete_native_inventory_scan = true;
    absent.expected_uuid_absent = true;
    assert(ConfirmMapChestTransferInventoryAbsent(
        inventory_state, inventory_chest, absent, &error));
    inventory_chest.phase = MapChestTransferPhase::InventoryConfirmed;
    assert(!ClearCommittedMapAnvilRenameJournal(
        state, current, inventory_chest, 1, &error));
    assert(ClearCommittedMapAnvilRenameJournal(
        inventory_state, current, inventory_chest, 1, &error));
    assert(ClearCommittedMapChestTransferJournal(
        inventory_state, inventory_chest, 1, &error));
    // Accepted Place plus the original window's positive close is another
    // durable commit path. A forged in-memory phase must not clear rename.
    const std::string closed_state =
        (directory / "closed.state").string();
    assert(std::filesystem::copy_file(
        sidecar, MapAnvilRenameJournalPath(closed_state)));
    assert(BeginMapChestTransferJournal(closed_state, chest, &error));
    assert(ArmMapChestTransferDispatch(closed_state, chest, -17, &error));
    MapChestTransferRecord closed_chest = chest;
    closed_chest.phase = MapChestTransferPhase::DispatchArmed;
    closed_chest.request_id = -17;
    assert(NoteMapChestTransferAccepted(
        closed_state, closed_chest, -17, &error));
    closed_chest.phase = MapChestTransferPhase::ResponseAccepted;
    MapChestTransferRecord forged_closed = closed_chest;
    forged_closed.phase = MapChestTransferPhase::AcceptedAndClosed;
    assert(!ClearCommittedMapAnvilRenameJournal(
        closed_state, current, forged_closed, 1, &error));
    MapChestAcceptedCloseProof closed;
    closed.accepted_request_id = -17;
    closed.original_chest_window_settled = true;
    assert(ConfirmMapChestTransferAcceptedClose(
        closed_state, closed_chest, closed, &error));
    closed_chest.phase = MapChestTransferPhase::AcceptedAndClosed;
    assert(ClearCommittedMapAnvilRenameJournal(
        closed_state, current, closed_chest, 1, &error));
    assert(ClearCommittedMapChestTransferJournal(
        closed_state, closed_chest, 1, &error));
    assert(!ClearCommittedMapAnvilRenameJournal(state, current, chest, 1, &error));
    assert(BeginMapChestTransferJournal(state, chest, &error));
    assert(ArmMapChestTransferDispatch(state, chest, -13, &error));
    chest.phase = MapChestTransferPhase::DispatchArmed;
    chest.request_id = -13;
    assert(ConfirmMapChestTransferReopen(state, chest, reopened(current),
                                         current.expected_title,
                                         MapItemNameSource::DisplayName, &error));
    chest.phase = MapChestTransferPhase::ReopenConfirmed;
    assert(ClearCommittedMapAnvilRenameJournal(state, current, chest, 1, &error));
    assert(LoadMapAnvilRenameJournal(state, &current, &error) ==
           MapAnvilRenameLoad::Missing);
    assert(ClearCommittedMapChestTransferJournal(state, chest, 1, &error));

    // Corruption, an unresolved temporary file, and a stale expectation all
    // fail closed without allowing a second rename send.
    first = prepared(1);
    assert(BeginMapAnvilRenameJournal(state, first, &error));
    current = loaded(state);
    assert(!ArmMapAnvilInputDispatch(state, prepared(0), -15,
                                      inputCorrelation(), &error));
    {
        std::fstream file(sidecar, std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        file.seekp(56);
        file.put(static_cast<char>(0x7F));
        assert(file);
    }
    assert(LoadMapAnvilRenameJournal(state, &current, &error) ==
           MapAnvilRenameLoad::Unsafe);
    assert(std::filesystem::remove(sidecar));
    {
        std::ofstream orphan(sidecar + ".tmp", std::ios::binary);
        assert(orphan);
        orphan.put('x');
    }
    assert(LoadMapAnvilRenameJournal(state, &current, &error) ==
           MapAnvilRenameLoad::Unsafe);
    assert(std::filesystem::remove(sidecar + ".tmp"));
    // A previous, shorter sidecar cannot be promoted: it lacks fields from
    // the current v4 record, including the pre-click no-resend barrier.
    assert(BeginMapAnvilRenameJournal(state, prepared(1), &error));
    assert(std::filesystem::file_size(sidecar) == 396U);
    std::filesystem::resize_file(sidecar, 388U);
    assert(LoadMapAnvilRenameJournal(state, &current, &error) ==
           MapAnvilRenameLoad::Unsafe);
    assert(std::filesystem::remove(sidecar));
    assert(std::filesystem::remove(directory));
    return 0;
}
