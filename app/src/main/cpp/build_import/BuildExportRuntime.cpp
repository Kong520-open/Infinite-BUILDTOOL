#include "BuildExportRuntime.h"

#include "BuildExportPaths.h"
#include "ContainerCaptureMailbox.h"
#include "ContainerClosePacketSender.h"
#include "ContainerEntityCodec.h"
#include "ContainerOpenPacketSender.h"
#include "InfiniteczBuildWriter.h"
#include "ItemRuntimeRegistry.h"
#include "SchematicWriter.h"
#include "SignEntityCodec.h"
#include "../main.h"
#include "../tp/BuildPacketReceiveHook.h"
#include "../tp/MinecraftUpdateHook.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace build_import {
namespace {

constexpr int64_t kWorldChunkSize = 16;
constexpr int32_t kMinimumSimulationChunkRange = 4;
constexpr int32_t kMaximumSimulationChunkRange = 8;
// The 4 ms deadline is the real limiter; the block cap only bounds the
// journal staging buffer. Both are sized so a fast device can use the whole
// deadline now that a block read no longer costs a memory-probe syscall.
constexpr size_t kMaximumBlocksPerTick = 65'536;
constexpr size_t kScanDeadlineCheckInterval = 256;
constexpr auto kMaximumScanTimePerTick = std::chrono::milliseconds(4);
constexpr auto kCheckpointTimeInterval = std::chrono::seconds(1);
constexpr int kInitialChunkWaitTicks = 12;
constexpr int kRetryChunkWaitTicks = 14;
constexpr uint32_t kMaximumBatchRetries = 24;
constexpr uint32_t kMaximumManualBatchRetries = 120;
constexpr uint32_t kRequiredReadableSamples = 2;
constexpr uint32_t kMaximumMissingPlayerTicks = 100;
// Retry every clickable block face.  A top-face-only ItemUse fails for a
// covered chest or a container embedded in a build, even though another face
// is reachable by the server's interaction validation.
constexpr uint32_t kMaximumContainerAttempts = 6;
constexpr int kContainerTeleportTimeoutTicks = 160;
// ContainerOpen and InventoryContent are two independent server round trips.
// Each stage receives a complete deadline: a late ContainerOpen must not leave
// only the tail of this timeout for the usually-immediate inventory payload.
constexpr int kContainerPacketTimeoutTicks = 100;
constexpr int kContainerSettleTicks = 2;
// Do not send a new ClickBlock while an incomplete request's late packets can
// still be in the receive queue.  The mailbox also quarantines that tail, so
// this is a bounded game-thread cooldown rather than a blocking sleep.
constexpr int kContainerOpenCooldownTicks = 10;
constexpr int64_t kContainerInteractionDistanceSquared = 25;
// A player inside this radius is near the center of a scan batch. The batch
// side is selected from the server's simulation range, so this constant does
// not assume the old fixed 64x64-block region.
constexpr double kManualArrivalRadiusBlocks = 16.0;
// Some servers reject a single /tp spanning more than an allowed distance
// ("teleported too far"), which previously disabled automatic teleport after
// one 6 s probe timeout. When the direct teleport to a batch centre is
// rejected, travel is retried as a chain of bounded hops along the straight
// line to the centre; the hop length halves after each further rejection and
// automatic teleport is only disabled once even the minimum hop fails.
constexpr int32_t kInitialTeleportHopBlocks = 96;
constexpr int32_t kMinimumTeleportHopBlocks = 24;

std::string displayNameForPath(const std::string& path) {
    const size_t separator = path.find_last_of("/\\");
    const size_t begin = separator == std::string::npos ? 0 : separator + 1;
    std::string name = begin < path.size() ? path.substr(begin) : std::string();
    return name.empty() ? "export" : name;
}

int containerInteractionFaceForAttempt(uint32_t attempt) noexcept {
    // Bedrock faces: down, up, north, south, west, east.  Prefer the usual
    // top interaction first, then exhaust the four sides before the underside.
    constexpr std::array<int, kMaximumContainerAttempts> kFaces{{1, 2, 3, 4, 5, 0}};
    const size_t index = attempt == 0U ? 0U :
        static_cast<size_t>((attempt - 1U) % kFaces.size());
    return kFaces[index];
}

bool serviceContainerCaptureQuarantine() {
    ContainerCaptureQuarantine quarantine;
    if (!PollContainerCaptureQuarantine(&quarantine)) return false;
    if (!quarantine.container_opened || quarantine.container_closed ||
        quarantine.content_captured || quarantine.close_sent) {
        return true;
    }
    std::string close_error;
    (void)ContainerClosePacketSender::send(quarantine.container_id,
                                           quarantine.container_type,
                                           &close_error);
    // This is an attempted best-effort cleanup.  Do not repeatedly invoke a
    // version-sensitive native packet path while the same delayed packet tail
    // is quarantined.
    MarkContainerCaptureQuarantineCloseSent(quarantine.container_id,
                                            quarantine.container_type);
    // Even if the raw close ABI is temporarily unavailable, retain the
    // receive quarantine until it expires.  Starting a second window here is
    // less safe than deferring one tick.
    return true;
}

bool isCommandBlockState(std::string_view state) {
    const size_t separator = state.find(':');
    const size_t bracket = state.find('[');
    const size_t begin = separator == std::string_view::npos ? 0 : separator + 1;
    const size_t end = bracket == std::string_view::npos ? state.size() : bracket;
    if (begin >= end) return false;
    const std::string_view leaf = state.substr(begin, end - begin);
    return leaf == "command_block" || leaf == "repeating_command_block" ||
        leaf == "chain_command_block";
}

std::string_view rawSnapshotLeaf(std::string_view name) {
    const size_t bracket = name.find('[');
    if (bracket != std::string_view::npos) name = name.substr(0, bracket);
    const size_t separator = name.rfind(':');
    return separator == std::string_view::npos ? name : name.substr(separator + 1);
}

bool isSafeRawIdentifier(std::string_view identifier) {
    if (identifier.empty() || identifier.size() > 1024U ||
        identifier.find(':') == std::string_view::npos) return false;
    for (const char character : identifier) {
        const unsigned char ch = static_cast<unsigned char>(character);
        if (!(std::islower(ch) || std::isdigit(ch) || character == ':' ||
              character == '_' || character == '-')) return false;
    }
    return true;
}

bool isRawEntityCandidate(std::string_view name) {
    const std::string_view leaf = rawSnapshotLeaf(name);
    // These blocks carry coordinate-specific block-entity payloads. Querying
    // them per coordinate is intentional; unlike a BlockState, an inventory,
    // sign text, or command payload cannot be cached by type/aux alone.
    return leaf == "barrel" || leaf == "beacon" || leaf == "blast_furnace" ||
        leaf == "brewing_stand" || leaf == "campfire" || leaf == "chest" ||
        leaf == "command_block" || leaf == "conduit" || leaf == "dropper" ||
        leaf == "dispenser" || leaf == "furnace" || leaf == "hopper" ||
        leaf == "jukebox" || leaf == "lectern" || leaf == "mob_spawner" ||
        leaf == "shulker_box" || leaf == "smoker" || leaf == "structure_block" ||
        leaf == "trapped_chest" || leaf == "flower_pot" ||
        leaf.find("chest") != std::string_view::npos ||
        leaf.find("shulker_box") != std::string_view::npos ||
        leaf.find("furnace") != std::string_view::npos ||
        leaf.find("banner") != std::string_view::npos ||
        leaf.find("sign") != std::string_view::npos ||
        leaf.find("skull") != std::string_view::npos;
}

bool isRawSnapshotCandidate(std::string_view name, uint16_t aux) {
    if (aux != 0) return true;
    const std::string_view leaf = rawSnapshotLeaf(name);
    // Preserve state for redstone and attachment blocks even when their
    // legacy aux is zero. Their placement/support information is commonly
    // represented by a modern state map rather than by the legacy palette.
    return isRawEntityCandidate(leaf) ||
        leaf.find("redstone") != std::string_view::npos ||
        leaf.find("repeater") != std::string_view::npos ||
        leaf.find("comparator") != std::string_view::npos ||
        leaf.find("piston") != std::string_view::npos ||
        leaf.find("observer") != std::string_view::npos ||
        leaf.find("torch") != std::string_view::npos ||
        leaf.find("button") != std::string_view::npos ||
        leaf == "lever" || leaf.find("rail") != std::string_view::npos ||
        leaf.find("tripwire") != std::string_view::npos ||
        leaf.find("door") != std::string_view::npos ||
        leaf.find("trapdoor") != std::string_view::npos ||
        leaf.find("stair") != std::string_view::npos ||
        leaf.find("slab") != std::string_view::npos ||
        leaf.find("wall") != std::string_view::npos ||
        leaf.find("fence") != std::string_view::npos ||
        leaf.find("pane") != std::string_view::npos ||
        leaf.find("pressure_plate") != std::string_view::npos ||
        leaf.find("fence_gate") != std::string_view::npos ||
        leaf == "ladder" || leaf == "scaffolding" || leaf == "vine" ||
        leaf.find("sand") != std::string_view::npos ||
        leaf.find("gravel") != std::string_view::npos ||
        leaf.find("concrete_powder") != std::string_view::npos ||
        leaf.find("anvil") != std::string_view::npos || leaf == "bed" ||
        leaf.find("_bed") != std::string_view::npos;
}

uint16_t commandBlockModeForState(std::string_view state) {
    const size_t separator = state.find(':');
    const size_t bracket = state.find('[');
    const size_t begin = separator == std::string_view::npos ? 0 : separator + 1;
    const size_t end = bracket == std::string_view::npos ? state.size() : bracket;
    if (begin >= end) return kCommandBlockModeImpulse;
    const std::string_view leaf = state.substr(begin, end - begin);
    if (leaf == "repeating_command_block") return kCommandBlockModeRepeat;
    if (leaf == "chain_command_block") return kCommandBlockModeChain;
    return kCommandBlockModeImpulse;
}

int32_t clampedTargetY(int32_t max_y, int32_t offset) {
    const int64_t value = static_cast<int64_t>(max_y) + offset;
    if (value < std::numeric_limits<int32_t>::min()) {
        return std::numeric_limits<int32_t>::min();
    }
    if (value > std::numeric_limits<int32_t>::max()) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(value);
}

int32_t batchCenterCoordinate(int32_t minimum, int32_t maximum) {
    // The sum is evaluated as int64_t because world coordinates can span the
    // complete int32 range. floor() gives a stable block coordinate for a
    // center that falls between two blocks, including negative coordinates.
    const double center = (static_cast<double>(minimum) +
                           static_cast<double>(maximum)) * 0.5;
    const double floored = std::floor(center);
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (floored > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(floored);
}

int64_t floorChunkCoordinate(int32_t block) {
    int64_t chunk = static_cast<int64_t>(block) / kWorldChunkSize;
    if (block < 0 && static_cast<int64_t>(block) % kWorldChunkSize != 0) --chunk;
    return chunk;
}

bool containsCaseInsensitive(const std::string& value, const char* needle) {
    if (!needle || !*needle) return true;
    const size_t needle_size = std::char_traits<char>::length(needle);
    if (needle_size > value.size()) return false;
    for (size_t start = 0; start + needle_size <= value.size(); ++start) {
        size_t offset = 0;
        while (offset < needle_size) {
            const unsigned char character = static_cast<unsigned char>(value[start + offset]);
            if (static_cast<char>(std::tolower(character)) != needle[offset]) break;
            ++offset;
        }
        if (offset == needle_size) return true;
    }
    return false;
}

bool isSupportedBatchSize(int32_t batch_size) {
    if (batch_size % static_cast<int32_t>(kWorldChunkSize) != 0) return false;
    const int32_t range = batch_size / static_cast<int32_t>(kWorldChunkSize);
    return range >= kMinimumSimulationChunkRange &&
        range <= kMaximumSimulationChunkRange;
}

}  // namespace

BuildExportRuntime& BuildExportRuntime::instance() {
    static BuildExportRuntime runtime;
    return runtime;
}

bool BuildExportRuntime::isActiveState(BuildExportState state) {
    return state == BuildExportState::Preparing ||
        state == BuildExportState::LoadingRegion ||
        state == BuildExportState::WaitingForPlayer ||
        state == BuildExportState::Scanning ||
        state == BuildExportState::CapturingContainers ||
        state == BuildExportState::Writing;
}

BuildExportRuntime::~BuildExportRuntime() {
    cancel();
    if (writer_thread_.joinable()) writer_thread_.join();
}

uint64_t BuildExportRuntime::batchVolume(const ScanBatch& batch, int32_t height) {
    if (height <= 0 || batch.min_x > batch.max_x || batch.min_z > batch.max_z) return 0;
    return static_cast<uint64_t>(static_cast<int64_t>(batch.max_x) - batch.min_x + 1) *
        static_cast<uint64_t>(static_cast<int64_t>(batch.max_z) - batch.min_z + 1) *
        static_cast<uint64_t>(height);
}

bool BuildExportRuntime::createBatches(int32_t min_x, int32_t max_x,
                                        int32_t min_z, int32_t max_z,
                                        int32_t height,
                                        int32_t batch_size,
                                        std::vector<ScanBatch>* batches,
                                        std::string* error) {
    if (!batches || min_x > max_x || min_z > max_z || height <= 0 ||
        !isSupportedBatchSize(batch_size)) {
        if (error) *error = "invalid export scan bounds";
        return false;
    }
    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(max_x) - min_x + 1);
    const uint64_t length = static_cast<uint64_t>(
        static_cast<int64_t>(max_z) - min_z + 1);
    const size_t x_batch_count = static_cast<size_t>(
        (width + static_cast<uint64_t>(batch_size) - 1) / batch_size);
    const size_t z_batch_count = static_cast<size_t>(
        (length + static_cast<uint64_t>(batch_size) - 1) / batch_size);
    try {
        batches->clear();
        batches->reserve(x_batch_count * z_batch_count);
        for (size_t x_index = 0; x_index < x_batch_count; ++x_index) {
            const int64_t x = static_cast<int64_t>(min_x) +
                static_cast<int64_t>(x_index) * batch_size;
            const bool reverse_z = (x_index & 1U) != 0;
            for (size_t scan_z_index = 0; scan_z_index < z_batch_count; ++scan_z_index) {
                const size_t z_index = reverse_z
                    ? z_batch_count - scan_z_index - 1 : scan_z_index;
                const int64_t z = static_cast<int64_t>(min_z) +
                    static_cast<int64_t>(z_index) * batch_size;
                ScanBatch batch;
                batch.min_x = static_cast<int32_t>(x);
                batch.max_x = static_cast<int32_t>(std::min<int64_t>(
                    x + batch_size - 1, max_x));
                batch.min_z = static_cast<int32_t>(z);
                batch.max_z = static_cast<int32_t>(std::min<int64_t>(
                    z + batch_size - 1, max_z));
                batches->push_back(batch);
            }
        }
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory for export scan regions";
        return false;
    }
    if (batches->empty()) {
        if (error) *error = "export selection is empty";
        return false;
    }
    return true;
}

