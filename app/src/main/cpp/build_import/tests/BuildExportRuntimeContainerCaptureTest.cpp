#include "../BuildExportRuntime.h"

#include "../BuildExportCheckpoint.h"
#include "../BuildExportPaths.h"
#include "../ContainerCaptureMailbox.h"
#include "../ContainerClosePacketSender.h"
#include "../ContainerEntityCodec.h"
#include "../ContainerOpenPacketSender.h"
#include "../InfiniteczBuildWriter.h"
#include "../ItemRuntimeRegistry.h"
#include "../SchematicWriter.h"
#include "../../main.h"
#include "../../tp/LightningEffect.h"
#include "../../tp/MinecraftUpdateHook.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kTargetX = -189;
constexpr int32_t kTargetY = -60;
constexpr int32_t kTargetZ = 61;
constexpr uintptr_t kFakeMinecraftBase = 0x10000000U;

struct OpenRequestCall {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    const void* native_block = nullptr;
    int face = 0;
};

struct CloseCall {
    uint8_t container_id = 0;
    uint8_t container_type = 0;
};

std::vector<OpenRequestCall> g_open_requests;
std::vector<CloseCall> g_closes;
std::vector<uint64_t> g_armed_tokens;
std::vector<uint64_t> g_cancelled_tokens;
uint64_t g_active_token = 0;
bool g_receive_hook_ready = false;
bool g_receive_hook_init_succeeds = true;
uint32_t g_receive_hook_init_calls = 0;
bool g_report_container_opened = true;

void resetHarness() {
    g_open_requests.clear();
    g_closes.clear();
    g_armed_tokens.clear();
    g_cancelled_tokens.clear();
    g_active_token = 0;
    g_receive_hook_ready = false;
    g_receive_hook_init_succeeds = true;
    g_receive_hook_init_calls = 0;
    g_report_container_opened = true;
}

}  // namespace

std::atomic<uintptr_t> Main::baseAddress{kFakeMinecraftBase};

namespace build_import {

struct BuildExportRuntimeContainerCaptureTestAccess {
    static std::unique_ptr<BuildExportRuntime> create() {
        return std::unique_ptr<BuildExportRuntime>(new BuildExportRuntime());
    }

    static void prepare(BuildExportRuntime* runtime) {
        assert(runtime != nullptr);
        runtime->state_.store(BuildExportState::CapturingContainers,
                              std::memory_order_release);
        runtime->checkpoint_initialized_ = false;
        runtime->allow_teleport_ = false;
        runtime->container_capture_pending_ = true;
        runtime->container_targets_.push_back({0U, kTargetX, kTargetY, kTargetZ});
        runtime->container_cursor_ = 0;
        runtime->container_attempt_ = 1;
        runtime->container_phase_ = BuildExportRuntime::ContainerCapturePhase::Navigate;
        runtime->raw_blocks_.push_back(
            {0, 0, 0, "minecraft:chest", 0, 54, std::string(), std::string()});
        runtime->raw_block_lookup_.emplace(0U, 0U);

        std::lock_guard<std::mutex> lock(runtime->position_mutex_);
        runtime->cached_position_valid_ = true;
        runtime->cached_x_ = kTargetX;
        runtime->cached_y_ = kTargetY;
        runtime->cached_z_ = kTargetZ;
        runtime->cached_position_at_ = std::chrono::steady_clock::now();
    }

    static void tick(BuildExportRuntime* runtime) {
        runtime->tickContainerCaptureLocked();
    }

    static uint32_t attempt(const BuildExportRuntime* runtime) {
        return runtime->container_attempt_;
    }

    static int waitTicks(const BuildExportRuntime* runtime) {
        return runtime->container_wait_ticks_;
    }

    static bool waitingForPackets(const BuildExportRuntime* runtime) {
        return runtime->container_phase_ ==
            BuildExportRuntime::ContainerCapturePhase::WaitingForPackets;
    }

    static void prepareSemiAutomaticAdvance(BuildExportRuntime* runtime,
                                            BuildExportState state) {
        assert(runtime != nullptr);
        runtime->travel_mode_ = BuildExportTravelMode::SemiAutomatic;
        runtime->allow_teleport_ = false;
        runtime->height_ = 1;
        runtime->current_batch_ = 0;
        runtime->batches_.clear();
        runtime->batches_.push_back({0, 15, 0, 15});
        runtime->state_.store(state, std::memory_order_release);
    }

    static bool nativeRawIdentityAuthoritative(std::string_view name) {
        return BuildExportRuntime::nativeRawIdentityAuthoritative(name);
    }

