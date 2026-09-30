#include "BuildImportCheckpoint.h"

#include <cstdio>
#include <fstream>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kMagic = 0x504D4942;  // "BIMP" in little-endian form.
constexpr uint32_t kMaxStringLength = 16 * 1024;

template <typename T>
bool writeValue(std::ofstream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(stream);
}

template <typename T>
bool readValue(std::ifstream& stream, T* value) {
    stream.read(reinterpret_cast<char*>(value), sizeof(T));
    return static_cast<bool>(stream);
}

bool writeString(std::ofstream& stream, const std::string& value) {
    if (value.size() > std::numeric_limits<uint32_t>::max()) return false;
    const uint32_t size = static_cast<uint32_t>(value.size());
    if (!writeValue(stream, size)) return false;
    if (size != 0) stream.write(value.data(), size);
    return static_cast<bool>(stream);
}

bool readString(std::ifstream& stream, std::string* value) {
    uint32_t size = 0;
    if (!readValue(stream, &size) || size > kMaxStringLength) return false;
    value->assign(size, '\0');
    if (size != 0) stream.read(&(*value)[0], size);
    return static_cast<bool>(stream);
}

bool validState(uint8_t state) {
    return state <= static_cast<uint8_t>(ImportState::Verifying);
}

bool validPhase(uint8_t phase) {
    return phase < static_cast<uint8_t>(ImportPhase::Count);
}

bool replaceFileAtomically(const std::string& source, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

}  // namespace

bool BuildImportCheckpoint::saveAtomically(const std::string& path,
                                           const CheckpointSnapshot& snapshot,
                                           std::string* error) {
    if (path.empty()) return true;

    const std::string temporary_path = path + ".tmp";
    std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "cannot open checkpoint temporary file";
        return false;
    }

    const uint8_t state = static_cast<uint8_t>(snapshot.state);
    const uint8_t policy = static_cast<uint8_t>(snapshot.config.overwrite_policy);
    const uint8_t has_active = snapshot.has_active_unit ? 1 : 0;
    const uint8_t active_phase = static_cast<uint8_t>(snapshot.active_phase);
    const uint8_t verify_after_import = snapshot.config.verify_after_import ? 1 : 0;
    const uint8_t verification_precision =
        static_cast<uint8_t>(snapshot.config.verification_precision);
    const uint8_t place_deny_layer = snapshot.config.place_deny_layer ? 1 : 0;
    const uint8_t source_type = static_cast<uint8_t>(snapshot.config.source_type);
    // Keep this reserved byte so existing checkpoint layouts remain readable.
    const uint8_t reserved_flags = 0;
    const uint8_t create_maps_after_import =
        snapshot.config.create_maps_after_import ? 1 : 0;
    const bool success =
        writeValue(stream, kMagic) &&
        writeValue(stream, kBuildImportCheckpointCurrentVersion) &&
        writeString(stream, snapshot.identity.job_id) &&
        writeString(stream, snapshot.identity.source_file) &&
        writeString(stream, snapshot.identity.source_hash) &&
        writeString(stream, snapshot.identity.options_hash) &&
        writeString(stream, snapshot.identity.mapper_version) &&
        writeString(stream, snapshot.world.world_id) &&
        writeValue(stream, snapshot.world.dimension_id) &&
        writeValue(stream, snapshot.config.chunk_size) &&
        writeValue(stream, snapshot.config.chunk_load_radius) &&
        writeValue(stream, snapshot.config.simulation_chunk_range) &&
        writeValue(stream, snapshot.config.chunk_wait_ticks) &&
        writeValue(stream, snapshot.config.blocks_per_second) &&
         writeValue(stream, snapshot.config.ticking_area_min_y) &&
         writeValue(stream, snapshot.config.ticking_area_max_y) &&
         writeValue(stream, snapshot.config.teleport_y_offset) &&
         writeValue(stream, snapshot.config.region_grid_origin.x) &&
         writeValue(stream, snapshot.config.region_grid_origin.z) &&
         writeValue(stream, policy) && writeValue(stream, state) &&
        writeValue(stream, snapshot.phase_index) && writeValue(stream, snapshot.chunk_index) &&
        writeValue(stream, has_active) && writeValue(stream, snapshot.active_coord.x) &&
        writeValue(stream, snapshot.active_coord.z) && writeValue(stream, active_phase) &&
        writeValue(stream, snapshot.active_spool_offset) &&
        writeValue(stream, snapshot.completed_command_count) &&
        writeValue(stream, snapshot.completed_block_count) &&
        writeValue(stream, snapshot.verification_sample_index) &&
        writeValue(stream, verify_after_import) &&
        writeValue(stream, verification_precision) &&
        writeString(stream, snapshot.degradation_notice) &&
        writeValue(stream, place_deny_layer) &&
        writeValue(stream, snapshot.config.deny_layer_bounds.min_x) &&
        writeValue(stream, snapshot.config.deny_layer_bounds.min_y) &&
        writeValue(stream, snapshot.config.deny_layer_bounds.min_z) &&
         writeValue(stream, snapshot.config.deny_layer_bounds.max_x) &&
         writeValue(stream, snapshot.config.deny_layer_bounds.max_y) &&
         writeValue(stream, snapshot.config.deny_layer_bounds.max_z) &&
         writeValue(stream, source_type) &&
         writeValue(stream, reserved_flags) &&
         writeValue(stream, create_maps_after_import);
    stream.flush();
    const bool flushed = static_cast<bool>(stream);
    stream.close();

    if (!success || !flushed) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot write checkpoint";
        return false;
    }
    if (!replaceFileAtomically(temporary_path, path)) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot atomically replace checkpoint";
        return false;
    }
    return true;
}