bool BuildExportRuntime::isStableWorldContext(const std::string& world_id,
                                               int32_t dimension_id) {
    constexpr char prefix[] = "stable:v1:";
    constexpr size_t digest_size = 64;
    if (world_id.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
    const size_t separator = world_id.rfind('|');
    if (separator != sizeof(prefix) - 1 + digest_size ||
        separator + 1 >= world_id.size()) {
        return false;
    }
    for (size_t index = sizeof(prefix) - 1; index < separator; ++index) {
        const unsigned char character = static_cast<unsigned char>(world_id[index]);
        if (!std::isxdigit(character) || std::isupper(character)) return false;
    }
    size_t cursor = separator + 1;
    bool negative = false;
    if (world_id[cursor] == '-') {
        negative = true;
        if (++cursor == world_id.size()) return false;
    }
    int64_t parsed = 0;
    for (; cursor < world_id.size(); ++cursor) {
        const char character = world_id[cursor];
        if (character < '0' || character > '9') return false;
        parsed = parsed * 10 + (character - '0');
        const int64_t limit = negative
            ? -static_cast<int64_t>(std::numeric_limits<int32_t>::min())
            : std::numeric_limits<int32_t>::max();
        if (parsed > limit) return false;
    }
    if (negative) parsed = -parsed;
    return parsed == dimension_id;
}

void BuildExportRuntime::releaseScanMemoryLocked() {
    resetContainerCaptureLocked();
    std::vector<ScanBatch>().swap(batches_);
    std::vector<std::string>().swap(palette_);
    decltype(palette_ids_)().swap(palette_ids_);
    decltype(block_state_ids_)().swap(block_state_ids_);
    decltype(raw_state_json_)().swap(raw_state_json_);
    decltype(raw_client_identifiers_)().swap(raw_client_identifiers_);
    decltype(raw_client_aux_)().swap(raw_client_aux_);
    decltype(raw_state_attempted_)().swap(raw_state_attempted_);
    std::vector<uint16_t>().swap(block_indices_);
    std::vector<SchematicRawBlock>().swap(raw_blocks_);
    decltype(raw_block_lookup_)().swap(raw_block_lookup_);
    std::vector<CommandBlockRecord>().swap(command_blocks_);
    std::vector<uint16_t>().swap(journal_scratch_);
    world_reader_.reset();
    block_state_source_generation_ = world_reader_.sourceGeneration();
}

bool BuildExportRuntime::start(BuildExportStartRequest request, std::string* error) {
    const auto reject = [&](std::string reason) {
        if (!isActiveState(state_.load(std::memory_order_acquire))) setStatus(reason);
        if (error) *error = std::move(reason);
        return false;
    };
    BuildExportPathResolution request_paths;
    if (!resolveBuildExportPath(request.output_path, &request_paths)) {
        return reject("output path must be a filename without a custom extension");
    }
    request.output_path = std::move(request_paths.output_stem);
    if (request.simulation_chunk_range < kMinimumSimulationChunkRange ||
        request.simulation_chunk_range > kMaximumSimulationChunkRange) {
        return reject("simulation chunk range must be between 4 and 8");
    }
    if (!isValidBuildExportTravelMode(static_cast<int32_t>(request.travel_mode))) {
        return reject("invalid export travel mode");
    }
    if (!isStableWorldContext(request.world_id, request.dimension_id)) {
        return reject("a stable world identity is required for resumable export");
    }
    const uintptr_t starting_dimension_token =
        GetCachedDimensionTokenForWorld(request.world_id);
    if (starting_dimension_token == 0) {
        return reject("the requested world context is no longer current");
    }

    const int64_t min_x = std::min<int64_t>(request.first_x, request.second_x);
    const int64_t min_y = std::min<int64_t>(request.first_y, request.second_y);
    const int64_t min_z = std::min<int64_t>(request.first_z, request.second_z);
    const int64_t max_x = std::max<int64_t>(request.first_x, request.second_x);
    const int64_t max_y = std::max<int64_t>(request.first_y, request.second_y);
    const int64_t max_z = std::max<int64_t>(request.first_z, request.second_z);
    const uint64_t width = static_cast<uint64_t>(max_x - min_x + 1);
    const uint64_t height = static_cast<uint64_t>(max_y - min_y + 1);
    const uint64_t length = static_cast<uint64_t>(max_z - min_z + 1);
    if (width > static_cast<uint64_t>(std::numeric_limits<int16_t>::max()) ||
        height > static_cast<uint64_t>(std::numeric_limits<int16_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<int16_t>::max())) {
        return reject("each export dimension must be between 1 and 32767");
    }
    if (width != 0 && height > std::numeric_limits<uint64_t>::max() / width) {
        return reject("export volume overflows");
    }
    const uint64_t layer = width * height;
    if (length != 0 && layer > std::numeric_limits<uint64_t>::max() / length) {
        return reject("export volume overflows");
    }
    const uint64_t volume = layer * length;
    if (volume > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        volume > kMaximumBlockCount) {
        return reject("export selection exceeds the 16,777,216 block safety limit");
    }
    const int32_t batch_size = request.simulation_chunk_range *
        static_cast<int32_t>(kWorldChunkSize);

    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    const BuildExportState current = state_.load(std::memory_order_acquire);
    if (isActiveState(current)) {
        return reject("a build export is already active");
    }
    if (writer_thread_.joinable()) {
        if (writer_running_.load(std::memory_order_acquire)) {
            return reject("the previous building writer is still stopping");
        }
        lock.unlock();
        writer_thread_.join();
        lock.lock();
    }
    const bool checkpoint_exists =
        BuildExportCheckpoint::hasArtifacts(request.output_path);
    if (checkpoint_exists && !request.replace_checkpoint) {
        return reject("an export checkpoint already exists; resume or delete it first");
    }

    std::vector<uint16_t> indices;
    try {
        indices.assign(static_cast<size_t>(volume), 0);
    } catch (const std::bad_alloc&) {
        return reject("not enough memory for the selected export volume");
    }

    std::vector<ScanBatch> batches;
    std::string batch_error;
    if (!createBatches(static_cast<int32_t>(min_x), static_cast<int32_t>(max_x),
                        static_cast<int32_t>(min_z), static_cast<int32_t>(max_z),
                        static_cast<int32_t>(height), batch_size,
                        &batches, &batch_error)) {
        return reject(std::move(batch_error));
    }
    std::string initial_display_name;
    std::vector<std::string> initial_palette;
    std::unordered_map<std::string, uint32_t> initial_palette_ids;
    std::unordered_map<CachedBlockStateKey, uint32_t,
                       CachedBlockStateKeyHash> initial_block_state_ids;
    try {
        initial_display_name = displayNameForPath(request.output_path);
        initial_palette.reserve(256);
        initial_palette.push_back("minecraft:air");
        initial_palette_ids.reserve(256);
        initial_palette_ids.emplace(initial_palette.front(), 0);
        initial_block_state_ids.reserve(512);
    } catch (const std::bad_alloc&) {
        return reject("not enough memory to initialize export state");
    }
    if (checkpoint_exists) {
        std::string discard_error;
        if (!BuildExportCheckpoint::discard(request.output_path, &discard_error)) {
            return reject(std::move(discard_error));
        }
    }

    generation_.fetch_add(1, std::memory_order_acq_rel);
    publication_committed_.store(false, std::memory_order_release);
    output_path_ = std::move(request.output_path);
    checkpoint_path_ = output_path_;
    published_output_path_.clear();
    display_name_ = std::move(initial_display_name);
    min_x_ = static_cast<int32_t>(min_x);
    min_y_ = static_cast<int32_t>(min_y);
    min_z_ = static_cast<int32_t>(min_z);
    max_x_ = static_cast<int32_t>(max_x);
    max_y_ = static_cast<int32_t>(max_y);
    max_z_ = static_cast<int32_t>(max_z);
    width_ = static_cast<int32_t>(width);
    height_ = static_cast<int32_t>(height);
    length_ = static_cast<int32_t>(length);
    world_id_ = std::move(request.world_id);
    dimension_id_ = request.dimension_id;
    dimension_token_ = starting_dimension_token;
    missing_player_ticks_ = 0;
    // Semi-automatic starts without a per-region grant. The grant is set by
    // requestNextRegionTeleport(), or when the player reaches the region on
    // foot, and is cleared before the next region begins.
    travel_mode_ = request.travel_mode;
    export_container_items_ = request.export_container_items;
    allow_teleport_ = travel_mode_ == BuildExportTravelMode::Automatic;
    teleport_permission_unverified_ = false;
    // A new export must never inherit a failed/successful probe from an older
    // task.  Its first batch creates a fresh server TP verification.
    CancelBuildExportTeleport();
    teleport_hop_limit_ = 0;
    hop_target_valid_ = false;
    hop_target_final_ = false;
    batch_size_ = batch_size;
    batches_ = std::move(batches);
    current_batch_ = 0;
    world_reader_.reset();
    block_state_source_generation_ = world_reader_.sourceGeneration();
    palette_ = std::move(initial_palette);
    palette_ids_ = std::move(initial_palette_ids);
    block_state_ids_ = std::move(initial_block_state_ids);
    raw_state_json_.clear();
    raw_client_identifiers_.clear();
    raw_client_aux_.clear();
    raw_state_attempted_.clear();
    block_indices_ = std::move(indices);
    raw_blocks_.clear();
    raw_block_lookup_.clear();
    resetContainerCaptureLocked();
    command_blocks_.clear();
    total_blocks_.store(volume, std::memory_order_release);
    processed_blocks_.store(0, std::memory_order_release);
    exported_blocks_ = 0;
    journal_entries_ = 0;
    committed_journal_entries_ = 0;
    committed_batch_ = 0;
    committed_batch_cursor_ = 0;
    committed_palette_size_ = palette_.size();
    committed_exported_blocks_ = 0;
    committed_raw_size_ = 0;
    checkpoint_initialized_ = false;
    checkpoint_commit_in_progress_ = false;
    checkpoint_data_dirty_ = false;
    preserve_cursor_on_prepare_ = false;
    last_checkpoint_at_ = std::chrono::steady_clock::now();
    std::string checkpoint_error;
    if (!BuildExportCheckpoint::createJournal(checkpoint_path_, &checkpoint_error)) {
        releaseScanMemoryLocked();
        return reject(std::move(checkpoint_error));
    }
    checkpoint_initialized_ = true;
    if (!commitCheckpointLocked(true, &checkpoint_error)) {
        checkpoint_initialized_ = false;
        BuildExportCheckpoint::discard(checkpoint_path_, nullptr);
        releaseScanMemoryLocked();
        return reject(std::move(checkpoint_error));
    }
    state_.store(BuildExportState::Preparing, std::memory_order_release);
    setStatus(teleport_permission_unverified_
        ? "Automatic teleport disabled: server TP could not be verified; "
          "preparing export in manual navigation mode"
        : "Preparing resumable export selection");
    if (error) error->clear();
    return true;
}

bool BuildExportRuntime::hasCheckpoint(const std::string& output_path) const {
    // Do not advertise arbitrary sidecar remnants as resumable.  A journal
    // can be present after a killed process before its first atomic snapshot;
    // only a checkpoint that parses completely can be continued safely.
    BuildExportPathResolution paths;
    if (!resolveBuildExportPath(output_path, &paths)) return false;
    for (const std::string& checkpoint_path : paths.checkpoint_keys) {
        if (BuildExportCheckpoint::load(checkpoint_path, nullptr).has_value()) return true;
    }
    return false;
}

bool BuildExportRuntime::discardCheckpoint(const std::string& output_path,
                                           std::string* error) {
    const auto reject = [&](std::string reason) {
        if (!isActiveState(state_.load(std::memory_order_acquire))) setStatus(reason);
        if (error) *error = std::move(reason);
        return false;
    };
    BuildExportPathResolution paths;
    if (!resolveBuildExportPath(output_path, &paths)) {
        return reject("output path must be a filename without a custom extension");
    }
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (isActiveState(state_.load(std::memory_order_acquire))) {
        return reject("cannot delete a checkpoint while an export is active");
    }
    if (writer_running_.load(std::memory_order_acquire)) {
        return reject("the building writer is still stopping");
    }
    std::string checkpoint_path = paths.output_stem;
    for (const std::string& candidate : paths.checkpoint_keys) {
        if (BuildExportCheckpoint::hasArtifacts(candidate)) {
            checkpoint_path = candidate;
            break;
        }
    }
    std::string discard_error;
    if (!BuildExportCheckpoint::discard(checkpoint_path, &discard_error)) {
        return reject(std::move(discard_error));
    }
    if (checkpoint_path_ == checkpoint_path) checkpoint_initialized_ = false;
    if (error) error->clear();
    return true;
}

bool BuildExportRuntime::appendJournalLocked(const std::vector<uint16_t>& values,
                                             std::string* error) {
    if (values.empty()) return true;
    const uint64_t total = total_blocks_.load(std::memory_order_acquire);
    if (!checkpoint_initialized_ || values.size() > total ||
        journal_entries_ > total - values.size()) {
        if (error) *error = "export journal cursor is invalid";
        return false;
    }
    if (!BuildExportCheckpoint::appendJournal(
            checkpoint_path_, values.data(), values.size(), error)) {
        return false;
    }
    journal_entries_ += values.size();
    return true;
}

bool BuildExportRuntime::commitCheckpointLocked(bool force, std::string* error) {
    if (!checkpoint_initialized_) {
        if (error) *error = "export checkpoint is not initialized";
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force) {
        if (journal_entries_ == committed_journal_entries_ &&
            !checkpoint_data_dirty_) return true;
        if (last_checkpoint_at_.time_since_epoch().count() != 0 &&
            now >= last_checkpoint_at_ && now - last_checkpoint_at_ < kCheckpointTimeInterval) {
            return true;
        }
    }
    if (checkpoint_commit_in_progress_) {
        if (error) *error = "recursive export checkpoint commit";
        return false;
    }
    const uint64_t expected_entries = current_batch_ < batches_.size()
        ? batches_[current_batch_].processed_start + batches_[current_batch_].cursor
        : total_blocks_.load(std::memory_order_acquire);
    if (journal_entries_ != expected_entries || exported_blocks_ > journal_entries_) {
        if (error) *error = "export checkpoint cursor is not journal-consistent";
        return false;
    }
    checkpoint_commit_in_progress_ = true;
    BuildExportCheckpointSnapshot snapshot;
    try {
        snapshot.output_path = checkpoint_path_;
        snapshot.world_id = world_id_;
        snapshot.dimension_id = dimension_id_;
        snapshot.min_x = min_x_;
        snapshot.min_y = min_y_;
        snapshot.min_z = min_z_;
        snapshot.max_x = max_x_;
        snapshot.max_y = max_y_;
        snapshot.max_z = max_z_;
        snapshot.batch_size = batch_size_;
        snapshot.traversal_version = kBuildExportTraversalVersion;
        snapshot.total_blocks = total_blocks_.load(std::memory_order_acquire);
        snapshot.journal_entries = journal_entries_;
        snapshot.exported_blocks = exported_blocks_;
        snapshot.current_batch = current_batch_;
        snapshot.batch_cursor = current_batch_ < batches_.size()
            ? batches_[current_batch_].cursor : 0;
        snapshot.writing = state_.load(std::memory_order_acquire) == BuildExportState::Writing;
        snapshot.export_container_items = export_container_items_;
        snapshot.container_capture_pending = container_capture_pending_;
        snapshot.container_target_count = container_capture_pending_
            ? static_cast<uint64_t>(container_targets_.size()) : 0U;
        snapshot.container_cursor = container_capture_pending_ ? container_cursor_ : 0U;
    } catch (const std::bad_alloc&) {
        checkpoint_commit_in_progress_ = false;
        if (error) *error = "not enough memory to save export checkpoint";
        return false;
    } catch (const std::exception&) {
        checkpoint_commit_in_progress_ = false;
        if (error) *error = "cannot construct export checkpoint snapshot";
        return false;
    } catch (...) {
        checkpoint_commit_in_progress_ = false;
        if (error) *error = "unknown export checkpoint snapshot failure";
        return false;
    }
    std::string checkpoint_error;
    bool ok = false;
    try {
        ok = BuildExportCheckpoint::syncJournal(checkpoint_path_, &checkpoint_error) &&
            BuildExportCheckpoint::saveAtomically(
                checkpoint_path_, snapshot, palette_, command_blocks_, raw_blocks_,
                &checkpoint_error);
    } catch (const std::bad_alloc&) {
        checkpoint_error = "not enough memory to write export checkpoint";
    } catch (const std::exception&) {
        checkpoint_error = "export checkpoint writer threw an exception";
    } catch (...) {
        checkpoint_error = "export checkpoint writer threw an unknown exception";
    }
    checkpoint_commit_in_progress_ = false;
    if (!ok) {
        if (error) *error = checkpoint_error.empty()
            ? "cannot save export checkpoint" : std::move(checkpoint_error);
        return false;
    }
    committed_journal_entries_ = journal_entries_;
    committed_batch_ = current_batch_;
    committed_batch_cursor_ = snapshot.batch_cursor;
    committed_palette_size_ = palette_.size();
    committed_exported_blocks_ = exported_blocks_;
    committed_raw_size_ = raw_blocks_.size();
    checkpoint_data_dirty_ = false;
    last_checkpoint_at_ = now;
    return true;
}

bool BuildExportRuntime::replayJournalLocked(uint64_t entry_count, std::string* error) {
    if (!BuildExportCheckpoint::truncateJournal(checkpoint_path_, entry_count, error)) return false;
    std::ifstream stream(BuildExportCheckpoint::journalPath(checkpoint_path_), std::ios::binary);
    if (!stream) {
        if (error) *error = "cannot open export block journal for replay";
        return false;
    }
    std::array<uint16_t, 8192> values{};
    uint64_t remaining = entry_count;
    uint64_t replayed = 0;
    uint64_t non_air = 0;
    const uint64_t height = static_cast<uint64_t>(height_);
    const uint64_t width = static_cast<uint64_t>(width_);
    const uint64_t layer_stride = width * static_cast<uint64_t>(length_);
    for (const ScanBatch& batch : batches_) {
        if (remaining == 0) break;
        const uint64_t volume = batchVolume(batch, height_);
        const uint64_t batch_entries = std::min(remaining, volume);
        const uint64_t batch_length = static_cast<uint64_t>(
            static_cast<int64_t>(batch.max_z) - batch.min_z + 1);
        const uint64_t batch_x_offset = static_cast<uint64_t>(
            static_cast<int64_t>(batch.min_x) - min_x_);
        const uint64_t batch_z_offset = static_cast<uint64_t>(
            static_cast<int64_t>(batch.min_z) - min_z_);
        uint64_t cursor = 0;
        uint64_t local_y = 0;
        uint64_t local_z = 0;
        uint64_t local_x = 0;
        while (cursor < batch_entries) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(
                values.size(), batch_entries - cursor));
            stream.read(reinterpret_cast<char*>(values.data()),
                        static_cast<std::streamsize>(count * sizeof(uint16_t)));
            if (stream.gcount() != static_cast<std::streamsize>(count * sizeof(uint16_t))) {
                if (error) *error = "export block journal ended during replay";
                return false;
            }
            for (size_t index_in_chunk = 0; index_in_chunk < count; ++index_in_chunk) {
                const uint16_t palette_id = values[index_in_chunk];
                if (palette_id >= palette_.size()) {
                    if (error) *error = "export block journal references an invalid palette id";
                    return false;
                }
                const uint64_t global_index = batch_x_offset + local_x +
                    (batch_z_offset + local_z) * width + local_y * layer_stride;
                if (global_index >= block_indices_.size()) {
                    if (error) *error = "export block journal produced an invalid index";
                    return false;
                }
                block_indices_[static_cast<size_t>(global_index)] = palette_id;
                if (palette_id != 0) ++non_air;
                ++cursor;
                ++replayed;
                if (++local_y == height) {
                    local_y = 0;
                    if (++local_z == batch_length) {
                        local_z = 0;
                        ++local_x;
                    }
                }
            }
        }
        remaining -= batch_entries;
    }
    if (remaining != 0 || replayed != entry_count || non_air != exported_blocks_) {
        if (error) *error = "export checkpoint counters do not match the block journal";
        return false;
    }
    return true;
}