    static uint16_t rawSnapshotAuxForExport(uint16_t native_aux,
                                            bool client_aux_available,
                                            uint16_t client_aux) {
        return BuildExportRuntime::rawSnapshotAuxForExport(
            native_aux, client_aux_available, client_aux);
    }
};

bool NativeWorldReader::open() {
    return true;
}

void NativeWorldReader::reset() {}

bool NativeWorldReader::getBlockView(int32_t x, int32_t y, int32_t z,
                                     NativeBlockView* output) {
    static const std::string kChest = "minecraft:chest";
    assert(x == kTargetX && y == kTargetY && z == kTargetZ);
    assert(output != nullptr);
    output->name = &kChest;
    output->type_token = &kChest;
    output->aux = 0;
    output->legacy_id = 54;
    return true;
}

bool NativeWorldAccess::getLocalPlayerBlockPosition(int32_t*, int32_t*, int32_t*) {
    return false;
}

uintptr_t NativeWorldAccess::dimensionToken() {
    return 1U;
}

const char* NativeWorldAccess::lastWorldReaderDiagnostic() {
    return "host test";
}

bool NativeWorldAccess::getCommandBlockData(int32_t, int32_t, int32_t,
                                            NativeCommandBlockData*) {
    return false;
}

bool NativeWorldAccess::getBlockSnapshot(int32_t, int32_t, int32_t,
                                         NativeBlockSnapshot*, bool, int32_t) {
    return false;
}

bool IsItemRuntimeRegistryReady() noexcept {
    return true;
}

void ArmContainerCapture(uint64_t token, int32_t x, int32_t y, int32_t z) {
    assert(token != 0U);
    assert(x == kTargetX && y == kTargetY && z == kTargetZ);
    g_active_token = token;
    g_armed_tokens.push_back(token);
}

void CancelContainerCapture(uint64_t token) noexcept {
    if (token == 0U) return;
    g_cancelled_tokens.push_back(token);
    if (g_active_token == token) g_active_token = 0U;
}

ContainerCapturePollState PollContainerCapture(uint64_t token,
                                               ContainerCaptureResult* output) {
    assert(token != 0U && token == g_active_token);
    if (output && g_report_container_opened) {
        output->token = token;
        output->x = kTargetX;
        output->y = kTargetY;
        output->z = kTargetZ;
        output->container_id = 17U;
        output->container_type = 0U;
        output->container_opened = true;
        output->container_closed = false;
    }
    return g_report_container_opened
        ? ContainerCapturePollState::WaitingForContent
        : ContainerCapturePollState::WaitingForOpen;
}

bool ObserveContainerCapturePacket(std::string_view) noexcept { return false; }

bool PollContainerCaptureQuarantine(ContainerCaptureQuarantine* output) noexcept {
    if (output) *output = {};
    return false;
}

void MarkContainerCaptureQuarantineCloseSent(uint8_t, uint8_t) noexcept {}

bool encodeCapturedContainerItemsJson(const ContainerCaptureResult&,
                                      std::string*, std::string*) {
    assert(false && "timeout test must not encode container contents");
    return false;
}

bool normalizeContainerEntityJson(std::string_view, std::string*, std::string*) {
    assert(false && "timeout test must not normalize container contents");
    return false;
}

bool normalizeSignEntityJson(std::string_view, std::string*, std::string*) {
    assert(false && "container timeout test must not normalize sign contents");
    return false;
}

bool deferredContainerIdentifier(std::string_view identifier) {
    return identifier.find("chest") != std::string_view::npos;
}

bool packetCapturableContainerIdentifier(std::string_view identifier) {
    return deferredContainerIdentifier(identifier);
}

bool resolveBuildExportPath(const std::string&, BuildExportPathResolution*) {
    return false;
}

std::string buildExportNativePublicationPath(const std::string&) {
    return {};
}

bool removeStaleBuildExportPublications(const std::string&, const std::string&,
                                        std::string*) {
    return false;
}

bool InfiniteczBuildWriter::write(const InfiniteczBuildWriteRequest&, std::string*) {
    return false;
}

std::string SchematicWriter::blockStateForExport(std::string raw_name, uint16_t,
                                                 int32_t) {
    return raw_name;
}

bool BuildExportCheckpoint::hasArtifacts(const std::string&) {
    return false;
}

bool BuildExportCheckpoint::createJournal(const std::string&, std::string*) {
    return false;
}

bool BuildExportCheckpoint::appendJournal(const std::string&, const uint16_t*, size_t,
                                          std::string*) {
    return false;
}

bool BuildExportCheckpoint::syncJournal(const std::string&, std::string*) {
    return false;
}

bool BuildExportCheckpoint::truncateJournal(const std::string&, uint64_t,
                                            std::string*) {
    return false;
}

std::string BuildExportCheckpoint::journalPath(const std::string&) {
    return {};
}

bool BuildExportCheckpoint::saveAtomically(
        const std::string&, const BuildExportCheckpointSnapshot&,
        const std::vector<std::string>&,
        const std::vector<CommandBlockRecord>&,
        const std::vector<SchematicRawBlock>&, std::string*) {
    return false;
}

std::optional<BuildExportCheckpointSnapshot> BuildExportCheckpoint::load(
        const std::string&, std::string*) {
    return std::nullopt;
}

bool BuildExportCheckpoint::discard(const std::string&, std::string*) {
    return false;
}

bool ContainerOpenPacketSender::send(int32_t x, int32_t y, int32_t z,
                                      const void* native_block, int face,
                                      std::string* error,
                                      int32_t /* expected_hotbar_slot */,
                                      bool /* has_expected_network_stack_id */,
                                      int32_t /* expected_network_stack_id */) {
    g_open_requests.push_back({x, y, z, native_block, face});
    if (error) error->clear();
    return true;
}

bool ContainerClosePacketSender::send(uint8_t container_id,
                                      uint8_t container_type,
                                      std::string* error) {
    g_closes.push_back({container_id, container_type});
    if (error) error->clear();
    return true;
}

}  // namespace build_import

