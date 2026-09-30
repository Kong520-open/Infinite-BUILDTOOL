#include "MapAnvilCreatedOutputProbe.h"

#include <cassert>
#include <cstring>
#include <string>

using namespace build_import;

namespace {

template <typename T>
void write(std::string* bytes, size_t offset, T value) {
    assert(bytes && offset + sizeof(value) <= bytes->size());
    std::memcpy(&(*bytes)[offset], &value, sizeof(value));
}

void decodesOnlyScalarFields() {
    std::string header(24, '\x7f');
    std::string variant(24, '\x55');
    write<uint32_t>(&header, 0, 4U);
    write<uint8_t>(&header, 4, 61U);
    write<uint32_t>(&header, 8, 0U);
    write<uint8_t>(&header, 12, 0U);
    write<uint8_t>(&header, 16, 50U);
    // Deliberately retain nonzero bytes after the one-byte slot. They are
    // uninitialized native padding and must not affect the decoded slot.
    write<uint32_t>(&header, 20, 1U);
    write<int32_t>(&variant, 0, -401722112);
    write<int32_t>(&variant, 8, 7);
    write<int32_t>(&variant, 16, 3);
    MapAnvilCreatedOutputObservation output;
    assert(DecodeMapAnvilCreatedOutputScalars(header, variant, &output));
    assert(output.kind == 4U);
    assert(output.container_name == 61U);
    assert(output.dynamic_id == 0U && output.has_dynamic_id == 0U);
    assert(output.slot == 50U && output.count == 1U);
    assert(output.network_id == -401722112);
    assert(output.secondary_id == 7 && output.tag == 3);
    assert(output.ticket == 0 && output.site == 0);
}

void rejectsTruncatedRegions() {
    std::string header(24, '\0');
    std::string variant(24, '\0');
    MapAnvilCreatedOutputObservation output;
    assert(!DecodeMapAnvilCreatedOutputScalars(
        std::string_view(header.data(), 23), variant, &output));
    assert(!DecodeMapAnvilCreatedOutputScalars(
        header, std::string_view(variant.data(), 23), &output));
    assert(!DecodeMapAnvilCreatedOutputScalars(header, variant, nullptr));
}

void lateReadyCallbackIsNotLost() {
    MapAnvilCreatedOutputSnapshot snapshot;
    snapshot.callback_count = 4;
    snapshot.captured_count = 1;
    snapshot.records[0].callback_index = 3;
    const uint32_t first = MapAnvilCreatedOutputNewRecordBits(snapshot, 0U);
    assert(first == (1U << 3));
    // Callback 1 publishes after callback 3. Packed snapshot index 0 is now
    // older than the previously observed packed count, but its raw bit is new.
    snapshot.captured_count = 2;
    snapshot.records[0].callback_index = 1;
    snapshot.records[1].callback_index = 3;
    const uint32_t second = MapAnvilCreatedOutputNewRecordBits(snapshot, first);
    assert(second == (1U << 1));
    assert(MapAnvilCreatedOutputNewRecordBits(snapshot, first | second) == 0U);
}

void targetFilterAndIndependentSiteQuotas() {
    MapAnvilCreatedOutputObservation record;
    record.kind = 4U;
    record.container_name = 61U;
    record.slot = 50U;
    assert(MapAnvilCreatedOutputIsTarget(record));
    record.slot = 49U;
    assert(!MapAnvilCreatedOutputIsTarget(record));
    record.slot = 50U;
    record.container_name = 29U;
    assert(!MapAnvilCreatedOutputIsTarget(record));
    record.container_name = 61U;
    record.kind = 5U;
    assert(!MapAnvilCreatedOutputIsTarget(record));

    uint8_t index = 255U;
    for (uint32_t i = 0; i < 8U; ++i) {
        assert(MapAnvilCreatedOutputStorageIndex(1U, i, &index));
        assert(index == i);
    }
    assert(!MapAnvilCreatedOutputStorageIndex(1U, 8U, &index));
    for (uint32_t i = 0; i < 24U; ++i) {
        assert(MapAnvilCreatedOutputStorageIndex(2U, i, &index));
        assert(index == 8U + i);
    }
    assert(!MapAnvilCreatedOutputStorageIndex(2U, 24U, &index));
    assert(!MapAnvilCreatedOutputStorageIndex(3U, 0U, &index));
    assert(!MapAnvilCreatedOutputStorageIndex(2U, 0U, nullptr));
}

void hostBuildNeverArmsNativeHook() {
#if !defined(__aarch64__)
    MapAnvilWindowEvidence evidence;
    evidence.container_open_packet = "\x2e\x03\x05";
    evidence.window_id = 3;
    evidence.observed_container_type = 5;
    evidence.anvil_block_and_window_confirmed = true;
    assert(!ArmMapAnvilCreatedOutputProbe(1U, 1U, 123, evidence));
    MapAnvilCreatedOutputSnapshot snapshot;
    assert(!ReadMapAnvilCreatedOutputProbe(1U, &snapshot));
    assert(snapshot.callback_count == 0 && snapshot.captured_count == 0);
    assert(!snapshot.anvil_simulation_entry.installed);
    assert(!snapshot.anvil_type2_entry.installed);
    assert(!snapshot.craft_optional_constructor.installed);
    assert(snapshot.craft_constructor_recorded == 0U);
    uintptr_t caller = 1U;
    assert(!LookupMapAnvilCraftCtorCaller(0x1234U, &caller));
    assert(caller == 0U);
    assert(!LookupMapAnvilCraftCtorCaller(0U, &caller));
    assert(!LookupMapAnvilCraftCtorCaller(0x1234U, nullptr));
#endif
}

} // namespace

int main() {
    decodesOnlyScalarFields();
    rejectsTruncatedRegions();
    lateReadyCallbackIsNotLost();
    targetFilterAndIndependentSiteQuotas();
    hostBuildNeverArmsNativeHook();
    return 0;
}