bool BuildExportRuntime::restoreSnapshotLocked(
        BuildExportCheckpointSnapshot snapshot, std::string output_path,
        std::string checkpoint_path, BuildExportTravelMode travel_mode,
        std::string* error) {
    const bool restored_container_pending = snapshot.container_capture_pending;
    const uint64_t restored_container_target_count = snapshot.container_target_count;
    const uint64_t restored_container_cursor = snapshot.container_cursor;
    std::vector<ScanBatch> batches;
    if (!isSupportedBatchSize(snapshot.batch_size) ||
        snapshot.traversal_version != kBuildExportTraversalVersion ||
        !createBatches(snapshot.min_x, snapshot.max_x, snapshot.min_z, snapshot.max_z,
                       static_cast<int32_t>(static_cast<int64_t>(snapshot.max_y) -
                                            snapshot.min_y + 1), snapshot.batch_size,
                        &batches, error)) {
        if (error && error->empty()) *error = "export checkpoint uses an unsupported scan plan";
        return false;
    }
    if (snapshot.current_batch > batches.size()) {
        if (error) *error = "export checkpoint batch cursor is invalid";
        return false;
    }
    uint64_t expected_entries = 0;
    for (size_t index = 0; index < static_cast<size_t>(snapshot.current_batch); ++index) {
        expected_entries += batchVolume(batches[index],
            static_cast<int32_t>(static_cast<int64_t>(snapshot.max_y) - snapshot.min_y + 1));
    }
    if (snapshot.current_batch < batches.size()) {
        if (snapshot.batch_cursor > batchVolume(
                batches[static_cast<size_t>(snapshot.current_batch)],
                static_cast<int32_t>(static_cast<int64_t>(snapshot.max_y) - snapshot.min_y + 1))) {
            if (error) *error = "export checkpoint batch offset is invalid";
            return false;
        }
        expected_entries += snapshot.batch_cursor;
    } else if (snapshot.batch_cursor != 0) {
        if (error) *error = "export checkpoint has a trailing batch offset";
        return false;
    }
    if (expected_entries != snapshot.journal_entries) {
        if (error) *error = "export checkpoint progress does not match its scan plan";
        return false;
    }
    if (restored_container_pending &&
        (snapshot.current_batch >= batches.size() ||
         snapshot.batch_cursor != batchVolume(
             batches[static_cast<size_t>(snapshot.current_batch)],
             static_cast<int32_t>(static_cast<int64_t>(snapshot.max_y) -
                                  snapshot.min_y + 1)))) {
        if (error) *error = "export checkpoint container phase is not at a batch boundary";
        return false;
    }
    {
        std::unordered_set<std::string_view> unique_palette;
        try {
            unique_palette.reserve(snapshot.palette.size());
            for (const std::string& state : snapshot.palette) {
                if (!unique_palette.emplace(state).second) {
                    if (error) *error = "export checkpoint palette contains duplicates";
                    return false;
                }
            }
        } catch (const std::bad_alloc&) {
            if (error) *error = "not enough memory to validate export checkpoint palette";
            return false;
        }
    }

    std::vector<uint16_t> indices;
    try {
        indices.assign(static_cast<size_t>(snapshot.total_blocks), 0);
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory to restore export checkpoint";
        return false;
    }

    size_t restored_batch = static_cast<size_t>(snapshot.current_batch);
    uint64_t restored_cursor = snapshot.batch_cursor;
    while (!restored_container_pending && restored_batch < batches.size() &&
           restored_cursor == batchVolume(
               batches[restored_batch],
               static_cast<int32_t>(static_cast<int64_t>(snapshot.max_y) -
                                    snapshot.min_y + 1))) {
        ++restored_batch;
        restored_cursor = 0;
    }

    output_path_ = std::move(output_path);
    checkpoint_path_ = std::move(checkpoint_path);
    published_output_path_.clear();
    display_name_ = displayNameForPath(output_path_);
    min_x_ = snapshot.min_x;
    min_y_ = snapshot.min_y;
    min_z_ = snapshot.min_z;
    max_x_ = snapshot.max_x;
    max_y_ = snapshot.max_y;
    max_z_ = snapshot.max_z;
    width_ = static_cast<int32_t>(static_cast<int64_t>(max_x_) - min_x_ + 1);
    height_ = static_cast<int32_t>(static_cast<int64_t>(max_y_) - min_y_ + 1);
    length_ = static_cast<int32_t>(static_cast<int64_t>(max_z_) - min_z_ + 1);
    world_id_ = snapshot.world_id;
    dimension_id_ = snapshot.dimension_id;
    dimension_token_ = 0;
    missing_player_ticks_ = 0;
    travel_mode_ = travel_mode;
    export_container_items_ = snapshot.export_container_items;
    allow_teleport_ = travel_mode_ == BuildExportTravelMode::Automatic;
    teleport_permission_unverified_ = false;
    teleport_hop_limit_ = 0;
    hop_target_valid_ = false;
    hop_target_final_ = false;
    batch_size_ = snapshot.batch_size;
    batches_ = std::move(batches);
    current_batch_ = restored_batch;
    if (current_batch_ < batches_.size()) {
        batches_[current_batch_].cursor = restored_cursor;
        batches_[current_batch_].processed_start =
            snapshot.journal_entries - restored_cursor;
        batches_[current_batch_].exported_start = snapshot.exported_blocks;
        batches_[current_batch_].palette_start = snapshot.palette.size();
        batches_[current_batch_].raw_start = snapshot.raw_blocks.size();
    }
    world_reader_.reset();
    block_state_source_generation_ = world_reader_.sourceGeneration();
    palette_ = std::move(snapshot.palette);
    palette_ids_.clear();
    try {
        palette_ids_.reserve(palette_.size());
        for (size_t index = 0; index < palette_.size(); ++index) {
            palette_ids_.emplace(palette_[index], static_cast<uint32_t>(index));
        }
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory to restore export palette index";
        return false;
    }
    block_state_ids_.clear();
    block_state_ids_.reserve(512);
    raw_state_json_.clear();
    raw_client_identifiers_.clear();
    raw_client_aux_.clear();
    raw_state_attempted_.clear();
    block_indices_ = std::move(indices);
    raw_blocks_ = std::move(snapshot.raw_blocks);
    raw_block_lookup_.clear();
    try {
        raw_block_lookup_.reserve(raw_blocks_.size());
        const uint64_t raw_width = static_cast<uint64_t>(width_);
        const uint64_t raw_layer = raw_width * static_cast<uint64_t>(length_);
        for (size_t raw_index = 0; raw_index < raw_blocks_.size(); ++raw_index) {
            const SchematicRawBlock& raw = raw_blocks_[raw_index];
            const uint64_t key = static_cast<uint64_t>(raw.x) +
                static_cast<uint64_t>(raw.z) * raw_width +
                static_cast<uint64_t>(raw.y) * raw_layer;
            raw_block_lookup_[key] = raw_index;
        }
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory to restore raw-state index";
        return false;
    }
    resetContainerCaptureLocked();
    if (restored_container_pending) {
        container_capture_pending_ = true;
        container_cursor_ = restored_container_cursor;
        std::string target_error;
        if (!rebuildContainerTargetsLocked(&target_error) ||
            container_targets_.size() != restored_container_target_count ||
            container_cursor_ > container_targets_.size()) {
            if (error) {
                *error = target_error.empty()
                    ? "export checkpoint container target list is inconsistent"
                    : std::move(target_error);
            }
            resetContainerCaptureLocked();
            return false;
        }
        container_attempt_ = 1;
        container_phase_ = ContainerCapturePhase::Navigate;
    }
    total_blocks_.store(snapshot.total_blocks, std::memory_order_release);
    processed_blocks_.store(snapshot.journal_entries, std::memory_order_release);
    exported_blocks_ = snapshot.exported_blocks;
    journal_entries_ = snapshot.journal_entries;
    committed_journal_entries_ = snapshot.journal_entries;
    committed_batch_ = current_batch_;
    committed_batch_cursor_ = restored_cursor;
    committed_palette_size_ = palette_.size();
    committed_exported_blocks_ = exported_blocks_;
    committed_raw_size_ = raw_blocks_.size();
    checkpoint_initialized_ = true;
    checkpoint_commit_in_progress_ = false;
    checkpoint_data_dirty_ = false;
    preserve_cursor_on_prepare_ = !snapshot.writing;
    last_checkpoint_at_ = std::chrono::steady_clock::now();
    command_blocks_ = std::move(snapshot.command_blocks);
    if (!replayJournalLocked(snapshot.journal_entries, error)) return false;
    if (!allow_teleport_) CancelBuildExportTeleport();
    return true;
}