bool LightningEffect::init(uintptr_t base_address) {
    assert(base_address == kFakeMinecraftBase);
    ++g_receive_hook_init_calls;
    if (g_receive_hook_init_succeeds) g_receive_hook_ready = true;
    return g_receive_hook_ready;
}

bool LightningEffect::isReceiveHookReady() {
    return g_receive_hook_ready;
}

uintptr_t GetCachedDimensionTokenForWorld(const std::string&) {
    return 0U;
}

bool RequestTeleport(float, float, float) {
    return false;
}

bool IsTeleportPermissionProbePending() {
    return false;
}

void CancelPendingTeleportRequest() {}

namespace {

void advanceToPacketWait(build_import::BuildExportRuntime* runtime) {
    using Access = build_import::BuildExportRuntimeContainerCaptureTestAccess;
    for (int guard = 0; guard < 16 && !Access::waitingForPackets(runtime); ++guard) {
        Access::tick(runtime);
    }
    assert(Access::waitingForPackets(runtime));
    assert(Access::waitTicks(runtime) == 100);
}

}  // namespace

int main() {
    using Access = build_import::BuildExportRuntimeContainerCaptureTestAccess;

    assert(Access::nativeRawIdentityAuthoritative("minecraft:standing_sign"));
    assert(Access::nativeRawIdentityAuthoritative("minecraft:pale_oak_hanging_sign"));
    assert(Access::nativeRawIdentityAuthoritative("minecraft:fence_gate"));
    assert(Access::nativeRawIdentityAuthoritative("minecraft:dark_oak_fence_gate"));
    assert(!Access::nativeRawIdentityAuthoritative("minecraft:oak_fence"));
    assert(!Access::nativeRawIdentityAuthoritative("minecraft:oak_stairs"));

    // The native identifier preserves the exact material, but the public SDK
    // aux preserves command-compatible orientation. A modern internal Block
    // encoding must not overwrite the standing/wall/hanging sign or gate data.
    assert(Access::rawSnapshotAuxForExport(0U, true, 12U) == 12U);
    assert(Access::rawSnapshotAuxForExport(3U, true, 5U) == 5U);
    assert(Access::rawSnapshotAuxForExport(0x0182U, true, 0x0085U) == 0x0085U);
    assert(Access::rawSnapshotAuxForExport(0U, true, 7U) == 7U);
    assert(Access::rawSnapshotAuxForExport(11U, false, 0U) == 11U);

    resetHarness();
    auto runtime = Access::create();
    Access::prepare(runtime.get());

    for (uint32_t expected_attempt = 1; expected_attempt <= 6; ++expected_attempt) {
        advanceToPacketWait(runtime.get());
        assert(g_open_requests.size() == expected_attempt);
        assert(g_armed_tokens.size() == expected_attempt);
        assert(g_armed_tokens.back() != 0U);
        if (g_armed_tokens.size() > 1U) {
            assert(g_armed_tokens[g_armed_tokens.size() - 1U] !=
                   g_armed_tokens[g_armed_tokens.size() - 2U]);
        }

        for (int tick = 0; tick < 100; ++tick) {
            Access::tick(runtime.get());
            const bool terminal_tick = expected_attempt == 6U && tick == 99;
            assert(runtime->state() == (terminal_tick
                ? build_import::BuildExportState::Failed
                : build_import::BuildExportState::CapturingContainers));
        }

        if (expected_attempt < 6U) {
            assert(Access::attempt(runtime.get()) == expected_attempt + 1U);
            // A retry is also subject to a ten-tick (~500 ms) receive-queue
            // cooldown, so repeated packet failures cannot hammer the server.
            assert(Access::waitTicks(runtime.get()) == 10);
        }
    }

    assert(runtime->state() == build_import::BuildExportState::Failed);
    assert(g_open_requests.size() == 6U);
    assert(g_closes.size() == 6U);
    assert(g_cancelled_tokens.size() == 6U);
    constexpr int kExpectedFaces[] = {1, 2, 3, 4, 5, 0};
    for (size_t index = 0; index < g_open_requests.size(); ++index) {
        const OpenRequestCall& call = g_open_requests[index];
        assert(call.x == kTargetX && call.y == kTargetY && call.z == kTargetZ);
        assert(call.native_block != nullptr);
        assert(call.face == kExpectedFaces[index]);
    }
    for (const CloseCall& call : g_closes) {
        assert(call.container_id == 17U && call.container_type == 0U);
    }
    assert(runtime->status().find("after 6 attempts") != std::string::npos);
    assert(runtime->status().find("InventoryContent packet timed out") !=
           std::string::npos);

    // A transient early-install failure must be repaired on the game thread
    // before the first interaction is armed.
    assert(g_receive_hook_init_calls == 1U);
    assert(g_receive_hook_ready);

    // ContainerOpen and InventoryContent need independent deadlines. Simulate
    // the open packet arriving on the last tick of the original request
    // window; the exporter must still wait a full content window before it
    // consumes an attempt.
    runtime.reset();
    resetHarness();
    g_report_container_opened = false;
    runtime = Access::create();
    Access::prepare(runtime.get());
    advanceToPacketWait(runtime.get());
    for (int tick = 0; tick < 99; ++tick) {
        Access::tick(runtime.get());
    }
    assert(Access::attempt(runtime.get()) == 1U);
    assert(Access::waitTicks(runtime.get()) == 1);
    g_report_container_opened = true;
    Access::tick(runtime.get());
    assert(Access::attempt(runtime.get()) == 1U);
    assert(Access::waitTicks(runtime.get()) == 99);
    for (int tick = 0; tick < 98; ++tick) {
        Access::tick(runtime.get());
    }
    assert(Access::attempt(runtime.get()) == 1U);
    assert(Access::waitTicks(runtime.get()) == 1);
    Access::tick(runtime.get());
    assert(Access::attempt(runtime.get()) == 2U);
    assert(Access::waitTicks(runtime.get()) == 10);

    // A semi-automatic click is accepted only at the explicit region
    // boundary. In particular, it cannot be queued while block scanning or
    // container capture for the current region is still in progress.
    runtime.reset();
    resetHarness();
    runtime = Access::create();
    Access::prepareSemiAutomaticAdvance(
        runtime.get(), build_import::BuildExportState::Scanning);
    assert(!runtime->requestNextRegionTeleport());
    Access::prepareSemiAutomaticAdvance(
        runtime.get(), build_import::BuildExportState::CapturingContainers);
    assert(!runtime->requestNextRegionTeleport());
    Access::prepareSemiAutomaticAdvance(
        runtime.get(), build_import::BuildExportState::WaitingForPlayer);
    assert(runtime->requestNextRegionTeleport());
    assert(!runtime->requestNextRegionTeleport());

    runtime.reset();
    resetHarness();
    g_report_container_opened = false;
    runtime = Access::create();
    Access::prepare(runtime.get());
    for (uint32_t expected_attempt = 1; expected_attempt <= 6; ++expected_attempt) {
        advanceToPacketWait(runtime.get());
        assert(g_open_requests.size() == expected_attempt);
        for (int tick = 0; tick < 100; ++tick) {
            Access::tick(runtime.get());
            const bool terminal_tick = expected_attempt == 6U && tick == 99;
            assert(runtime->state() == (terminal_tick
                ? build_import::BuildExportState::Failed
                : build_import::BuildExportState::CapturingContainers));
        }
    }
    assert(g_open_requests.size() == 6U);
    assert(g_armed_tokens.size() == 6U);
    assert(g_cancelled_tokens.size() == 6U);
    assert(g_closes.empty());
    assert(runtime->status().find("after 6 attempts") != std::string::npos);
    assert(runtime->status().find("ContainerOpen packet timed out") !=
           std::string::npos);

    runtime.reset();
    resetHarness();
    g_receive_hook_init_succeeds = false;
    runtime = Access::create();
    Access::prepare(runtime.get());
    for (int guard = 0;
         guard < 128 && runtime->state() == build_import::BuildExportState::CapturingContainers;
         ++guard) {
        Access::tick(runtime.get());
    }
    assert(runtime->state() == build_import::BuildExportState::Failed);
    assert(g_receive_hook_init_calls == 6U);
    assert(g_open_requests.empty());
    assert(g_armed_tokens.empty());
    assert(g_closes.empty());
    assert(g_cancelled_tokens.empty());
    assert(runtime->status().find("after 6 attempts") != std::string::npos);
    assert(runtime->status().find("container packet receive hook is unavailable") !=
           std::string::npos);
    return 0;
}