std::optional<CheckpointSnapshot> BuildImportCheckpoint::load(const std::string& path,
                                                                std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    uint32_t magic = 0;
    uint32_t version = 0;
    uint8_t policy = 0;
    uint8_t state = 0;
    uint8_t has_active = 0;
    uint8_t active_phase = 0;
    uint8_t verify_after_import = 1;
    uint8_t verification_precision =
        static_cast<uint8_t>(VerificationPrecision::Thorough);
    uint8_t place_deny_layer = 0;
    uint8_t source_type = static_cast<uint8_t>(ImportSourceType::Schematic);
    uint8_t reserved_flags = 0;
    uint8_t create_maps_after_import = 0;
    CheckpointSnapshot snapshot;

    bool success = stream && readValue(stream, &magic) && readValue(stream, &version) &&
        magic == kMagic && version >= kBuildImportCheckpointOldestSupportedVersion &&
        version <= kBuildImportCheckpointCurrentVersion &&
        readString(stream, &snapshot.identity.job_id) &&
        readString(stream, &snapshot.identity.source_file) &&
        readString(stream, &snapshot.identity.source_hash) &&
        readString(stream, &snapshot.identity.options_hash) &&
        readString(stream, &snapshot.identity.mapper_version) &&
        readString(stream, &snapshot.world.world_id) &&
        readValue(stream, &snapshot.world.dimension_id) &&
        readValue(stream, &snapshot.config.chunk_size) &&
        readValue(stream, &snapshot.config.chunk_load_radius) &&
        (version < 10 || readValue(stream, &snapshot.config.simulation_chunk_range)) &&
        readValue(stream, &snapshot.config.chunk_wait_ticks) &&
        readValue(stream, &snapshot.config.blocks_per_second) &&
         readValue(stream, &snapshot.config.ticking_area_min_y) &&
         readValue(stream, &snapshot.config.ticking_area_max_y) &&
         readValue(stream, &snapshot.config.teleport_y_offset) &&
         (version < 7 ||
          (readValue(stream, &snapshot.config.region_grid_origin.x) &&
           readValue(stream, &snapshot.config.region_grid_origin.z))) &&
         readValue(stream, &policy) && readValue(stream, &state) &&
        readValue(stream, &snapshot.phase_index) && readValue(stream, &snapshot.chunk_index) &&
        readValue(stream, &has_active) && readValue(stream, &snapshot.active_coord.x) &&
        readValue(stream, &snapshot.active_coord.z) && readValue(stream, &active_phase) &&
        readValue(stream, &snapshot.active_spool_offset) &&
        readValue(stream, &snapshot.completed_command_count) &&
        readValue(stream, &snapshot.completed_block_count);

    if (success && version >= 5) {
        success = readValue(stream, &snapshot.verification_sample_index);
    }
    if (success && version >= 8) {
        success = readValue(stream, &verify_after_import) &&
                  readValue(stream, &verification_precision);
    }
    if (success && version >= 9) {
        success = readString(stream, &snapshot.degradation_notice);
    }
    if (success && version >= 11) {
        success = readValue(stream, &place_deny_layer) &&
                  readValue(stream, &snapshot.config.deny_layer_bounds.min_x) &&
                  readValue(stream, &snapshot.config.deny_layer_bounds.min_y) &&
                  readValue(stream, &snapshot.config.deny_layer_bounds.min_z) &&
                  readValue(stream, &snapshot.config.deny_layer_bounds.max_x) &&
                  readValue(stream, &snapshot.config.deny_layer_bounds.max_y) &&
                   readValue(stream, &snapshot.config.deny_layer_bounds.max_z);
    }
    if (success && version >= 12) {
        success = readValue(stream, &source_type) && readValue(stream, &reserved_flags);
    }
    if (success && version >= 13) {
        success = readValue(stream, &create_maps_after_import);
    }

    // A v4-v9 checkpoint has no native simulation-range field.  Normalize it
    // before validation so its historical 32-block (or custom) partitioning
    // remains eligible for safe restoration.
    if (version < 10) snapshot.config.simulation_chunk_range = 0;

    // v10 and older checkpoints predate the optional deny foundation. Keep
    // their default-invalid bounds so no resumed legacy job can place one.
    if (version < 11) {
        snapshot.config.place_deny_layer = false;
        snapshot.config.deny_layer_bounds = {};
    }
    // Checkpoints before v12 have no explicit source-type field.
    if (version < 12) {
        snapshot.config.source_type = ImportSourceType::Schematic;
    }

    char trailing_byte = 0;
    const bool has_trailing_data = success && static_cast<bool>(stream.read(&trailing_byte, 1));
    const bool deny_bounds_ready = snapshot.config.deny_layer_bounds.isValid() &&
        snapshot.config.deny_layer_bounds.min_y == snapshot.config.deny_layer_bounds.max_y;
    // The first Planning checkpoint is written before parsing has resolved the
    // source volume. Every checkpoint that can resume placement needs a
    // complete, single-height deny layer when the option is enabled.
    const bool unresolved_deny_bounds = place_deny_layer != 0 &&
        state == static_cast<uint8_t>(ImportState::Planning);
    if (!success || has_trailing_data || has_active > 1 || verify_after_import > 1 ||
        place_deny_layer > 1 || create_maps_after_import > 1 ||
         verification_precision > static_cast<uint8_t>(VerificationPrecision::Thorough) ||
          source_type > static_cast<uint8_t>(ImportSourceType::CommandMusicMidi) ||
         reserved_flags > 1 ||
        (create_maps_after_import != 0 &&
         source_type != static_cast<uint8_t>(ImportSourceType::PixelArtPng)) ||
        policy > static_cast<uint8_t>(OverwritePolicy::ClearImportedBounds) ||
        !validState(state) || !validPhase(active_phase) || snapshot.identity.job_id.empty() ||
        snapshot.world.world_id.empty() || snapshot.config.chunk_size <= 0 ||
        snapshot.config.chunk_load_radius < 0 || snapshot.config.chunk_load_radius > 3 ||
        (snapshot.config.simulation_chunk_range != 0 &&
         (!isValidSimulationChunkRange(snapshot.config.simulation_chunk_range) ||
          snapshot.config.chunk_size != ImportConfig::kVanillaChunkSize)) ||
        snapshot.config.chunk_wait_ticks < 0 ||
        snapshot.config.blocks_per_second < 1 ||
        snapshot.config.blocks_per_second > kMaximumBlocksPerSecond ||
        snapshot.config.ticking_area_min_y > snapshot.config.ticking_area_max_y ||
        (place_deny_layer != 0 && !unresolved_deny_bounds && !deny_bounds_ready) ||
        (has_active == 0 && snapshot.active_spool_offset != 0)) {
        if (error) *error = "checkpoint is missing, corrupt, or unsupported";
        return std::nullopt;
    }

    snapshot.config.overwrite_policy = static_cast<OverwritePolicy>(policy);
    snapshot.config.verify_after_import = verify_after_import != 0;
    snapshot.config.verification_precision =
        static_cast<VerificationPrecision>(verification_precision);
    snapshot.config.place_deny_layer = place_deny_layer != 0;
    snapshot.config.source_type = static_cast<ImportSourceType>(source_type);
    snapshot.config.create_maps_after_import = create_maps_after_import != 0;
    snapshot.format_version = version;
    snapshot.state = static_cast<ImportState>(state);
    snapshot.has_active_unit = has_active != 0;
    snapshot.active_phase = static_cast<ImportPhase>(active_phase);
    snapshot.config.checkpoint_path = path;
    return snapshot;
}

}  // namespace build_import