bool BuildExportRuntime::resume(const std::string& output_path,
                                const std::string& world_id,
                                int32_t dimension_id,
                                BuildExportTravelMode travel_mode,
                                std::string* error) {
    const auto reject = [&](std::string reason) {
        if (!isActiveState(state_.load(std::memory_order_acquire))) setStatus(reason);
        if (error) *error = std::move(reason);
        return false;
    };
    BuildExportPathResolution paths;
    if (!resolveBuildExportPath(output_path, &paths)) {
        return reject("output path must be a filename without a custom extension");
    }
    if (!isStableWorldContext(world_id, dimension_id)) {
        return reject("a stable world identity is required to resume export");
    }
    if (!isValidBuildExportTravelMode(static_cast<int32_t>(travel_mode))) {
        return reject("invalid export travel mode");
    }
    std::string checkpoint_path;
    std::string load_error;
    bool error_from_artifact = false;
    std::optional<BuildExportCheckpointSnapshot> snapshot;
    for (const std::string& candidate : paths.checkpoint_keys) {
        std::string candidate_error;
        snapshot = BuildExportCheckpoint::load(candidate, &candidate_error);
        if (snapshot) {
            checkpoint_path = candidate;
            break;
        }
        const bool has_artifacts = BuildExportCheckpoint::hasArtifacts(candidate);
        if ((has_artifacts && !error_from_artifact) || load_error.empty()) {
            load_error = std::move(candidate_error);
            error_from_artifact = has_artifacts;
        }
    }
    if (!snapshot) return reject(std::move(load_error));
    if (snapshot->world_id != world_id || snapshot->dimension_id != dimension_id) {
        return reject("export checkpoint belongs to a different world or dimension");
    }
    const uintptr_t restored_dimension_token = GetCachedDimensionTokenForWorld(world_id);
    if (restored_dimension_token == 0) {
        return reject("the checkpoint world context is no longer current");
    }

    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (isActiveState(state_.load(std::memory_order_acquire))) {
        return reject("a build export is already active");
    }
    if (writer_thread_.joinable()) {
        if (writer_running_.load(std::memory_order_acquire)) {
            return reject("the previous building writer is still stopping");
        }
        lock.unlock();
        writer_thread_.join();
        lock.lock();
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
    publication_committed_.store(false, std::memory_order_release);
    // The restored batch must not inherit a stale result from a task that was
    // cancelled before this resume call.
    CancelBuildExportTeleport();
    std::string restore_error;
    bool restored = false;
    try {
        restored = restoreSnapshotLocked(std::move(*snapshot),
                                         std::move(paths.output_stem),
                                         std::move(checkpoint_path), travel_mode,
                                         &restore_error);
    } catch (const std::bad_alloc&) {
        restore_error = "not enough memory to restore export checkpoint";
    } catch (const std::exception&) {
        restore_error = "export checkpoint recovery threw an exception";
    } catch (...) {
        restore_error = "export checkpoint recovery threw an unknown exception";
    }
    if (!restored) {
        releaseScanMemoryLocked();
        checkpoint_initialized_ = false;
        state_.store(BuildExportState::Failed, std::memory_order_release);
        setStatus((restore_error.empty() ? "Cannot restore export checkpoint" : restore_error) +
                  "; export checkpoint retained");
        return reject(restore_error.empty() ? "cannot restore export checkpoint" :
            std::move(restore_error));
    }
    dimension_token_ = restored_dimension_token;
    if (journal_entries_ == total_blocks_.load(std::memory_order_acquire) &&
        !container_capture_pending_) {
        current_batch_ = batches_.size();
        preserve_cursor_on_prepare_ = false;
        beginWriteLocked();
    } else {
        state_.store(BuildExportState::Preparing, std::memory_order_release);
        const std::string resumed = "Resumed export checkpoint at " +
            std::to_string(journal_entries_) + "/" +
            std::to_string(total_blocks_.load(std::memory_order_acquire));
        setStatus(teleport_permission_unverified_
            ? "Automatic teleport disabled: server TP could not be verified; " + resumed
            : resumed);
    }
    if (error) error->clear();
    return isActiveState(state_.load(std::memory_order_acquire));
}

bool BuildExportRuntime::resume(const std::string& output_path,
                                const std::string& world_id,
                                int32_t dimension_id, bool allow_teleport,
                                bool teleport_requested,
                                std::string* error) {
    const bool automatic = allow_teleport || teleport_requested;
    return resume(output_path, world_id, dimension_id,
                  automatic ? BuildExportTravelMode::Automatic
                            : BuildExportTravelMode::Disabled,
                  error);
}

void BuildExportRuntime::updateCachedPlayerPosition() {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    const bool valid = NativeWorldAccess::getLocalPlayerBlockPosition(&x, &y, &z);
    {
        std::lock_guard<std::mutex> lock(position_mutex_);
        cached_position_valid_ = valid;
        if (valid) {
            cached_x_ = x;
            cached_y_ = y;
            cached_z_ = z;
            cached_position_at_ = std::chrono::steady_clock::now();
        }
        ++position_refresh_generation_;
    }
    position_refresh_condition_.notify_all();
}

void BuildExportRuntime::refreshPlayerPositionCache() {
    // UI position capture must work before an export is started, but an idle
    // form must not enter Python/ModSDK every few ticks while the IME is
    // editing coordinates. Refresh only in response to an awaiting JNI
    // capture request; active exports update the cache through onGameTick().
    bool refresh_position = false;
    {
        std::lock_guard<std::mutex> lock(position_mutex_);
        if (position_refresh_requested_) {
            position_refresh_requested_ = false;
            refresh_position = true;
        }
    }
    if (refresh_position) updateCachedPlayerPosition();
}

bool BuildExportRuntime::awaitPlayerBlockPosition(int32_t* x, int32_t* y, int32_t* z,
                                                   std::chrono::milliseconds timeout) {
    if (!x || !y || !z) return false;

    const auto deadline = std::chrono::steady_clock::now() +
        std::max(timeout, std::chrono::milliseconds::zero());
    std::unique_lock<std::mutex> lock(position_mutex_);
    const auto copy_if_fresh = [&]() {
        const auto now = std::chrono::steady_clock::now();
        if (!cached_position_valid_ || cached_position_at_.time_since_epoch().count() == 0 ||
            now < cached_position_at_ || now - cached_position_at_ > std::chrono::seconds(2)) {
            return false;
        }
        *x = cached_x_;
        *y = cached_y_;
        *z = cached_z_;
        return true;
    };
    if (copy_if_fresh()) return true;

    uint64_t observed_generation = position_refresh_generation_;
    while (std::chrono::steady_clock::now() < deadline) {
        // refreshPlayerPositionCache() is serviced exclusively from the local
        // player's game tick.  The request flag avoids querying Python/world
        // state from this JNI caller's worker thread.
        position_refresh_requested_ = true;
        if (!position_refresh_condition_.wait_until(lock, deadline, [&] {
                return position_refresh_generation_ != observed_generation;
            })) {
            break;
        }
        observed_generation = position_refresh_generation_;
        if (copy_if_fresh()) return true;
    }
    return false;
}

void BuildExportRuntime::onGameTick() {
    BuildExportState current = state_.load(std::memory_order_acquire);
    if (!isActiveState(current) || current == BuildExportState::Writing) {
        // Explicit corner-capture requests are serviced on this game thread.
        // With no pending request this is a no-op, so an idle form never polls
        // Python/ModSDK while the user is typing coordinates.
        refreshPlayerPositionCache();
        return;
    }
    // The position query has a Python primary path and a native render-camera
    // fallback. Do not evaluate the primary path on every scanning tick.
    if (current != BuildExportState::Scanning || !allow_teleport_) {
        updateCachedPlayerPosition();
    }

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    current = state_.load(std::memory_order_acquire);
    if (!isActiveState(current) || current == BuildExportState::Writing) return;

    const uintptr_t current_dimension = NativeWorldAccess::dimensionToken();
    if (current_dimension == 0) {
        if (++missing_player_ticks_ >= kMaximumMissingPlayerTicks) {
            failLocked("Local player or dimension is unavailable");
        }
        return;
    }
    missing_player_ticks_ = 0;
    if (dimension_token_ == 0) dimension_token_ = current_dimension;
    else if (dimension_token_ != current_dimension) {
        failLocked("Dimension changed while exporting");
        return;
    }

    if (current == BuildExportState::Preparing) {
        const bool reset_progress = !preserve_cursor_on_prepare_;
        preserve_cursor_on_prepare_ = false;
        beginBatchLoadLocked(reset_progress);
        return;
    }
    if (current_batch_ >= batches_.size()) {
        beginWriteLocked();
        return;
    }

    if (current == BuildExportState::CapturingContainers) {
        try {
            tickContainerCaptureLocked();
        } catch (const std::bad_alloc&) {
            failLocked("Container capture ran out of memory");
        } catch (const std::exception&) {
            failLocked("Container capture threw an exception");
        } catch (...) {
            failLocked("Container capture threw an unknown exception");
        }
        return;
    }

    ScanBatch& batch = batches_[current_batch_];
    if (current == BuildExportState::WaitingForPlayer) {
        // A manual export deliberately does not ask the shared teleport slot
        // to move the player. Once the player is close enough to the batch,
        // resume through the normal loading path so two readable samples still
        // gate the scan.
        if (batch.wait_ticks > 0) {
            --batch.wait_ticks;
            return;
        }
        if (!playerNearCurrentBatchLocked()) {
            setWaitingForPlayerStatusLocked();
            return;
        }
        if (travel_mode_ == BuildExportTravelMode::SemiAutomatic) {
            // Walking into a semi-automatic region is also a valid grant. This
            // keeps the mode usable when a server rejects teleport packets and
            // avoids making the player press a button after doing the walk.
            allow_teleport_ = true;
        }
        if (batch.manual_retry_wait) {
            // The retry cooldown already gave the client time to stream the
            // chunks. Probe immediately on the next loading tick.
            batch.manual_retry_wait = false;
            batch.wait_ticks = 0;
        } else {
            batch.wait_ticks = kInitialChunkWaitTicks;
        }
        state_.store(BuildExportState::LoadingRegion, std::memory_order_release);
        setStatus("Loading region " + std::to_string(current_batch_ + 1) + "/" +
                  std::to_string(batches_.size()));
        return;
    }

    if (current == BuildExportState::LoadingRegion) {
        if (allow_teleport_ && batch.teleport_verification_pending) {
            if (!hop_target_valid_) {
                if (!requestBatchTeleportLocked(batch)) return;
            } else if (!RequestBuildExportTeleport(hop_target_x_, hop_target_y_, hop_target_z_)) {
                // The active hop was rejected or never moved the player
                // (for example a server that refuses long-distance TPs).
                // Retry with a shorter hop before giving up on automatic
                // teleport entirely.
                if (!requestBatchTeleportLocked(batch)) return;
            }
            if (IsBuildExportTeleportPending()) {
                setStatus("Teleporting to region " + std::to_string(current_batch_ + 1) + "/" +
                          std::to_string(batches_.size()) + " and verifying player coordinates");
                return;
            }
            if (!hop_target_final_) {
                // An intermediate hop arrived; continue toward the centre.
                if (!requestBatchTeleportLocked(batch)) return;
                setStatus("Approaching region " + std::to_string(current_batch_ + 1) + "/" +
                          std::to_string(batches_.size()) + " in bounded teleport hops");
                return;
            }
            batch.teleport_verification_pending = false;
            hop_target_valid_ = false;
            batch.readable_samples = 0;
            batch.wait_ticks = kInitialChunkWaitTicks;
            setStatus("Teleport verified for region " + std::to_string(current_batch_ + 1) + "/" +
                      std::to_string(batches_.size()) + "; loading chunks");
            return;
        }
        if (!allow_teleport_ && !playerNearCurrentBatchLocked()) {
            batch.wait_ticks = 0;
            batch.readable_samples = 0;
            state_.store(BuildExportState::WaitingForPlayer, std::memory_order_release);
            setWaitingForPlayerStatusLocked();
            return;
        }
        // wait_ticks is a deadline, not a mandatory sleep: probe the region on
        // every tick and start scanning as soon as two consecutive samples are
        // readable. Chunks that are already streamed (a short hop, a cached
        // region, an adjacent batch) then cost a couple of ticks instead of
        // the full conservative wait, and a region that never becomes readable
        // still falls back to the same retry path once the deadline expires.
        if (world_reader_.open() && batchReadableLocked(&world_reader_)) {
            if (++batch.readable_samples < kRequiredReadableSamples) return;
        } else {
            batch.readable_samples = 0;
            if (batch.wait_ticks > 0) {
                --batch.wait_ticks;
                return;
            }
            retryBatchLocked(std::string("region is not readable: ") +
                             NativeWorldAccess::lastWorldReaderDiagnostic());
            return;
        }
        batch.exported_start = exported_blocks_;
        const bool resume_container_capture = container_capture_pending_;
        state_.store(resume_container_capture
                         ? BuildExportState::CapturingContainers
                         : BuildExportState::Scanning,
                     std::memory_order_release);
        setStatus(resume_container_capture
            ? "Resuming container capture in region " +
                  std::to_string(current_batch_ + 1) + "/" +
                  std::to_string(batches_.size())
            : "Scanning region " + std::to_string(current_batch_ + 1) + "/" +
                  std::to_string(batches_.size()));
        return;
    }

    // The player must remain near the active batch for manual exports. If the
    // chunks unload while scanning, discard this partial attempt and ask the
    // player to return before reading it again.
    if (!allow_teleport_ && !playerNearCurrentBatchLocked()) {
        retryBatchLocked("player moved away from the scan region");
        return;
    }

    if (!world_reader_.open()) {
        retryBatchLocked(std::string("world reader is unavailable: ") +
                         NativeWorldAccess::lastWorldReaderDiagnostic());
        return;
    }
    try {
        if (!scanBatchLocked(&world_reader_)) return;
    } catch (const std::bad_alloc&) {
        failLocked("Export scanner ran out of memory");
        return;
    } catch (const std::exception&) {
        failLocked("Export scanner threw an exception");
        return;
    } catch (...) {
        failLocked("Export scanner threw an unknown exception");
        return;
    }
    const uint64_t completed_batch_volume = batchVolume(batch, height_);
    if (batch.cursor != completed_batch_volume) return;

    processed_blocks_.store(batch.processed_start + batch.cursor,
                            std::memory_order_release);
    beginContainerCaptureLocked();
}

void BuildExportRuntime::beginBatchLoadLocked(bool reset_progress) {
    if (current_batch_ >= batches_.size()) {
        beginWriteLocked();
        return;
    }
    ScanBatch& batch = batches_[current_batch_];
    if (reset_progress) batch.cursor = 0;
    const uint64_t processed = processed_blocks_.load(std::memory_order_acquire);
    if (batch.cursor > processed) {
        failLocked("Export batch cursor exceeds processed progress");
        return;
    }
    batch.processed_start = processed - batch.cursor;
    batch.exported_start = exported_blocks_;
    batch.palette_start = palette_.size();
    batch.raw_start = raw_blocks_.size();
    batch.readable_samples = 0;
    batch.manual_retry_wait = false;
    batch.teleport_verification_pending = false;
    if (allow_teleport_) {
        // Every active region begins with a real server TP to its centre.  The
        // coordinate verifier is also the OP check, so cached chunks alone
        // must not bypass it.
        batch.wait_ticks = kInitialChunkWaitTicks;
        hop_target_valid_ = false;
        if (!requestBatchTeleportLocked(batch)) return;
        batch.teleport_verification_pending =
            !hop_target_final_ || IsBuildExportTeleportPending();
        state_.store(BuildExportState::LoadingRegion, std::memory_order_release);
        setStatus(batch.teleport_verification_pending
            ? "Teleporting to region " + std::to_string(current_batch_ + 1) + "/" +
                  std::to_string(batches_.size()) + " and verifying player coordinates"
            : "Loading region " + std::to_string(current_batch_ + 1) + "/" +
                  std::to_string(batches_.size()));
    } else {
        // Manual mode starts in a visible waiting state even when a region
        // happens to be cached. This makes the required player position
        // explicit and prevents scanning chunks that may unload mid-pass.
        CancelBuildExportTeleport();
        batch.wait_ticks = 0;
        state_.store(BuildExportState::WaitingForPlayer, std::memory_order_release);
        setWaitingForPlayerStatusLocked();
    }
}

void BuildExportRuntime::retryBatchLocked(const std::string& reason) {
    if (current_batch_ >= batches_.size()) {
        failLocked("Export region cursor is invalid");
        return;
    }
    std::string checkpoint_error;
    if (!commitCheckpointLocked(true, &checkpoint_error)) {
        failLocked(checkpoint_error.empty() ? "Cannot checkpoint export retry" :
                   checkpoint_error);
        return;
    }
    if (committed_batch_ != current_batch_ ||
        committed_batch_cursor_ > batchVolume(batches_[current_batch_], height_) ||
        committed_palette_size_ > palette_.size() ||
        committed_raw_size_ > raw_blocks_.size()) {
        failLocked("Export checkpoint rollback cursor is invalid");
        return;
    }
    ScanBatch& batch = batches_[current_batch_];
    journal_entries_ = committed_journal_entries_;
    exported_blocks_ = committed_exported_blocks_;
    palette_.resize(committed_palette_size_);
    raw_blocks_.resize(committed_raw_size_);
    raw_block_lookup_.clear();
    try {
        raw_block_lookup_.reserve(raw_blocks_.size());
        const uint64_t raw_width = static_cast<uint64_t>(width_);
        const uint64_t raw_layer = raw_width * static_cast<uint64_t>(length_);
        for (size_t raw_index = 0; raw_index < raw_blocks_.size(); ++raw_index) {
            const SchematicRawBlock& raw = raw_blocks_[raw_index];
            const uint64_t key = static_cast<uint64_t>(raw.x) +
                static_cast<uint64_t>(raw.z) * raw_width +
                static_cast<uint64_t>(raw.y) * raw_layer;
            raw_block_lookup_[key] = raw_index;
        }
    } catch (const std::bad_alloc&) {
        failLocked("Not enough memory to rebuild raw-state index");
        return;
    }
    try {
        palette_ids_.clear();
        palette_ids_.reserve(palette_.size());
        for (size_t index = 0; index < palette_.size(); ++index) {
            palette_ids_.emplace(palette_[index], static_cast<uint32_t>(index));
        }
    } catch (const std::bad_alloc&) {
        failLocked("Not enough memory to rebuild the export palette index");
        return;
    }
    block_state_ids_.clear();
    raw_state_json_.clear();
    raw_client_identifiers_.clear();
    raw_client_aux_.clear();
    raw_state_attempted_.clear();
    batch.cursor = committed_batch_cursor_;
    batch.processed_start = committed_journal_entries_ - committed_batch_cursor_;
    batch.exported_start = committed_exported_blocks_;
    batch.palette_start = committed_palette_size_;
    batch.raw_start = committed_raw_size_;
    processed_blocks_.store(committed_journal_entries_, std::memory_order_release);
    batch.readable_samples = 0;
    batch.teleport_verification_pending = false;
    ++batch.retries;
    const uint32_t maximum_retries = allow_teleport_
        ? kMaximumBatchRetries : kMaximumManualBatchRetries;
    if (batch.retries > maximum_retries) {
        failLocked("Region " + std::to_string(current_batch_ + 1) + " could not be loaded: " + reason);
        return;
    }
    if (allow_teleport_) {
        hop_target_valid_ = false;
        if (!requestBatchTeleportLocked(batch)) return;
        batch.teleport_verification_pending =
            !hop_target_final_ || IsBuildExportTeleportPending();
        batch.wait_ticks = kRetryChunkWaitTicks;
        state_.store(BuildExportState::LoadingRegion, std::memory_order_release);
        setStatus("Retrying region " + std::to_string(current_batch_ + 1) + "/" +
                  std::to_string(batches_.size()) + " (" + std::to_string(batch.retries) +
                  "/" + std::to_string(kMaximumBatchRetries) + ")");
    } else {
        // In manual mode an unreadable region is expected while chunks are
        // still arriving. Never teleport or fail solely because loading is
        // slow; wait for the player to remain near the target and try again.
        batch.wait_ticks = kRetryChunkWaitTicks;
        batch.manual_retry_wait = true;
        state_.store(BuildExportState::WaitingForPlayer, std::memory_order_release);
        setWaitingForPlayerStatusLocked(reason);
    }
}

bool BuildExportRuntime::playerNearCurrentBatchLocked(int32_t* distance_blocks) const {
    if (distance_blocks) *distance_blocks = -1;
    if (current_batch_ >= batches_.size()) return false;

    const ScanBatch& batch = batches_[current_batch_];
    const int32_t target_x = batchCenterCoordinate(batch.min_x, batch.max_x);
    const int32_t target_z = batchCenterCoordinate(batch.min_z, batch.max_z);

    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    {
        std::lock_guard<std::mutex> lock(position_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!cached_position_valid_ || cached_position_at_.time_since_epoch().count() == 0 ||
            now < cached_position_at_ || now - cached_position_at_ > std::chrono::seconds(2)) {
            return false;
        }
        player_x = cached_x_;
        player_y = cached_y_;
        player_z = cached_z_;
    }

    (void)player_y;
    const double dx = static_cast<double>(player_x) - target_x;
    const double dz = static_cast<double>(player_z) - target_z;
    const double distance = std::hypot(dx, dz);
    if (!std::isfinite(distance)) return false;

    if (distance_blocks) {
        const double rounded = std::round(distance);
        *distance_blocks = rounded >= static_cast<double>(std::numeric_limits<int32_t>::max())
            ? std::numeric_limits<int32_t>::max()
            : static_cast<int32_t>(std::max(0.0, rounded));
    }
    return distance <= kManualArrivalRadiusBlocks;
}

void BuildExportRuntime::setWaitingForPlayerStatusLocked(const std::string& reason) {
    if (current_batch_ >= batches_.size()) {
        setStatus(reason.empty() ? "Waiting for player" : reason);
        return;
    }

    const ScanBatch& batch = batches_[current_batch_];
    const int32_t target_x = batchCenterCoordinate(batch.min_x, batch.max_x);
    const int32_t target_z = batchCenterCoordinate(batch.min_z, batch.max_z);
    int32_t distance = -1;
    playerNearCurrentBatchLocked(&distance);

    std::string status = "Travel to X=" + std::to_string(target_x) +
        " Z=" + std::to_string(target_z) + " at a safe height";
    if (distance >= 0) {
        status += " (distance " + std::to_string(distance) + " blocks)";
    } else {
        status += " (player position unavailable)";
    }
    status += "; region " + std::to_string(current_batch_ + 1) + "/" +
        std::to_string(batches_.size());
    if (!reason.empty()) status += ": " + reason;
    if (teleport_permission_unverified_) {
        status = "Automatic teleport disabled because server TP did not reach the target. " +
                 status;
    }
    setStatus(status);
}

bool BuildExportRuntime::shrinkTeleportHopLimitLocked() {
    if (teleport_hop_limit_ <= 0) {
        teleport_hop_limit_ = kInitialTeleportHopBlocks;
        return true;
    }
    if (teleport_hop_limit_ <= kMinimumTeleportHopBlocks) return false;
    teleport_hop_limit_ = std::max(kMinimumTeleportHopBlocks, teleport_hop_limit_ / 2);
    return true;
}

bool BuildExportRuntime::requestBatchTeleportLocked(ScanBatch& batch) {
    const double center_x = (static_cast<double>(batch.min_x) + batch.max_x) * 0.5;
    const double center_z = (static_cast<double>(batch.min_z) + batch.max_z) * 0.5;
    const float target_y = static_cast<float>(clampedTargetY(max_y_, 12));
    for (;;) {
        double hop_x = center_x;
        double hop_z = center_z;
        bool final_hop = true;
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        if (teleport_hop_limit_ > 0 &&
            NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y,
                                                           &player_z)) {
            const double dx = center_x - player_x;
            const double dz = center_z - player_z;
            const double distance = std::hypot(dx, dz);
            if (std::isfinite(distance) &&
                distance > static_cast<double>(teleport_hop_limit_)) {
                const double scale =
                    static_cast<double>(teleport_hop_limit_) / distance;
                hop_x = std::floor(static_cast<double>(player_x) + dx * scale) + 0.5;
                hop_z = std::floor(static_cast<double>(player_z) + dz * scale) + 0.5;
                final_hop = false;
            }
        }
        if (RequestBuildExportTeleport(static_cast<float>(hop_x), target_y,
                            static_cast<float>(hop_z))) {
            hop_target_valid_ = true;
            hop_target_final_ = final_hop;
            hop_target_x_ = static_cast<float>(hop_x);
            hop_target_y_ = target_y;
            hop_target_z_ = static_cast<float>(hop_z);
            return true;
        }
        // The previous probe with these coordinates failed. Halve the hop
        // length so the next attempt asks the server for a shorter move; only
        // a failure at the minimum hop disables automatic teleport.
        if (!shrinkTeleportHopLimitLocked()) {
            hop_target_valid_ = false;
            disableTeleportLocked(
                "server TP did not move the player to the export region; automatic teleport was disabled");
            return false;
        }
    }
}

void BuildExportRuntime::disableTeleportLocked(const std::string& reason) {
    allow_teleport_ = false;
    travel_mode_ = BuildExportTravelMode::Disabled;
    teleport_permission_unverified_ = true;
    hop_target_valid_ = false;
    CancelBuildExportTeleport();
    if (current_batch_ >= batches_.size()) {
        setStatus("Automatic teleport disabled because server TP did not reach the target");
        return;
    }
    ScanBatch& batch = batches_[current_batch_];
    batch.wait_ticks = 0;
    batch.readable_samples = 0;
    batch.manual_retry_wait = false;
    batch.teleport_verification_pending = false;
    state_.store(BuildExportState::WaitingForPlayer, std::memory_order_release);
    setWaitingForPlayerStatusLocked(reason);
}

bool BuildExportRuntime::batchReadableLocked(NativeWorldReader* reader) const {
    if (!reader || current_batch_ >= batches_.size()) return false;
    const ScanBatch& batch = batches_[current_batch_];
    const int32_t sample_y = static_cast<int32_t>(
        (static_cast<int64_t>(min_y_) + max_y_) / 2);
    const int64_t min_chunk_x = floorChunkCoordinate(batch.min_x);
    const int64_t max_chunk_x = floorChunkCoordinate(batch.max_x);
    const int64_t min_chunk_z = floorChunkCoordinate(batch.min_z);
    const int64_t max_chunk_z = floorChunkCoordinate(batch.max_z);

    // A scan batch can intersect one extra chunk along either side when its
    // selection edge is not chunk-aligned. Probe one point inside every
    // intersecting chunk so a loaded center/corner cannot hide an unloaded
    // interior strip.
    for (int64_t chunk_x = min_chunk_x; chunk_x <= max_chunk_x; ++chunk_x) {
        const int64_t chunk_min_x = chunk_x * kWorldChunkSize;
        const int64_t sample_min_x = std::max<int64_t>(batch.min_x, chunk_min_x);
        const int64_t sample_max_x = std::min<int64_t>(
            batch.max_x, chunk_min_x + kWorldChunkSize - 1);
        for (int64_t chunk_z = min_chunk_z; chunk_z <= max_chunk_z; ++chunk_z) {
            const int64_t chunk_min_z = chunk_z * kWorldChunkSize;
            const int64_t sample_min_z = std::max<int64_t>(batch.min_z, chunk_min_z);
            const int64_t sample_max_z = std::min<int64_t>(
                batch.max_z, chunk_min_z + kWorldChunkSize - 1);
            const int32_t sample_x = static_cast<int32_t>((sample_min_x + sample_max_x) / 2);
            const int32_t sample_z = static_cast<int32_t>((sample_min_z + sample_max_z) / 2);
            NativeBlockView block;
            if (!reader->getBlockView(sample_x, sample_y, sample_z, &block) ||
                !block.name || isPlaceholderName(*block.name)) {
                return false;
            }
        }
    }
    return true;
}

bool BuildExportRuntime::scanBatchLocked(NativeWorldReader* reader) {
    if (!reader || current_batch_ >= batches_.size()) return false;
    ScanBatch& batch = batches_[current_batch_];
    const uint64_t batch_length = static_cast<uint64_t>(
        static_cast<int64_t>(batch.max_z) - batch.min_z + 1);
    const uint64_t volume =
        static_cast<uint64_t>(static_cast<int64_t>(batch.max_x) - batch.min_x + 1) *
        static_cast<uint64_t>(static_cast<int64_t>(batch.max_z) - batch.min_z + 1) *
        static_cast<uint64_t>(height_);
    if (block_state_source_generation_ != reader->sourceGeneration()) {
        block_state_ids_.clear();
        raw_state_json_.clear();
        raw_client_identifiers_.clear();
        raw_client_aux_.clear();
        raw_state_attempted_.clear();
        block_state_source_generation_ = reader->sourceGeneration();
    }

    const uint64_t height = static_cast<uint64_t>(height_);
    uint64_t local_y = batch.cursor % height;
    const uint64_t horizontal = batch.cursor / height;
    uint64_t local_z_in_batch = horizontal % batch_length;
    uint64_t local_x_in_batch = horizontal / batch_length;
    const uint64_t batch_x_offset = static_cast<uint64_t>(
        static_cast<int64_t>(batch.min_x) - min_x_);
    const uint64_t batch_z_offset = static_cast<uint64_t>(
        static_cast<int64_t>(batch.min_z) - min_z_);
    const uint64_t width = static_cast<uint64_t>(width_);
    const uint64_t layer_stride = width * static_cast<uint64_t>(length_);
    const auto deadline = std::chrono::steady_clock::now() + kMaximumScanTimePerTick;
    size_t scanned_this_tick = 0;
    // Reused across ticks so a full-budget scan does not allocate and free a
    // journal buffer every tick.
    std::vector<uint16_t>& journal_values = journal_scratch_;
    journal_values.clear();
    try {
        journal_values.reserve(static_cast<size_t>(std::min<uint64_t>(
            kMaximumBlocksPerTick, volume - batch.cursor)));
    } catch (const std::bad_alloc&) {
        failLocked("Not enough memory for the export journal buffer");
        return false;
    }
    const auto checkpointAndRetry = [&](const std::string& reason) {
        std::string journal_error;
        if (!appendJournalLocked(journal_values, &journal_error)) {
            failLocked(journal_error.empty() ? "Cannot append export block journal" :
                       journal_error);
            return false;
        }
        processed_blocks_.store(batch.processed_start + batch.cursor,
                                std::memory_order_release);
        retryBatchLocked(reason);
        return false;
    };
    // Keep one native snapshot per final local coordinate. A retry can revisit
    // a coordinate after the durable prefix, and a world refresh can expose a
    // newer state for an already-recorded coordinate; the lookup therefore
    // replaces the prior record instead of appending duplicates.
    const auto upsertRawBlock = [this](uint64_t index, SchematicRawBlock record) {
        const auto existing = raw_block_lookup_.find(index);
        if (existing != raw_block_lookup_.end()) {
            if (existing->second >= raw_blocks_.size()) return false;
            raw_blocks_[existing->second] = std::move(record);
            return true;
        }
        const size_t raw_index = raw_blocks_.size();
        auto inserted = raw_block_lookup_.emplace(index, raw_index);
        if (!inserted.second) {
            if (inserted.first->second >= raw_blocks_.size()) return false;
            raw_blocks_[inserted.first->second] = std::move(record);
            return true;
        }
        try {
            raw_blocks_.push_back(std::move(record));
        } catch (...) {
            raw_block_lookup_.erase(inserted.first);
            throw;
        }
        return true;
    };
    while (batch.cursor < volume && scanned_this_tick < kMaximumBlocksPerTick) {
        if (scanned_this_tick != 0 &&
            scanned_this_tick % kScanDeadlineCheckInterval == 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        const int32_t x = static_cast<int32_t>(static_cast<int64_t>(batch.min_x) +
                                               static_cast<int64_t>(local_x_in_batch));
        const int32_t y = static_cast<int32_t>(static_cast<int64_t>(min_y_) +
                                               static_cast<int64_t>(local_y));
        const int32_t z = static_cast<int32_t>(static_cast<int64_t>(batch.min_z) +
                                               static_cast<int64_t>(local_z_in_batch));
        NativeBlockView block;
        if (!reader->getBlockView(x, y, z, &block) || !block.name || !block.type_token) {
            return checkpointAndRetry("a block returned unloaded or placeholder data");
        }

        const bool command_block = isCommandBlockState(*block.name);
        NativeCommandBlockData command_entity;
        const bool command_data_read = command_block &&
            NativeWorldAccess::getCommandBlockData(x, y, z, &command_entity);
        const bool has_command_entity = command_data_read && command_entity.available;
        // The Block object layout is version-specific. For command blocks the
        // public ModSDK GetBlock() result is the stable authority for facing
        // and the conditional bit; keep the native aux only as a fallback.
        const uint16_t export_aux = command_data_read && command_entity.shell_aux_available
            ? command_entity.shell_aux : block.aux;

        const CachedBlockStateKey state_key{block.type_token, export_aux};
        const auto cached_state = block_state_ids_.find(state_key);
        uint32_t palette_id = 0;
        if (cached_state != block_state_ids_.end()) {
            palette_id = cached_state->second;
        } else {
            if (isPlaceholderName(*block.name)) {
                return checkpointAndRetry("a block returned unloaded or placeholder data");
            }
            // Modern Bedrock direction-sensitive blocks (stairs, doors,
            // trapdoors, copper variants, ...) can have a stale/zeroed
            // Block.aux once their orientation moved to a modern state
            // property; only the client SDK's GetBlock(name, aux) result is
            // the placement-command-compatible value (see
            // rawSnapshotAuxForExport, which already prefers it for the raw
            // snapshot below). The Sponge palette text generated here must
            // use that same corrected aux -- previously it always used the
            // raw export_aux, so every new-generation block (pale oak, resin
            // brick, copper doors/stairs/trapdoors, ...) was exported with a
            // wrong/default facing even though raw.aux was silently correct.
            if (isRawSnapshotCandidate(*block.name, export_aux) &&
                raw_state_attempted_.find(state_key) == raw_state_attempted_.end()) {
                try {
                    NativeBlockSnapshot early_snapshot;
                    const bool early_ok = NativeWorldAccess::getBlockSnapshot(
                        x, y, z, &early_snapshot, false, dimension_id_);
                    raw_state_attempted_.emplace(state_key);
                    raw_state_json_[state_key] =
                        early_ok && early_snapshot.state_available
                            ? early_snapshot.state_json : std::string();
                    if (early_ok && early_snapshot.client_block_available &&
                        isSafeRawIdentifier(early_snapshot.client_identifier)) {
                        raw_client_identifiers_[state_key] = early_snapshot.client_identifier;
                    }
                    if (early_ok && early_snapshot.client_aux_available) {
                        raw_client_aux_[state_key] = early_snapshot.client_aux;
                    }
                } catch (const std::bad_alloc&) {
                    failLocked("Not enough memory for native block snapshots");
                    return false;
                } catch (...) {
                    failLocked("Native block snapshot query failed");
                    return false;
                }
            }
            const auto early_client_aux = raw_client_aux_.find(state_key);
            const uint16_t palette_aux = early_client_aux != raw_client_aux_.end()
                ? early_client_aux->second : export_aux;

            std::string state = SchematicWriter::blockStateForExport(
                *block.name, palette_aux, block.legacy_id);
            if (state != "minecraft:air") {
                const auto found = palette_ids_.find(state);
                if (found == palette_ids_.end()) {
                    if (palette_.size() >= SchematicWriter::kMaximumPaletteSize) {
                        failLocked("Schematic palette exceeds 65,536 states");
                        return false;
                    }
                    palette_id = static_cast<uint32_t>(palette_.size());
                    palette_.push_back(std::move(state));
                    palette_ids_.emplace(palette_.back(), palette_id);
                } else {
                    palette_id = found->second;
                }
            } else {
                palette_id = 0;
            }
            block_state_ids_.emplace(state_key, palette_id);
        }
        if (palette_id != 0) ++exported_blocks_;

        const uint64_t index = batch_x_offset + local_x_in_batch +
            (batch_z_offset + local_z_in_batch) * width + local_y * layer_stride;
        if (index >= block_indices_.size()) {
            failLocked("Schematic scan produced an invalid block index");
            return false;
        }
        const uint16_t compact_palette_id = static_cast<uint16_t>(palette_id);
        block_indices_[static_cast<size_t>(index)] = compact_palette_id;

        if (palette_id != 0 && isRawSnapshotCandidate(*block.name, export_aux)) {
            // State JSON is safe to cache by the native Block instance and aux;
            // block entities are coordinate-specific, so entity candidates are
            // queried for every occurrence while still reusing the state part.
            const bool entity_candidate = isRawEntityCandidate(*block.name) || command_block;
            NativeBlockSnapshot native_snapshot;
            bool snapshot_read = false;
            bool snapshot_ok = false;
            try {
                const auto attempted = raw_state_attempted_.find(state_key);
                if (entity_candidate || attempted == raw_state_attempted_.end()) {
                    snapshot_read = true;
                    // Inventory APIs exposed by the client SDK are not stable
                    // on this NetEase build. Container items are captured in
                    // the verified packet phase after this voxel batch; this
                    // snapshot query keeps only safe block/entity metadata.
                    snapshot_ok = NativeWorldAccess::getBlockSnapshot(
                        x, y, z, &native_snapshot, false, dimension_id_);
                    if (attempted == raw_state_attempted_.end()) {
                        raw_state_attempted_.emplace(state_key);
                        raw_state_json_[state_key] =
                            snapshot_ok && native_snapshot.state_available
                                ? native_snapshot.state_json : std::string();
                    } else if (snapshot_ok && native_snapshot.state_available) {
                        auto cached_native_state = raw_state_json_.find(state_key);
                        if (cached_native_state == raw_state_json_.end()) {
                            raw_state_json_.emplace(state_key, native_snapshot.state_json);
                        } else if (cached_native_state->second.empty()) {
                            cached_native_state->second = native_snapshot.state_json;
                        }
                    }
                    if (snapshot_ok && native_snapshot.client_block_available &&
                        isSafeRawIdentifier(native_snapshot.client_identifier)) {
                        raw_client_identifiers_[state_key] = native_snapshot.client_identifier;
                    }
                    if (snapshot_ok && native_snapshot.client_aux_available) {
                        raw_client_aux_[state_key] = native_snapshot.client_aux;
                    }
                }
                const auto cached_native_state = raw_state_json_.find(state_key);
                const bool has_state_json = cached_native_state != raw_state_json_.end() &&
                    !cached_native_state->second.empty() &&
                    cached_native_state->second.size() <= 4U * 1024U * 1024U;
                const bool has_entity_json = snapshot_read && snapshot_ok &&
                    native_snapshot.entity_available &&
                    !native_snapshot.entity_json.empty() &&
                    native_snapshot.entity_json.size() <= 64U * 1024U * 1024U;
                // The native name/aux pair is the lossless baseline. JSON is an
                // optional enhancement; a client-only SDK may not expose it,
                // but that must never make a directional/stateful block vanish
                // from the canonical export.
                if (isRawSnapshotCandidate(*block.name, export_aux)) {
                    SchematicRawBlock raw;
                    raw.x = static_cast<int32_t>(static_cast<int64_t>(x) - min_x_);
                    raw.y = static_cast<int32_t>(static_cast<int64_t>(y) - min_y_);
                    raw.z = static_cast<int32_t>(static_cast<int64_t>(z) - min_z_);
                    const bool native_identity =
                        nativeRawIdentityAuthoritative(*block.name);
                    const auto cached_identifier = raw_client_identifiers_.find(state_key);
                    // GetBlock() can flatten a sign or fence-gate name, so the
                    // native identifier remains authoritative for material.
                    // Its aux is a different contract: the SDK value is the
                    // command-compatible placement data, while the Block field
                    // can be an internal modern-state encoding.
                    raw.identifier = native_identity ||
                        cached_identifier == raw_client_identifiers_.end()
                        ? *block.name : cached_identifier->second;
                    const auto cached_aux = raw_client_aux_.find(state_key);
                    const auto cached_state_for_aux = raw_state_json_.find(state_key);
                    const std::string_view state_json_for_aux =
                        (cached_state_for_aux != raw_state_json_.end())
                            ? std::string_view(cached_state_for_aux->second) : std::string_view{};
                    raw.aux = rawSnapshotAuxForExport(
                        export_aux, cached_aux != raw_client_aux_.end(),
                        cached_aux == raw_client_aux_.end() ? 0U : cached_aux->second,
                        rawSnapshotLeaf(raw.identifier), state_json_for_aux);
                    raw.legacy_id = block.legacy_id;
                    if (has_state_json) raw.state_json = cached_native_state->second;
                    if (has_entity_json) {
                        // Container inventories are intentionally reduced to
                        // the four fields supported by the deferred
                        // /replaceitem stage. Sign payloads are separately
                        // reduced to persistent fields before encryption.
                        const bool container =
                            deferredContainerIdentifier(raw.identifier) ||
                            deferredContainerIdentifier(*block.name);
                        if (container && export_container_items_) {
                            std::string normalized_entity;
                            if (normalizeContainerEntityJson(native_snapshot.entity_json,
                                                              &normalized_entity)) {
                                raw.entity_json = std::move(normalized_entity);
                            } else {
                                // Keep a valid, empty container payload rather
                                // than leaking unsupported NBT into .infinity.
                                raw.entity_json = "{\"Items\":[]}";
                            }
                        } else if (container) {
                            // The caller explicitly asked for block-only
                            // containers. Do not retain an inventory that the
                            // native snapshot happened to expose.
                            raw.entity_json.clear();
                        } else if (rawSnapshotLeaf(raw.identifier).find("sign") !=
                                   std::string_view::npos ||
                                   rawSnapshotLeaf(*block.name).find("sign") !=
                                   std::string_view::npos) {
                            std::string normalized_entity;
                            std::string sign_error;
                            if (!normalizeSignEntityJson(native_snapshot.entity_json,
                                                         &normalized_entity,
                                                         &sign_error)) {
                                failLocked("cannot normalize sign data at (" +
                                    std::to_string(x) + "," + std::to_string(y) + "," +
                                    std::to_string(z) + "): " +
                                    (sign_error.empty() ? "invalid sign payload" : sign_error));
                                return false;
                            }
                            raw.entity_json = std::move(normalized_entity);
                        } else {
                            raw.entity_json = std::move(native_snapshot.entity_json);
                        }
                    }
                    if (raw.identifier.size() > 1024U || raw.identifier.find('\0') != std::string::npos) {
                        raw.identifier = *block.name;
                    }
                    if (raw.identifier.empty() || raw.identifier.size() > 1024U) {
                        failLocked("native block snapshot has an invalid identifier");
                        return false;
                    }
                    if (!upsertRawBlock(index, std::move(raw))) {
                        failLocked("native block snapshot index is inconsistent");
                        return false;
                    }
                }
            } catch (const std::bad_alloc&) {
                failLocked("Not enough memory for native block snapshots");
                return false;
            } catch (...) {
                failLocked("Native block snapshot query failed");
                return false;
            }
        }

        if (command_block) {
            CommandBlockRecord record;
            record.x = static_cast<int32_t>(static_cast<int64_t>(x) - min_x_);
            record.y = static_cast<int32_t>(static_cast<int64_t>(y) - min_y_);
            record.z = static_cast<int32_t>(static_cast<int64_t>(z) - min_z_);
            record.mode = commandBlockModeForState(*block.name);
            record.conditional = (export_aux & 0x08U) != 0U;
            record.redstone_mode = true;
            if (has_command_entity) {
                record.redstone_mode = command_entity.redstone_mode;
                record.command = std::move(command_entity.command);
                record.name = std::move(command_entity.name);
                record.last_output = std::move(command_entity.last_output);
                record.output_tracked = command_entity.output_tracked;
                record.tick_delay = command_entity.tick_delay;
                record.executing_on_first_tick = command_entity.executing_on_first_tick;
            }
            command_blocks_.push_back(std::move(record));
        }
        journal_values.push_back(compact_palette_id);
        ++batch.cursor;
        ++scanned_this_tick;
        if (++local_y == height) {
            local_y = 0;
            if (++local_z_in_batch == batch_length) {
                local_z_in_batch = 0;
                ++local_x_in_batch;
            }
        }
    }
    std::string journal_error;
    if (!appendJournalLocked(journal_values, &journal_error)) {
        failLocked(journal_error.empty() ? "Cannot append export block journal" : journal_error);
        return false;
    }
    processed_blocks_.store(batch.processed_start + batch.cursor,
                            std::memory_order_release);
    // A full voxel cursor is not a complete batch until its containers have
    // been captured. Let onGameTick publish the v4 pending marker and raw
    // target count in the same atomic checkpoint instead of briefly exposing
    // a full-cursor checkpoint that recovery would treat as finished.
    if (batch.cursor < volume) {
        std::string checkpoint_error;
        if (!commitCheckpointLocked(false, &checkpoint_error)) {
            failLocked(checkpoint_error.empty() ? "Cannot save export checkpoint" :
                       checkpoint_error);
            return false;
        }
    }
    if (batch.cursor < volume) {
        setStatus("Scanning region " + std::to_string(current_batch_ + 1) + "/" +
                  std::to_string(batches_.size()) + " (" +
                  std::to_string(batch.cursor) + "/" + std::to_string(volume) + ")");
    }
    return true;
}

void BuildExportRuntime::resetContainerCaptureLocked() noexcept {
    if (container_request_token_ != 0U) {
        CancelContainerCapture(container_request_token_);
    }
    container_request_token_ = 0;
    container_id_ = 0;
    container_type_ = 0;
    container_wait_ticks_ = 0;
    container_cursor_ = 0;
    container_attempt_ = 0;
    container_capture_pending_ = false;
    container_window_open_ = false;
    container_phase_ = ContainerCapturePhase::Inactive;
    container_targets_.clear();
}

bool BuildExportRuntime::rebuildContainerTargetsLocked(std::string* error) {
    container_targets_.clear();
    if (current_batch_ >= batches_.size() || width_ <= 0 || length_ <= 0) {
        if (error) *error = "container target batch is invalid";
        return false;
    }
    const ScanBatch& batch = batches_[current_batch_];
    const uint64_t width = static_cast<uint64_t>(width_);
    const uint64_t layer = width * static_cast<uint64_t>(length_);
    try {
        container_targets_.reserve(std::min(raw_blocks_.size(),
                                             static_cast<size_t>(1024U)));
        for (const SchematicRawBlock& raw : raw_blocks_) {
            if (!packetCapturableContainerIdentifier(raw.identifier)) {
                continue;
            }
            const int64_t world_x = static_cast<int64_t>(min_x_) + raw.x;
            const int64_t world_y = static_cast<int64_t>(min_y_) + raw.y;
            const int64_t world_z = static_cast<int64_t>(min_z_) + raw.z;
            if (world_x < batch.min_x || world_x > batch.max_x ||
                world_z < batch.min_z || world_z > batch.max_z ||
                world_x < std::numeric_limits<int32_t>::min() ||
                world_x > std::numeric_limits<int32_t>::max() ||
                world_y < std::numeric_limits<int32_t>::min() ||
                world_y > std::numeric_limits<int32_t>::max() ||
                world_z < std::numeric_limits<int32_t>::min() ||
                world_z > std::numeric_limits<int32_t>::max()) {
                continue;
            }
            const uint64_t block_index = static_cast<uint64_t>(raw.x) +
                static_cast<uint64_t>(raw.z) * width +
                static_cast<uint64_t>(raw.y) * layer;
            const auto lookup = raw_block_lookup_.find(block_index);
            if (lookup == raw_block_lookup_.end() ||
                lookup->second >= raw_blocks_.size()) {
                if (error) *error = "container target has no raw-state record";
                container_targets_.clear();
                return false;
            }
            container_targets_.push_back({
                block_index, static_cast<int32_t>(world_x),
                static_cast<int32_t>(world_y), static_cast<int32_t>(world_z)});
        }
        std::sort(container_targets_.begin(), container_targets_.end(),
                  [](const ContainerCaptureTarget& left,
                     const ContainerCaptureTarget& right) {
                      return left.block_index < right.block_index;
                  });
        for (size_t index = 1; index < container_targets_.size(); ++index) {
            if (container_targets_[index - 1U].block_index ==
                container_targets_[index].block_index) {
                if (error) *error = "container target list contains duplicate coordinates";
                container_targets_.clear();
                return false;
            }
        }
    } catch (const std::bad_alloc&) {
        container_targets_.clear();
        if (error) *error = "not enough memory for container capture targets";
        return false;
    }
    return true;
}

bool BuildExportRuntime::beginContainerCaptureLocked() {
    if (!export_container_items_) {
        completeContainerBatchLocked();
        return state_.load(std::memory_order_acquire) != BuildExportState::Failed;
    }
    if (container_capture_pending_) {
        state_.store(BuildExportState::CapturingContainers,
                     std::memory_order_release);
        return true;
    }
    std::string target_error;
    if (!rebuildContainerTargetsLocked(&target_error)) {
        failLocked(target_error.empty() ? "Cannot build container capture targets" :
                   target_error);
        return false;
    }
    if (container_targets_.empty()) {
        completeContainerBatchLocked();
        return state_.load(std::memory_order_acquire) != BuildExportState::Failed;
    }

    container_capture_pending_ = true;
    container_cursor_ = 0;
    container_attempt_ = 1;
    container_phase_ = ContainerCapturePhase::Navigate;
    checkpoint_data_dirty_ = true;
    state_.store(BuildExportState::CapturingContainers,
                 std::memory_order_release);
    std::string checkpoint_error;
    if (!commitCheckpointLocked(true, &checkpoint_error)) {
        failLocked(checkpoint_error.empty()
            ? "Cannot checkpoint pending container capture" : checkpoint_error);
        return false;
    }
    setStatus("Capturing container 1/" +
              std::to_string(container_targets_.size()) + " in region " +
              std::to_string(current_batch_ + 1) + "/" +
              std::to_string(batches_.size()));
    return true;
}

uint64_t BuildExportRuntime::nextContainerCaptureTokenLocked() {
    ++container_request_sequence_;
    uint64_t token = generation_.load(std::memory_order_acquire) *
        0x9e3779b97f4a7c15ULL ^ container_request_sequence_;
    if (token == 0U) token = ++container_request_sequence_;
    return token;
}

bool BuildExportRuntime::playerNearCurrentContainerLocked(
        int32_t* distance_blocks) const {
    if (distance_blocks) *distance_blocks = -1;
    if (!container_capture_pending_ || container_cursor_ >= container_targets_.size()) {
        return false;
    }
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    {
        std::lock_guard<std::mutex> lock(position_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!cached_position_valid_ || cached_position_at_.time_since_epoch().count() == 0 ||
            now < cached_position_at_ ||
            now - cached_position_at_ > std::chrono::seconds(2)) {
            return false;
        }
        player_x = cached_x_;
        player_y = cached_y_;
        player_z = cached_z_;
    }
    const ContainerCaptureTarget& target =
        container_targets_[static_cast<size_t>(container_cursor_)];
    const double dx = static_cast<double>(player_x) - target.x;
    const double dy = static_cast<double>(player_y) - target.y;
    const double dz = static_cast<double>(player_z) - target.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance)) return false;
    if (distance_blocks) {
        const double rounded = std::round(distance);
        *distance_blocks = rounded >= static_cast<double>(std::numeric_limits<int32_t>::max())
            ? std::numeric_limits<int32_t>::max()
            : static_cast<int32_t>(std::max(0.0, rounded));
    }
    return dx * dx + dy * dy + dz * dz <=
        static_cast<double>(kContainerInteractionDistanceSquared);
}

bool BuildExportRuntime::validateCurrentContainerLocked(const void** native_block,
                                                         bool* changed,
                                                         std::string* error) {
    if (native_block) *native_block = nullptr;
    if (changed) *changed = false;
    if (!container_capture_pending_ || container_cursor_ >= container_targets_.size()) {
        if (error) *error = "container capture cursor is invalid";
        return false;
    }
    const ContainerCaptureTarget& target =
        container_targets_[static_cast<size_t>(container_cursor_)];
    const auto raw_lookup = raw_block_lookup_.find(target.block_index);
    if (raw_lookup == raw_block_lookup_.end() || raw_lookup->second >= raw_blocks_.size()) {
        if (changed) *changed = true;
        if (error) *error = "container raw-state record disappeared";
        return false;
    }
    if (!world_reader_.open()) {
        if (error) *error = "container region is not readable";
        return false;
    }
    NativeBlockView block;
    if (!world_reader_.getBlockView(target.x, target.y, target.z, &block) ||
        !block.name || isPlaceholderName(*block.name)) {
        if (error) *error = "container block is temporarily unavailable";
        return false;
    }
    const std::string_view expected =
        rawSnapshotLeaf(raw_blocks_[raw_lookup->second].identifier);
    const std::string_view actual = rawSnapshotLeaf(*block.name);
    if (!packetCapturableContainerIdentifier(*block.name) || actual != expected) {
        if (changed) *changed = true;
        if (error) {
            *error = "container changed at (" + std::to_string(target.x) + "," +
                std::to_string(target.y) + "," + std::to_string(target.z) + ")";
        }
        return false;
    }
    if (!block.type_token) {
        if (error) *error = "native container Block is temporarily unavailable";
        return false;
    }
    if (native_block) *native_block = block.type_token;
    return true;
}

bool BuildExportRuntime::applyContainerCaptureLocked(
        const ContainerCaptureResult& result, std::string* error) {
    if (!container_capture_pending_ || container_cursor_ >= container_targets_.size()) {
        if (error) *error = "container capture cursor is invalid";
        return false;
    }
    const ContainerCaptureTarget& target =
        container_targets_[static_cast<size_t>(container_cursor_)];
    if (result.token != container_request_token_ || result.x != target.x ||
        result.y != target.y || result.z != target.z || result.slot_count == 0U ||
        result.slot_count > 256U) {
        if (error) *error = "container capture result does not match its request";
        return false;
    }
    std::string entity_json;
    if (!encodeCapturedContainerItemsJson(result, &entity_json, error)) return false;
    const auto raw_lookup = raw_block_lookup_.find(target.block_index);
    if (raw_lookup == raw_block_lookup_.end() || raw_lookup->second >= raw_blocks_.size()) {
        if (error) *error = "captured container has no raw-state destination";
        return false;
    }
    raw_blocks_[raw_lookup->second].entity_json = std::move(entity_json);
    checkpoint_data_dirty_ = true;
    return true;
}

void BuildExportRuntime::retryContainerCaptureLocked(const std::string& reason) {
    if (container_window_open_) {
        std::string close_error;
        if (!ContainerClosePacketSender::send(container_id_, container_type_,
                                              &close_error)) {
            CancelContainerCapture(container_request_token_);
            container_request_token_ = 0;
            container_window_open_ = false;
            failLocked(close_error.empty()
                ? "Cannot close container after a capture failure"
                : "Cannot close container after a capture failure: " + close_error);
            return;
        }
    }
    if (container_request_token_ != 0U) {
        CancelContainerCapture(container_request_token_);
    }
    container_request_token_ = 0;
    container_window_open_ = false;
    container_id_ = 0;
    container_type_ = 0;
    if (container_phase_ == ContainerCapturePhase::WaitingForTeleport) {
        CancelBuildExportTeleport();
    }
    if (container_attempt_ >= kMaximumContainerAttempts) {
        const ContainerCaptureTarget& target =
            container_targets_[static_cast<size_t>(container_cursor_)];
        failLocked("Container capture failed at (" + std::to_string(target.x) + "," +
                   std::to_string(target.y) + "," + std::to_string(target.z) +
                   ") after " + std::to_string(kMaximumContainerAttempts) +
                   " attempts: " + reason);
        return;
    }
    ++container_attempt_;
    // Apply the same open-packet cooldown to retries. Without this, a timeout
    // or rejected packet could re-open the same container faster than the
    // normal next-container path.
    container_wait_ticks_ = kContainerOpenCooldownTicks;
    container_phase_ = ContainerCapturePhase::Cooldown;
    setStatus("Retrying container " + std::to_string(container_cursor_ + 1U) + "/" +
              std::to_string(container_targets_.size()) + " (attempt " +
              std::to_string(container_attempt_) + "/" +
              std::to_string(kMaximumContainerAttempts) + "): " + reason);
}

void BuildExportRuntime::completeContainerBatchLocked() {
    if (container_request_token_ != 0U) {
        CancelContainerCapture(container_request_token_);
    }
    container_request_token_ = 0;
    container_window_open_ = false;
    container_capture_pending_ = false;
    container_phase_ = ContainerCapturePhase::Inactive;
    container_cursor_ = 0;
    container_attempt_ = 0;
    container_targets_.clear();
    checkpoint_data_dirty_ = true;
    std::string checkpoint_error;
    if (!commitCheckpointLocked(true, &checkpoint_error)) {
        failLocked(checkpoint_error.empty()
            ? "Cannot save completed container capture" : checkpoint_error);
        return;
    }
    if (travel_mode_ == BuildExportTravelMode::SemiAutomatic) {
        allow_teleport_ = false;
    }
    ++current_batch_;
    if (current_batch_ >= batches_.size()) beginWriteLocked();
    else beginBatchLoadLocked();
}

void BuildExportRuntime::tickContainerCaptureLocked() {
    if (!container_capture_pending_ || container_targets_.empty() ||
        container_cursor_ > container_targets_.size()) {
        failLocked("Container capture state is inconsistent");
        return;
    }
    if (container_cursor_ == container_targets_.size()) {
        completeContainerBatchLocked();
        return;
    }
    if (serviceContainerCaptureQuarantine()) {
        setStatus("Settling delayed container packets before retrying");
        return;
    }
    const ContainerCaptureTarget& target =
        container_targets_[static_cast<size_t>(container_cursor_)];
    const std::string progress = std::to_string(container_cursor_ + 1U) + "/" +
        std::to_string(container_targets_.size());

    switch (container_phase_) {
        case ContainerCapturePhase::Navigate: {
            if (playerNearCurrentContainerLocked()) {
                container_wait_ticks_ = kContainerSettleTicks;
                container_phase_ = ContainerCapturePhase::Settling;
                setStatus("Preparing container " + progress);
                return;
            }
            if (!allow_teleport_) {
                int32_t distance = -1;
                playerNearCurrentContainerLocked(&distance);
                setStatus("Move near container " + progress + " at X=" +
                          std::to_string(target.x) + " Y=" +
                          std::to_string(target.y) + " Z=" +
                          std::to_string(target.z) +
                          (distance >= 0 ? " (distance " + std::to_string(distance) + ")"
                                         : " (player position unavailable)"));
                return;
            }
            const int32_t above_y = target.y == std::numeric_limits<int32_t>::max()
                ? target.y : target.y + 1;
            if (!RequestBuildExportTeleport(static_cast<float>(target.x) + 0.5F,
                                 static_cast<float>(above_y),
                                 static_cast<float>(target.z) + 0.5F)) {
                retryContainerCaptureLocked("server teleport request was rejected");
                return;
            }
            container_wait_ticks_ = kContainerTeleportTimeoutTicks;
            container_phase_ = ContainerCapturePhase::WaitingForTeleport;
            setStatus("Teleporting to container " + progress);
            return;
        }

        case ContainerCapturePhase::WaitingForTeleport:
            if (container_wait_ticks_-- <= 0) {
                retryContainerCaptureLocked("teleport verification timed out");
                return;
            }
            if (IsBuildExportTeleportPending()) return;
            if (!playerNearCurrentContainerLocked()) {
                retryContainerCaptureLocked("player did not arrive within interaction range");
                return;
            }
            container_wait_ticks_ = kContainerSettleTicks;
            container_phase_ = ContainerCapturePhase::Settling;
            setStatus("Preparing container " + progress);
            return;

        case ContainerCapturePhase::Settling: {
            if (!playerNearCurrentContainerLocked()) {
                container_phase_ = ContainerCapturePhase::Navigate;
                return;
            }
            if (container_wait_ticks_-- > 0) return;
            const void* native_block = nullptr;
            bool changed = false;
            std::string validation_error;
            if (!validateCurrentContainerLocked(&native_block, &changed,
                                                &validation_error)) {
                if (changed) {
                    failLocked(validation_error.empty()
                        ? "Container changed before capture" : validation_error);
                } else {
                    retryContainerCaptureLocked(validation_error.empty()
                        ? "container block is unavailable" : validation_error);
                }
                return;
            }
            if (!IsItemRuntimeRegistryReady()) {
                failLocked(
                    "Item registry packet is unavailable; leave and re-enter the world, then resume export");
                return;
            }
            // Hook installation can run before libminecraftpe has finished
            // mapping. Retry it here on the game thread instead of spending
            // all container-face attempts waiting on a hook that never ran.
            if (!BuildPacketReceiveHook::isReceiveHookReady() &&
                !BuildPacketReceiveHook::init(Main::getBaseAddress())) {
                retryContainerCaptureLocked(
                    "container packet receive hook is unavailable");
                return;
            }
            container_request_token_ = nextContainerCaptureTokenLocked();
            container_window_open_ = false;
            container_id_ = 0;
            container_type_ = 0;
            ArmContainerCapture(container_request_token_, target.x, target.y, target.z);
            std::string request_error;
            if (!ContainerOpenPacketSender::send(
                    target.x, target.y, target.z, native_block,
                    containerInteractionFaceForAttempt(container_attempt_),
                    &request_error)) {
                CancelContainerCapture(container_request_token_);
                container_request_token_ = 0;
                retryContainerCaptureLocked(request_error.empty()
                    ? "packet-only container request failed" : request_error);
                return;
            }
            container_wait_ticks_ = kContainerPacketTimeoutTicks;
            container_phase_ = ContainerCapturePhase::WaitingForPackets;
            setStatus("Reading container " + progress);
            return;
        }

        case ContainerCapturePhase::WaitingForPackets: {
            ContainerCaptureResult result;
            const ContainerCapturePollState poll =
                PollContainerCapture(container_request_token_, &result);
            const bool opened_now = result.container_opened &&
                !container_window_open_ && !result.container_closed;
            if (result.container_opened) {
                container_window_open_ = !result.container_closed;
                container_id_ = result.container_id;
                container_type_ = result.container_type;
            }
            if (poll == ContainerCapturePollState::Inactive) {
                retryContainerCaptureLocked("container capture request became inactive");
                return;
            }
            if (poll == ContainerCapturePollState::Failed) {
                std::string failure = result.error.empty()
                    ? "container packet capture failed" : result.error;
                if (container_window_open_) {
                    std::string close_error;
                    if (!ContainerClosePacketSender::send(
                            container_id_, container_type_, &close_error)) {
                        failure += close_error.empty()
                            ? "; the open container could not be closed"
                            : "; the open container could not be closed: " + close_error;
                    }
                }
                container_window_open_ = false;
                CancelContainerCapture(container_request_token_);
                container_request_token_ = 0;
                failLocked("Container capture stopped after an invalid packet: " + failure);
                return;
            }
            if (poll != ContainerCapturePollState::Ready) {
                if (opened_now &&
                    poll == ContainerCapturePollState::WaitingForContent) {
                    // The old state machine started this timer when ClickBlock
                    // was sent. A delayed open could therefore consume almost
                    // the whole inventory deadline and make every retry fail
                    // while the server was behaving correctly.
                    container_wait_ticks_ = kContainerPacketTimeoutTicks;
                    setStatus("Container opened; reading items " + progress);
                }
                if (--container_wait_ticks_ <= 0) {
                    retryContainerCaptureLocked(
                        poll == ContainerCapturePollState::WaitingForOpen
                            ? "ContainerOpen packet timed out; the packet-only ClickBlock request did not open the container"
                            : "InventoryContent packet timed out after the container opened");
                }
                return;
            }

            if (!result.container_closed) {
                std::string close_error;
                if (!ContainerClosePacketSender::send(result.container_id,
                                                      result.container_type,
                                                      &close_error)) {
                    container_window_open_ = false;
                    CancelContainerCapture(container_request_token_);
                    container_request_token_ = 0;
                    failLocked(close_error.empty()
                        ? "Container close packet failed; close the window manually before resuming"
                        : "Container close packet failed; close the window manually before resuming: " +
                              close_error);
                    return;
                }
            }
            container_window_open_ = false;
            std::string capture_error;
            if (!applyContainerCaptureLocked(result, &capture_error)) {
                CancelContainerCapture(container_request_token_);
                container_request_token_ = 0;
                failLocked(capture_error.empty()
                    ? "Captured inventory is invalid"
                    : "Captured inventory cannot be exported: " + capture_error);
                return;
            }
            CancelContainerCapture(container_request_token_);
            container_request_token_ = 0;
            ++container_cursor_;
            container_attempt_ = 1;
            container_wait_ticks_ = kContainerOpenCooldownTicks;
            container_phase_ = ContainerCapturePhase::Cooldown;
            std::string checkpoint_error;
            // Keep the cursor and captured data dirty, but let the one-second
            // checkpoint cadence coalesce adjacent containers. A terminal
            // failure/cancel and the end of the batch still force a durable
            // checkpoint, while a sudden process loss only repeats the small
            // uncommitted suffix after resume.
            if (!commitCheckpointLocked(false, &checkpoint_error)) {
                failLocked(checkpoint_error.empty()
                    ? "Cannot checkpoint captured container" : checkpoint_error);
                return;
            }
            setStatus("Captured container " + progress);
            return;
        }

        case ContainerCapturePhase::Cooldown:
            if (container_wait_ticks_ > 0) {
                --container_wait_ticks_;
                return;
            }
            if (container_cursor_ >= container_targets_.size()) {
                completeContainerBatchLocked();
            } else {
                container_phase_ = ContainerCapturePhase::Navigate;
            }
            return;

        case ContainerCapturePhase::Inactive:
            failLocked("Container capture phase is inactive");
            return;
    }
}

void BuildExportRuntime::rebuildCommandBlocksFromPaletteLocked() {
    if (palette_.empty() || block_indices_.empty()) return;
    const uint64_t width = static_cast<uint64_t>(width_);
    const uint64_t length = static_cast<uint64_t>(length_);
    const uint64_t layer = width * length;
    std::unordered_map<uint64_t, CommandBlockRecord> scanned_records;
    try {
        scanned_records.reserve(command_blocks_.size());
        for (const CommandBlockRecord& record : command_blocks_) {
            if (record.x < 0 || record.y < 0 || record.z < 0 ||
                record.x >= width_ || record.y >= height_ || record.z >= length_) {
                continue;
            }
            const uint64_t index = static_cast<uint64_t>(record.x) +
                static_cast<uint64_t>(record.z) * width +
                static_cast<uint64_t>(record.y) * layer;
            scanned_records[index] = record;
        }
    } catch (const std::bad_alloc&) {
        failLocked("Not enough memory to rebuild command-block records");
        return;
    }
    // Command-block entity text is intentionally not part of the compact
    // checkpoint journal.  When a job is resumed, the structural record is
    // rebuilt from the palette below; refresh its editable payload from the
    // live block entity so the resulting BDX does not silently lose commands
    // that were scanned before the interruption.
    const auto refreshEntityData = [this](CommandBlockRecord* record) {
        if (!record) return;
        const int64_t world_x = static_cast<int64_t>(min_x_) + record->x;
        const int64_t world_y = static_cast<int64_t>(min_y_) + record->y;
        const int64_t world_z = static_cast<int64_t>(min_z_) + record->z;
        if (world_x < std::numeric_limits<int32_t>::min() ||
            world_x > std::numeric_limits<int32_t>::max() ||
            world_y < std::numeric_limits<int32_t>::min() ||
            world_y > std::numeric_limits<int32_t>::max() ||
            world_z < std::numeric_limits<int32_t>::min() ||
            world_z > std::numeric_limits<int32_t>::max()) {
            return;
        }
        NativeCommandBlockData entity;
        if (!NativeWorldAccess::getCommandBlockData(
                static_cast<int32_t>(world_x), static_cast<int32_t>(world_y),
                static_cast<int32_t>(world_z), &entity) || !entity.available) {
            return;
        }
        record->redstone_mode = entity.redstone_mode;
        record->command = std::move(entity.command);
        record->name = std::move(entity.name);
        record->last_output = std::move(entity.last_output);
        record->output_tracked = entity.output_tracked;
        record->tick_delay = entity.tick_delay;
        record->executing_on_first_tick = entity.executing_on_first_tick;
    };
    std::vector<CommandBlockRecord> rebuilt;
    try {
        rebuilt.reserve(scanned_records.size());
    } catch (const std::bad_alloc&) {
        failLocked("Not enough memory to rebuild command-block records");
        return;
    }
    for (uint64_t index = 0; index < block_indices_.size(); ++index) {
        const uint16_t palette_id = block_indices_[static_cast<size_t>(index)];
        if (palette_id == 0 || palette_id >= palette_.size()) continue;
        const std::string& state = palette_[palette_id];
        if (!isCommandBlockState(state)) continue;
        const auto scanned = scanned_records.find(index);
        if (scanned != scanned_records.end()) {
            rebuilt.push_back(scanned->second);
            continue;
        }
        CommandBlockRecord record;
        record.x = static_cast<int32_t>(index % width);
        record.z = static_cast<int32_t>((index / width) % length);
        record.y = static_cast<int32_t>(index / layer);
        record.mode = commandBlockModeForState(state);
        record.conditional = state.find("conditional=true") != std::string::npos;
        record.redstone_mode = true;
        refreshEntityData(&record);
        rebuilt.push_back(std::move(record));
    }
    command_blocks_ = std::move(rebuilt);
}

void BuildExportRuntime::beginWriteLocked() {
    if (state_.load(std::memory_order_acquire) == BuildExportState::Writing) return;
    if (current_batch_ != batches_.size() ||
        journal_entries_ != total_blocks_.load(std::memory_order_acquire) ||
        container_capture_pending_ || container_request_token_ != 0U ||
        container_phase_ != ContainerCapturePhase::Inactive) {
        failLocked("Export scan is incomplete at the writer boundary");
        return;
    }
    state_.store(BuildExportState::Writing, std::memory_order_release);
    std::string checkpoint_error;
    if (!commitCheckpointLocked(true, &checkpoint_error)) {
        failLocked(checkpoint_error.empty() ? "Cannot checkpoint .infinity writing stage" :
                   checkpoint_error);
        return;
    }
    const uint64_t generation = generation_.load(std::memory_order_acquire);
    const std::string checkpoint_path = checkpoint_path_;
    const std::string output_stem = output_path_;
    // The native format is the canonical export. Sponge/BDX remain available
    // as standalone writers for older callers, but the UI export no longer
    // routes lossless data through either lossy representation.
    constexpr bool use_native = true;
    std::string publication_path;
    std::function<bool(std::string*)> write_request;
    try {
        rebuildCommandBlocksFromPaletteLocked();
        if (state_.load(std::memory_order_acquire) == BuildExportState::Failed) return;
        publication_path = buildExportNativePublicationPath(output_stem);
        published_output_path_ = publication_path;
        const auto cancellation_requested = [this, generation]() {
            return generation_.load(std::memory_order_acquire) != generation;
        };
        if (use_native) {
            InfiniteczBuildWriteRequest request;
            request.output_path = publication_path;
            request.display_name = display_name_;
            request.origin_x = min_x_;
            request.origin_y = min_y_;
            request.origin_z = min_z_;
            request.width = width_;
            request.height = height_;
            request.length = length_;
            request.palette = std::move(palette_);
            request.block_indices = std::move(block_indices_);
            request.raw_blocks = std::move(raw_blocks_);
            request.command_blocks = std::move(command_blocks_);
            request.cancellation_requested = cancellation_requested;
            request.publish_mutex = publish_mutex_;
            request.publication_committed = &publication_committed_;
            write_request = [request = std::move(request)](std::string* error) mutable {
                return InfiniteczBuildWriter::write(request, error);
            };
        }
        palette_.clear();
        palette_ids_.clear();
        block_state_ids_.clear();
        raw_blocks_.clear();
        raw_block_lookup_.clear();
        raw_state_json_.clear();
        raw_client_identifiers_.clear();
        raw_client_aux_.clear();
        raw_state_attempted_.clear();
        world_reader_.reset();
        block_state_source_generation_ = world_reader_.sourceGeneration();
        batches_.clear();
        setStatus("Writing lossless Infinitecz building file (.infinity)");
    } catch (const std::bad_alloc&) {
        failLocked("Not enough memory to initialize the Infinitecz writer");
        return;
    } catch (...) {
        failLocked("Cannot initialize the Infinitecz writer");
        return;
    }
    writer_running_.store(true, std::memory_order_release);
    try {
        writer_thread_ = std::thread([this, generation, checkpoint_path,
                                       output_stem, publication_path,
                                       write_request = std::move(write_request)]() mutable {
            // Nothing is allowed to escape a std::thread entry point: an
            // allocation failure in the writer must become a terminal export
            // state instead of invoking std::terminate on the process.
            std::string error;
            bool written = false;
            const char* exception_reason = nullptr;
            try {
                written = write_request(&error);
            } catch (const std::bad_alloc&) {
                exception_reason = "Infinitecz writer ran out of memory";
            } catch (const std::exception&) {
                exception_reason = "Infinitecz writer threw an exception";
            } catch (...) {
                exception_reason = "Infinitecz writer threw an unknown exception";
            }

            if (exception_reason) {
                // The writer owns the normal cleanup path. An exception can
                // bypass it, so remove only this request's temporary file.
                try {
                    const std::string part_path = publication_path + ".part";
                    std::remove(part_path.c_str());
                    const std::string plaintext_part_path =
                        publication_path + ".plaintext.part";
                    std::remove(plaintext_part_path.c_str());
                    const std::string compressed_part_path =
                        publication_path + ".compressed.part";
                    std::remove(compressed_part_path.c_str());
                } catch (...) {
                }
            }

            std::string stale_publication_error;
            if (written) {
                removeStaleBuildExportPublications(
                    output_stem, publication_path, &stale_publication_error);
            }

            try {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                if (generation_.load(std::memory_order_acquire) == generation &&
                    state_.load(std::memory_order_acquire) == BuildExportState::Writing) {
                    if (exception_reason) {
                        state_.store(BuildExportState::Failed, std::memory_order_release);
                        setStatus(exception_reason);
                    } else if (written) {
                        state_.store(BuildExportState::Completed, std::memory_order_release);
                        std::string cleanup_error;
                        if (BuildExportCheckpoint::discard(checkpoint_path, &cleanup_error)) {
                            checkpoint_initialized_ = false;
                            setStatus(stale_publication_error.empty()
                                ? "Export completed: " + publication_path
                                : "Export completed, but stale output cleanup failed: " +
                                      stale_publication_error);
                        } else {
                            setStatus("Export completed, but checkpoint cleanup failed: " +
                                      cleanup_error +
                                      (stale_publication_error.empty()
                                          ? std::string()
                                          : "; stale output cleanup also failed: " +
                                                stale_publication_error));
                        }
                    } else {
                        state_.store(BuildExportState::Failed, std::memory_order_release);
                        setStatus(error.empty() ? "Infinitecz writer failed" : error);
                    }
                }
            } catch (...) {
                // Status formatting/allocation must not make the worker escape.
                // Preserve the terminal state when the failure publication
                // itself runs out of memory.
                try {
                    if (generation_.load(std::memory_order_acquire) == generation) {
                        BuildExportState expected = BuildExportState::Writing;
                        state_.compare_exchange_strong(
                            expected, BuildExportState::Failed,
                            std::memory_order_acq_rel, std::memory_order_acquire);
                    }
                } catch (...) {
                }
            }
            writer_running_.store(false, std::memory_order_release);
        });
    } catch (...) {
        writer_running_.store(false, std::memory_order_release);
        failLocked("Cannot start Infinitecz writer thread");
    }
}

void BuildExportRuntime::failLocked(const std::string& reason) {
    const BuildExportState previous = state_.load(std::memory_order_acquire);
    bool checkpoint_saved = true;
    std::string checkpoint_error;
    if (checkpoint_initialized_ && previous != BuildExportState::Writing &&
        !checkpoint_commit_in_progress_) {
        checkpoint_saved = commitCheckpointLocked(true, &checkpoint_error);
    }
    CancelBuildExportTeleport();
    state_.store(BuildExportState::Failed, std::memory_order_release);
    releaseScanMemoryLocked();
    if (!checkpoint_initialized_) {
        setStatus(reason);
    } else if (checkpoint_saved) {
        setStatus(reason + "; export checkpoint retained");
    } else {
        setStatus(reason + "; latest progress could not be checkpointed (the previous durable "
                  "checkpoint is retained): " + checkpoint_error);
    }
}

void BuildExportRuntime::cancel() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const BuildExportState current = state_.load(std::memory_order_acquire);
    if (!isActiveState(current)) return;
    std::lock_guard<std::mutex> publish_lock(*publish_mutex_);
    if (current == BuildExportState::Writing &&
        publication_committed_.load(std::memory_order_acquire)) {
        state_.store(BuildExportState::Completed, std::memory_order_release);
        if (BuildExportCheckpoint::discard(checkpoint_path_, nullptr)) {
            checkpoint_initialized_ = false;
        }
        setStatus("Export completed: " +
                  (published_output_path_.empty() ? output_path_ : published_output_path_));
        return;
    }
    bool checkpoint_saved = true;
    std::string checkpoint_error;
    if (checkpoint_initialized_ && current != BuildExportState::Writing) {
        checkpoint_saved = commitCheckpointLocked(true, &checkpoint_error);
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
    CancelBuildExportTeleport();
    state_.store(BuildExportState::Cancelled, std::memory_order_release);
    releaseScanMemoryLocked();
    if (!checkpoint_initialized_) {
        setStatus("Export cancelled");
    } else if (checkpoint_saved) {
        setStatus("Export cancelled; checkpoint retained and can be resumed");
    } else {
        setStatus("Export cancelled; final progress could not be checkpointed (the previous "
                  "durable checkpoint can still be resumed): " + checkpoint_error);
    }
}

BuildExportState BuildExportRuntime::state() const {
    return state_.load(std::memory_order_acquire);
}

std::string BuildExportRuntime::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

uint64_t BuildExportRuntime::totalBlockCount() const {
    return total_blocks_.load(std::memory_order_acquire);
}

uint64_t BuildExportRuntime::processedBlockCount() const {
    return processed_blocks_.load(std::memory_order_acquire);
}

bool BuildExportRuntime::teleportAllowed() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return isActiveState(state_.load(std::memory_order_acquire)) && allow_teleport_;
}

BuildExportTravelMode BuildExportRuntime::travelMode() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return travel_mode_;
}

bool BuildExportRuntime::requestNextRegionTeleport() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const BuildExportState current = state_.load(std::memory_order_acquire);
    // WaitingForPlayer is entered at the boundary before a semi-automatic
    // region starts. Scanning and CapturingContainers deliberately reject the
    // click, so a request can never be queued while the current region still
    // has block or inventory work outstanding.
    if (travel_mode_ != BuildExportTravelMode::SemiAutomatic ||
        current != BuildExportState::WaitingForPlayer ||
        current_batch_ >= batches_.size()) {
        return false;
    }
    // Leave WaitingForPlayer before returning to the overlay so its worker
    // completion cannot re-enable the button before the next game tick. The
    // actual teleport remains in beginBatchLoadLocked() on the game thread.
    allow_teleport_ = true;
    batches_[current_batch_].manual_retry_wait = false;
    preserve_cursor_on_prepare_ = true;
    setStatus("Starting region " + std::to_string(current_batch_ + 1) + "/" +
              std::to_string(batches_.size()) + " on the next game tick");
    state_.store(BuildExportState::Preparing, std::memory_order_release);
    return true;
}

bool BuildExportRuntime::waitingForPlayer() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const BuildExportState current = state_.load(std::memory_order_acquire);
    return current == BuildExportState::WaitingForPlayer ||
        (current == BuildExportState::CapturingContainers && !allow_teleport_ &&
         container_phase_ == ContainerCapturePhase::Navigate &&
         !playerNearCurrentContainerLocked());
}

bool BuildExportRuntime::travelTarget(BuildExportTravelTarget* output) const {
    if (!output) return false;
    *output = BuildExportTravelTarget{};

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const BuildExportState current = state_.load(std::memory_order_acquire);
    if (!isActiveState(current) || current == BuildExportState::Writing ||
        current_batch_ >= batches_.size()) {
        return false;
    }

    if (current == BuildExportState::CapturingContainers &&
        container_capture_pending_ && container_cursor_ < container_targets_.size()) {
        const ContainerCaptureTarget& target =
            container_targets_[static_cast<size_t>(container_cursor_)];
        output->valid = true;
        output->x = target.x;
        output->y = target.y == std::numeric_limits<int32_t>::max()
            ? target.y : target.y + 1;
        output->z = target.z;
        const bool near = playerNearCurrentContainerLocked(&output->distance_blocks);
        output->waiting = !allow_teleport_ && !near;
        output->batch_index = static_cast<uint32_t>(current_batch_ + 1);
        output->batch_count = static_cast<uint32_t>(batches_.size());
        return true;
    }

    const ScanBatch& batch = batches_[current_batch_];
    output->valid = true;
    output->x = batchCenterCoordinate(batch.min_x, batch.max_x);
    output->y = clampedTargetY(max_y_, allow_teleport_ ? 12 : 1);
    output->z = batchCenterCoordinate(batch.min_z, batch.max_z);
    output->waiting = current == BuildExportState::WaitingForPlayer;
    output->batch_index = static_cast<uint32_t>(current_batch_ + 1);
    output->batch_count = static_cast<uint32_t>(batches_.size());
    // playerNearCurrentBatchLocked also computes a distance when the player is
    // outside the arrival radius. Its boolean result is intentionally ignored.
    playerNearCurrentBatchLocked(&output->distance_blocks);
    return true;
}

bool BuildExportRuntime::travelTarget(int32_t* x, int32_t* y, int32_t* z,
                                      int32_t* distance_blocks, bool* waiting) const {
    BuildExportTravelTarget target;
    if (!travelTarget(&target)) return false;
    if (x) *x = target.x;
    if (y) *y = target.y;
    if (z) *z = target.z;
    if (distance_blocks) *distance_blocks = target.distance_blocks;
    if (waiting) *waiting = target.waiting;
    return true;
}

bool BuildExportRuntime::playerBlockPosition(int32_t* x, int32_t* y, int32_t* z) const {
    if (!x || !y || !z) return false;
    std::lock_guard<std::mutex> lock(position_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!cached_position_valid_ || cached_position_at_.time_since_epoch().count() == 0 ||
        now < cached_position_at_ || now - cached_position_at_ > std::chrono::seconds(2)) {
        return false;
    }
    *x = cached_x_;
    *y = cached_y_;
    *z = cached_z_;
    return true;
}

void BuildExportRuntime::setStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = status;
}

bool BuildExportRuntime::nativeRawIdentityAuthoritative(std::string_view name) {
    const std::string_view leaf = rawSnapshotLeaf(name);
    if (leaf.find("sign") != std::string_view::npos || leaf == "fence_gate") {
        return true;
    }
    constexpr std::string_view kFenceGateSuffix = "_fence_gate";
    return leaf.size() > kFenceGateSuffix.size() &&
        leaf.substr(leaf.size() - kFenceGateSuffix.size()) == kFenceGateSuffix;
}

uint16_t BuildExportRuntime::rawSnapshotAuxForExport(uint16_t native_aux,
                                                     bool client_aux_available,
                                                     uint16_t client_aux,
                                                     std::string_view leaf,
                                                     std::string_view state_json) {
    // GetBlock(name, aux) is the public ModSDK representation accepted by the
    // target's block commands. The raw Block field is ABI-specific and is only
    // a fallback when the SDK snapshot cannot provide an aux value.
    if (client_aux_available) return client_aux;

    // Modern Bedrock stairs store their orientation in weirdo_direction and
    // upside_down_bit state properties, not in the legacy Block.aux field
    // (which can be 0 regardless of facing).  When client_aux is unavailable,
    // decode the orientation directly from the native state JSON so the palette
    // entry and the .infinity raw snapshot both carry the correct direction.
    const bool is_stair = !leaf.empty() && (leaf.find("stairs") != std::string_view::npos ||
                                             leaf == "normal_stone_stairs");
    if (is_stair && !state_json.empty()) {
        const auto parseInt = [](std::string_view json, std::string_view key,
                                 int fallback) -> int {
            // Minimal JSON integer property reader: find "key":N or "key": N
            const std::string needle = "\"" + std::string(key) + "\"";
            const size_t pos = json.find(needle);
            if (pos == std::string_view::npos) return fallback;
            size_t cursor = pos + needle.size();
            while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == ':')) ++cursor;
            if (cursor >= json.size()) return fallback;
            bool neg = false;
            if (json[cursor] == '-') { neg = true; ++cursor; }
            if (cursor >= json.size() || json[cursor] < '0' || json[cursor] > '9') return fallback;
            int value = 0;
            while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
                value = value * 10 + (json[cursor] - '0');
                ++cursor;
            }
            return neg ? -value : value;
        };
        const auto parseBool = [](std::string_view json, std::string_view key,
                                  bool fallback) -> bool {
            const std::string needle = "\"" + std::string(key) + "\"";
            const size_t pos = json.find(needle);
            if (pos == std::string_view::npos) return fallback;
            size_t cursor = pos + needle.size();
            while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == ':')) ++cursor;
            if (cursor + 4 <= json.size() && json.substr(cursor, 4) == "true") return true;
            if (cursor + 5 <= json.size() && json.substr(cursor, 5) == "false") return false;
            return fallback;
        };
        const int wd = parseInt(state_json, "weirdo_direction", -1);
        if (wd >= 0 && wd <= 3) {
            const bool upside_down = parseBool(state_json, "upside_down_bit", false);
            return static_cast<uint16_t>((wd & 3) | (upside_down ? 4 : 0));
        }
    }

    return native_aux;
}

bool BuildExportRuntime::isPlaceholderName(const std::string& name) {
    if (name.empty()) return true;
    return containsCaseInsensitive(name, "unknown") ||
        containsCaseInsensitive(name, "placeholder") ||
        containsCaseInsensitive(name, "reserved") ||
        containsCaseInsensitive(name, "client_request") ||
        containsCaseInsensitive(name, "invisible_bedrock") ||
        containsCaseInsensitive(name, "client") ||
        containsCaseInsensitive(name, "invisible");
}

}  // namespace build_import
