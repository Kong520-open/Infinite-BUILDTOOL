#include "BuildProjectionRenderer.h"

#include "BuildImportTypes.h"
#include "BuildProjectionOutline.h"
#include "ProjectionBlockIdentity.h"
#include "ProjectionWorldMatchRuntime.h"
#include "../main.h"
#include "../tp/FunctionsAddress.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"
#include "dobby.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#define LOG_TAG "Infinitecz_BuildProjection"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace build_import {
namespace {

constexpr size_t kUploadBudgetPerFrame = 2U * 1024U * 1024U;
constexpr size_t kTextureUploadBudgetPerFrame = 2U * 1024U * 1024U;
constexpr size_t kCpuReadBudgetPerFrame = 2U * 1024U * 1024U;
constexpr auto kCpuReadStartBudgetPerFrame = std::chrono::microseconds(1500);
constexpr size_t kMaximumCpuLoadsPerFrame = 32;
constexpr size_t kMaximumUploadsPerFrame = 32;
constexpr size_t kMaximumFillInstancesPerFrame = 300000;
constexpr size_t kMaximumDrawCallsPerFrame = 1024;
constexpr size_t kMaximumFillDrawCallsPerFrame = 768;
constexpr size_t kReservedOutlineDrawCallsPerFrame =
    kMaximumDrawCallsPerFrame - kMaximumFillDrawCallsPerFrame;
constexpr size_t kMaximumGroupsInspectedPerFrame = 16384;
constexpr size_t kMaximumGroupsInspectedPerColumn = 256;
constexpr size_t kMaximumResidentBuffers = 4096;
constexpr size_t kMaximumResidentBufferBytes = 32U * 1024U * 1024U;
constexpr size_t kMaximumEvictionsPerFrame = 64;
// Worker threads may finish several nearby source partitions while the game is
// drawing.  Keep their CPU-only handoff bounded, then let the render thread
// append a small amount of metadata/spool data per frame.
constexpr size_t kMaximumPendingLazyBytes = 16U * 1024U * 1024U;
constexpr size_t kLazyCommitBudgetPerFrame = 768U * 1024U;
constexpr size_t kMaximumLazyGroupsCommittedPerFrame = 8;
constexpr int32_t kMinimumRangeChunks = 1;
// Large city schematics can exceed the old 512-block radius even when their
// parser output is complete. Keep the normal default small, but let the UI
// request a 1024-block view before users mistake distance culling for holes.
constexpr int32_t kMaximumRangeChunks = 64;
constexpr int32_t kMaximumOutlineRangeChunks = 6;
constexpr int32_t kGroupSpan = 16;
constexpr uint32_t kPositionFixedScale = 256U;
constexpr uint32_t kMaximumFixedPosition =
    static_cast<uint32_t>(kGroupSpan) * kPositionFixedScale;
constexpr uint16_t kPositionValueMask = 0x1fffU;
constexpr uint8_t kPositionMetadataShift = 13U;
constexpr uint16_t kPositionMetadataMask = 0x07U;
constexpr uint16_t kAllOutlineBits =
    static_cast<uint16_t>(kProjectionOutlineBaseMask | kProjectionOutlineCutMask);
constexpr size_t kMaximumOutlinedInstancesPerFrame = 120000;
// The render hook describes only groups that survived its real draw budget to
// the game-tick matcher.  Keep that handoff bounded independently of the
// source projection's total size.
constexpr size_t kMaximumWorldMatchInterestRegions =
    kMaximumDrawCallsPerFrame * 2U;
constexpr size_t kMaximumWorldMatchInterestCandidates =
    kMaximumDrawCallsPerFrame * 2U;
// Current Camera ABI, verified from an on-device sample.  The two map fields
// each point to the first (and active) Matrix block; the view matrix is stored
// directly in the Camera object.  The old Camera+0x298 position member is not
// a world coordinate in this build.
constexpr uintptr_t kCameraModelMapOffset = 0x18;
constexpr uintptr_t kCameraProjectionMapOffset = 0x60;
constexpr uintptr_t kCameraToWorldMatrixOffset = 0xA8;
// The current Level::_render owner exposes the same render-camera origin the
// game uses for its world draw: owner[172] points to camera state and floats
// 903..905 are its X/Y/Z origin.  This is a native render-thread read, not a
// Python/JNI query.
constexpr uintptr_t kLevelRenderCameraStateOffset = 172U * sizeof(void*);
constexpr uintptr_t kLevelRenderCameraPositionOffset = 903U * sizeof(float);
constexpr uint8_t kAllFaces = 0x3fU;
// The two bits between the six face flags and the material pattern are part of
// the on-GPU instance word but were previously unused.  Keep world-match
// presentation there so the projection spool and the 20-byte instance ABI
// remain immutable.
constexpr uint32_t kWorldMatchStateShift = 6U;
constexpr uint32_t kWorldMatchStateMask = 0x03U << kWorldMatchStateShift;
constexpr uint32_t kWorldMatchStateExact = 0x03U << kWorldMatchStateShift;
// Material patterns currently occupy only values 0..5.  Reserve the high
// pattern bit in the transient VBO copy for the game-tick reachability overlay
// rather than changing the compact 20-byte source/spool instance ABI.
constexpr uint32_t kReachabilityPreviewBit = 0x00008000U;
static_assert(kProjectionWorldMatchGroupSpan == kGroupSpan,
              "world-match pages must use the projection group grid");
constexpr uint64_t kInvalidSpoolOffset = std::numeric_limits<uint64_t>::max();
constexpr GLint kBlockTextureUnitIndex = 7;
constexpr GLint kMaterialLookupTextureUnitIndex = 6;
constexpr GLenum kBlockTextureUnit = GL_TEXTURE0 + kBlockTextureUnitIndex;
constexpr GLenum kMaterialLookupTextureUnit =
    GL_TEXTURE0 + kMaterialLookupTextureUnitIndex;

enum class ProjectionRenderDiagnostic : uint8_t {
    NotRendered = 0,
    NoEglContext,
    NoLocalPlayer,
    NoClientInstance,
    NoCamera,
    CameraMatrixLayoutInvalid,
    CameraMatrixInvalid,
    CameraPositionUnavailable,
    CameraPositionInvalid,
    Ready,
};

std::atomic<ProjectionRenderDiagnostic> g_projectionRenderDiagnostic{
    ProjectionRenderDiagnostic::NotRendered};

void setProjectionRenderDiagnostic(ProjectionRenderDiagnostic value) {
    g_projectionRenderDiagnostic.store(value, std::memory_order_release);
}

const char* projectionRenderDiagnosticText(ProjectionRenderDiagnostic value) {
    switch (value) {
        case ProjectionRenderDiagnostic::NotRendered: return "projection renderer has not received a frame";
        case ProjectionRenderDiagnostic::NoEglContext: return "projection render thread has no EGL context";
        case ProjectionRenderDiagnostic::NoLocalPlayer: return "projection local player is unavailable";
        case ProjectionRenderDiagnostic::NoClientInstance:
            return "projection Actor::getClientInstance returned null";
        case ProjectionRenderDiagnostic::NoCamera: return "projection ClientInstance camera is unavailable";
        case ProjectionRenderDiagnostic::CameraMatrixLayoutInvalid:
            return "projection Camera matrix layout is not valid for this game version";
        case ProjectionRenderDiagnostic::CameraMatrixInvalid:
            return "projection Camera matrix data is not finite";
        case ProjectionRenderDiagnostic::CameraPositionUnavailable:
            return "projection Level render camera position is unavailable";
        case ProjectionRenderDiagnostic::CameraPositionInvalid:
            return "projection Camera position is not finite";
        case ProjectionRenderDiagnostic::Ready: return "projection renderer ready";
    }
    return "projection renderer diagnostics unavailable";
}

static_assert(kMaximumFixedPosition <= kPositionValueMask,
              "fixed projection positions must leave metadata bits available");

struct Matrix {
    float values[16]{};
};

#pragma pack(push, 1)
struct ProjectionInstance {
    // Group-local bounds use 8.8 fixed point. A 16-block group needs only
    // 0..4096. The unused top three bits of each coordinate carry the 12 base
    // outline edges and four horizontal cut-plane sides without increasing the
    // disk, CPU, or GPU instance stride.
    uint16_t bounds[6];
    uint8_t color[4];
    // face mask: bits 0..5, material pattern: bits 8..15, material id: 16..31.
    uint32_t packed_material;
};
#pragma pack(pop)

static_assert(sizeof(ProjectionInstance) == 20,
              "projection instance layout must match the GLES attribute stride");

constexpr size_t kMaximumInstancesPerUpload =
    kUploadBudgetPerFrame / sizeof(ProjectionInstance);
static_assert(kMaximumInstancesPerUpload > 0,
              "projection upload budget must hold at least one instance");
static_assert(kMaximumFillDrawCallsPerFrame < kMaximumDrawCallsPerFrame,
              "projection draw budget must reserve calls for outlines");

constexpr uint32_t kInvalidGroupIndex = std::numeric_limits<uint32_t>::max();

struct ProjectionDrawBatch {
    uint32_t group_index = 0;
    uint32_t first_instance = 0;
    uint32_t instance_count = 0;
    float distance_squared = 0.f;
};

struct CompactGroupBounds {
    uint8_t min_x = UINT8_MAX;
    uint8_t min_y = UINT8_MAX;
    uint8_t min_z = UINT8_MAX;
    uint8_t max_x = 0;
    uint8_t max_y = 0;
    uint8_t max_z = 0;

    bool isValid() const {
        return min_x <= max_x && min_y <= max_y && min_z <= max_z;
    }
};

static_assert(sizeof(CompactGroupBounds) == 6,
              "projection group bounds must remain byte-packed");

struct ProjectionGroupFlags {
    uint8_t in_resident_lru : 1;
    uint8_t in_cpu_lru : 1;
    uint8_t interior_only : 1;
    uint8_t has_outline : 1;
    uint8_t gpu_outline_prepared : 1;
    uint8_t reserved : 3;
};

static_assert(sizeof(ProjectionGroupFlags) == 1,
              "projection group flags must share one byte");

struct ProjectionGroup {
    uint64_t spool_offset = kInvalidSpoolOffset;
    // Streaming plans keep almost every group disk-only. Indirection avoids a
    // 24-byte empty vector object in every one of those persistent groups.
    std::unique_ptr<std::vector<ProjectionInstance>> instances;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t origin_z = 0;
    uint32_t spool_byte_count = 0;
    GLuint instance_buffer = 0;
    GLsizei instance_count = 0;
    uint32_t gpu_byte_count = 0;
    uint32_t lru_previous = kInvalidGroupIndex;
    uint32_t lru_next = kInvalidGroupIndex;
    uint32_t cpu_lru_previous = kInvalidGroupIndex;
    uint32_t cpu_lru_next = kInvalidGroupIndex;
    uint32_t last_used_frame = 0;
    uint32_t cpu_last_used_frame = 0;
    // Instances are ordered by integer block Y. These offsets make exact
    // single/range layer rendering one contiguous instanced draw per group.
    std::array<uint16_t, kGroupSpan + 1> layer_offsets{};
    std::array<uint16_t, 2> gpu_source_first{};
    std::array<uint16_t, 2> gpu_source_count{};
    // Disk-backed groups may hold only the one or two source ranges selected by
    // the current layer filter. The vector itself is tightly packed; these
    // fields retain its mapping back to the immutable spool offsets.
    std::array<uint16_t, 2> cpu_source_first{};
    std::array<uint16_t, 2> cpu_source_count{};
    CompactGroupBounds bounds;
    uint8_t gpu_batch_count = 0;
    uint8_t cpu_batch_count = 0;
    ProjectionGroupFlags flags{};
    // The source instances and disk spool never contain game-tick presentation
    // bits. This per-group fingerprint combines world-match diagnostics and
    // the reachability preview, so a changed snapshot makes only visible
    // groups re-upload from their pristine CPU/spool data.
    uint64_t gpu_presentation_fingerprint = 0;
    // Fully enclosed cubes are kept in a separate disk-backed group. Normal
    // projection rendering never reads or uploads them; layer filters use only
    // the one or two groups that contain the newly exposed horizontal cuts.
};

static_assert(sizeof(ProjectionDrawBatch) == 16,
              "projection draw batches must remain compact");
#if defined(__aarch64__)
static_assert(sizeof(ProjectionGroup) == 136,
              "projection group metadata layout changed on arm64");
#endif

struct ProjectionColumn {
    std::vector<uint32_t> all_groups;
    std::vector<uint32_t> surface_groups;
};

struct ProjectionTexturePack {
    // Flattened as material_id * 6 + face. Layer zero is deliberately the
    // programmatic-color fallback; decoded texture layers start at one.
    std::vector<uint16_t> material_face_layers;
    std::vector<uint8_t> rgba_layers;
    int32_t tile_size = 0;
    int32_t layer_count = 0;
};

struct ProjectionPlan {
    std::vector<ProjectionGroup> groups;
    // All groups are indexed for layer-cut rendering. The surface-only index is
    // the common All-mode fast path and avoids even inspecting enclosed groups.
    std::unordered_map<uint64_t, ProjectionColumn> columns;
    std::string spool_path;
    std::string owned_cache_directory;
    std::ifstream spool_reader;
    // The writer is retained only for lazy plans.  It is exclusively consumed
    // by render(), which also owns groups/columns and all GPU/LRU state.
    std::ofstream spool_writer;
    size_t spool_bytes = 0;
    size_t maximum_spool_bytes = 0;
    bool lazy_surface_streaming = false;
    uint64_t topology_revision = 0;
    // Background workers never touch groups or columns.  They place fully
    // extracted CPU groups here, and render() drains the queue under a fixed
    // budget before scanning visible columns.
    std::mutex pending_lazy_mutex;
    std::condition_variable pending_lazy_cv;
    std::deque<ProjectionGroup> pending_lazy_groups;
    size_t pending_lazy_bytes = 0;
    std::string lazy_append_error;
    std::vector<ProjectionMaterialRequest> material_requests;
    // Immutable once a plan is published.  Lazy plans pre-register every
    // source material before texture loading so worker builds can only look up
    // stable IDs, never mutate the palette from another thread.
    std::unordered_map<std::string, uint16_t> material_ids;
    bool material_palette_sealed = false;
    std::shared_ptr<const ProjectionTexturePack> texture_pack;
    uint64_t identity = 0;
    int32_t relative_origin_y = 0;
    uint64_t source_block_count = 0;
    uint64_t detailed_instance_count = 0;
    size_t maximum_cpu_cache_bytes = 0;
    size_t resident_cpu_bytes = 0;
    size_t resident_cpu_group_count = 0;
    uint32_t cpu_lru_head = kInvalidGroupIndex;
    uint32_t cpu_lru_tail = kInvalidGroupIndex;
    size_t resident_buffer_count = 0;
    size_t resident_buffer_bytes = 0;
    uint32_t resident_lru_head = kInvalidGroupIndex;
    uint32_t resident_lru_tail = kInvalidGroupIndex;

    ~ProjectionPlan() {
        spool_reader.close();
        spool_writer.close();
        if (!spool_path.empty()) std::remove(spool_path.c_str());
        if (!owned_cache_directory.empty()) {
#if defined(_WIN32)
            ::_rmdir(owned_cache_directory.c_str());
#else
            ::rmdir(owned_cache_directory.c_str());
#endif
        }
    }
};

// Reachability positions are produced by the local-player game tick and read
// by the renderer. Keep them in the same 16-cube grid as world-match pages so
// one changed nearby candidate does not invalidate every visible VBO.
struct ProjectionReachabilityPreviewGroupState {
    static constexpr size_t kWordCount =
        kProjectionWorldMatchGroupCellCount / 64U;
    std::array<uint64_t, kWordCount> cells{};
    uint64_t fingerprint = 0;
};

static_assert(kProjectionWorldMatchGroupCellCount % 64U == 0U,
              "reachability preview cells must pack into whole words");

struct ProjectionReachabilityPreviewSnapshot {
    uint64_t plan_identity = 0;
    uint64_t revision = 0;
    std::unordered_map<ProjectionWorldMatchGroupKey,
                       std::shared_ptr<const ProjectionReachabilityPreviewGroupState>,
                       ProjectionWorldMatchGroupKeyHash> groups;
};

struct ProjectionStreamBuild {
    ProjectionStreamingOptions options;
    std::shared_ptr<ProjectionPlan> plan;
    std::ofstream spool_writer;
    size_t spool_bytes = 0;
    uint64_t appended_core_blocks = 0;
    std::unordered_map<std::string, uint16_t> material_ids;
};

struct BlockPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const BlockPosition& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct BlockPositionHash {
    size_t operator()(const BlockPosition& position) const {
        uint64_t hash = 1469598103934665603ULL;
        const auto add = [&](uint32_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        add(static_cast<uint32_t>(position.x));
        add(static_cast<uint32_t>(position.y));
        add(static_cast<uint32_t>(position.z));
        return static_cast<size_t>(hash ^ (hash >> 32U));
    }
};

struct GroupPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator<(const GroupPosition& other) const {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }
};

struct LocalBounds {
    float min_x = 0.f;
    float min_y = 0.f;
    float min_z = 0.f;
    float max_x = 1.f;
    float max_y = 1.f;
    float max_z = 1.f;
};

struct BlockModel {
    std::array<LocalBounds, 2> parts{};
    uint8_t part_count = 1;
};

enum class TranslucentCullClass : uint8_t {
    Opaque = 0,
    Glass,
    Ice,
    Water,
    Lava,
    Leaves,
    Slime,
    Honey,
    Beacon,
};

struct CellData {
    size_t block_index = 0;
    // Only a complete one-block cube can hide the complete face of another
    // cube. Thin or multipart models must never create holes in neighbours.
    uint8_t full_cube = 0;
    // Transparent full cubes may have removable contact faces, but must not
    // suppress the face of an adjacent opaque cube.
    uint8_t occludes_adjacent = 0;
    TranslucentCullClass translucent_cull_class = TranslucentCullClass::Opaque;
};

constexpr std::array<OutlineOffset, 6> kFaceNeighborOffsets{{
    {0, 0, -1}, {0, 0, 1}, {-1, 0, 0},
    {1, 0, 0}, {0, -1, 0}, {0, 1, 0},
}};

constexpr size_t neighborCacheIndex(int32_t x, int32_t y, int32_t z) {
    return static_cast<size_t>((z + 1) * 9 + (y + 1) * 3 + (x + 1));
}

bool offsetPrecedesCurrent(const OutlineOffset& offset) {
    if (offset.x != 0) return offset.x < 0;
    if (offset.y != 0) return offset.y < 0;
    return offset.z < 0;
}

std::shared_ptr<ProjectionPlan> g_published_plan;
std::shared_ptr<ProjectionPlan> g_render_plan;
std::unique_ptr<ProjectionStreamBuild> g_stream_build;
std::mutex g_stream_mutex;
const std::shared_ptr<const ProjectionReachabilityPreviewSnapshot>
    g_empty_reachability_preview_snapshot =
        std::make_shared<const ProjectionReachabilityPreviewSnapshot>();
std::shared_ptr<const ProjectionReachabilityPreviewSnapshot>
    g_reachability_preview_snapshot = g_empty_reachability_preview_snapshot;
std::mutex g_reachability_preview_mutex;
uint64_t g_reachability_preview_revision = 0U;
// Plan publication/clearing runs on JNI workers, while g_render_plan and its
// GLES names belong exclusively to the world-render thread. A clear ticket
// lets a later load wait for that handoff instead of overlapping two plans.
std::atomic<uint64_t> g_plan_clear_requested{0};
std::atomic<uint64_t> g_plan_clear_completed{0};
std::mutex g_plan_clear_mutex;
std::condition_variable g_plan_clear_cv;

void completePlanClearTicket(uint64_t ticket) {
    uint64_t completed = g_plan_clear_completed.load(std::memory_order_acquire);
    while (completed < ticket &&
           !g_plan_clear_completed.compare_exchange_weak(
               completed, ticket, std::memory_order_release,
               std::memory_order_acquire)) {
    }
    if (completed < ticket) g_plan_clear_cv.notify_all();
}

void acknowledgePlanClearOnRenderThread() {
    const uint64_t requested = g_plan_clear_requested.load(std::memory_order_acquire);
    completePlanClearTicket(requested);
}

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_outline_enabled{true};
std::atomic<bool> g_reachability_preview_enabled{false};
std::atomic<float> g_fill_alpha{0.25f};
std::atomic<int32_t> g_range_chunks{12};
std::shared_ptr<const ProjectionLayerFilter> g_layer_filter =
    std::make_shared<const ProjectionLayerFilter>();
std::atomic<bool> g_hook_installed{false};
std::atomic<uint64_t> g_spool_sequence{0};
std::atomic<uint64_t> g_plan_sequence{0};
std::mutex g_hook_mutex;

GLuint g_program = 0;
GLuint g_cube_vao = 0;
GLuint g_cube_vertex_buffer = 0;
GLuint g_cube_index_buffer = 0;
GLint g_mvp_location = -1;
GLint g_offset_location = -1;
GLint g_alpha_location = -1;
GLint g_line_pass_location = -1;
GLint g_group_origin_y_location = -1;
GLint g_layer_minimum_location = -1;
GLint g_layer_maximum_location = -1;
GLint g_has_layer_minimum_location = -1;
GLint g_has_layer_maximum_location = -1;
GLint g_block_textures_location = -1;
GLint g_material_faces_location = -1;
GLint g_material_lookup_width_location = -1;
GLint g_uploaded_texture_layers_location = -1;
GLint g_textures_available_location = -1;
GLfloat g_outline_line_width = 1.f;
GLuint g_block_texture_array = 0;
GLuint g_material_face_texture = 0;
GLint g_material_lookup_width = 1;
GLint g_uploaded_texture_layers = 0;
GLint g_allocated_texture_layers = 0;
std::shared_ptr<const ProjectionTexturePack> g_gpu_texture_pack;
EGLContext g_gl_context = EGL_NO_CONTEXT;
// EGL implementations are allowed to recycle an EGLContext handle after a
// game activity/world is torn down.  Numeric GL names must therefore be
// validated against this module's shader before they are reused or deleted.
uint32_t g_render_frame = 0;

// Preserve x0 for the reference header's pointer-return ABI. A true void
// caller simply ignores the returned register.
using LevelRenderFunction = void* (*)(void*, void*, void*);
void* g_level_render_original = nullptr;
std::atomic<void*> g_level_render_owner{nullptr};
std::atomic<uint32_t> g_render_camera_position_sequence{0};
std::atomic<uint32_t> g_render_camera_position_x{0};
std::atomic<uint32_t> g_render_camera_position_y{0};
std::atomic<uint32_t> g_render_camera_position_z{0};
std::atomic<bool> g_render_camera_position_valid{false};

constexpr GLsizei kTriangleIndexCount = 36;
constexpr GLsizei kLineIndexCount = 24;

bool finiteMatrix(const Matrix& matrix) {
    for (float value : matrix.values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

bool readCameraMapMatrix(const void* raw_camera, uintptr_t map_offset, Matrix* output) {
    if (!raw_camera || !output) return false;
    const uintptr_t camera_address = reinterpret_cast<uintptr_t>(raw_camera);
    if (camera_address > std::numeric_limits<uintptr_t>::max() - map_offset) return false;
    const auto* map_field = reinterpret_cast<const void*>(camera_address + map_offset);
    uintptr_t map_address = 0;
    if (!IsMemoryReadable(map_field, sizeof(map_address))) return false;
    std::memcpy(&map_address, map_field, sizeof(map_address));
    if (!map_address || !IsMemoryReadable(reinterpret_cast<const void*>(map_address),
                                          sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t matrix_address = 0;
    std::memcpy(&matrix_address, reinterpret_cast<const void*>(map_address),
                sizeof(matrix_address));
    if (!matrix_address || !IsMemoryReadable(reinterpret_cast<const void*>(matrix_address),
                                             sizeof(*output))) {
        return false;
    }
    std::memcpy(output, reinterpret_cast<const void*>(matrix_address), sizeof(*output));
    return finiteMatrix(*output);
}

bool readCameraInlineMatrix(const void* raw_camera, uintptr_t offset, Matrix* output) {
    if (!raw_camera || !output) return false;
    const uintptr_t camera_address = reinterpret_cast<uintptr_t>(raw_camera);
    if (camera_address > std::numeric_limits<uintptr_t>::max() - offset) return false;
    const auto* matrix_address = reinterpret_cast<const void*>(camera_address + offset);
    if (!IsMemoryReadable(matrix_address, sizeof(*output))) return false;
    std::memcpy(output, matrix_address, sizeof(*output));
    return finiteMatrix(*output);
}

bool readLevelRenderCameraPosition(Vec3* output) {
    if (!output) return false;
    void* render_owner = g_level_render_owner.load(std::memory_order_acquire);
    const uintptr_t owner_address = reinterpret_cast<uintptr_t>(render_owner);
    if (!owner_address || owner_address > std::numeric_limits<uintptr_t>::max() -
        kLevelRenderCameraStateOffset) {
        return false;
    }
    const auto* state_field = reinterpret_cast<const void*>(
        owner_address + kLevelRenderCameraStateOffset);
    void* camera_state = nullptr;
    if (!IsMemoryReadable(state_field, sizeof(camera_state))) return false;
    std::memcpy(&camera_state, state_field, sizeof(camera_state));
    if (!camera_state || !IsMemoryReadable(
            reinterpret_cast<const uint8_t*>(camera_state) +
                kLevelRenderCameraPositionOffset,
            sizeof(*output))) {
        return false;
    }
    std::memcpy(output, reinterpret_cast<const uint8_t*>(camera_state) +
                    kLevelRenderCameraPositionOffset,
                sizeof(*output));
    return std::isfinite(output->x) && std::isfinite(output->y) &&
        std::isfinite(output->z);
}

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void publishLevelRenderCameraPosition(const Vec3& position) {
    // A compact sequence lock avoids exposing a half-updated X/Y/Z tuple to
    // the game tick thread without putting a mutex on the render hot path.
    g_render_camera_position_sequence.fetch_add(1, std::memory_order_acq_rel);
    g_render_camera_position_x.store(floatBits(position.x), std::memory_order_relaxed);
    g_render_camera_position_y.store(floatBits(position.y), std::memory_order_relaxed);
    g_render_camera_position_z.store(floatBits(position.z), std::memory_order_relaxed);
    g_render_camera_position_valid.store(true, std::memory_order_release);
    g_render_camera_position_sequence.fetch_add(1, std::memory_order_release);
}

Matrix transposeMatrix(const Matrix& source) {
    Matrix result;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            result.values[column * 4 + row] = source.values[row * 4 + column];
        }
    }
    return result;
}

void multiplyMatrices(float* output, const float* left, const float* right) {
    float temporary[16]{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int index = 0; index < 4; ++index) {
                temporary[column * 4 + row] +=
                    left[index * 4 + row] * right[column * 4 + index];
            }
        }
    }
    std::memcpy(output, temporary, sizeof(temporary));
}

bool extendGroupBounds(ProjectionGroup* group, const ProjectionBlock& block) {
    if (!group) return false;
    const int64_t local_x = static_cast<int64_t>(block.x) - group->origin_x;
    const int64_t local_y = static_cast<int64_t>(block.y) - group->origin_y;
    const int64_t local_z = static_cast<int64_t>(block.z) - group->origin_z;
    if (local_x < 0 || local_x >= kGroupSpan ||
        local_y < 0 || local_y >= kGroupSpan ||
        local_z < 0 || local_z >= kGroupSpan) {
        return false;
    }
    const uint8_t x = static_cast<uint8_t>(local_x);
    const uint8_t y = static_cast<uint8_t>(local_y);
    const uint8_t z = static_cast<uint8_t>(local_z);
    group->bounds.min_x = std::min(group->bounds.min_x, x);
    group->bounds.min_y = std::min(group->bounds.min_y, y);
    group->bounds.min_z = std::min(group->bounds.min_z, z);
    group->bounds.max_x = std::max(group->bounds.max_x, x);
    group->bounds.max_y = std::max(group->bounds.max_y, y);
    group->bounds.max_z = std::max(group->bounds.max_z, z);
    return true;
}

bool isAirName(const std::string& name) {
    const size_t separator = name.find(':');
    const std::string leaf = separator == std::string::npos
        ? name : name.substr(separator + 1);
    return leaf == "air" || leaf == "cave_air" || leaf == "void_air";
}

std::string_view blockLeaf(const std::string& name) {
    const size_t separator = name.find(':');
    return separator == std::string::npos
        ? std::string_view(name)
        : std::string_view(name).substr(separator + 1);
}

uint64_t columnKey(int32_t group_x, int32_t group_z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(group_x)) << 32U) |
           static_cast<uint32_t>(group_z);
}

uint32_t hashBlock(const std::string& name, uint8_t aux) {
    uint32_t hash = 2166136261U;
    for (unsigned char character : name) {
        hash ^= character;
        hash *= 16777619U;
    }
    return hash ^ (static_cast<uint32_t>(aux) * 2246822519U);
}

std::array<uint8_t, 4> colorForBlock(const std::string& name, uint8_t aux) {
    const std::string_view leaf = blockLeaf(name);
    const auto contains = [&](const char* token) {
        return name.find(token) != std::string::npos;
    };
    struct NamedColor {
        const char* token;
        std::array<uint8_t, 4> color;
    };
    static constexpr NamedColor named_dye_colors[] = {
        {"light_blue", {92, 157, 211, 255}}, {"light_gray", {157, 157, 151, 255}},
        {"magenta", {190, 70, 201, 255}}, {"orange", {229, 125, 36, 255}},
        {"yellow", {235, 198, 39, 255}}, {"lime", {112, 185, 25, 255}},
        {"pink", {226, 126, 160, 255}}, {"cyan", {46, 139, 148, 255}},
        {"purple", {130, 65, 188, 255}}, {"brown", {120, 79, 49, 255}},
        {"green", {77, 119, 34, 255}}, {"red", {176, 52, 48, 255}},
        {"black", {47, 50, 55, 255}}, {"gray", {82, 88, 91, 255}},
        {"blue", {56, 78, 158, 255}}, {"white", {224, 226, 219, 255}},
    };
    static constexpr std::array<std::array<uint8_t, 4>, 16> aux_dye_colors{{
        {{224, 226, 219, 255}}, {{229, 125, 36, 255}}, {{190, 70, 201, 255}},
        {{92, 157, 211, 255}}, {{235, 198, 39, 255}}, {{112, 185, 25, 255}},
        {{226, 126, 160, 255}}, {{82, 88, 91, 255}}, {{157, 157, 151, 255}},
        {{46, 139, 148, 255}}, {{130, 65, 188, 255}}, {{56, 78, 158, 255}},
        {{120, 79, 49, 255}}, {{77, 119, 34, 255}}, {{176, 52, 48, 255}},
        {{47, 50, 55, 255}},
    }};
    const bool dyeable = contains("wool") || contains("concrete") ||
        contains("terracotta") || contains("hardened_clay") ||
        contains("stained_glass") || contains("carpet") || contains("glazed") ||
        contains("candle") || contains("shulker");
    if (dyeable) {
        for (const NamedColor& entry : named_dye_colors) {
            if (contains(entry.token)) return entry.color;
        }
        if (leaf == "wool" || leaf == "concrete" || leaf == "concrete_powder" ||
            leaf == "stained_glass" || leaf == "stained_glass_pane" ||
            leaf == "carpet" || leaf == "stained_hardened_clay") {
            return aux_dye_colors[aux & 0x0fU];
        }
    }

    static constexpr std::array<std::array<uint8_t, 4>, 6> wood_colors{{
        {{164, 131, 77, 255}}, {{105, 79, 48, 255}}, {{202, 186, 126, 255}},
        {{154, 111, 77, 255}}, {{174, 92, 57, 255}}, {{79, 54, 32, 255}},
    }};
    if (leaf == "planks" || leaf == "wooden_slab" ||
        leaf == "double_wooden_slab") {
        return wood_colors[std::min<size_t>(aux & 0x07U, wood_colors.size() - 1U)];
    }
    if (leaf == "log") return wood_colors[aux & 0x03U];
    if (leaf == "log2") return wood_colors[4U + std::min<uint8_t>(aux & 0x03U, 1U)];

    static constexpr std::array<std::array<uint8_t, 4>, 6> leaf_colors{{
        {{76, 145, 55, 255}}, {{66, 112, 61, 255}}, {{116, 161, 70, 255}},
        {{57, 135, 53, 255}}, {{83, 137, 59, 255}}, {{59, 112, 45, 255}},
    }};
    if (leaf == "leaves") return leaf_colors[aux & 0x03U];
    if (leaf == "leaves2") {
        return leaf_colors[4U + std::min<uint8_t>(aux & 0x03U, 1U)];
    }

    static constexpr std::array<std::array<uint8_t, 4>, 7> stone_colors{{
        {{139, 143, 149, 255}}, {{151, 105, 88, 255}}, {{177, 126, 105, 255}},
        {{188, 188, 184, 255}}, {{210, 210, 205, 255}}, {{132, 135, 136, 255}},
        {{157, 160, 161, 255}},
    }};
    if (leaf == "stone") {
        return stone_colors[std::min<size_t>(aux & 0x07U, stone_colors.size() - 1U)];
    }

    static constexpr std::array<std::array<uint8_t, 4>, 8> stone_slab_colors{{
        {{184, 184, 180, 255}}, {{220, 203, 139, 255}}, {{164, 131, 77, 255}},
        {{126, 128, 130, 255}}, {{159, 83, 70, 255}}, {{124, 126, 123, 255}},
        {{80, 43, 49, 255}}, {{222, 217, 205, 255}},
    }};
    if (leaf == "stone_slab" || leaf == "double_stone_slab") {
        return stone_slab_colors[aux & 0x07U];
    }
    static constexpr std::array<std::array<uint8_t, 4>, 8> stone_slab2_colors{{
        {{181, 98, 70, 255}}, {{170, 124, 170, 255}}, {{90, 158, 145, 255}},
        {{61, 104, 94, 255}}, {{101, 171, 158, 255}}, {{107, 124, 92, 255}},
        {{223, 205, 151, 255}}, {{119, 49, 49, 255}},
    }};
    if (leaf == "stone_slab2" || leaf == "double_stone_slab2") {
        return stone_slab2_colors[aux & 0x07U];
    }

    if (contains("water")) return {48, 128, 230, 255};
    if (contains("lava")) return {244, 93, 35, 255};
    if (contains("amethyst")) return {151, 104, 211, 255};
    if (contains("diamond")) return {63, 220, 209, 255};
    if (contains("emerald")) return {58, 201, 105, 255};
    if (contains("lapis")) return {45, 86, 176, 255};
    if (contains("gold")) return {242, 190, 55, 255};
    if (contains("copper")) return contains("oxidized") ?
        std::array<uint8_t, 4>{73, 157, 131, 255} :
        std::array<uint8_t, 4>{193, 111, 74, 255};
    if (contains("iron")) return {207, 201, 190, 255};
    if (contains("redstone")) return {202, 60, 58, 255};
    if (contains("grass") || contains("leaves") || contains("vine") ||
        contains("moss") || contains("azalea")) {
        return {92, 174, 79, 255};
    }
    if (contains("dirt") || contains("mud")) return {133, 94, 66, 255};
    if (contains("cherry")) return {214, 153, 156, 255};
    if (contains("crimson")) return {143, 57, 82, 255};
    if (contains("warped")) return {50, 139, 135, 255};
    if (contains("dark_oak") || contains("spruce")) return {91, 67, 44, 255};
    if (contains("birch")) return {210, 197, 139, 255};
    if (contains("acacia")) return {174, 92, 59, 255};
    if (contains("mangrove")) return {117, 55, 48, 255};
    if (contains("jungle")) return {154, 111, 77, 255};
    if (contains("oak") || contains("bamboo")) return {164, 131, 77, 255};
    if (contains("wood") || contains("log") || contains("planks")) return {171, 126, 73, 255};
    if (contains("glass") || contains("ice")) return {116, 205, 218, 255};
    if (contains("sand")) return {220, 203, 139, 255};
    if (contains("quartz") || contains("calcite")) return {222, 217, 205, 255};
    if (contains("end_stone")) return {214, 218, 157, 255};
    if (contains("nether_brick")) return {80, 43, 49, 255};
    if (contains("netherrack")) return {117, 58, 55, 255};
    if (contains("brick")) return {159, 83, 70, 255};
    if (contains("deepslate") || contains("blackstone")) return {73, 76, 79, 255};
    if (contains("stone") || contains("cobble") || contains("andesite")) return {139, 143, 149, 255};
    if (contains("prismarine")) return {90, 158, 145, 255};
    if (contains("purpur")) return {170, 124, 170, 255};

    const uint32_t hash = hashBlock(name, aux);
    return {
        static_cast<uint8_t>(72U + (hash & 0x7fU)),
        static_cast<uint8_t>(72U + ((hash >> 8U) & 0x7fU)),
        static_cast<uint8_t>(72U + ((hash >> 16U) & 0x7fU)),
        255,
    };
}

uint8_t materialPattern(const std::string& name, uint8_t aux) {
    if (name.find("brick") != std::string::npos) return 2;
    if (name.find("ore") != std::string::npos) return 3;
    if (name.find("glass") != std::string::npos || name.find("ice") != std::string::npos) return 4;
    if (name.find("leaves") != std::string::npos || name.find("moss") != std::string::npos) return 5;
    if (name.find("planks") != std::string::npos || name.find("log") != std::string::npos ||
        name.find("wood") != std::string::npos) return 1;
    return static_cast<uint8_t>(hashBlock(name, aux) % 3U);
}

bool leafEquals(const std::string& name, const char* leaf) {
    return blockLeaf(name) == leaf;
}

bool leafEndsWith(const std::string& name, const char* suffix) {
    const size_t separator = name.find(':');
    const size_t offset = separator == std::string::npos ? 0 : separator + 1;
    const size_t length = std::strlen(suffix);
    return name.size() >= offset + length &&
           name.compare(name.size() - length, length, suffix) == 0;
}

// The parsed projection stores the target's historical fluid level in the
// low four aux bits.  Source blocks are level zero; values one through seven
// are the stepped horizontal flow heights, while eight through fifteen are a
// falling column and occupy the full cell height.  The renderer cannot retain
// the per-corner slope without a more complex mesh, but a correctly-heighted
// flat surface is much less misleading than a solid cube.
float fluidSurfaceHeight(uint8_t aux) {
    const uint8_t level = aux & 0x0fU;
    if (level >= 8U) return 1.f;
    return 1.f - static_cast<float>(level) / 9.f;
}

bool isProjectionFluidLeaf(std::string_view leaf) {
    return leaf == "water" || leaf == "flowing_water" ||
        leaf == "lava" || leaf == "flowing_lava" || leaf == "bubble_column";
}

// Keep this intentionally exact instead of testing whether the identifier
// contains "torch": `torchflower` is a plant, not a wall/floor attachment.
bool isProjectionTorchLeaf(std::string_view leaf) {
    return leaf == "torch" || leaf == "wall_torch" ||
        leaf == "soul_torch" || leaf == "soul_wall_torch" ||
        leaf == "redstone_torch" || leaf == "redstone_wall_torch" ||
        leaf == "unlit_redstone_torch";
}

LocalBounds rotateLocalBounds(LocalBounds bounds, uint8_t quarter_turns) {
    const uint8_t turns = quarter_turns & 0x03U;
    for (uint8_t turn = 0; turn < turns; ++turn) {
        const float old_min_x = bounds.min_x;
        const float old_max_x = bounds.max_x;
        bounds.min_x = 1.f - bounds.max_z;
        bounds.max_x = 1.f - bounds.min_z;
        bounds.min_z = old_min_x;
        bounds.max_z = old_max_x;
    }
    return bounds;
}

// Modern direct-state blocks such as chains and rods store their axis as
// y=0, x=1, z=2.  A centred rectangular rod is a sufficiently faithful
// approximation for the projection's cube-instance renderer while retaining
// the important axis information.
LocalBounds centeredAxisRodBounds(uint8_t axis, float half_width) {
    LocalBounds bounds;
    const float minimum = 0.5f - half_width;
    const float maximum = 0.5f + half_width;
    switch (axis) {
        case 1U:  // X axis.
            bounds.min_y = bounds.min_z = minimum;
            bounds.max_y = bounds.max_z = maximum;
            break;
        case 2U:  // Z axis.
            bounds.min_x = bounds.min_y = minimum;
            bounds.max_x = bounds.max_y = maximum;
            break;
        default:  // Y axis, including malformed legacy fallback values.
            bounds.min_x = bounds.min_z = minimum;
            bounds.max_x = bounds.max_z = maximum;
            break;
    }
    return bounds;
}

bool isFullCube(const LocalBounds& bounds) {
    return bounds.min_x == 0.f && bounds.min_y == 0.f && bounds.min_z == 0.f &&
           bounds.max_x == 1.f && bounds.max_y == 1.f && bounds.max_z == 1.f;
}

TranslucentCullClass translucentCullClass(const std::string& name) {
    const std::string_view leaf = blockLeaf(name);
    if (leaf.find("glass") != std::string_view::npos) {
        return TranslucentCullClass::Glass;
    }
    if (leaf == "ice" || leafEndsWith(name, "_ice")) {
        return TranslucentCullClass::Ice;
    }
    if (leaf == "water" || leaf == "flowing_water" || leaf == "bubble_column") {
        return TranslucentCullClass::Water;
    }
    if (leaf == "lava" || leaf == "flowing_lava") {
        return TranslucentCullClass::Lava;
    }
    if (leaf == "leaves" || leaf == "leaves2" || leafEndsWith(name, "_leaves")) {
        return TranslucentCullClass::Leaves;
    }
    if (leaf == "slime" || leaf == "slime_block") {
        return TranslucentCullClass::Slime;
    }
    if (leaf == "honey_block") return TranslucentCullClass::Honey;
    if (leaf == "beacon") return TranslucentCullClass::Beacon;
    return TranslucentCullClass::Opaque;
}

bool sameTranslucentCullGroup(const CellData& first, const CellData& second,
                              const std::vector<ProjectionBlock>& blocks,
                              const std::vector<std::string>& names) {
    if (first.translucent_cull_class == TranslucentCullClass::Opaque ||
        first.translucent_cull_class != second.translucent_cull_class ||
        first.block_index >= blocks.size() || second.block_index >= blocks.size()) {
        return false;
    }
    const ProjectionBlock& first_block = blocks[first.block_index];
    const ProjectionBlock& second_block = blocks[second.block_index];
    if (first_block.name_index >= names.size() || second_block.name_index >= names.size()) {
        return false;
    }

    const std::string& first_name = names[first_block.name_index];
    const std::string& second_name = names[second_block.name_index];
    switch (first.translucent_cull_class) {
        case TranslucentCullClass::Water:
        case TranslucentCullClass::Lava:
            return true;
        case TranslucentCullClass::Leaves: {
            const std::string_view first_leaf = blockLeaf(first_name);
            const std::string_view second_leaf = blockLeaf(second_name);
            if (first_leaf != second_leaf) return false;
            if (first_leaf == "leaves") {
                return (first_block.aux & 0x03U) == (second_block.aux & 0x03U);
            }
            if (first_leaf == "leaves2") {
                return (first_block.aux & 0x01U) == (second_block.aux & 0x01U);
            }
            return true;
        }
        case TranslucentCullClass::Glass:
            return blockLeaf(first_name) == blockLeaf(second_name) &&
                first_block.aux == second_block.aux;
        case TranslucentCullClass::Ice:
        case TranslucentCullClass::Slime:
        case TranslucentCullClass::Honey:
        case TranslucentCullClass::Beacon:
            return blockLeaf(first_name) == blockLeaf(second_name);
        case TranslucentCullClass::Opaque:
            return false;
    }
    return false;
}

using NeighborCellCache = std::array<const CellData*, 27>;

NeighborCellCache gatherNeighborCells(
    const std::unordered_map<BlockPosition, CellData, BlockPositionHash>& cells,
    const ProjectionBlock& block, const CellData& current) {
    NeighborCellCache result{};
    result[neighborCacheIndex(0, 0, 0)] = &current;
    for (int32_t z = -1; z <= 1; ++z) {
        for (int32_t y = -1; y <= 1; ++y) {
            for (int32_t x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0 && z == 0) continue;
                if (x != 0 && y != 0 && z != 0) continue;
                const int64_t world_x = static_cast<int64_t>(block.x) + x;
                const int64_t world_y = static_cast<int64_t>(block.y) + y;
                const int64_t world_z = static_cast<int64_t>(block.z) + z;
                if (world_x < INT32_MIN || world_x > INT32_MAX ||
                    world_y < INT32_MIN || world_y > INT32_MAX ||
                    world_z < INT32_MIN || world_z > INT32_MAX) {
                    continue;
                }
                const auto neighbor = cells.find({
                    static_cast<int32_t>(world_x), static_cast<int32_t>(world_y),
                    static_cast<int32_t>(world_z)});
                if (neighbor != cells.end()) {
                    result[neighborCacheIndex(x, y, z)] = &neighbor->second;
                }
            }
        }
    }
    return result;
}

const CellData* cachedNeighbor(const NeighborCellCache& cache,
                               const OutlineOffset& offset) {
    return cache[neighborCacheIndex(offset.x, offset.y, offset.z)];
}

bool isFullCell(const CellData* cell) {
    return cell && cell->full_cube != 0;
}

bool sameOutlineMaterial(const CellData& first, const CellData& second,
                         const std::vector<ProjectionBlock>& blocks,
                         const std::vector<std::string>& names) {
    if (first.full_cube == 0 || second.full_cube == 0 ||
        first.block_index >= blocks.size() || second.block_index >= blocks.size()) {
        return false;
    }
    const ProjectionBlock& first_block = blocks[first.block_index];
    const ProjectionBlock& second_block = blocks[second.block_index];
    if (first_block.aux != second_block.aux ||
        (first_block.rotation_quarters & 0x03U) !=
            (second_block.rotation_quarters & 0x03U) ||
        first_block.name_index >= names.size() || second_block.name_index >= names.size()) {
        return false;
    }
    return first_block.name_index == second_block.name_index ||
        names[first_block.name_index] == names[second_block.name_index];
}

uint16_t outlineMaskForFullCube(const CellData& current,
                                const NeighborCellCache& neighbors,
                                const std::vector<ProjectionBlock>& blocks,
                                const std::vector<std::string>& names) {
    uint16_t mask = 0;
    const OutlineCell current_cell{true, true};
    for (size_t edge = 0; edge < kProjectionOutlineEdges.size(); ++edge) {
        const OutlineEdgeNeighborhood& neighborhood = kProjectionOutlineEdges[edge];
        const OutlineOffset diagonal = outlineDiagonalOffset(neighborhood);
        const CellData* side_a = cachedNeighbor(neighbors, neighborhood.side_a);
        const CellData* side_b = cachedNeighbor(neighbors, neighborhood.side_b);
        const CellData* diagonal_cell = cachedNeighbor(neighbors, diagonal);
        const auto describe = [&](const CellData* cell) {
            return OutlineCell{
                isFullCell(cell),
                isFullCell(cell) && sameOutlineMaterial(current, *cell, blocks, names),
            };
        };
        if (!outlineEdgeVisibleForOwner(
                neighborhood.current_quadrant, current_cell, describe(side_a),
                describe(side_b), describe(diagonal_cell))) continue;
        mask = static_cast<uint16_t>(mask | (1U << static_cast<uint32_t>(edge)));
    }

    // Horizontal layer cuts need the merged X/Z perimeter independently of
    // the original 3D feature edges. The order is -Z,+Z,-X,+X.
    std::array<OutlineCell, 4> cut_neighbors{};
    for (uint8_t side = 0; side < 4; ++side) {
        const OutlineOffset& offset = kProjectionHorizontalCutNeighbors[side];
        const CellData* neighbor = cachedNeighbor(neighbors, offset);
        cut_neighbors[side] = {
            isFullCell(neighbor),
            isFullCell(neighbor) && sameOutlineMaterial(current, *neighbor, blocks, names),
        };
    }
    uint8_t cut_mask = outlineHorizontalCutMask(true, cut_neighbors);
    for (size_t side = 0; side < cut_neighbors.size(); ++side) {
        if (!cut_neighbors[side].full || cut_neighbors[side].same_material) continue;
        if (offsetPrecedesCurrent(kProjectionHorizontalCutNeighbors[side])) {
            cut_mask = static_cast<uint8_t>(
                cut_mask & ~(1U << static_cast<uint32_t>(side)));
        }
    }
    return packProjectionOutlineMask(mask, cut_mask);
}

BlockModel modelForBlock(const std::string& name, uint8_t aux,
                           uint8_t rotation_quarters) {
    const auto contains = [&](const char* token) { return name.find(token) != std::string::npos; };
    const std::string_view leaf = blockLeaf(name);
    BlockModel model;
    LocalBounds bounds;

    if (isProjectionFluidLeaf(leaf)) {
        // Fluids are not full cubes unless they are a source or a falling
        // column.  In particular, retaining a flow's lower liquid surface
        // prevents it from hiding the shape of nearby partial blocks.
        bounds.max_y = fluidSurfaceHeight(aux);
        model.parts[0] = bounds;
    } else if (contains("stairs")) {
        LocalBounds full_height;
        LocalBounds half_height;
        if ((aux & 0x04U) != 0) {
            half_height.min_y = 0.5f;
        } else {
            half_height.max_y = 0.5f;
        }
        switch (aux & 0x03U) {
            case 0:
                full_height.min_x = 0.5f;
                half_height.max_x = 0.5f;
                break;
            case 1:
                full_height.max_x = 0.5f;
                half_height.min_x = 0.5f;
                break;
            case 2:
                full_height.min_z = 0.5f;
                half_height.max_z = 0.5f;
                break;
            default:
                full_height.max_z = 0.5f;
                half_height.min_z = 0.5f;
                break;
        }
        model.parts[0] = full_height;
        model.parts[1] = half_height;
        model.part_count = 2;
    } else if (IsProjectionSlabBlock(name)) {
        // BDX can encode a current top slab as aux=1, while older native and
        // Infinity records use bit 3. Use the same source-state decoder as
        // the printer/world matcher so a top half never renders at the floor
        // and a complete slab remains a full cube.
        bool top = false;
        bool is_double = false;
        if (TryProjectionSlabPlacement(name, aux, &top, &is_double) && !is_double) {
            if (top) bounds.min_y = 0.5f; else bounds.max_y = 0.5f;
            model.parts[0] = bounds;
        }
    } else if (contains("trapdoor")) {
        constexpr float thickness = 0.1875f;
        if ((aux & 0x04U) == 0) {
            if ((aux & 0x08U) != 0) bounds.min_y = 1.f - thickness;
            else bounds.max_y = thickness;
        } else {
            switch (aux & 0x03U) {
                case 0: bounds.max_z = thickness; break;
                case 1: bounds.min_z = 1.f - thickness; break;
                case 2: bounds.max_x = thickness; break;
                default: bounds.min_x = 1.f - thickness; break;
            }
        }
        model.parts[0] = bounds;
    } else if (contains("button")) {
        const float thickness = (aux & 0x08U) != 0 ? 0.0625f : 0.125f;
        bounds.min_x = bounds.min_z = 0.3125f;
        bounds.max_x = bounds.max_z = 0.6875f;
        bounds.min_y = 0.375f;
        bounds.max_y = 0.625f;
        switch (aux & 0x07U) {
            case 0: bounds.min_y = 1.f - thickness; bounds.max_y = 1.f; break;
            case 1: bounds.min_x = 0.f; bounds.max_x = thickness; break;
            case 2: bounds.min_x = 1.f - thickness; bounds.max_x = 1.f; break;
            case 3: bounds.min_z = 0.f; bounds.max_z = thickness; break;
            case 4: bounds.min_z = 1.f - thickness; bounds.max_z = 1.f; break;
            default: bounds.min_y = 0.f; bounds.max_y = thickness; break;
        }
        model.parts[0] = bounds;
    } else if (leaf == "ladder") {
        constexpr float thickness = 0.0625f;
        bounds.min_y = 0.0625f;
        bounds.max_y = 0.9375f;
        switch (aux & 0x07U) {
            case 2: bounds.max_z = thickness; break;
            case 3: bounds.min_z = 1.f - thickness; break;
            case 4: bounds.max_x = thickness; break;
            default: bounds.min_x = 1.f - thickness; break;
        }
        model.parts[0] = bounds;
    } else if (leaf.find("wall_sign") != std::string_view::npos) {
        constexpr float thickness = 0.0625f;
        bounds.min_y = 0.25f;
        bounds.max_y = 0.8125f;
        switch (aux & 0x07U) {
            case 2:
                bounds.min_x = 0.125f; bounds.max_x = 0.875f;
                bounds.max_z = thickness;
                break;
            case 3:
                bounds.min_x = 0.125f; bounds.max_x = 0.875f;
                bounds.min_z = 1.f - thickness;
                break;
            case 4:
                bounds.min_z = 0.125f; bounds.max_z = 0.875f;
                bounds.max_x = thickness;
                break;
            default:
                bounds.min_z = 0.125f; bounds.max_z = 0.875f;
                bounds.min_x = 1.f - thickness;
                break;
        }
        model.parts[0] = bounds;
    } else if (leaf.find("sign") != std::string_view::npos) {
        LocalBounds board;
        board.min_y = 0.35f;
        board.max_y = 0.875f;
        const uint8_t direction = static_cast<uint8_t>(((aux & 0x0fU) + 2U) / 4U) & 0x03U;
        if ((direction & 0x01U) == 0) {
            board.min_x = 0.125f;
            board.max_x = 0.875f;
            board.min_z = 0.46875f;
            board.max_z = 0.53125f;
        } else {
            board.min_x = 0.46875f;
            board.max_x = 0.53125f;
            board.min_z = 0.125f;
            board.max_z = 0.875f;
        }
        LocalBounds post;
        post.min_x = post.min_z = 0.46875f;
        post.max_x = post.max_z = 0.53125f;
        post.max_y = 0.5f;
        model.parts[0] = board;
        model.parts[1] = post;
        model.part_count = 2;
    } else if (contains("carpet") || contains("pressure_plate") || contains("rail") ||
               contains("redstone_wire")) {
        bounds.max_y = 0.075f;
        model.parts[0] = bounds;
    } else if (contains("snow_layer")) {
        bounds.max_y = static_cast<float>((aux & 0x07U) + 1U) / 8.f;
        model.parts[0] = bounds;
    } else if (isProjectionTorchLeaf(leaf)) {
        // Bedrock's old torch attachment metadata is 1=east, 2=west,
        // 3=south, 4=north and 5=floor.  A wall torch occupies the side of
        // the target cell adjacent to its supporting block; preserving that
        // offset makes both source and rotated projections read correctly.
        constexpr float kTorchHalfWidth = 0.0625f;
        constexpr float kWallTorchThickness = 0.125f;
        const uint8_t attachment = aux & 0x07U;
        bounds.min_x = bounds.min_z = 0.5f - kTorchHalfWidth;
        bounds.max_x = bounds.max_z = 0.5f + kTorchHalfWidth;
        if (attachment == 1U) {  // East face of the west-side support.
            bounds.min_x = 0.f;
            bounds.max_x = kWallTorchThickness;
            bounds.min_y = 0.1875f;
            bounds.max_y = 0.8125f;
        } else if (attachment == 2U) {  // West face of the east-side support.
            bounds.min_x = 1.f - kWallTorchThickness;
            bounds.max_x = 1.f;
            bounds.min_y = 0.1875f;
            bounds.max_y = 0.8125f;
        } else if (attachment == 3U) {  // South face of the north-side support.
            bounds.min_z = 0.f;
            bounds.max_z = kWallTorchThickness;
            bounds.min_y = 0.1875f;
            bounds.max_y = 0.8125f;
        } else if (attachment == 4U) {  // North face of the south-side support.
            bounds.min_z = 1.f - kWallTorchThickness;
            bounds.max_z = 1.f;
            bounds.min_y = 0.1875f;
            bounds.max_y = 0.8125f;
        } else {
            // 5 is a normal floor torch.  Treat a malformed/legacy zero as
            // floor-mounted as a conservative visual fallback.
            bounds.max_y = 0.625f;
        }
        model.parts[0] = bounds;
    } else if (leaf == "fire" || leaf == "soul_fire") {
        // Fire is a crossed, non-solid sprite.  Two thin boxes keep the
        // existing cube-only instancing path while avoiding a full block-sized
        // projected flame and preserving a recognisable silhouette.
        LocalBounds north_south;
        north_south.min_x = 0.4375f;
        north_south.max_x = 0.5625f;
        north_south.min_z = 0.1875f;
        north_south.max_z = 0.8125f;
        north_south.max_y = 0.9375f;
        LocalBounds east_west;
        east_west.min_x = 0.1875f;
        east_west.max_x = 0.8125f;
        east_west.min_z = 0.4375f;
        east_west.max_z = 0.5625f;
        east_west.max_y = 0.9375f;
        model.parts[0] = north_south;
        model.parts[1] = east_west;
        model.part_count = 2;
    } else if (leaf == "chain") {
        // Direct-state chain metadata uses y=0, x=1, z=2.  The link texture
        // itself is not representable by an AABB, but a slim axial rod keeps
        // both its occupied volume and orientation clear in the projection.
        model.parts[0] = centeredAxisRodBounds(aux & 0x03U, 0.09375f);
    } else if (leaf == "end_rod") {
        // End-rod metadata is six-way: 0/1=Y, 2/3=Z, 4/5=X.  The sign has no
        // effect on its symmetric collision silhouette, only the axis does.
        const uint8_t facing = aux & 0x07U;
        const uint8_t axis = facing >= 4U ? 1U : (facing >= 2U ? 2U : 0U);
        model.parts[0] = centeredAxisRodBounds(axis, 0.0625f);
    } else if (leaf == "lightning_rod" || leafEndsWith(name, "_lightning_rod")) {
        // The low three bits encode the same six-way layout as end rods; bit 3
        // is the transient powered state and must not affect the shape.
        const uint8_t facing = aux & 0x07U;
        const uint8_t axis = facing >= 4U ? 1U : (facing >= 2U ? 2U : 0U);
        model.parts[0] = centeredAxisRodBounds(axis, 0.125f);
    } else if (leaf == "lantern" || leaf == "soul_lantern") {
        // Lanterns have a compact body.  Hanging metadata moves the body to
        // the top of the cell instead of showing a full block-sized cube.
        bounds.min_x = bounds.min_z = 0.1875f;
        bounds.max_x = bounds.max_z = 0.8125f;
        if ((aux & 0x01U) != 0U) {
            bounds.min_y = 0.3125f;
        } else {
            bounds.max_y = 0.6875f;
        }
        model.parts[0] = bounds;
    } else if (leaf == "repeater" || leaf == "powered_repeater" ||
               leaf == "unpowered_repeater" || leaf == "comparator" ||
               leaf == "powered_comparator" || leaf == "unpowered_comparator") {
        // Redstone repeaters and comparators sit on a very low base.  Keeping
        // the base-only model also avoids occluding nearby wires and buttons.
        bounds.max_y = 0.125f;
        model.parts[0] = bounds;
    } else if (leaf == "daylight_detector" || leaf == "daylight_detector_inverted") {
        // The daylight detector is taller than redstone components but still
        // distinctly below a full cube.
        bounds.max_y = 0.375f;
        model.parts[0] = bounds;
    } else if (leaf == "lily_pad" || leaf == "waterlily") {
        // A lily pad is a wafer-thin surface resting on water.
        bounds.max_y = 0.0625f;
        model.parts[0] = bounds;
    } else if (leaf == "cobweb" || leaf == "web") {
        // Cobwebs are crossed sprites rather than opaque cubes.  Two thin
        // perpendicular boxes mirror the fire approximation without hiding
        // the block space behind the web.
        LocalBounds north_south;
        north_south.min_x = 0.4375f;
        north_south.max_x = 0.5625f;
        north_south.min_y = 0.0625f;
        north_south.max_y = 0.9375f;
        north_south.min_z = 0.0625f;
        north_south.max_z = 0.9375f;
        LocalBounds east_west;
        east_west.min_x = 0.0625f;
        east_west.max_x = 0.9375f;
        east_west.min_y = 0.0625f;
        east_west.max_y = 0.9375f;
        east_west.min_z = 0.4375f;
        east_west.max_z = 0.5625f;
        model.parts[0] = north_south;
        model.parts[1] = east_west;
        model.part_count = 2;
    } else if (contains("flower") || contains("sapling") ||
               contains("mushroom") || contains("tallgrass") || contains("deadbush")) {
        bounds.min_x = bounds.min_z = 0.25f;
        bounds.max_x = bounds.max_z = 0.75f;
        bounds.max_y = 0.9f;
        model.parts[0] = bounds;
    } else if (contains("glass_pane") || contains("iron_bars")) {
        bounds.min_x = bounds.min_z = 0.4375f;
        bounds.max_x = bounds.max_z = 0.5625f;
        model.parts[0] = bounds;
    } else if (contains("fence") || leafEndsWith(name, "_wall")) {
        bounds.min_x = bounds.min_z = 0.25f;
        bounds.max_x = bounds.max_z = 0.75f;
        model.parts[0] = bounds;
    } else if (leafEquals(name, "door") || leafEndsWith(name, "_door")) {
        if ((aux & 0x01U) == 0) {
            bounds.max_z = 0.1875f;
        } else {
            bounds.max_x = 0.1875f;
        }
        model.parts[0] = bounds;
    } else if (leafEquals(name, "bed") || leafEndsWith(name, "_bed")) {
        bounds.max_y = 0.5625f;
        model.parts[0] = bounds;
    } else if (contains("chest")) {
        bounds.min_x = bounds.min_z = 0.0625f;
        bounds.max_x = bounds.max_z = 0.9375f;
        bounds.max_y = 0.875f;
        model.parts[0] = bounds;
    } else {
        model.parts[0] = bounds;
    }
    for (uint8_t index = 0; index < model.part_count; ++index) {
        model.parts[index] = rotateLocalBounds(model.parts[index], rotation_quarters);
    }
    return model;
}

uint16_t encodeLocalPosition(float value) {
    const float clamped = std::max(0.f, std::min(static_cast<float>(kGroupSpan), value));
    const long fixed = std::lround(clamped * static_cast<float>(kPositionFixedScale));
    return static_cast<uint16_t>(std::max<long>(
        0, std::min<long>(static_cast<long>(kMaximumFixedPosition), fixed)));
}

ProjectionInstance makeInstance(const ProjectionBlock& block, const std::string& name,
                                 uint16_t material_id, const LocalBounds& bounds,
                                 int32_t origin_x, int32_t origin_y, int32_t origin_z,
                                 uint8_t face_mask, uint16_t outline_mask) {
    ProjectionInstance instance{};
    const float local_x = static_cast<float>(static_cast<int64_t>(block.x) - origin_x);
    const float local_y = static_cast<float>(static_cast<int64_t>(block.y) - origin_y);
    const float local_z = static_cast<float>(static_cast<int64_t>(block.z) - origin_z);
    instance.bounds[0] = encodeLocalPosition(local_x + bounds.min_x);
    instance.bounds[1] = encodeLocalPosition(local_y + bounds.min_y);
    instance.bounds[2] = encodeLocalPosition(local_z + bounds.min_z);
    instance.bounds[3] = encodeLocalPosition(local_x + bounds.max_x);
    instance.bounds[4] = encodeLocalPosition(local_y + bounds.max_y);
    instance.bounds[5] = encodeLocalPosition(local_z + bounds.max_z);
    for (uint8_t index = 0; index < 6; ++index) {
        const uint16_t metadata = static_cast<uint16_t>(
            (outline_mask >> (index * 3U)) & kPositionMetadataMask);
        instance.bounds[index] = static_cast<uint16_t>(
            instance.bounds[index] | (metadata << kPositionMetadataShift));
    }
    const std::array<uint8_t, 4> color = colorForBlock(name, block.aux);
    std::copy(color.begin(), color.end(), instance.color);
    instance.packed_material = static_cast<uint32_t>(face_mask & kAllFaces) |
        (static_cast<uint32_t>(materialPattern(name, block.aux)) << 8U) |
        (static_cast<uint32_t>(material_id) << 16U);
    return instance;
}

std::string materialKeyFor(const std::string& name, uint8_t aux,
                           uint8_t rotation_quarters) {
    std::string key;
    key.reserve(name.size() + 3U);
    key.append(name);
    key.push_back('\0');
    key.push_back(static_cast<char>(aux));
    key.push_back(static_cast<char>(rotation_quarters & 0x03U));
    return key;
}

uint16_t materialIdFor(ProjectionPlan* plan,
                       std::unordered_map<std::string, uint16_t>* material_ids,
                       const std::string& name, uint8_t aux,
                       uint8_t rotation_quarters) {
    if (!plan || !material_ids) return 0;
    // Surface extraction emits long runs of the same material, and the map key
    // is a heap allocation for any name past the small-string limit. Remember
    // the previous hit so those runs skip the key build and the hash probe.
    // The cache compares the name by value: an interned name's address can be
    // reused by a later allocation, so pointer identity would not be sound.
    struct MaterialIdCache {
        const ProjectionPlan* plan = nullptr;
        const void* material_ids = nullptr;
        std::string name;
        uint8_t aux = 0;
        uint8_t rotation = 0;
        uint16_t id = 0;
    };
    thread_local MaterialIdCache cache;
    const uint8_t rotation = static_cast<uint8_t>(rotation_quarters & 0x03U);
    if (cache.id != 0 && cache.plan == plan && cache.material_ids == material_ids &&
        cache.aux == aux && cache.rotation == rotation && cache.name == name) {
        return cache.id;
    }
    const auto remember = [&](uint16_t id) {
        cache.plan = plan;
        cache.material_ids = material_ids;
        cache.name = name;
        cache.aux = aux;
        cache.rotation = rotation;
        cache.id = id;
        return id;
    };
    std::string key = materialKeyFor(name, aux, rotation);
    const auto existing = material_ids->find(key);
    if (existing != material_ids->end()) return remember(existing->second);
    // A lazy plan exposes its complete palette to the texture loader before
    // workers start extracting surfaces.  Do not create a late material ID:
    // it would not exist in the already-installed texture lookup table.
    if (plan->material_palette_sealed) return 0;
    if (plan->material_requests.size() >= UINT16_MAX) return 0;
    const uint16_t id = static_cast<uint16_t>(plan->material_requests.size() + 1U);
    plan->material_requests.push_back({id, name, aux, rotation});
    material_ids->emplace(std::move(key), id);
    return remember(id);
}

bool blockInsideBounds(const ProjectionBlock& block, const BlockBounds& bounds) {
    return bounds.isValid() && block.x >= bounds.min_x && block.x <= bounds.max_x &&
        block.y >= bounds.min_y && block.y <= bounds.max_y &&
        block.z >= bounds.min_z && block.z <= bounds.max_z;
}

uint8_t instanceLayer(const ProjectionInstance& instance) {
    return static_cast<uint8_t>(std::min<uint32_t>(
        kGroupSpan - 1,
        (instance.bounds[1] & kPositionValueMask) / kPositionFixedScale));
}

uint16_t instanceOutlineMask(const ProjectionInstance& instance) {
    uint32_t mask = 0;
    for (uint8_t index = 0; index < 6; ++index) {
        mask |= static_cast<uint32_t>(
            (instance.bounds[index] >> kPositionMetadataShift) &
            kPositionMetadataMask) << (index * 3U);
    }
    return static_cast<uint16_t>(mask);
}

bool sortGroupByLayer(ProjectionGroup* group) {
    if (!group || !group->instances || group->instances->size() > UINT16_MAX) return false;
    std::vector<ProjectionInstance>& instances = *group->instances;
    std::array<uint16_t, kGroupSpan> layer_counts{};
    for (const ProjectionInstance& instance : instances) {
        ++layer_counts[instanceLayer(instance)];
    }
    group->layer_offsets[0] = 0;
    for (size_t layer = 0; layer < layer_counts.size(); ++layer) {
        group->layer_offsets[layer + 1] = static_cast<uint16_t>(
            group->layer_offsets[layer] + layer_counts[layer]);
    }
    // Sixteen fixed buckets avoid O(n log n) comparison sorting for every 16^3
    // render group while preserving the source order inside each layer.
    std::array<uint16_t, kGroupSpan> destinations{};
    std::copy_n(group->layer_offsets.begin(), destinations.size(), destinations.begin());
    std::vector<ProjectionInstance> ordered(instances.size());
    for (const ProjectionInstance& instance : instances) {
        ordered[destinations[instanceLayer(instance)]++] = instance;
    }
    instances.swap(ordered);
    group->instance_count = static_cast<GLsizei>(instances.size());
    return true;
}

bool buildPartitionGroups(const ProjectionBlueprint& blueprint,
                          const BlockBounds* core_bounds,
                          size_t maximum_partition_blocks,
                          ProjectionPlan* material_plan,
                          std::unordered_map<std::string, uint16_t>* material_ids,
                          const BuildProjectionRenderer::CancelCheck& cancel_check,
                          std::vector<ProjectionGroup>* output_groups,
                          uint64_t* core_block_count,
                          std::string* error) {
    if (!output_groups || !core_block_count) return false;
    output_groups->clear();
    *core_block_count = 0;
    const std::vector<ProjectionBlock>& blocks = blueprint.blocks;
    const std::vector<std::string>& names = blueprint.names;
    if (blocks.empty()) {
        if (error) *error = "projection partition contains no blocks";
        return false;
    }
    if (core_bounds && !core_bounds->isValid()) {
        if (error) *error = "projection partition core bounds are invalid";
        return false;
    }
    if (maximum_partition_blocks != 0 && blocks.size() > maximum_partition_blocks) {
        if (error) {
            *error = "projection partition exceeds its temporary-memory block budget; "
                     "split it into smaller spatial partitions";
        }
        return false;
    }

    std::unordered_map<BlockPosition, CellData, BlockPositionHash> cells;
    cells.reserve(blocks.size());
    for (size_t index = 0; index < blocks.size(); ++index) {
        if ((index & 0x7ffU) == 0 && cancel_check && cancel_check()) {
            if (error) *error = "building projection loading was cancelled";
            return false;
        }
        const ProjectionBlock& block = blocks[index];
        if (block.name_index >= names.size()) {
            if (error) *error = "projection block references an invalid material name";
            return false;
        }
        const std::string& name = names[block.name_index];
        if (name.empty() || isAirName(name)) continue;
        const BlockModel model = modelForBlock(name, block.aux, block.rotation_quarters);
        const bool full_cube = model.part_count == 1 && isFullCube(model.parts[0]);
        const TranslucentCullClass cull_class = translucentCullClass(name);
        cells.insert_or_assign({block.x, block.y, block.z}, CellData{
            index,
            static_cast<uint8_t>(full_cube),
            static_cast<uint8_t>(full_cube && cull_class == TranslucentCullClass::Opaque),
            cull_class,
        });
    }
    if (cells.empty()) return true;

    std::map<GroupPosition, size_t> surface_group_indices;
    std::map<GroupPosition, size_t> interior_group_indices;
    size_t visited = 0;
    for (const auto& cell : cells) {
        if ((visited++ & 0x7ffU) == 0 && cancel_check && cancel_check()) {
            if (error) *error = "building projection loading was cancelled";
            return false;
        }
        const ProjectionBlock& block = blocks[cell.second.block_index];
        if (core_bounds && !blockInsideBounds(block, *core_bounds)) continue;
        ++*core_block_count;
        const std::string& name = names[block.name_index];
        const BlockModel model = modelForBlock(name, block.aux, block.rotation_quarters);
        uint8_t face_mask = kAllFaces;
        uint16_t outline_mask = kAllOutlineBits;
        if (cell.second.full_cube != 0) {
            const NeighborCellCache neighbors = gatherNeighborCells(
                cells, block, cell.second);
            for (uint8_t face = 0; face < 6; ++face) {
                const CellData* neighbor = cachedNeighbor(
                    neighbors, kFaceNeighborOffsets[face]);
                if (neighbor && neighbor->full_cube != 0 &&
                    (neighbor->occludes_adjacent != 0 ||
                      (cell.second.occludes_adjacent == 0 &&
                       sameTranslucentCullGroup(
                           cell.second, *neighbor, blocks, names)))) {
                    face_mask &= static_cast<uint8_t>(~(1U << face));
                }
            }
            outline_mask = outlineMaskForFullCube(
                cell.second, neighbors, blocks, names);
        }

        const bool interior_only = cell.second.full_cube != 0 && face_mask == 0;

        const GroupPosition position{
            floorDiv(block.x, kGroupSpan), floorDiv(block.y, kGroupSpan),
            floorDiv(block.z, kGroupSpan),
        };
        auto& group_indices = interior_only ? interior_group_indices : surface_group_indices;
        auto entry = group_indices.find(position);
        if (entry == group_indices.end()) {
            ProjectionGroup group;
            group.origin_x = position.x * kGroupSpan;
            group.origin_y = position.y * kGroupSpan;
            group.origin_z = position.z * kGroupSpan;
            group.flags.interior_only = interior_only;
            group.instances = std::make_unique<std::vector<ProjectionInstance>>();
            group.instances->reserve(256);
            const size_t index = output_groups->size();
            output_groups->push_back(std::move(group));
            entry = group_indices.emplace(position, index).first;
        }
        ProjectionGroup& group = (*output_groups)[entry->second];
        if (!extendGroupBounds(&group, block)) {
            if (error) *error = "projection block falls outside its compact render group";
            return false;
        }
        const uint16_t material_id = materialIdFor(
            material_plan, material_ids, name, block.aux, block.rotation_quarters);
        if (outline_mask != 0) group.flags.has_outline = 1;
        for (uint8_t part = 0; part < model.part_count; ++part) {
            group.instances->push_back(makeInstance(
                block, name, material_id, model.parts[part], group.origin_x, group.origin_y,
                group.origin_z, cell.second.full_cube != 0 ? face_mask : kAllFaces,
                outline_mask));
        }
    }
    for (ProjectionGroup& group : *output_groups) {
        if (!sortGroupByLayer(&group)) {
            if (error) *error = "projection render group exceeds its compact instance range";
            return false;
        }
    }
    return true;
}

std::string makeSpoolPath(const std::string& directory) {
    const char separator = directory.empty() || directory.back() == '/' ||
        directory.back() == '\\' ? '\0' : '/';
    const uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t sequence = g_spool_sequence.fetch_add(1, std::memory_order_relaxed);
    return directory + (separator == '\0' ? "" : std::string(1, separator)) +
        "projection_render_" + std::to_string(timestamp) + "_" +
        std::to_string(sequence) + ".bin";
}

bool spoolGroupToWriter(std::ofstream* writer, size_t maximum_spool_bytes,
                        size_t* spool_bytes, ProjectionGroup* group,
                        std::string* error) {
    if (!writer || !spool_bytes || !group || !group->instances ||
        group->instances->empty()) {
        return false;
    }
    const size_t byte_count = group->instances->size() * sizeof(ProjectionInstance);
    if (byte_count > UINT32_MAX ||
        (maximum_spool_bytes != 0 &&
         (*spool_bytes > maximum_spool_bytes ||
          byte_count > maximum_spool_bytes - *spool_bytes))) {
        if (error) *error = "projection render spool exceeds its configured disk budget";
        return false;
    }
    const std::streampos position = writer->tellp();
    if (position < 0) {
        if (error) *error = "cannot determine projection render spool position";
        return false;
    }
    writer->write(
        reinterpret_cast<const char*>(group->instances->data()),
        static_cast<std::streamsize>(byte_count));
    if (!*writer) {
        if (error) *error = "cannot write projection render spool; storage may be full";
        return false;
    }
    group->spool_offset = static_cast<uint64_t>(position);
    group->spool_byte_count = static_cast<uint32_t>(byte_count);
    *spool_bytes += byte_count;
    group->instances.reset();
    return true;
}

bool spoolGroup(ProjectionStreamBuild* build, ProjectionGroup* group,
                std::string* error) {
    if (!build) return false;
    return spoolGroupToWriter(&build->spool_writer,
                              build->options.maximum_spool_bytes,
                              &build->spool_bytes, group, error);
}

size_t pendingGroupBytes(const ProjectionGroup& group) {
    if (!group.instances) return 0;
    const size_t count = group.instances->size();
    if (count > std::numeric_limits<size_t>::max() / sizeof(ProjectionInstance)) {
        return std::numeric_limits<size_t>::max();
    }
    return count * sizeof(ProjectionInstance);
}

bool appendGroupMetadata(ProjectionPlan* plan, ProjectionGroup group,
                         std::string* error) {
    if (!plan || group.instance_count <= 0 ||
        plan->groups.size() >= static_cast<size_t>(kInvalidGroupIndex)) {
        if (error) *error = "projection contains more render groups than supported";
        return false;
    }
    const uint32_t compact_index = static_cast<uint32_t>(plan->groups.size());
    if (UINT64_MAX - plan->detailed_instance_count <
        static_cast<uint64_t>(group.instance_count)) {
        if (error) *error = "projection detailed instance count overflow";
        return false;
    }
    const uint64_t key = columnKey(floorDiv(group.origin_x, kGroupSpan),
                                   floorDiv(group.origin_z, kGroupSpan));
    ProjectionColumn& column = plan->columns[key];
    plan->detailed_instance_count += static_cast<uint64_t>(group.instance_count);
    plan->groups.push_back(std::move(group));
    const auto insertSorted = [&](std::vector<uint32_t>* indices) {
        const auto before = std::lower_bound(
            indices->begin(), indices->end(), compact_index,
            [&](uint32_t left, uint32_t right) {
                const ProjectionGroup& left_group = plan->groups[left];
                const ProjectionGroup& right_group = plan->groups[right];
                return left_group.origin_y != right_group.origin_y
                    ? left_group.origin_y < right_group.origin_y : left < right;
            });
        indices->insert(before, compact_index);
    };
    insertSorted(&column.all_groups);
    if (!plan->groups[compact_index].flags.interior_only) {
        insertSorted(&column.surface_groups);
    }
    return true;
}

void failLazyAppend(ProjectionPlan* plan, std::string error) {
    if (!plan) return;
    std::lock_guard<std::mutex> lock(plan->pending_lazy_mutex);
    if (plan->lazy_append_error.empty()) {
        plan->lazy_append_error = std::move(error);
    }
    plan->pending_lazy_groups.clear();
    plan->pending_lazy_bytes = 0;
    plan->pending_lazy_cv.notify_all();
}

void drainLazyGroups(ProjectionPlan* plan) {
    if (!plan || !plan->lazy_surface_streaming || !plan->spool_writer) return;
    std::vector<ProjectionGroup> ready_groups;
    ready_groups.reserve(kMaximumLazyGroupsCommittedPerFrame);
    size_t remaining_bytes = kLazyCommitBudgetPerFrame;
    bool queue_error = false;
    {
        std::lock_guard<std::mutex> lock(plan->pending_lazy_mutex);
        queue_error = !plan->lazy_append_error.empty();
    }
    if (queue_error) return;

    std::string error;
    while (ready_groups.size() < kMaximumLazyGroupsCommittedPerFrame) {
        ProjectionGroup group;
        size_t group_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(plan->pending_lazy_mutex);
            if (!plan->lazy_append_error.empty() || plan->pending_lazy_groups.empty()) break;
            group_bytes = pendingGroupBytes(plan->pending_lazy_groups.front());
            // Permit a single unusually complex group through an empty budget;
            // otherwise it could starve forever behind the fixed frame cap.
            if (!ready_groups.empty() && group_bytes > remaining_bytes) break;
            group = std::move(plan->pending_lazy_groups.front());
            plan->pending_lazy_groups.pop_front();
            if (group_bytes > plan->pending_lazy_bytes) {
                plan->pending_lazy_bytes = 0;
            } else {
                plan->pending_lazy_bytes -= group_bytes;
            }
            plan->pending_lazy_cv.notify_all();
        }
        if (!spoolGroupToWriter(&plan->spool_writer, plan->maximum_spool_bytes,
                                &plan->spool_bytes, &group, &error)) {
            failLazyAppend(plan, error.empty()
                ? "cannot append lazy projection render data" : std::move(error));
            return;
        }
        ready_groups.push_back(std::move(group));
        if (group_bytes >= remaining_bytes) break;
        remaining_bytes -= group_bytes;
    }
    if (ready_groups.empty()) return;
    plan->spool_writer.flush();
    if (!plan->spool_writer) {
        failLazyAppend(plan, "cannot finalize lazy projection render data");
        return;
    }
    for (ProjectionGroup& group : ready_groups) {
        if (!appendGroupMetadata(plan, std::move(group), &error)) {
            failLazyAppend(plan, error.empty()
                ? "cannot publish lazy projection render group" : std::move(error));
            return;
        }
    }
    ++plan->topology_revision;
}

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    char log[512]{};
    GLsizei length = 0;
    glGetShaderInfoLog(shader, sizeof(log), &length, log);
    LOGE("shader compilation failed: %.*s", static_cast<int>(length), log);
    glDeleteShader(shader);
    return 0;
}

void invalidateGlResourcesForContextChange();

// Program, buffers and textures can belong to an EGL share group, while VAOs
// are strictly local to a single EGL context.  Keep these tests separate: a
// world re-entry can create a new child context which still sees our program
// and buffers, but whose numeric VAO names now belong to the game.
bool projectionProgramSignatureMatchesCurrentNamespace() {
    if (!g_program || glIsProgram(g_program) != GL_TRUE) {
        return false;
    }
    return glGetUniformLocation(g_program, "uUploadedTextureLayers") ==
               g_uploaded_texture_layers_location &&
        glGetUniformLocation(g_program, "uTexturesAvailable") ==
               g_textures_available_location;
}

bool projectionVaoBelongsToCurrentContext() {
    if (!g_cube_vao || !g_cube_vertex_buffer || !g_cube_index_buffer ||
        glIsVertexArray(g_cube_vao) != GL_TRUE) {
        return false;
    }
    GLint previous_vao = 0;
    GLint element_buffer = 0;
    GLint position_buffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vao);
    glBindVertexArray(g_cube_vao);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &position_buffer);
    glBindVertexArray(static_cast<GLuint>(previous_vao));
    return static_cast<GLuint>(element_buffer) == g_cube_index_buffer &&
        static_cast<GLuint>(position_buffer) == g_cube_vertex_buffer;
}

// This validates objects which are shareable by EGL contexts.  It deliberately
// does not compare EGLContext values, because a new child context can retain a
// valid shared namespace and must still release these module-owned objects.
bool projectionSharedGlObjectsBelongToCurrentNamespace() {
    return projectionProgramSignatureMatchesCurrentNamespace();
}

// Full renderer ownership additionally requires the same live EGL context and
// a VAO whose local bindings match our cube geometry.
bool projectionGlObjectsBelongToCurrentContext() {
    return g_gl_context != EGL_NO_CONTEXT &&
        eglGetCurrentContext() == g_gl_context &&
        projectionSharedGlObjectsBelongToCurrentNamespace() &&
        projectionVaoBelongsToCurrentContext();
}

void invalidateTextureResourcesForContextChange() {
    g_block_texture_array = 0;
    g_material_face_texture = 0;
    g_material_lookup_width = 1;
    g_uploaded_texture_layers = 0;
    g_allocated_texture_layers = 0;
    g_gpu_texture_pack.reset();
}

void releaseTextureResourcesForCurrentContext() {
    if (projectionSharedGlObjectsBelongToCurrentNamespace()) {
        if (g_block_texture_array) glDeleteTextures(1, &g_block_texture_array);
        if (g_material_face_texture) glDeleteTextures(1, &g_material_face_texture);
    }
    invalidateTextureResourcesForContextChange();
}

void releaseGlResourcesForCurrentContext() {
    const bool shared_objects_belong =
        projectionSharedGlObjectsBelongToCurrentNamespace();
    // Never delete a VAO merely because the program matches: VAOs are not
    // shared and a recycled numeric name can be a newly-created game VAO.
    const bool vao_belongs = shared_objects_belong &&
        g_gl_context != EGL_NO_CONTEXT &&
        eglGetCurrentContext() == g_gl_context &&
        projectionVaoBelongsToCurrentContext();
    if (vao_belongs) glDeleteVertexArrays(1, &g_cube_vao);
    if (shared_objects_belong) {
        if (g_block_texture_array) glDeleteTextures(1, &g_block_texture_array);
        if (g_material_face_texture) glDeleteTextures(1, &g_material_face_texture);
        if (g_cube_vertex_buffer) glDeleteBuffers(1, &g_cube_vertex_buffer);
        if (g_cube_index_buffer) glDeleteBuffers(1, &g_cube_index_buffer);
        if (g_program) glDeleteProgram(g_program);
    }
    invalidateGlResourcesForContextChange();
}

bool initializeGlResources() {
    const EGLContext current_context = eglGetCurrentContext();
    if (current_context == EGL_NO_CONTEXT || current_context != g_gl_context) {
        return false;
    }
    if (g_program && g_cube_vao && g_cube_vertex_buffer && g_cube_index_buffer &&
        projectionGlObjectsBelongToCurrentContext()) {
        // A recycled EGL handle can retain a shared program/VBO namespace while
        // its VAO is no longer valid.  Do not use the fast path until both the
        // shared shader signature and local VAO layout prove ownership.
        return true;
    }
    if (g_program || g_cube_vao || g_cube_vertex_buffer || g_cube_index_buffer) {
        releaseGlResourcesForCurrentContext();
    }
    static constexpr const char* vertex_source = R"GLSL(#version 300 es
precision highp float;
precision highp int;
layout(location = 0) in vec3 aCorner;
layout(location = 1) in uvec4 aBounds0;
layout(location = 2) in uvec2 aBounds1;
layout(location = 3) in vec4 aColor;
layout(location = 4) in float aFace;
layout(location = 5) in uint aPackedMaterial;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform int uGroupOriginY;
uniform bool uLinePass;
uniform int uLayerMinimum;
uniform int uLayerMaximum;
uniform bool uHasLayerMinimum;
uniform bool uHasLayerMaximum;
uniform highp usampler2D uMaterialFaces;
uniform int uMaterialLookupWidth;
uniform int uUploadedTextureLayers;
uniform bool uTexturesAvailable;
out vec4 vColor;
out vec2 vUv;
flat out vec3 vNormal;
flat out uint vPattern;
flat out uint vTextureLayer;
flat out uint vWorldMatchState;
flat out uint vReachabilityPreview;
void main() {
    const float fixedScale = 1.0 / 256.0;
    // Keep a mismatched projected block visibly outside the world block beneath
    // it without changing the packed source coordinates. Eight 8.8 fixed-point
    // units are 1/32 of a block on every side: large enough to remain legible
    // through the real block rather than collapsing into its depth surface.
    const float mismatchExpansion = 1.0 / 32.0;
    uvec4 bounds0 = aBounds0 & uvec4(0x1fffu);
    uvec2 bounds1 = aBounds1 & uvec2(0x1fffu);
    vec3 minimum = vec3(bounds0.xyz) * fixedScale;
    vec3 maximum = vec3(bounds0.w, bounds1.xy) * fixedScale;
    uint faceMask = aPackedMaterial & 0x3fu;
    uint worldMatchState = (aPackedMaterial >> 6u) & 0x3u;
    uint reachabilityPreview = (aPackedMaterial >> 15u) & 0x1u;
    uint materialId = aPackedMaterial >> 16u;
    vWorldMatchState = worldMatchState;
    vReachabilityPreview = reachabilityPreview;
    // A fully matching block must not merely be smaller than the real block:
    // translucent fills and the line pass can still make an inset primitive
    // visible. Move every vertex outside clip space before either pass builds
    // its geometry, leaving the target position genuinely hidden.
    if (worldMatchState == 3u) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vUv = vec2(0.0);
        vNormal = vec3(0.0, 1.0, 0.0);
        vPattern = 0u;
        vTextureLayer = 0u;
        return;
    }
    // A wrong material and a wrong state deliberately bypass the block
    // texture so their red/yellow diagnosis remains unmistakable even when a
    // texture pack is installed.
    if (worldMatchState == 1u) {
        vColor = vec4(0.96, 0.11, 0.10, 1.0);
        vPattern = 0u;
    } else if (worldMatchState == 2u) {
        vColor = vec4(1.0, 0.78, 0.08, 1.0);
        vPattern = 0u;
    } else if (reachabilityPreview != 0u) {
        // A bright mint/cyan signal remains readable on both dark builds and
        // ordinary green materials. It deliberately bypasses block textures.
        vColor = vec4(0.08, 1.0, 0.66, 1.0);
        vPattern = 0u;
    } else {
        vColor = aColor;
        vPattern = (aPackedMaterial >> 8u) & 0x7fu;
    }
    vTextureLayer = 0u;
    int blockY = uGroupOriginY + int(bounds0.y >> 8u);
    if (worldMatchState == 1u || worldMatchState == 2u) {
        // This applies only to the transient VBO state carried in bits 6..7.
        // Source instances and their disk spool retain their original bounds,
        // including at the edge of a 16-block render group.
        minimum -= vec3(mismatchExpansion);
        maximum += vec3(mismatchExpansion);
    } else if (reachabilityPreview != 0u) {
        // The target is known air, so a tiny expansion cannot cover a real
        // block but makes the preview survive depth/face culling at a glance.
        minimum -= vec3(1.0 / 64.0);
        maximum += vec3(1.0 / 64.0);
    }
    if (uLinePass) {
        uint outlineMask =
            ((aBounds0.x >> 13u) & 7u) |
            (((aBounds0.y >> 13u) & 7u) << 3u) |
            (((aBounds0.z >> 13u) & 7u) << 6u) |
            (((aBounds0.w >> 13u) & 7u) << 9u) |
            (((aBounds1.x >> 13u) & 7u) << 12u) |
            (((aBounds1.y >> 13u) & 1u) << 15u);
        int edge = int(aFace + 0.5) - 6;
        bool visible = false;
        if (edge >= 0 && edge < 12) {
            visible = (outlineMask & (1u << uint(edge))) != 0u;
        }
        int cutSide = -1;
        if (uHasLayerMinimum && blockY == uLayerMinimum) {
            if (edge == 0) cutSide = 0;
            else if (edge == 4) cutSide = 1;
            else if (edge == 8) cutSide = 2;
            else if (edge == 9) cutSide = 3;
        }
        if (uHasLayerMaximum && blockY == uLayerMaximum) {
            if (edge == 2) cutSide = 0;
            else if (edge == 6) cutSide = 1;
            else if (edge == 11) cutSide = 2;
            else if (edge == 10) cutSide = 3;
        }
        if (cutSide >= 0) {
            // A layer filter replaces the original horizontal feature edge
            // with the selected slice's merged perimeter at that cut plane.
            visible = (outlineMask & (1u << uint(12 + cutSide))) != 0u;
        }
        if (!visible) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        } else {
            vec3 position = mix(minimum, maximum, aCorner) + uOffset;
            gl_Position = uMvp * vec4(position, 1.0);
            // Keep a visible edge on its own surface without pulling a line far
            // enough forward to reveal geometry hidden by another block.
            gl_Position.z -= 2.0e-6 * gl_Position.w;
        }
        vUv = vec2(0.0);
        vNormal = vec3(0.0, 1.0, 0.0);
        return;
    }
    int face = int(aFace + 0.5);
    bool layerBoundaryFace =
        (face == 4 && uHasLayerMinimum && blockY == uLayerMinimum) ||
        (face == 5 && uHasLayerMaximum && blockY == uLayerMaximum);
    if (!layerBoundaryFace && (faceMask & (1u << uint(face))) == 0u) {
        // All four vertices of a hidden face land outside the same clip plane,
        // avoiding the much more expensive fragment-stage discard path.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vUv = vec2(0.0);
        vNormal = vec3(0.0, 1.0, 0.0);
        return;
    }
    vec3 position = mix(minimum, maximum, aCorner) + uOffset;
    gl_Position = uMvp * vec4(position, 1.0);
    vec3 extent = maximum - minimum;
    vec3 localMinimum = fract(minimum);
    if (face < 2) {
        vUv = localMinimum.xy + aCorner.xy * extent.xy;
        vNormal = vec3(0.0, 0.0, face == 0 ? -1.0 : 1.0);
    } else if (face < 4) {
        vUv = localMinimum.zy + aCorner.zy * extent.zy;
        vNormal = vec3(face == 2 ? -1.0 : 1.0, 0.0, 0.0);
    } else {
        vUv = localMinimum.xz + aCorner.xz * extent.xz;
        vNormal = vec3(0.0, face == 4 ? -1.0 : 1.0, 0.0);
    }
    if (reachabilityPreview == 0u && (worldMatchState == 0u || worldMatchState == 3u) &&
        uTexturesAvailable && materialId != 0u) {
        int lookupIndex = int(materialId) * 6 + face;
        ivec2 lookupCoordinate = ivec2(lookupIndex % uMaterialLookupWidth,
                                       lookupIndex / uMaterialLookupWidth);
        uint textureLayer = texelFetch(uMaterialFaces, lookupCoordinate, 0).r;
        if (textureLayer != 0u && int(textureLayer) < uUploadedTextureLayers) {
            vTextureLayer = textureLayer;
        }
    }
}
)GLSL";
    static constexpr const char* fragment_source = R"GLSL(#version 300 es
precision mediump float;
precision highp int;
in vec4 vColor;
in vec2 vUv;
flat in vec3 vNormal;
flat in uint vPattern;
flat in uint vTextureLayer;
flat in uint vWorldMatchState;
flat in uint vReachabilityPreview;
uniform float uAlpha;
uniform bool uLinePass;
uniform highp sampler2DArray uBlockTextures;
layout(location = 0) out vec4 outColor;

float materialTone(vec2 uv, uint pattern) {
    vec2 pixel = floor(clamp(uv, vec2(0.0), vec2(0.999)) * 4.0);
    if (pattern == 1u) {
        return mod(pixel.x, 2.0) < 1.0 ? 0.84 : 1.08;
    }
    if (pattern == 2u) {
        float row = floor(uv.y * 4.0);
        float brick = fract(uv.x * 2.0 + mod(row, 2.0) * 0.5);
        return brick < 0.10 || fract(uv.y * 4.0) < 0.10 ? 0.74 : 1.06;
    }
    if (pattern == 3u) {
        float noise = fract(dot(pixel + vec2(0.31, 0.73), vec2(0.7548777, 0.5698403)));
        return noise > 0.68 ? 1.23 : 0.88;
    }
    if (pattern == 4u) {
        float diagonal = abs(fract((uv.x + uv.y) * 3.0) - 0.5);
        return diagonal < 0.075 ? 1.18 : 0.90;
    }
    if (pattern == 5u) {
        float noise = fract(dot(pixel + vec2(7.0, 3.0), vec2(0.6180340, 0.4142136)));
        return 0.82 + noise * 0.30;
    }
    return mod(pixel.x + pixel.y, 2.0) < 1.0 ? 0.93 : 1.03;
}

void main() {
    if (uLinePass) {
        if (vWorldMatchState == 1u || vWorldMatchState == 2u ||
            vReachabilityPreview != 0u) {
            // Diagnostic and next-place edges should read as one continuous
            // signal rather than falling back to the normal contrast color.
            outColor = vec4(vColor.rgb, uAlpha);
            return;
        }
        float luminance = dot(vColor.rgb, vec3(0.2126, 0.7152, 0.0722));
        vec3 outlineColor = luminance >= 0.45 ? vec3(0.025) : vec3(0.975);
        outColor = vec4(outlineColor, uAlpha);
        return;
    }
    vec3 lightDirection = normalize(vec3(-0.35, 0.85, 0.40));
    float lighting = 0.70 + 0.30 * max(dot(vNormal, lightDirection), 0.0);
    if (vTextureLayer != 0u) {
        vec4 texel = texture(uBlockTextures,
                             vec3(clamp(vUv, vec2(0.0), vec2(1.0)),
                                  float(vTextureLayer)));
        if (texel.a <= (0.5 / 255.0)) discard;
        float alpha = texel.a * uAlpha;
        outColor = vec4(clamp(texel.rgb * lighting, 0.0, 1.0) * alpha, alpha);
        return;
    }
    vec3 shadedColor = clamp(vColor.rgb * lighting * materialTone(vUv, vPattern), 0.0, 1.0);
    outColor = vec4(shadedColor * uAlpha, uAlpha);
}
)GLSL";

    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    g_program = glCreateProgram();
    glAttachShader(g_program, vertex);
    glAttachShader(g_program, fragment);
    glLinkProgram(g_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[512]{};
        GLsizei length = 0;
        glGetProgramInfoLog(g_program, sizeof(log), &length, log);
        LOGE("shader link failed: %.*s", static_cast<int>(length), log);
        glDeleteProgram(g_program);
        g_program = 0;
        return false;
    }
    g_mvp_location = glGetUniformLocation(g_program, "uMvp");
    g_offset_location = glGetUniformLocation(g_program, "uOffset");
    g_alpha_location = glGetUniformLocation(g_program, "uAlpha");
    g_line_pass_location = glGetUniformLocation(g_program, "uLinePass");
    g_group_origin_y_location = glGetUniformLocation(g_program, "uGroupOriginY");
    g_layer_minimum_location = glGetUniformLocation(g_program, "uLayerMinimum");
    g_layer_maximum_location = glGetUniformLocation(g_program, "uLayerMaximum");
    g_has_layer_minimum_location = glGetUniformLocation(g_program, "uHasLayerMinimum");
    g_has_layer_maximum_location = glGetUniformLocation(g_program, "uHasLayerMaximum");
    g_block_textures_location = glGetUniformLocation(g_program, "uBlockTextures");
    g_material_faces_location = glGetUniformLocation(g_program, "uMaterialFaces");
    g_material_lookup_width_location = glGetUniformLocation(g_program, "uMaterialLookupWidth");
    g_uploaded_texture_layers_location =
        glGetUniformLocation(g_program, "uUploadedTextureLayers");
    g_textures_available_location = glGetUniformLocation(g_program, "uTexturesAvailable");
    glUseProgram(g_program);
    glUniform1i(g_block_textures_location, kBlockTextureUnitIndex);
    glUniform1i(g_material_faces_location, kMaterialLookupTextureUnitIndex);

    // Each face has its own four vertices so the shader can discard covered
    // faces. Lines use 24 additional endpoints because every endpoint needs a
    // stable edge ID (aFace = 6 + edge) for per-instance outline culling.
    static constexpr GLfloat vertices[] = {
        0.f, 0.f, 0.f, 0.f,  1.f, 0.f, 0.f, 0.f,  1.f, 1.f, 0.f, 0.f,  0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 1.f,  1.f, 0.f, 1.f, 1.f,  1.f, 1.f, 1.f, 1.f,  0.f, 1.f, 1.f, 1.f,
        0.f, 0.f, 0.f, 2.f,  0.f, 0.f, 1.f, 2.f,  0.f, 1.f, 1.f, 2.f,  0.f, 1.f, 0.f, 2.f,
        1.f, 0.f, 0.f, 3.f,  1.f, 1.f, 0.f, 3.f,  1.f, 1.f, 1.f, 3.f,  1.f, 0.f, 1.f, 3.f,
        0.f, 0.f, 0.f, 4.f,  1.f, 0.f, 0.f, 4.f,  1.f, 0.f, 1.f, 4.f,  0.f, 0.f, 1.f, 4.f,
        0.f, 1.f, 0.f, 5.f,  0.f, 1.f, 1.f, 5.f,  1.f, 1.f, 1.f, 5.f,  1.f, 1.f, 0.f, 5.f,
        0.f, 0.f, 0.f, 6.f,  1.f, 0.f, 0.f, 6.f,
        1.f, 0.f, 0.f, 7.f,  1.f, 1.f, 0.f, 7.f,
        1.f, 1.f, 0.f, 8.f,  0.f, 1.f, 0.f, 8.f,
        0.f, 1.f, 0.f, 9.f,  0.f, 0.f, 0.f, 9.f,
        0.f, 0.f, 1.f, 10.f, 1.f, 0.f, 1.f, 10.f,
        1.f, 0.f, 1.f, 11.f, 1.f, 1.f, 1.f, 11.f,
        1.f, 1.f, 1.f, 12.f, 0.f, 1.f, 1.f, 12.f,
        0.f, 1.f, 1.f, 13.f, 0.f, 0.f, 1.f, 13.f,
        0.f, 0.f, 0.f, 14.f, 0.f, 0.f, 1.f, 14.f,
        1.f, 0.f, 0.f, 15.f, 1.f, 0.f, 1.f, 15.f,
        1.f, 1.f, 0.f, 16.f, 1.f, 1.f, 1.f, 16.f,
        0.f, 1.f, 0.f, 17.f, 0.f, 1.f, 1.f, 17.f,
    };
    static constexpr GLushort indices[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11, 12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
        24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36, 37, 38, 39,
        40, 41, 42, 43, 44, 45, 46, 47,
    };

    glGenVertexArrays(1, &g_cube_vao);
    glGenBuffers(1, &g_cube_vertex_buffer);
    glGenBuffers(1, &g_cube_index_buffer);
    if (!g_cube_vao || !g_cube_vertex_buffer || !g_cube_index_buffer) {
        releaseGlResourcesForCurrentContext();
        return false;
    }
    glBindVertexArray(g_cube_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_cube_vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(3 * sizeof(GLfloat)));
    static constexpr std::array<GLuint, 4> instance_attributes{{1, 2, 3, 5}};
    for (const GLuint attribute : instance_attributes) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribDivisor(attribute, 1);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_cube_index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    GLint vertex_buffer_size = 0;
    GLint index_buffer_size = 0;
    glBindBuffer(GL_ARRAY_BUFFER, g_cube_vertex_buffer);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vertex_buffer_size);
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &index_buffer_size);
    if (vertex_buffer_size != static_cast<GLint>(sizeof(vertices)) ||
        index_buffer_size != static_cast<GLint>(sizeof(indices))) {
        glBindVertexArray(0);
        releaseGlResourcesForCurrentContext();
        return false;
    }
    GLfloat line_width_range[2] = {1.f, 1.f};
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, line_width_range);
    if (std::isfinite(line_width_range[0]) && std::isfinite(line_width_range[1]) &&
        line_width_range[0] > 0.f && line_width_range[1] >= line_width_range[0]) {
        g_outline_line_width = std::max(line_width_range[0],
                                        std::min(2.f, line_width_range[1]));
    } else {
        g_outline_line_width = 1.f;
    }
    glBindVertexArray(0);
    return true;
}

size_t groupInstanceBytes(const ProjectionGroup& group) {
    return group.instances ? group.instances->size() * sizeof(ProjectionInstance) : 0;
}

size_t batchInstanceBytes(const std::array<ProjectionDrawBatch, 2>& batches,
                          size_t batch_count) {
    if (batch_count == 0 || batch_count > batches.size()) return 0;
    size_t instances = 0;
    for (size_t index = 0; index < batch_count; ++index) {
        if (batches[index].instance_count == 0 ||
            batches[index].instance_count >
                std::numeric_limits<size_t>::max() - instances) {
            return 0;
        }
        instances += batches[index].instance_count;
    }
    if (instances > std::numeric_limits<size_t>::max() /
                        sizeof(ProjectionInstance)) return 0;
    return instances * sizeof(ProjectionInstance);
}

bool validSourceBatches(const ProjectionGroup& group,
                        const std::array<ProjectionDrawBatch, 2>& batches,
                        size_t batch_count) {
    if (group.instance_count <= 0 || batch_count == 0 ||
        batch_count > batches.size()) return false;
    const size_t total_instances = static_cast<size_t>(group.instance_count);
    size_t previous_end = 0;
    for (size_t index = 0; index < batch_count; ++index) {
        const size_t first = batches[index].first_instance;
        const size_t count = batches[index].instance_count;
        if (count == 0 || first > total_instances ||
            count > total_instances - first ||
            (index != 0 && first < previous_end)) {
            return false;
        }
        previous_end = first + count;
    }
    return true;
}

bool cpuBatchesForRequest(const ProjectionGroup& group,
                          const std::array<ProjectionDrawBatch, 2>& source_batches,
                          size_t batch_count,
                          std::array<ProjectionDrawBatch, 2>* staging_batches) {
    if (!staging_batches || !group.instances || group.instances->empty() ||
        !validSourceBatches(group, source_batches, batch_count)) return false;
    if (group.spool_offset == kInvalidSpoolOffset) {
        if (group.instances->size() != static_cast<size_t>(group.instance_count)) return false;
        for (size_t index = 0; index < batch_count; ++index) {
            (*staging_batches)[index] = source_batches[index];
        }
        return true;
    }
    if (group.cpu_batch_count != batch_count) return false;
    size_t staging_first = 0;
    for (size_t index = 0; index < batch_count; ++index) {
        if (group.cpu_source_first[index] != source_batches[index].first_instance ||
            group.cpu_source_count[index] != source_batches[index].instance_count ||
            source_batches[index].instance_count >
                group.instances->size() -
                    std::min(staging_first, group.instances->size())) {
            return false;
        }
        (*staging_batches)[index] = source_batches[index];
        (*staging_batches)[index].first_instance = static_cast<uint32_t>(staging_first);
        staging_first += source_batches[index].instance_count;
    }
    return staging_first == group.instances->size();
}

void clearCpuBatchMetadata(ProjectionGroup* group) {
    if (!group) return;
    group->cpu_batch_count = 0;
    group->cpu_source_first.fill(0);
    group->cpu_source_count.fill(0);
}

void unlinkCpuGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    if (!group.flags.in_cpu_lru) return;
    if (group.cpu_lru_previous != kInvalidGroupIndex) {
        plan->groups[group.cpu_lru_previous].cpu_lru_next = group.cpu_lru_next;
    } else {
        plan->cpu_lru_head = group.cpu_lru_next;
    }
    if (group.cpu_lru_next != kInvalidGroupIndex) {
        plan->groups[group.cpu_lru_next].cpu_lru_previous = group.cpu_lru_previous;
    } else {
        plan->cpu_lru_tail = group.cpu_lru_previous;
    }
    group.cpu_lru_previous = kInvalidGroupIndex;
    group.cpu_lru_next = kInvalidGroupIndex;
    group.flags.in_cpu_lru = false;
}

void touchCpuGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    if (!group.flags.in_cpu_lru) return;
    group.cpu_last_used_frame = g_render_frame;
    if (plan->cpu_lru_tail == index) return;
    const size_t previous = group.cpu_lru_previous;
    const size_t next = group.cpu_lru_next;
    if (previous != kInvalidGroupIndex) {
        plan->groups[previous].cpu_lru_next = next;
    } else {
        plan->cpu_lru_head = next;
    }
    if (next != kInvalidGroupIndex) {
        plan->groups[next].cpu_lru_previous = previous;
    }
    group.cpu_lru_previous = plan->cpu_lru_tail;
    group.cpu_lru_next = kInvalidGroupIndex;
    if (plan->cpu_lru_tail != kInvalidGroupIndex) {
        plan->groups[plan->cpu_lru_tail].cpu_lru_next = index;
    } else {
        plan->cpu_lru_head = index;
    }
    plan->cpu_lru_tail = index;
}

void registerCpuGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    if (group.spool_offset == kInvalidSpoolOffset || !group.instances ||
        group.instances->empty()) return;
    if (group.flags.in_cpu_lru) {
        touchCpuGroup(plan, index);
        return;
    }
    group.cpu_lru_previous = plan->cpu_lru_tail;
    group.cpu_lru_next = kInvalidGroupIndex;
    group.cpu_last_used_frame = g_render_frame;
    group.flags.in_cpu_lru = true;
    if (plan->cpu_lru_tail != kInvalidGroupIndex) {
        plan->groups[plan->cpu_lru_tail].cpu_lru_next = index;
    } else {
        plan->cpu_lru_head = index;
    }
    plan->cpu_lru_tail = index;
    ++plan->resident_cpu_group_count;
    plan->resident_cpu_bytes += groupInstanceBytes(group);
}

void evictCpuGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    if (group.spool_offset == kInvalidSpoolOffset) return;
    const size_t byte_count = groupInstanceBytes(group);
    if (group.flags.in_cpu_lru) {
        unlinkCpuGroup(plan, index);
        if (plan->resident_cpu_group_count > 0) --plan->resident_cpu_group_count;
        plan->resident_cpu_bytes = plan->resident_cpu_bytes >= byte_count
            ? plan->resident_cpu_bytes - byte_count : 0;
    }
    group.instances.reset();
    clearCpuBatchMetadata(&group);
}

bool ensureCpuCapacity(ProjectionPlan* plan, size_t required_bytes,
                       size_t* frame_evictions) {
    if (!plan || !frame_evictions || required_bytes == 0 ||
        required_bytes > plan->maximum_cpu_cache_bytes) return false;
    while (plan->resident_cpu_bytes > plan->maximum_cpu_cache_bytes - required_bytes) {
        if (*frame_evictions >= kMaximumEvictionsPerFrame) return false;
        const size_t candidate = plan->cpu_lru_head;
        if (candidate == kInvalidGroupIndex || candidate >= plan->groups.size()) return false;
        if (plan->groups[candidate].cpu_last_used_frame == g_render_frame) return false;
        evictCpuGroup(plan, candidate);
        ++*frame_evictions;
    }
    return true;
}

bool loadGroupBatchesFromSpool(
    ProjectionPlan* plan, size_t index,
    const std::array<ProjectionDrawBatch, 2>& source_batches, size_t batch_count,
    size_t* read_budget, size_t* read_calls, size_t* frame_evictions,
    const std::chrono::steady_clock::time_point& read_start_deadline) {
    if (!plan || index >= plan->groups.size() || !read_budget || !read_calls ||
        !frame_evictions) return false;
    ProjectionGroup& group = plan->groups[index];
    if (!validSourceBatches(group, source_batches, batch_count)) return false;

    std::array<ProjectionDrawBatch, 2> staging_batches{};
    if (group.spool_offset == kInvalidSpoolOffset) {
        return cpuBatchesForRequest(
            group, source_batches, batch_count, &staging_batches);
    }
    if (cpuBatchesForRequest(group, source_batches, batch_count, &staging_batches)) {
        if (group.flags.in_cpu_lru) touchCpuGroup(plan, index);
        else registerCpuGroup(plan, index);
        return true;
    }
    // A byte budget controls throughput but cannot protect a frame from slow
    // flash storage. Once the start deadline expires, defer every new disk
    // operation to a later frame. An individual blocking read can still run
    // long, but multiple slow groups can no longer accumulate in one frame.
    // The frame's first read keeps a bounded extension of that deadline so a
    // long candidate walk cannot starve streaming entirely, but it can no
    // longer begin an unbounded read arbitrarily late in the frame.
    {
        const auto now = std::chrono::steady_clock::now();
        const auto first_read_deadline = read_start_deadline +
            2 * kCpuReadStartBudgetPerFrame;
        if (now >= (*read_calls == 0 ? first_read_deadline : read_start_deadline)) {
            return false;
        }
    }

    if (group.flags.in_cpu_lru || group.instances) {
        if (*frame_evictions >= kMaximumEvictionsPerFrame) return false;
        evictCpuGroup(plan, index);
        ++*frame_evictions;
    } else {
        clearCpuBatchMetadata(&group);
    }

    const size_t byte_count = batchInstanceBytes(source_batches, batch_count);
    const size_t full_byte_count = static_cast<size_t>(group.instance_count) *
        sizeof(ProjectionInstance);
    if (byte_count == 0 || full_byte_count != group.spool_byte_count ||
        byte_count > *read_budget) return false;

    size_t read_operation_count = 1;
    for (size_t batch = 1; batch < batch_count; ++batch) {
        const uint64_t previous_end = static_cast<uint64_t>(
            source_batches[batch - 1].first_instance) +
            source_batches[batch - 1].instance_count;
        if (previous_end != source_batches[batch].first_instance) {
            ++read_operation_count;
        }
    }
    if (read_operation_count > kMaximumCpuLoadsPerFrame ||
        *read_calls > kMaximumCpuLoadsPerFrame - read_operation_count ||
        !ensureCpuCapacity(plan, byte_count, frame_evictions)) return false;

    if (!plan->spool_reader.is_open()) {
        try {
            plan->spool_reader.clear();
            plan->spool_reader.open(plan->spool_path, std::ios::binary);
        } catch (...) {
            plan->spool_reader.clear();
            return false;
        }
    }
    if (!plan->spool_reader) return false;

    const size_t selected_instance_count = byte_count / sizeof(ProjectionInstance);
    std::vector<ProjectionInstance> staging;
    try {
        staging.resize(selected_instance_count);
    } catch (...) {
        return false;
    }

    // Reserve the complete operation before issuing any reads. A failed seek or
    // short read therefore cannot cause repeated attempts to exceed a frame's
    // disk budget.
    *read_budget -= byte_count;
    *read_calls += read_operation_count;

    bool read_succeeded = true;
    size_t batch = 0;
    size_t staging_first = 0;
    try {
        while (batch < batch_count) {
            const size_t source_first = source_batches[batch].first_instance;
            size_t source_count = source_batches[batch].instance_count;
            size_t next_batch = batch + 1;
            while (next_batch < batch_count &&
                   static_cast<uint64_t>(source_first) + source_count ==
                       source_batches[next_batch].first_instance) {
                source_count += source_batches[next_batch].instance_count;
                ++next_batch;
            }

            const uint64_t relative_offset = static_cast<uint64_t>(source_first) *
                sizeof(ProjectionInstance);
            const size_t span_bytes = source_count * sizeof(ProjectionInstance);
            if (relative_offset > group.spool_byte_count ||
                span_bytes > static_cast<size_t>(group.spool_byte_count - relative_offset) ||
                group.spool_offset > std::numeric_limits<uint64_t>::max() - relative_offset) {
                read_succeeded = false;
                break;
            }
            const uint64_t absolute_offset = group.spool_offset + relative_offset;
            if (absolute_offset > static_cast<uint64_t>(
                    (std::numeric_limits<std::streamoff>::max)()) ||
                span_bytes > static_cast<size_t>(
                    (std::numeric_limits<std::streamsize>::max)())) {
                read_succeeded = false;
                break;
            }

            plan->spool_reader.clear();
            plan->spool_reader.seekg(
                static_cast<std::streamoff>(absolute_offset), std::ios::beg);
            if (!plan->spool_reader) {
                read_succeeded = false;
                break;
            }
            plan->spool_reader.read(
                reinterpret_cast<char*>(staging.data() + staging_first),
                static_cast<std::streamsize>(span_bytes));
            if (!plan->spool_reader || plan->spool_reader.gcount() !=
                    static_cast<std::streamsize>(span_bytes)) {
                read_succeeded = false;
                break;
            }
            staging_first += source_count;
            batch = next_batch;
        }
    } catch (...) {
        read_succeeded = false;
    }
    if (!read_succeeded || staging_first != staging.size()) {
        plan->spool_reader.clear();
        return false;
    }

    group.instances = std::make_unique<std::vector<ProjectionInstance>>(
        std::move(staging));
    group.cpu_batch_count = static_cast<uint8_t>(batch_count);
    for (size_t source = 0; source < batch_count; ++source) {
        group.cpu_source_first[source] = static_cast<uint16_t>(
            source_batches[source].first_instance);
        group.cpu_source_count[source] = static_cast<uint16_t>(
            source_batches[source].instance_count);
    }
    registerCpuGroup(plan, index);
    return true;
}

void unlinkResidentGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    if (!group.flags.in_resident_lru) return;

    if (group.lru_previous != kInvalidGroupIndex) {
        plan->groups[group.lru_previous].lru_next = group.lru_next;
    } else {
        plan->resident_lru_head = group.lru_next;
    }
    if (group.lru_next != kInvalidGroupIndex) {
        plan->groups[group.lru_next].lru_previous = group.lru_previous;
    } else {
        plan->resident_lru_tail = group.lru_previous;
    }
    group.lru_previous = kInvalidGroupIndex;
    group.lru_next = kInvalidGroupIndex;
    group.flags.in_resident_lru = false;
}

void touchResidentGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    if (!group.flags.in_resident_lru) return;
    group.last_used_frame = g_render_frame;
    if (plan->resident_lru_tail == index) return;

    const size_t previous = group.lru_previous;
    const size_t next = group.lru_next;
    if (previous != kInvalidGroupIndex) {
        plan->groups[previous].lru_next = next;
    } else {
        plan->resident_lru_head = next;
    }
    if (next != kInvalidGroupIndex) {
        plan->groups[next].lru_previous = previous;
    }

    group.lru_previous = plan->resident_lru_tail;
    group.lru_next = kInvalidGroupIndex;
    if (plan->resident_lru_tail != kInvalidGroupIndex) {
        plan->groups[plan->resident_lru_tail].lru_next = index;
    } else {
        plan->resident_lru_head = index;
    }
    plan->resident_lru_tail = index;
}

void registerResidentGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    if (group.flags.in_resident_lru) {
        touchResidentGroup(plan, index);
        return;
    }
    group.lru_previous = plan->resident_lru_tail;
    group.lru_next = kInvalidGroupIndex;
    group.last_used_frame = g_render_frame;
    group.flags.in_resident_lru = true;
    if (plan->resident_lru_tail != kInvalidGroupIndex) {
        plan->groups[plan->resident_lru_tail].lru_next = index;
    } else {
        plan->resident_lru_head = index;
    }
    plan->resident_lru_tail = index;
    ++plan->resident_buffer_count;
    plan->resident_buffer_bytes += group.gpu_byte_count;
}

void evictResidentGroup(ProjectionPlan* plan, size_t index) {
    if (!plan || index >= plan->groups.size()) return;
    ProjectionGroup& group = plan->groups[index];
    const size_t byte_count = group.gpu_byte_count;
    if (group.flags.in_resident_lru) {
        unlinkResidentGroup(plan, index);
        if (plan->resident_buffer_count > 0) --plan->resident_buffer_count;
        plan->resident_buffer_bytes = plan->resident_buffer_bytes >= byte_count
            ? plan->resident_buffer_bytes - byte_count : 0;
    }
    if (group.instance_buffer) {
        glDeleteBuffers(1, &group.instance_buffer);
        group.instance_buffer = 0;
    }
    group.gpu_byte_count = 0;
    group.gpu_batch_count = 0;
    group.flags.gpu_outline_prepared = false;
    group.gpu_presentation_fingerprint = 0;
    group.gpu_source_first.fill(0);
    group.gpu_source_count.fill(0);
    // instance_count describes disk/GPU contents and remains valid even when
    // the CPU vector is evicted.
}

bool ensureGpuCapacity(ProjectionPlan* plan, size_t required_bytes,
                       size_t* frame_evictions) {
    if (!plan || !frame_evictions || required_bytes == 0 ||
        required_bytes > kMaximumResidentBufferBytes) return false;
    while (plan->resident_buffer_count >= kMaximumResidentBuffers ||
           plan->resident_buffer_bytes > kMaximumResidentBufferBytes - required_bytes) {
        if (*frame_evictions >= kMaximumEvictionsPerFrame) return false;
        const size_t candidate = plan->resident_lru_head;
        if (candidate == kInvalidGroupIndex || candidate >= plan->groups.size()) return false;
        if (plan->groups[candidate].last_used_frame == g_render_frame) return false;
        evictResidentGroup(plan, candidate);
        ++*frame_evictions;
    }
    return true;
}

void releasePlanGpu(const std::shared_ptr<ProjectionPlan>& plan) {
    if (!plan) return;
    // Instance VBOs are share-group objects.  A valid module program proves
    // that the current namespace can still see the projection share group,
    // even when its local VAO was recreated with the world renderer.
    const bool can_delete = projectionSharedGlObjectsBelongToCurrentNamespace();
    std::array<GLuint, 128> buffers{};
    size_t buffer_count = 0;
    size_t index = plan->resident_lru_head;
    size_t remaining = std::min(plan->resident_buffer_count, kMaximumResidentBuffers);
    while (index != kInvalidGroupIndex && index < plan->groups.size() && remaining-- > 0) {
        ProjectionGroup& group = plan->groups[index];
        const size_t next = group.lru_next;
        if (group.instance_buffer && can_delete) {
            buffers[buffer_count++] = group.instance_buffer;
            if (buffer_count == buffers.size()) {
                glDeleteBuffers(static_cast<GLsizei>(buffer_count), buffers.data());
                buffer_count = 0;
            }
        }
        group.instance_buffer = 0;
        group.gpu_byte_count = 0;
        group.gpu_batch_count = 0;
        group.flags.gpu_outline_prepared = false;
        group.gpu_presentation_fingerprint = 0;
        group.gpu_source_first.fill(0);
        group.gpu_source_count.fill(0);
        // Preserve the count stored in the immutable group metadata.
        group.lru_previous = kInvalidGroupIndex;
        group.lru_next = kInvalidGroupIndex;
        group.last_used_frame = 0;
        group.flags.in_resident_lru = false;
        index = next;
    }
    if (can_delete && buffer_count > 0) {
        glDeleteBuffers(static_cast<GLsizei>(buffer_count), buffers.data());
    }
    plan->resident_buffer_count = 0;
    plan->resident_buffer_bytes = 0;
    plan->resident_lru_head = kInvalidGroupIndex;
    plan->resident_lru_tail = kInvalidGroupIndex;
}

void invalidatePlanGpu(const std::shared_ptr<ProjectionPlan>& plan) {
    if (!plan) return;
    size_t index = plan->resident_lru_head;
    size_t remaining = std::min(plan->resident_buffer_count, kMaximumResidentBuffers);
    while (index != kInvalidGroupIndex && index < plan->groups.size() && remaining-- > 0) {
        ProjectionGroup& group = plan->groups[index];
        const size_t next = group.lru_next;
        // The previous EGL context owns these names. They cannot be deleted
        // from the new context, but the retained CPU instances can be uploaded
        // lazily after the game recreates its renderer.
        group.instance_buffer = 0;
        group.gpu_byte_count = 0;
        group.gpu_batch_count = 0;
        group.flags.gpu_outline_prepared = false;
        group.gpu_presentation_fingerprint = 0;
        group.gpu_source_first.fill(0);
        group.gpu_source_count.fill(0);
        // Preserve the count stored in the immutable group metadata.
        group.lru_previous = kInvalidGroupIndex;
        group.lru_next = kInvalidGroupIndex;
        group.last_used_frame = 0;
        group.flags.in_resident_lru = false;
        index = next;
    }
    plan->resident_buffer_count = 0;
    plan->resident_buffer_bytes = 0;
    plan->resident_lru_head = kInvalidGroupIndex;
    plan->resident_lru_tail = kInvalidGroupIndex;
}

void invalidateGlResourcesForContextChange() {
    invalidateTextureResourcesForContextChange();
    g_program = 0;
    g_cube_vao = 0;
    g_cube_vertex_buffer = 0;
    g_cube_index_buffer = 0;
    g_mvp_location = -1;
    g_offset_location = -1;
    g_alpha_location = -1;
    g_line_pass_location = -1;
    g_group_origin_y_location = -1;
    g_layer_minimum_location = -1;
    g_layer_maximum_location = -1;
    g_has_layer_minimum_location = -1;
    g_has_layer_maximum_location = -1;
    g_block_textures_location = -1;
    g_material_faces_location = -1;
    g_material_lookup_width_location = -1;
    g_uploaded_texture_layers_location = -1;
    g_textures_available_location = -1;
    g_outline_line_width = 1.f;
}

struct GlStateSnapshot {
    GLint program = 0;
    GLint vertex_array = 0;
    GLint array_buffer = 0;
    GLint depth_function = GL_LESS;
    GLint blend_source_rgb = GL_ONE;
    GLint blend_destination_rgb = GL_ZERO;
    GLint blend_source_alpha = GL_ONE;
    GLint blend_destination_alpha = GL_ZERO;
    GLint blend_equation_rgb = GL_FUNC_ADD;
    GLint blend_equation_alpha = GL_FUNC_ADD;
    GLint cull_face_mode = GL_BACK;
    GLint active_texture = GL_TEXTURE0;
    GLint block_texture_array_binding = 0;
    GLint material_texture_2d_binding = 0;
    std::array<GLint, 2> sampler_bindings{};
    GLfloat line_width = 1.f;
    GLboolean depth_mask = GL_TRUE;
    GLboolean depth_enabled = GL_FALSE;
    GLboolean blend_enabled = GL_FALSE;
    GLboolean cull_enabled = GL_FALSE;
    mutable bool restored = false;

    GlStateSnapshot() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_function);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_source_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_destination_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_source_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_destination_alpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation_rgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_equation_alpha);
        glGetIntegerv(GL_CULL_FACE_MODE, &cull_face_mode);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glActiveTexture(kBlockTextureUnit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &block_texture_array_binding);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler_bindings[0]);
        glActiveTexture(kMaterialLookupTextureUnit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &material_texture_2d_binding);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler_bindings[1]);
        glActiveTexture(static_cast<GLenum>(active_texture));
        glGetFloatv(GL_LINE_WIDTH, &line_width);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        depth_enabled = glIsEnabled(GL_DEPTH_TEST);
        blend_enabled = glIsEnabled(GL_BLEND);
        cull_enabled = glIsEnabled(GL_CULL_FACE);
    }

    ~GlStateSnapshot() { restore(); }

    void restore() const {
        if (restored) return;
        restored = true;
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vertex_array));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        glDepthFunc(static_cast<GLenum>(depth_function));
        glDepthMask(depth_mask);
        glBlendFuncSeparate(static_cast<GLenum>(blend_source_rgb),
                            static_cast<GLenum>(blend_destination_rgb),
                            static_cast<GLenum>(blend_source_alpha),
                            static_cast<GLenum>(blend_destination_alpha));
        glBlendEquationSeparate(static_cast<GLenum>(blend_equation_rgb),
                                 static_cast<GLenum>(blend_equation_alpha));
        glCullFace(static_cast<GLenum>(cull_face_mode));
        glActiveTexture(kBlockTextureUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY,
                      static_cast<GLuint>(block_texture_array_binding));
        glBindSampler(kBlockTextureUnitIndex,
                      static_cast<GLuint>(sampler_bindings[0]));
        glActiveTexture(kMaterialLookupTextureUnit);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(material_texture_2d_binding));
        glBindSampler(kMaterialLookupTextureUnitIndex,
                      static_cast<GLuint>(sampler_bindings[1]));
        glActiveTexture(static_cast<GLenum>(active_texture));
        glLineWidth(line_width);
        if (depth_enabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (blend_enabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (cull_enabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    }
};

void bindProjectionSamplers() {
    // RenderDragon may leave sampler objects on these units. A sampler object
    // overrides the filtering stored on the texture itself; LINEAR filtering
    // also makes the integer material lookup incomplete on GLES.
    glBindSampler(kBlockTextureUnitIndex, 0);
    glBindSampler(kMaterialLookupTextureUnitIndex, 0);
}

void clearGlErrors() {
    // Keep an earlier game-render error from being attributed to this upload.
    // The bound avoids looping forever after a lost context.
    for (size_t attempt = 0; attempt < 8U; ++attempt) {
        if (glGetError() == GL_NO_ERROR) break;
    }
}

bool glOperationSucceeded(const char* operation) {
    const GLenum first_error = glGetError();
    if (first_error == GL_NO_ERROR) return true;
    for (size_t attempt = 1; attempt < 8U; ++attempt) {
        if (glGetError() == GL_NO_ERROR) break;
    }
    LOGE("%s failed with GLES error 0x%x", operation,
         static_cast<unsigned int>(first_error));
    return false;
}

struct PixelUnpackState {
    GLint pixel_unpack_buffer = 0;
    GLint alignment = 4;
    GLint row_length = 0;
    GLint image_height = 0;
    GLint skip_pixels = 0;
    GLint skip_rows = 0;
    GLint skip_images = 0;
    bool captured = false;

    ~PixelUnpackState() {
        if (!captured) return;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pixel_unpack_buffer));
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, image_height);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES, skip_images);
    }

    void prepare() {
        if (!captured) {
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pixel_unpack_buffer);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
            glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image_height);
            glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
            glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
            glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
            captured = true;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    }
};

void abandonTextureObjectsForCurrentPack() {
    const std::shared_ptr<const ProjectionTexturePack> pack = g_gpu_texture_pack;
    releaseTextureResourcesForCurrentContext();
    g_gpu_texture_pack = pack;
}

void synchronizeTextureResources(const std::shared_ptr<ProjectionPlan>& plan) {
    bindProjectionSamplers();
    PixelUnpackState unpack_state;
    const std::shared_ptr<const ProjectionTexturePack> pack = plan
        ? std::atomic_load_explicit(&plan->texture_pack, std::memory_order_acquire)
        : std::shared_ptr<const ProjectionTexturePack>{};
    if (pack.get() != g_gpu_texture_pack.get()) {
        releaseTextureResourcesForCurrentContext();
        g_gpu_texture_pack = pack;
        if (!pack) return;

        GLint fragment_texture_units = 0;
        GLint vertex_texture_units = 0;
        GLint maximum_array_layers = 0;
        GLint maximum_texture_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &fragment_texture_units);
        glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertex_texture_units);
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maximum_array_layers);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
        if (fragment_texture_units <= kBlockTextureUnitIndex ||
            vertex_texture_units <= kMaterialLookupTextureUnitIndex ||
            maximum_array_layers <= 0 || maximum_texture_size <= 0) {
            LOGE("device cannot provide the GLES texture units required by projection textures");
            return;
        }

        g_allocated_texture_layers = std::min(pack->layer_count, maximum_array_layers);
        if (g_allocated_texture_layers <= 0) return;
        if (g_allocated_texture_layers < pack->layer_count) {
            LOGI("projection texture array limited by device: requested=%d available=%d",
                 pack->layer_count, g_allocated_texture_layers);
        }

        const size_t lookup_entries = pack->material_face_layers.size();
        g_material_lookup_width = static_cast<GLint>(std::min<size_t>(
            static_cast<size_t>(maximum_texture_size), 1024U));
        const size_t lookup_height =
            (lookup_entries + static_cast<size_t>(g_material_lookup_width) - 1U) /
            static_cast<size_t>(g_material_lookup_width);
        if (lookup_entries == 0 || lookup_height == 0 ||
            lookup_height > static_cast<size_t>(maximum_texture_size)) {
            LOGE("projection material lookup exceeds GLES texture-size limits");
            g_material_lookup_width = 1;
            g_allocated_texture_layers = 0;
            return;
        }

        glGenTextures(1, &g_block_texture_array);
        glGenTextures(1, &g_material_face_texture);
        if (!g_block_texture_array || !g_material_face_texture) {
            abandonTextureObjectsForCurrentPack();
            return;
        }

        glActiveTexture(kBlockTextureUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g_block_texture_array);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, pack->tile_size,
                       pack->tile_size, g_allocated_texture_layers);
        GLint array_storage_is_immutable = GL_FALSE;
        glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_IMMUTABLE_FORMAT,
                            &array_storage_is_immutable);
        if (array_storage_is_immutable != GL_TRUE) {
            LOGE("cannot allocate projection texture array");
            abandonTextureObjectsForCurrentPack();
            return;
        }

        glActiveTexture(kMaterialLookupTextureUnit);
        glBindTexture(GL_TEXTURE_2D, g_material_face_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16UI, g_material_lookup_width,
                       static_cast<GLsizei>(lookup_height));
        GLint lookup_storage_is_immutable = GL_FALSE;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT,
                            &lookup_storage_is_immutable);
        if (lookup_storage_is_immutable != GL_TRUE) {
            LOGE("cannot allocate projection material lookup texture");
            abandonTextureObjectsForCurrentPack();
            return;
        }

        // Upload the flat table without padding the CPU vector to a complete
        // final row. UNPACK_ALIGNMENT is restored by GlStateSnapshot.
        unpack_state.prepare();
        const size_t complete_rows = lookup_entries /
            static_cast<size_t>(g_material_lookup_width);
        const size_t final_row_entries = lookup_entries %
            static_cast<size_t>(g_material_lookup_width);
        clearGlErrors();
        if (complete_rows != 0) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_material_lookup_width,
                            static_cast<GLsizei>(complete_rows), GL_RED_INTEGER,
                            GL_UNSIGNED_SHORT, pack->material_face_layers.data());
        }
        if (final_row_entries != 0) {
            glTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, static_cast<GLint>(complete_rows),
                static_cast<GLsizei>(final_row_entries), 1, GL_RED_INTEGER,
                GL_UNSIGNED_SHORT,
                pack->material_face_layers.data() +
                    complete_rows * static_cast<size_t>(g_material_lookup_width));
        }
        if (!glOperationSucceeded("projection material lookup upload")) {
            abandonTextureObjectsForCurrentPack();
            return;
        }
        g_uploaded_texture_layers = 0;
    }

    if (!pack || !g_block_texture_array || !g_material_face_texture ||
        g_uploaded_texture_layers >= g_allocated_texture_layers) {
        return;
    }
    const size_t bytes_per_layer = static_cast<size_t>(pack->tile_size) *
        static_cast<size_t>(pack->tile_size) * 4U;
    const size_t maximum_layers_this_frame =
        kTextureUploadBudgetPerFrame / bytes_per_layer;
    if (maximum_layers_this_frame == 0) return;
    const GLint layer_count = static_cast<GLint>(std::min<size_t>(
        static_cast<size_t>(g_allocated_texture_layers - g_uploaded_texture_layers),
        maximum_layers_this_frame));
    glActiveTexture(kBlockTextureUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_block_texture_array);
    unpack_state.prepare();
    clearGlErrors();
    glTexSubImage3D(
        GL_TEXTURE_2D_ARRAY, 0, 0, 0, g_uploaded_texture_layers,
        pack->tile_size, pack->tile_size, layer_count, GL_RGBA,
        GL_UNSIGNED_BYTE,
        pack->rgba_layers.data() +
            static_cast<size_t>(g_uploaded_texture_layers) * bytes_per_layer);
    if (!glOperationSucceeded("projection texture array upload")) {
        abandonTextureObjectsForCurrentPack();
        return;
    }
    g_uploaded_texture_layers += layer_count;
}

bool readCamera(float* mvp, Vec3* camera_position) {
    if (!mvp || !camera_position) return false;
    const uintptr_t base_address = Main::getBaseAddress();
    if (!base_address) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::NoLocalPlayer);
        return false;
    }
    void* actor = GetLocalPlayerPointer();
    if (!actor || !IsMemoryReadable(actor, sizeof(void*))) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::NoLocalPlayer);
        return false;
    }
    using ObjectFunction = void* (*)(void*);
    const auto get_client = reinterpret_cast<ObjectFunction>(
        base_address + FunctionsAddress::Actor_getClientInstance);
    const auto get_camera = reinterpret_cast<ObjectFunction>(
        base_address + FunctionsAddress::ClientInstance_getCamera);
    if (!get_client || !get_camera) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::NoClientInstance);
        return false;
    }
    void* client = get_client(actor);
    if (!client) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::NoClientInstance);
        return false;
    }
    void* raw_camera = get_camera(client);
    if (!raw_camera || !IsMemoryReadable(raw_camera, sizeof(void*))) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::NoCamera);
        return false;
    }

    // Verified current Camera layout.  Do not cast this object to the old
    // std::deque-based Camera type: these copies validate every indirection
    // before reading a single matrix and are safe when a future update changes
    // the layout again.
    Matrix model;
    Matrix projection;
    Matrix camera_to_world;
    if (!readCameraMapMatrix(raw_camera, kCameraModelMapOffset, &model) ||
        !readCameraMapMatrix(raw_camera, kCameraProjectionMapOffset, &projection) ||
        !readCameraInlineMatrix(raw_camera, kCameraToWorldMatrixOffset, &camera_to_world)) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::CameraMatrixLayoutInvalid);
        return false;
    }
    // The inline matrix is the camera basis in world space.  The shader takes
    // world-relative vertices, therefore it requires the inverse basis.  The
    // camera basis is orthonormal, so inverse equals transpose.
    const Matrix view = transposeMatrix(camera_to_world);
    if (!finiteMatrix(view) || !finiteMatrix(model) || !finiteMatrix(projection)) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::CameraMatrixInvalid);
        return false;
    }

    if (!readLevelRenderCameraPosition(camera_position)) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::CameraPositionUnavailable);
        return false;
    }
    if (!std::isfinite(camera_position->x) || !std::isfinite(camera_position->y) ||
        !std::isfinite(camera_position->z)) {
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::CameraPositionInvalid);
        return false;
    }

    float temporary[16]{};
    multiplyMatrices(temporary, projection.values, view.values);
    multiplyMatrices(mvp, temporary, model.values);
    setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::Ready);
    return true;
}

float horizontalDistanceSquared(const ProjectionGroup& group, const Vec3& camera) {
    const float minimum_x = static_cast<float>(
        static_cast<int64_t>(group.origin_x) + group.bounds.min_x);
    const float maximum_x = static_cast<float>(
        static_cast<int64_t>(group.origin_x) + group.bounds.max_x + 1);
    const float minimum_z = static_cast<float>(
        static_cast<int64_t>(group.origin_z) + group.bounds.min_z);
    const float maximum_z = static_cast<float>(
        static_cast<int64_t>(group.origin_z) + group.bounds.max_z + 1);
    const float dx = camera.x < minimum_x ? minimum_x - camera.x
        : camera.x > maximum_x ? camera.x - maximum_x : 0.f;
    const float dz = camera.z < minimum_z ? minimum_z - camera.z
        : camera.z > maximum_z ? camera.z - maximum_z : 0.f;
    return dx * dx + dz * dz;
}

float groupDistanceSquared(const ProjectionGroup& group, const Vec3& camera) {
    const float center_x = static_cast<float>(group.origin_x) +
        (static_cast<float>(group.bounds.min_x) + group.bounds.max_x + 1.f) * 0.5f;
    const float center_y = static_cast<float>(group.origin_y) +
        (static_cast<float>(group.bounds.min_y) + group.bounds.max_y + 1.f) * 0.5f;
    const float center_z = static_cast<float>(group.origin_z) +
        (static_cast<float>(group.bounds.min_z) + group.bounds.max_z + 1.f) * 0.5f;
    const float dx = center_x - camera.x;
    const float dy = center_y - camera.y;
    const float dz = center_z - camera.z;
    return dx * dx + dy * dy + dz * dz;
}

struct FrustumPlane {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float w = 0.f;
};

using Frustum = std::array<FrustumPlane, 6>;

Frustum extractFrustum(const float* matrix) {
    const auto plane = [&](int row, float sign) {
        return FrustumPlane{
            matrix[3] + sign * matrix[row],
            matrix[7] + sign * matrix[4 + row],
            matrix[11] + sign * matrix[8 + row],
            matrix[15] + sign * matrix[12 + row],
        };
    };
    return {{
        plane(0, 1.f), plane(0, -1.f),
        plane(1, 1.f), plane(1, -1.f),
        plane(2, 1.f), plane(2, -1.f),
    }};
}

bool intersectsFrustum(const ProjectionGroup& group, const Vec3& camera,
                       const Frustum& frustum) {
    const float minimum_x = static_cast<float>(
        static_cast<int64_t>(group.origin_x) + group.bounds.min_x) - camera.x;
    const float minimum_y = static_cast<float>(
        static_cast<int64_t>(group.origin_y) + group.bounds.min_y) - camera.y;
    const float minimum_z = static_cast<float>(
        static_cast<int64_t>(group.origin_z) + group.bounds.min_z) - camera.z;
    const float maximum_x = static_cast<float>(
        static_cast<int64_t>(group.origin_x) + group.bounds.max_x + 1) - camera.x;
    const float maximum_y = static_cast<float>(
        static_cast<int64_t>(group.origin_y) + group.bounds.max_y + 1) - camera.y;
    const float maximum_z = static_cast<float>(
        static_cast<int64_t>(group.origin_z) + group.bounds.max_z + 1) - camera.z;
    for (const FrustumPlane& plane : frustum) {
        const float x = plane.x >= 0.f ? maximum_x : minimum_x;
        const float y = plane.y >= 0.f ? maximum_y : minimum_y;
        const float z = plane.z >= 0.f ? maximum_z : minimum_z;
        if (plane.x * x + plane.y * y + plane.z * z + plane.w < 0.f) return false;
    }
    return true;
}

bool drawBatchForLayerRange(const ProjectionGroup& group, int64_t minimum_y,
                            int64_t maximum_y, ProjectionDrawBatch* batch) {
    const int64_t group_minimum_y =
        static_cast<int64_t>(group.origin_y) + group.bounds.min_y;
    const int64_t group_maximum_y =
        static_cast<int64_t>(group.origin_y) + group.bounds.max_y;
    if (!batch || group.instance_count <= 0 || !group.bounds.isValid() ||
        maximum_y < group_minimum_y || minimum_y > group_maximum_y) {
        return false;
    }
    const int64_t first_layer64 = std::max<int64_t>(
        0, minimum_y - static_cast<int64_t>(group.origin_y));
    const int64_t last_layer64 = std::min<int64_t>(
        kGroupSpan - 1, maximum_y - static_cast<int64_t>(group.origin_y));
    if (first_layer64 > last_layer64) return false;
    const size_t first_layer = static_cast<size_t>(first_layer64);
    const size_t one_past_last_layer = static_cast<size_t>(last_layer64 + 1);
    const uint32_t first = group.layer_offsets[first_layer];
    const uint32_t end = group.layer_offsets[one_past_last_layer];
    if (end <= first || end > static_cast<uint32_t>(group.instance_count)) return false;
    batch->first_instance = first;
    batch->instance_count = end - first;
    return true;
}

size_t drawBatchesForLayerRange(const ProjectionGroup& group, int64_t minimum_y,
                                int64_t maximum_y,
                                std::array<ProjectionDrawBatch, 2>* batches) {
    if (!batches) return 0;
    if (!group.flags.interior_only) {
        return drawBatchForLayerRange(group, minimum_y, maximum_y, &(*batches)[0]) ? 1U : 0U;
    }

    // Interior cubes have no normally visible faces. They are required only at
    // finite horizontal cut boundaries, where the fragment shader restores the
    // down/up face. Keeping them out of All mode is the main large-build fast path.
    size_t count = 0;
    const auto append_boundary = [&](int64_t layer_y) {
        const int64_t group_minimum_y =
            static_cast<int64_t>(group.origin_y) + group.bounds.min_y;
        const int64_t group_maximum_y =
            static_cast<int64_t>(group.origin_y) + group.bounds.max_y;
        if (layer_y == INT32_MIN || layer_y == INT32_MAX ||
            layer_y < group_minimum_y || layer_y > group_maximum_y) {
            return;
        }
        if (count != 0 && minimum_y == maximum_y) return;
        ProjectionDrawBatch batch;
        if (drawBatchForLayerRange(group, layer_y, layer_y, &batch)) {
            (*batches)[count++] = batch;
        }
    };
    append_boundary(minimum_y);
    if (maximum_y != minimum_y) append_boundary(maximum_y);
    return count;
}

struct ProjectionWorldMatchInterestCandidate {
    ProjectionWorldMatchInterestRegion region;
};

bool projectionWorldMatchInterestRegionForBatch(
        const ProjectionGroup& group, const ProjectionDrawBatch& batch,
        ProjectionWorldMatchInterestCandidate* candidate) noexcept {
    if (!candidate || !group.bounds.isValid() || group.instance_count <= 0 ||
        batch.instance_count == 0 ||
        group.bounds.min_x >= kGroupSpan || group.bounds.max_x >= kGroupSpan ||
        group.bounds.min_y >= kGroupSpan || group.bounds.max_y >= kGroupSpan ||
        group.bounds.min_z >= kGroupSpan || group.bounds.max_z >= kGroupSpan) {
        return false;
    }
    const uint64_t batch_first = batch.first_instance;
    const uint64_t batch_end = batch_first + batch.instance_count;
    if (batch_end <= batch_first ||
        batch_end > static_cast<uint64_t>(group.instance_count)) {
        return false;
    }

    uint8_t minimum_local_y = static_cast<uint8_t>(kGroupSpan);
    uint8_t maximum_local_y = 0;
    bool has_selected_layer = false;
    for (size_t local_y = 0; local_y < kGroupSpan; ++local_y) {
        const uint64_t layer_first = group.layer_offsets[local_y];
        const uint64_t layer_end = group.layer_offsets[local_y + 1U];
        if (layer_end <= layer_first || batch_first >= layer_end ||
            batch_end <= layer_first) {
            continue;
        }
        const uint8_t y = static_cast<uint8_t>(local_y);
        minimum_local_y = std::min(minimum_local_y, y);
        maximum_local_y = std::max(maximum_local_y, y);
        has_selected_layer = true;
    }
    if (!has_selected_layer) return false;

    const auto add_origin = [](int32_t origin, uint8_t local, int32_t* output) {
        if (!output) return false;
        const int64_t coordinate = static_cast<int64_t>(origin) + local;
        if (coordinate < INT32_MIN || coordinate > INT32_MAX) return false;
        *output = static_cast<int32_t>(coordinate);
        return true;
    };
    ProjectionWorldMatchInterestRegion region;
    if (!add_origin(group.origin_x, group.bounds.min_x, &region.min_x) ||
        !add_origin(group.origin_x, group.bounds.max_x, &region.max_x) ||
        !add_origin(group.origin_y, minimum_local_y, &region.min_y) ||
        !add_origin(group.origin_y, maximum_local_y, &region.max_y) ||
        !add_origin(group.origin_z, group.bounds.min_z, &region.min_z) ||
        !add_origin(group.origin_z, group.bounds.max_z, &region.max_z) ||
        !region.isValid()) {
        return false;
    }
    candidate->region = region;
    return true;
}

bool projectionWorldMatchInterestGeometryEqual(
        const ProjectionWorldMatchInterestRegion& left,
        const ProjectionWorldMatchInterestRegion& right) noexcept {
    return left.min_x == right.min_x && left.min_y == right.min_y &&
        left.min_z == right.min_z && left.max_x == right.max_x &&
        left.max_y == right.max_y && left.max_z == right.max_z;
}

uint64_t projectionWorldMatchInterestFingerprint(
        const std::vector<ProjectionWorldMatchInterestRegion>& regions) noexcept {
    uint64_t hash = UINT64_C(0x14650fb0739d0383);
    const auto add = [&hash](uint64_t value) {
        hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6U) +
            (hash >> 2U);
        hash *= UINT64_C(0x100000001b3);
    };
    add(regions.size());
    for (const ProjectionWorldMatchInterestRegion& region : regions) {
        add(static_cast<uint32_t>(region.min_x));
        add(static_cast<uint32_t>(region.min_y));
        add(static_cast<uint32_t>(region.min_z));
        add(static_cast<uint32_t>(region.max_x));
        add(static_cast<uint32_t>(region.max_y));
        add(static_cast<uint32_t>(region.max_z));
        add(region.priority);
    }
    return hash == 0U ? UINT64_C(1) : hash;
}

struct ProjectionWorldMatchInterestPublicationCache {
    uint64_t plan_identity = 0;
    uint64_t fingerprint = 0;
    uint64_t revision = 0;
    size_t region_count = 0;
    bool active = false;
};

ProjectionWorldMatchInterestPublicationCache& projectionWorldMatchInterestCache() {
    static thread_local ProjectionWorldMatchInterestPublicationCache cache;
    return cache;
}

void clearProjectionWorldMatchInterest() noexcept {
    ProjectionWorldMatchInterestPublicationCache& cache =
        projectionWorldMatchInterestCache();
    if (!cache.active) return;
    ClearProjectionWorldMatchInterest();
    cache.plan_identity = 0;
    cache.fingerprint = 0;
    cache.region_count = 0;
    cache.active = false;
}

void publishProjectionWorldMatchInterest(
        uint64_t plan_identity,
        const std::vector<ProjectionWorldMatchInterestCandidate>& candidates) noexcept {
    if (plan_identity == 0U || candidates.empty()) {
        clearProjectionWorldMatchInterest();
        return;
    }
    ProjectionWorldMatchInterestPublicationCache& cache =
        projectionWorldMatchInterestCache();
    try {
        std::vector<ProjectionWorldMatchInterestRegion> regions;
        regions.reserve(std::min(candidates.size(), kMaximumWorldMatchInterestRegions));
        for (const ProjectionWorldMatchInterestCandidate& candidate : candidates) {
            if (!candidate.region.isValid()) continue;
            bool duplicate = false;
            for (const ProjectionWorldMatchInterestRegion& existing : regions) {
                if (projectionWorldMatchInterestGeometryEqual(existing, candidate.region)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            ProjectionWorldMatchInterestRegion region = candidate.region;
            region.priority = static_cast<uint32_t>(regions.size());
            regions.push_back(region);
            if (regions.size() >= kMaximumWorldMatchInterestRegions) break;
        }
        if (regions.empty()) {
            clearProjectionWorldMatchInterest();
            return;
        }
        const uint64_t fingerprint = projectionWorldMatchInterestFingerprint(regions);
        if (cache.active && cache.plan_identity == plan_identity &&
            cache.region_count == regions.size() && cache.fingerprint == fingerprint) {
            return;
        }
        std::shared_ptr<ProjectionWorldMatchInterestSnapshot> snapshot =
            std::make_shared<ProjectionWorldMatchInterestSnapshot>();
        snapshot->plan_identity = plan_identity;
        ++cache.revision;
        if (cache.revision == 0U) cache.revision = 1U;
        snapshot->revision = cache.revision;
        snapshot->regions = std::move(regions);
        PublishProjectionWorldMatchInterest(snapshot);
        cache.plan_identity = plan_identity;
        cache.fingerprint = fingerprint;
        cache.region_count = snapshot->regions.size();
        cache.active = true;
    } catch (...) {
        // An interest handoff is optional presentation work. Preserve the
        // previous immutable mailbox instead of letting allocation pressure
        // interrupt the game render hook or reset matcher progress.
    }
}

int32_t reachabilityPreviewGroupCoordinate(int32_t value) noexcept {
    const int64_t wide = value;
    const int64_t span = kProjectionWorldMatchGroupSpan;
    return static_cast<int32_t>(wide >= 0 ? wide / span :
        -(((-wide) + span - 1) / span));
}

uint8_t reachabilityPreviewLocalCoordinate(
        int32_t value, int32_t group_coordinate) noexcept {
    const int64_t local = static_cast<int64_t>(value) -
        static_cast<int64_t>(group_coordinate) * kProjectionWorldMatchGroupSpan;
    return local >= 0 && local < kProjectionWorldMatchGroupSpan
        ? static_cast<uint8_t>(local) : 0U;
}

size_t reachabilityPreviewCellIndex(uint8_t local_x, uint8_t local_y,
                                    uint8_t local_z) noexcept {
    return static_cast<size_t>(local_x) |
        (static_cast<size_t>(local_z) << 4U) |
        (static_cast<size_t>(local_y) << 8U);
}

uint64_t reachabilityPreviewFingerprint(
        const ProjectionReachabilityPreviewGroupState& group) noexcept {
    uint64_t hash = UINT64_C(0x9e3779b97f4a7c15);
    bool any = false;
    for (size_t index = 0; index < group.cells.size(); ++index) {
        const uint64_t word = group.cells[index];
        if (word != 0U) any = true;
        hash ^= word + UINT64_C(0x517cc1b727220a95) + (hash << 6U) + (hash >> 2U);
        hash *= UINT64_C(0x100000001b3);
    }
    return any && hash == 0U ? UINT64_C(1) : (any ? hash : 0U);
}

const ProjectionReachabilityPreviewGroupState* findReachabilityPreviewGroup(
        const ProjectionReachabilityPreviewSnapshot* snapshot,
        int32_t group_origin_x, int32_t group_origin_y,
        int32_t group_origin_z) noexcept {
    if (!snapshot) return nullptr;
    const ProjectionWorldMatchGroupKey key{
        reachabilityPreviewGroupCoordinate(group_origin_x),
        reachabilityPreviewGroupCoordinate(group_origin_y),
        reachabilityPreviewGroupCoordinate(group_origin_z)};
    const auto entry = snapshot->groups.find(key);
    return entry == snapshot->groups.end() || !entry->second ? nullptr : entry->second.get();
}

uint64_t reachabilityPreviewGroupFingerprint(
        const ProjectionReachabilityPreviewSnapshot* snapshot,
        int32_t group_origin_x, int32_t group_origin_y,
        int32_t group_origin_z) noexcept {
    const ProjectionReachabilityPreviewGroupState* group =
        findReachabilityPreviewGroup(snapshot, group_origin_x, group_origin_y, group_origin_z);
    return group ? group->fingerprint : 0U;
}

bool reachabilityPreviewGroupContains(
        const ProjectionReachabilityPreviewGroupState* group,
        uint8_t local_x, uint8_t local_y, uint8_t local_z) noexcept {
    if (!group || local_x >= kProjectionWorldMatchGroupSpan ||
        local_y >= kProjectionWorldMatchGroupSpan ||
        local_z >= kProjectionWorldMatchGroupSpan) {
        return false;
    }
    const size_t cell_index = reachabilityPreviewCellIndex(local_x, local_y, local_z);
    const size_t word_index = cell_index / 64U;
    const uint64_t bit = UINT64_C(1) << (cell_index % 64U);
    return word_index < group->cells.size() && (group->cells[word_index] & bit) != 0U;
}

uint64_t combinedPresentationFingerprint(uint64_t world_match,
                                         uint64_t reachability) noexcept {
    if (world_match == 0U && reachability == 0U) return 0U;
    uint64_t hash = world_match ^ UINT64_C(0x9e3779b97f4a7c15);
    hash ^= reachability + UINT64_C(0x517cc1b727220a95) + (hash << 6U) + (hash >> 2U);
    hash *= UINT64_C(0x100000001b3);
    return hash == 0U ? UINT64_C(1) : hash;
}

bool sameReachabilityPreview(
        const ProjectionReachabilityPreviewSnapshot& left,
        const ProjectionReachabilityPreviewSnapshot& right) noexcept {
    if (left.plan_identity != right.plan_identity ||
        left.groups.size() != right.groups.size()) {
        return false;
    }
    for (const auto& entry : left.groups) {
        const auto other = right.groups.find(entry.first);
        if (other == right.groups.end() || !entry.second || !other->second ||
            entry.second->cells != other->second->cells) {
            return false;
        }
    }
    return true;
}

struct ProjectionGroupUploadStaging {
    std::array<ProjectionDrawBatch, 2> batches{};
    // Always upload from a transient copy.  The copy is the only place where
    // world-match bits are applied, keeping both in-memory source instances
    // and disk spools reusable after the player changes a block.
    std::vector<ProjectionInstance> fill_instances;
    std::vector<ProjectionInstance> outline_instances;
    size_t fill_byte_count = 0;
    size_t total_byte_count = 0;
    bool outline_prepared = false;
};

bool projectionInstanceWorldMatchCell(const ProjectionInstance& instance,
                                      uint8_t* local_x, uint8_t* local_y,
                                      uint8_t* local_z) {
    if (!local_x || !local_y || !local_z) return false;
    const auto decode = [](uint16_t encoded) {
        return static_cast<uint32_t>(encoded & kPositionValueMask) /
            kPositionFixedScale;
    };
    const uint32_t x = decode(instance.bounds[0]);
    const uint32_t y = decode(instance.bounds[1]);
    const uint32_t z = decode(instance.bounds[2]);
    if (x >= static_cast<uint32_t>(kProjectionWorldMatchGroupSpan) ||
        y >= static_cast<uint32_t>(kProjectionWorldMatchGroupSpan) ||
        z >= static_cast<uint32_t>(kProjectionWorldMatchGroupSpan)) {
        return false;
    }
    *local_x = static_cast<uint8_t>(x);
    *local_y = static_cast<uint8_t>(y);
    *local_z = static_cast<uint8_t>(z);
    return true;
}

uint32_t worldMatchStateBits(
        const ProjectionInstance& instance,
        const ProjectionWorldMatchGroupState* world_match_group) {
    if (!world_match_group) return 0;
    uint8_t local_x = 0;
    uint8_t local_y = 0;
    uint8_t local_z = 0;
    if (!projectionInstanceWorldMatchCell(instance, &local_x, &local_y, &local_z)) {
        return 0;
    }
    switch (GetProjectionWorldMatchGroupCellState(
            world_match_group, local_x, local_y, local_z)) {
        case ProjectionWorldMatchState::WrongBlock:
            return 0x01U << kWorldMatchStateShift;
        case ProjectionWorldMatchState::WrongState:
            return 0x02U << kWorldMatchStateShift;
        case ProjectionWorldMatchState::Exact:
            return kWorldMatchStateExact;
        case ProjectionWorldMatchState::Unknown:
            return 0;
    }
    return 0;
}

void applyWorldMatchState(
        ProjectionInstance* instance,
        const ProjectionWorldMatchGroupState* world_match_group,
        const ProjectionReachabilityPreviewGroupState* reachability_group) {
    if (!instance) return;
    const uint32_t state_bits = worldMatchStateBits(*instance, world_match_group);
    instance->packed_material = (instance->packed_material & ~kWorldMatchStateMask) |
        state_bits;
    instance->packed_material &= ~kReachabilityPreviewBit;
    if (state_bits == kWorldMatchStateExact) {
        // Keep the source/GPU instance count and layer offsets unchanged, but
        // turn this transient upload copy into an actual rendering hole. Face
        // masking handles ordinary fills; collapsed position values also make
        // finite layer-cut faces degenerate. Preserve the high outline metadata
        // bits so the packed instance ABI remains intact.
        instance->packed_material &= ~static_cast<uint32_t>(kAllFaces);
        for (size_t axis = 0; axis < 3U; ++axis) {
            const uint16_t minimum = static_cast<uint16_t>(
                instance->bounds[axis] & kPositionValueMask);
            instance->bounds[axis + 3U] = static_cast<uint16_t>(
                (instance->bounds[axis + 3U] & ~kPositionValueMask) | minimum);
        }
        return;
    }
    // Reaching a target is meaningful only while it remains an ordinary,
    // unresolved projected cell. Red/yellow diagnostics always take priority.
    if (state_bits == 0U) {
        uint8_t local_x = 0;
        uint8_t local_y = 0;
        uint8_t local_z = 0;
        if (projectionInstanceWorldMatchCell(*instance, &local_x, &local_y, &local_z) &&
            reachabilityPreviewGroupContains(
                reachability_group, local_x, local_y, local_z)) {
            instance->packed_material |= kReachabilityPreviewBit;
        }
    }
}

bool prepareGroupUpload(const ProjectionGroup& group,
                        const std::array<ProjectionDrawBatch, 2>& source_batches,
                        size_t batch_count, bool prepare_outline,
                        const ProjectionWorldMatchGroupState* world_match_group,
                        const ProjectionReachabilityPreviewGroupState* reachability_group,
                        ProjectionGroupUploadStaging* staging) {
    if (!staging) return false;
    staging->batches = {};
    staging->fill_instances.clear();
    staging->outline_instances.clear();
    staging->fill_byte_count = batchInstanceBytes(source_batches, batch_count);
    staging->total_byte_count = 0;
    staging->outline_prepared = prepare_outline && group.flags.has_outline != 0;
    if (staging->fill_byte_count == 0 ||
        !cpuBatchesForRequest(
            group, source_batches, batch_count, &staging->batches)) return false;

    const size_t fill_instance_count = staging->fill_byte_count /
        sizeof(ProjectionInstance);
    try {
        staging->fill_instances.resize(fill_instance_count);
    } catch (...) {
        staging->fill_instances.clear();
        return false;
    }
    size_t destination_first = 0;
    for (size_t batch_index = 0; batch_index < batch_count; ++batch_index) {
        const ProjectionDrawBatch& batch = staging->batches[batch_index];
        if (destination_first > fill_instance_count ||
            batch.first_instance > group.instances->size() ||
            batch.instance_count > group.instances->size() - batch.first_instance ||
            batch.instance_count > fill_instance_count - destination_first) {
            staging->fill_instances.clear();
            return false;
        }
        for (uint32_t offset = 0; offset < batch.instance_count; ++offset) {
            ProjectionInstance instance = (*group.instances)[batch.first_instance + offset];
            applyWorldMatchState(&instance, world_match_group, reachability_group);
            staging->fill_instances[destination_first + offset] = instance;
        }
        destination_first += batch.instance_count;
    }
    if (destination_first != fill_instance_count) {
        staging->fill_instances.clear();
        return false;
    }

    if (staging->outline_prepared) {
        size_t outline_first = 0;
        for (size_t batch_index = 0; batch_index < batch_count; ++batch_index) {
            const ProjectionDrawBatch& batch = staging->batches[batch_index];
            for (uint32_t offset = 0; offset < batch.instance_count; ++offset) {
                const ProjectionInstance& instance =
                    staging->fill_instances[outline_first + offset];
                // Matching cells are hidden entirely. Do not retain them in
                // the compact line tail: an outline must not survive after the
                // fill vertex path has been clipped.
                if ((instance.packed_material & kWorldMatchStateMask) !=
                        kWorldMatchStateExact &&
                    instanceOutlineMask(instance) != 0) {
                    staging->outline_instances.push_back(instance);
                }
            }
            outline_first += batch.instance_count;
        }
    }
    const size_t outline_bytes = staging->outline_instances.size() *
        sizeof(ProjectionInstance);
    if (outline_bytes > static_cast<size_t>(INT32_MAX) ||
        staging->fill_byte_count >
            static_cast<size_t>(INT32_MAX) - outline_bytes) return false;
    staging->total_byte_count = staging->fill_byte_count + outline_bytes;
    return true;
}

bool gpuBatchesForRequest(const ProjectionGroup& group,
                          const std::array<ProjectionDrawBatch, 2>& source_batches,
                          size_t batch_count, bool require_outline,
                          std::array<ProjectionDrawBatch, 2>* gpu_batches) {
    if (!gpu_batches || !group.instance_buffer || batch_count == 0 || batch_count > 2) {
        return false;
    }
    if (require_outline && group.flags.has_outline != 0 &&
        group.flags.gpu_outline_prepared == 0) return false;
    // A compact outline tail is built for the exact uploaded layer selection.
    // Without per-layer tail metadata it cannot safely serve a narrower range.
    // Fill-only requests may reuse any resident single batch that contains them.
    if ((!require_outline || group.flags.has_outline == 0) &&
        group.gpu_batch_count == 1) {
        const uint64_t resident_first = group.gpu_source_first[0];
        const uint64_t resident_end = resident_first + group.gpu_source_count[0];
        for (size_t index = 0; index < batch_count; ++index) {
            const uint64_t requested_first = source_batches[index].first_instance;
            const uint64_t requested_end = requested_first +
                source_batches[index].instance_count;
            if (requested_first < resident_first || requested_end > resident_end) return false;
            (*gpu_batches)[index] = source_batches[index];
            (*gpu_batches)[index].first_instance = static_cast<uint32_t>(
                requested_first - resident_first);
        }
        return true;
    }
    if (group.gpu_batch_count != batch_count) return false;
    uint32_t destination_first = 0;
    for (size_t index = 0; index < batch_count; ++index) {
        if (group.gpu_source_first[index] != source_batches[index].first_instance ||
            group.gpu_source_count[index] != source_batches[index].instance_count) {
            return false;
        }
        (*gpu_batches)[index] = source_batches[index];
        (*gpu_batches)[index].first_instance = destination_first;
        destination_first += source_batches[index].instance_count;
    }
    return true;
}

bool uploadGroupBatches(ProjectionGroup* group,
                        const std::array<ProjectionDrawBatch, 2>& source_batches,
                        size_t batch_count,
                        const ProjectionGroupUploadStaging& staging,
                         uint64_t presentation_fingerprint,
                        std::array<ProjectionDrawBatch, 2>* gpu_batches) {
    if (!group || !gpu_batches || !group->instances || group->instances->empty() ||
        batch_count == 0 ||
        batch_count > 2 || group->instance_buffer) return false;
    const size_t expected_fill_bytes = batchInstanceBytes(source_batches, batch_count);
    const size_t outline_bytes = staging.outline_instances.size() *
        sizeof(ProjectionInstance);
    if (expected_fill_bytes == 0 || staging.fill_byte_count != expected_fill_bytes ||
        staging.fill_instances.size() !=
            staging.fill_byte_count / sizeof(ProjectionInstance) ||
        (!staging.outline_prepared && !staging.outline_instances.empty()) ||
        (staging.outline_prepared && group->flags.has_outline == 0) ||
        outline_bytes > static_cast<size_t>(INT32_MAX) ||
        staging.fill_byte_count > static_cast<size_t>(INT32_MAX) - outline_bytes ||
        staging.total_byte_count != staging.fill_byte_count + outline_bytes) return false;
    for (size_t index = 0; index < batch_count; ++index) {
        if (staging.batches[index].instance_count !=
            source_batches[index].instance_count) return false;
    }

    clearGlErrors();
    group->flags.gpu_outline_prepared = false;
    glGenBuffers(1, &group->instance_buffer);
    if (!group->instance_buffer) {
        glOperationSucceeded("projection instance buffer creation");
        return false;
    }
    glBindBuffer(GL_ARRAY_BUFFER, group->instance_buffer);
    if (batch_count == 1 && staging.outline_instances.empty()) {
        glBufferData(
            GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(staging.fill_byte_count),
            staging.fill_instances.data(), GL_STATIC_DRAW);
    } else {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(staging.total_byte_count), nullptr,
                     GL_STATIC_DRAW);
        size_t destination_bytes = 0;
        size_t destination_instances = 0;
        for (size_t index = 0; index < batch_count; ++index) {
            const size_t batch_bytes = static_cast<size_t>(
                source_batches[index].instance_count) * sizeof(ProjectionInstance);
            glBufferSubData(
                GL_ARRAY_BUFFER, static_cast<GLintptr>(destination_bytes),
                static_cast<GLsizeiptr>(batch_bytes),
                staging.fill_instances.data() + destination_instances);
            destination_bytes += batch_bytes;
            destination_instances += source_batches[index].instance_count;
        }
        if (!staging.outline_instances.empty()) {
            glBufferSubData(
                GL_ARRAY_BUFFER, static_cast<GLintptr>(staging.fill_byte_count),
                static_cast<GLsizeiptr>(outline_bytes),
                staging.outline_instances.data());
        }
    }
    if (!glOperationSucceeded("projection instance buffer upload")) {
        glDeleteBuffers(1, &group->instance_buffer);
        group->instance_buffer = 0;
        group->gpu_byte_count = 0;
        group->gpu_batch_count = 0;
        group->flags.gpu_outline_prepared = false;
        group->gpu_presentation_fingerprint = 0;
        group->gpu_source_first.fill(0);
        group->gpu_source_count.fill(0);
        return false;
    }
    group->gpu_byte_count = static_cast<uint32_t>(staging.total_byte_count);
    group->gpu_batch_count = static_cast<uint8_t>(batch_count);
    group->flags.gpu_outline_prepared = staging.outline_prepared;
    group->gpu_presentation_fingerprint = presentation_fingerprint;
    for (size_t index = 0; index < batch_count; ++index) {
        group->gpu_source_first[index] = static_cast<uint16_t>(
            source_batches[index].first_instance);
        group->gpu_source_count[index] = static_cast<uint16_t>(
            source_batches[index].instance_count);
    }
    if (gpuBatchesForRequest(*group, source_batches, batch_count,
                             staging.outline_prepared, gpu_batches)) return true;
    glDeleteBuffers(1, &group->instance_buffer);
    group->instance_buffer = 0;
    group->gpu_byte_count = 0;
    group->gpu_batch_count = 0;
    group->flags.gpu_outline_prepared = false;
    group->gpu_presentation_fingerprint = 0;
    group->gpu_source_first.fill(0);
    group->gpu_source_count.fill(0);
    return false;
}

bool compactOutlineBatchForGroup(const ProjectionGroup& group,
                                 uint32_t group_index, float distance_squared,
                                 ProjectionDrawBatch* batch) {
    if (!batch || group.flags.has_outline == 0 ||
        group.flags.gpu_outline_prepared == 0 || !group.instance_buffer ||
        group.gpu_batch_count == 0 || group.gpu_batch_count > 2 ||
        group.gpu_byte_count % sizeof(ProjectionInstance) != 0) return false;
    uint32_t fill_instances = 0;
    for (uint8_t index = 0; index < group.gpu_batch_count; ++index) {
        fill_instances += group.gpu_source_count[index];
    }
    const uint32_t total_instances = group.gpu_byte_count /
        static_cast<uint32_t>(sizeof(ProjectionInstance));
    if (total_instances <= fill_instances) return false;
    const uint32_t outline_instances = total_instances - fill_instances;
    if (outline_instances > static_cast<uint32_t>(INT32_MAX)) return false;
    batch->group_index = group_index;
    batch->first_instance = fill_instances;
    batch->instance_count = outline_instances;
    batch->distance_squared = distance_squared;
    return true;
}

void bindInstanceBuffer(const ProjectionGroup& group, uint32_t first_instance) {
    glBindBuffer(GL_ARRAY_BUFFER, group.instance_buffer);
    const size_t base_offset = static_cast<size_t>(first_instance) *
        sizeof(ProjectionInstance);
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_SHORT, sizeof(ProjectionInstance),
                            reinterpret_cast<const void*>(
                                base_offset + offsetof(ProjectionInstance, bounds)));
    glVertexAttribIPointer(2, 2, GL_UNSIGNED_SHORT, sizeof(ProjectionInstance),
                            reinterpret_cast<const void*>(
                                base_offset + offsetof(ProjectionInstance, bounds) +
                                    4U * sizeof(uint16_t)));
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ProjectionInstance),
                           reinterpret_cast<const void*>(
                               base_offset + offsetof(ProjectionInstance, color)));
    glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(ProjectionInstance),
                            reinterpret_cast<const void*>(
                                base_offset + offsetof(ProjectionInstance, packed_material)));
}

void drawGroup(const ProjectionGroup& group, const Vec3& camera, GLenum primitive,
               GLsizei index_count, size_t index_offset,
               uint32_t first_instance, uint32_t instance_count) {
    bindInstanceBuffer(group, first_instance);
    glUniform3f(g_offset_location,
                static_cast<float>(group.origin_x) - camera.x,
                static_cast<float>(group.origin_y) - camera.y,
                static_cast<float>(group.origin_z) - camera.z);
    glUniform1i(g_group_origin_y_location, group.origin_y);
    glDrawElementsInstanced(primitive, index_count, GL_UNSIGNED_SHORT,
                            reinterpret_cast<const void*>(index_offset),
                            static_cast<GLsizei>(instance_count));
}

void* LevelRenderHook(void* first, void* second, void* third) noexcept {
    g_level_render_owner.store(first, std::memory_order_release);
    const auto original = reinterpret_cast<LevelRenderFunction>(
        __atomic_load_n(&g_level_render_original, __ATOMIC_ACQUIRE));
    void* result = original ? original(first, second, third) : nullptr;
    Vec3 camera_position;
    if (readLevelRenderCameraPosition(&camera_position)) {
        publishLevelRenderCameraPosition(camera_position);
    }
    try {
        BuildProjectionRenderer::instance().render();
    } catch (...) {
        LOGE("projection render failed with an unexpected C++ exception");
    }
    return result;
}

}  // namespace

bool resolveProjectionWorldLayerRange(int32_t relative_origin_y,
                                      const ProjectionLayerFilter& filter,
                                      int64_t* minimum_y,
                                      int64_t* maximum_y) noexcept {
    if (!minimum_y || !maximum_y) return false;
    if (filter.mode == ProjectionLayerMode::All) {
        *minimum_y = INT32_MIN;
        *maximum_y = INT32_MAX;
        return true;
    }

    int64_t lower = filter.minimum_y;
    int64_t upper = filter.maximum_y;
    switch (filter.mode) {
        case ProjectionLayerMode::AtOrAbove:
            upper = INT32_MAX;
            break;
        case ProjectionLayerMode::AtOrBelow:
            lower = INT32_MIN;
            break;
        case ProjectionLayerMode::Single:
            upper = lower;
            break;
        case ProjectionLayerMode::Range:
            if (lower > upper) std::swap(lower, upper);
            break;
        case ProjectionLayerMode::All:
            break;
    }
    if (filter.space == ProjectionLayerSpace::Relative) {
        const int64_t origin = relative_origin_y;
        if (lower != INT32_MIN) lower += origin;
        if (upper != INT32_MAX) upper += origin;
    }
    *minimum_y = lower;
    *maximum_y = upper;
    return lower <= upper;
}

bool makeProjectionDisplayScope(int32_t relative_origin_y, int32_t range_chunks,
                                const ProjectionLayerFilter& filter,
                                ProjectionDisplayScope* scope) noexcept {
    if (!scope) return false;
    ProjectionDisplayScope result;
    // Keep this normalization aligned with BuildProjectionRenderer::setRangeChunks.
    result.range_chunks = std::max(1, std::min(64, range_chunks));
    if (!resolveProjectionWorldLayerRange(relative_origin_y, filter,
                                          &result.minimum_world_y,
                                          &result.maximum_world_y)) {
        return false;
    }
    *scope = result;
    return true;
}

bool projectionDisplayScopeContainsBlock(const ProjectionDisplayScope& scope,
                                         float view_x, float view_z,
                                         int32_t block_x, int32_t block_y,
                                         int32_t block_z) noexcept {
    if (!std::isfinite(view_x) || !std::isfinite(view_z) ||
        block_y < scope.minimum_world_y || block_y > scope.maximum_world_y) {
        return false;
    }
    const double range = static_cast<double>(scope.range_chunks) * 16.0;
    const double minimum_x = static_cast<double>(block_x);
    const double maximum_x = minimum_x + 1.0;
    const double minimum_z = static_cast<double>(block_z);
    const double maximum_z = minimum_z + 1.0;
    const double x = static_cast<double>(view_x);
    const double z = static_cast<double>(view_z);
    const double dx = x < minimum_x ? minimum_x - x : (x > maximum_x ? x - maximum_x : 0.0);
    const double dz = z < minimum_z ? minimum_z - z : (z > maximum_z ? z - maximum_z : 0.0);
    return dx * dx + dz * dz <= range * range;
}

BuildProjectionRenderer& BuildProjectionRenderer::instance() {
    static BuildProjectionRenderer renderer;
    return renderer;
}

const char* BuildProjectionRenderer::renderDiagnostic() const noexcept {
    return projectionRenderDiagnosticText(
        g_projectionRenderDiagnostic.load(std::memory_order_acquire));
}

bool GetLatestBuildRenderCameraPosition(float* x, float* y, float* z) noexcept {
    if (!x || !y || !z ||
        !g_render_camera_position_valid.load(std::memory_order_acquire)) {
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t begin = g_render_camera_position_sequence.load(std::memory_order_acquire);
        if ((begin & 1U) != 0U) continue;
        const float current_x = bitsFloat(g_render_camera_position_x.load(std::memory_order_relaxed));
        const float current_y = bitsFloat(g_render_camera_position_y.load(std::memory_order_relaxed));
        const float current_z = bitsFloat(g_render_camera_position_z.load(std::memory_order_relaxed));
        const uint32_t end = g_render_camera_position_sequence.load(std::memory_order_acquire);
        if (begin != end || (end & 1U) != 0U) continue;
        if (!std::isfinite(current_x) || !std::isfinite(current_y) ||
            !std::isfinite(current_z)) {
            return false;
        }
        *x = current_x;
        *y = current_y;
        *z = current_z;
        return true;
    }
    return false;
}

bool BuildProjectionRenderer::beginStreamingPlan(ProjectionStreamingOptions options,
                                                  std::string* error) {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    g_stream_build.reset();
    if (options.cache_directory.empty()) {
        if (error) *error = "projection render cache directory is empty";
        return false;
    }
    if (options.maximum_partition_blocks == 0) {
        if (error) *error = "projection partition block budget must be positive";
        return false;
    }
    if (options.maximum_cpu_cache_bytes < kUploadBudgetPerFrame) {
        if (error) *error = "projection CPU cache budget must be at least 2 MiB";
        return false;
    }

    auto build = std::make_unique<ProjectionStreamBuild>();
    build->options = std::move(options);
    build->plan = std::make_shared<ProjectionPlan>();
    build->plan->identity =
        g_plan_sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    build->plan->source_block_count = build->options.source_block_count;
    build->plan->relative_origin_y = build->options.relative_origin_y;
    build->plan->maximum_cpu_cache_bytes = build->options.maximum_cpu_cache_bytes;
    build->plan->owned_cache_directory = build->options.cache_directory;
    build->plan->maximum_spool_bytes = build->options.maximum_spool_bytes;
    build->plan->lazy_surface_streaming = build->options.lazy_surface_streaming;
    build->plan->material_palette_sealed = build->options.lazy_surface_streaming;
    if (!build->options.material_requests.empty()) {
        build->plan->material_requests = build->options.material_requests;
        build->material_ids.reserve(build->options.material_requests.size());
        for (size_t index = 0; index < build->options.material_requests.size(); ++index) {
            const ProjectionMaterialRequest& request =
                build->options.material_requests[index];
            if (request.material_id != index + 1U || request.material_id == 0 ||
                request.name.empty()) {
                if (error) *error = "projection material registry is invalid";
                return false;
            }
            const std::string key = materialKeyFor(
                request.name, request.aux, request.rotation_quarters);
            if (!build->material_ids.emplace(key, request.material_id).second) {
                if (error) *error = "projection material registry contains duplicates";
                return false;
            }
        }
    } else if (build->options.lazy_surface_streaming) {
        if (error) *error = "lazy projection material registry is empty";
        return false;
    }
    build->plan->spool_path = makeSpoolPath(build->options.cache_directory);
    build->spool_writer.open(build->plan->spool_path,
                             std::ios::binary | std::ios::out | std::ios::trunc);
    if (!build->spool_writer) {
        if (error) *error = "cannot create projection render spool in cache directory";
        return false;
    }
    g_stream_build = std::move(build);
    return true;
}

bool BuildProjectionRenderer::appendStreamingPartition(
        ProjectionPartitionInput partition, const CancelCheck& cancel_check,
        std::string* error) {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (!g_stream_build || !g_stream_build->spool_writer) {
        if (error) *error = "projection streaming plan has not been started";
        return false;
    }
    std::vector<ProjectionGroup> groups;
    uint64_t core_blocks = 0;
    if (!buildPartitionGroups(partition.blueprint, &partition.core_bounds,
                              g_stream_build->options.maximum_partition_blocks,
                              g_stream_build->plan.get(), &g_stream_build->material_ids,
                              cancel_check, &groups, &core_blocks, error)) {
        return false;
    }
    if (cancel_check && cancel_check()) {
        if (error) *error = "building projection loading was cancelled";
        return false;
    }

    ProjectionPlan& plan = *g_stream_build->plan;
    for (ProjectionGroup& group : groups) {
        if (!group.instances || group.instances->empty()) continue;
        if (!spoolGroup(g_stream_build.get(), &group, error)) return false;
        const size_t index = plan.groups.size();
        if (index >= static_cast<size_t>(kInvalidGroupIndex)) {
            if (error) *error = "projection contains more render groups than supported";
            return false;
        }
        const uint32_t compact_index = static_cast<uint32_t>(index);
        plan.detailed_instance_count += static_cast<uint64_t>(group.instance_count);
        const uint64_t key = columnKey(floorDiv(group.origin_x, kGroupSpan),
                                       floorDiv(group.origin_z, kGroupSpan));
        ProjectionColumn& column = plan.columns[key];
        column.all_groups.push_back(compact_index);
        if (!group.flags.interior_only) column.surface_groups.push_back(compact_index);
        plan.groups.push_back(std::move(group));
    }
    if (UINT64_MAX - g_stream_build->appended_core_blocks < core_blocks) {
        if (error) *error = "projection source block count overflow";
        return false;
    }
    g_stream_build->appended_core_blocks += core_blocks;
    return true;
}

bool BuildProjectionRenderer::commitStreamingPlan(const CancelCheck& cancel_check,
                                                   std::string* error) {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (!g_stream_build) {
        if (error) *error = "projection streaming plan has not been started";
        return false;
    }
    if (cancel_check && cancel_check()) {
        if (error) *error = "building projection loading was cancelled";
        g_stream_build.reset();
        return false;
    }
    ProjectionPlan& plan = *g_stream_build->plan;
    const bool lazy_surface_streaming = g_stream_build->options.lazy_surface_streaming;
    if (plan.groups.empty() && !lazy_surface_streaming) {
        if (error) *error = "building projection contains no exposed block faces";
        g_stream_build.reset();
        return false;
    }
    g_stream_build->spool_writer.flush();
    if (!g_stream_build->spool_writer) {
        if (error) *error = "cannot finalize projection render spool";
        g_stream_build.reset();
        return false;
    }
    plan.spool_bytes = g_stream_build->spool_bytes;
    if (lazy_surface_streaming) {
        plan.spool_writer = std::move(g_stream_build->spool_writer);
        plan.material_ids = std::move(g_stream_build->material_ids);
    } else {
        g_stream_build->spool_writer.close();
    }
    if (plan.source_block_count == 0) {
        plan.source_block_count = g_stream_build->appended_core_blocks;
    }
    for (auto& column : plan.columns) {
        std::sort(column.second.all_groups.begin(), column.second.all_groups.end(),
                  [&](uint32_t left, uint32_t right) {
            const ProjectionGroup& left_group = plan.groups[left];
            const ProjectionGroup& right_group = plan.groups[right];
            return left_group.origin_y != right_group.origin_y
                ? left_group.origin_y < right_group.origin_y : left < right;
        });
        std::sort(column.second.surface_groups.begin(), column.second.surface_groups.end(),
                  [&](uint32_t left, uint32_t right) {
            const ProjectionGroup& left_group = plan.groups[left];
            const ProjectionGroup& right_group = plan.groups[right];
            return left_group.origin_y != right_group.origin_y
                ? left_group.origin_y < right_group.origin_y : left < right;
        });
    }
    if (!plan.groups.empty()) ++plan.topology_revision;
    std::shared_ptr<ProjectionPlan> completed = std::move(g_stream_build->plan);
    const size_t spool_bytes = completed->spool_bytes;
    g_stream_build.reset();
    LOGI("streamed projection ready: source=%llu surface=%llu groups=%zu spool_bytes=%llu",
         static_cast<unsigned long long>(completed->source_block_count),
         static_cast<unsigned long long>(completed->detailed_instance_count),
         completed->groups.size(), static_cast<unsigned long long>(spool_bytes));
    std::atomic_store_explicit(&g_published_plan, std::move(completed),
                               std::memory_order_release);
    return true;
}

bool BuildProjectionRenderer::enqueueLazyPartition(
        uint64_t expected_plan_identity, ProjectionPartitionInput partition,
        const CancelCheck& cancel_check, std::string* error) {
    if (cancel_check && cancel_check()) {
        if (error) *error = "building projection loading was cancelled";
        return false;
    }
    const std::shared_ptr<ProjectionPlan> plan =
        std::atomic_load_explicit(&g_published_plan, std::memory_order_acquire);
    if (!plan || plan->identity != expected_plan_identity ||
        !plan->lazy_surface_streaming) {
        if (error) *error = "projection lazy plan was replaced or cleared";
        return false;
    }

    // This is deliberately outside the queue lock and outside render().  Cell
    // hashing, neighbour culling and outline extraction are the expensive work
    // that used to make the initial loading screen stall on huge blueprints.
    std::vector<ProjectionGroup> groups;
    uint64_t core_blocks = 0;
    if (!buildPartitionGroups(partition.blueprint, &partition.core_bounds,
                              0, plan.get(), &plan->material_ids, cancel_check,
                              &groups, &core_blocks, error)) {
        return false;
    }
    if (cancel_check && cancel_check()) {
        if (error) *error = "building projection loading was cancelled";
        return false;
    }
    (void)core_blocks;

    size_t queued_bytes = 0;
    for (const ProjectionGroup& group : groups) {
        const size_t bytes = pendingGroupBytes(group);
        if (bytes == std::numeric_limits<size_t>::max() ||
            queued_bytes > std::numeric_limits<size_t>::max() - bytes) {
            if (error) *error = "lazy projection group queue size overflow";
            return false;
        }
        queued_bytes += bytes;
    }
    if (groups.empty()) return true;

    std::unique_lock<std::mutex> lock(plan->pending_lazy_mutex);
    const auto planStillCurrent = [&]() {
        return std::atomic_load_explicit(&g_published_plan,
                                         std::memory_order_acquire).get() == plan.get() &&
            plan->identity == expected_plan_identity;
    };
    while (plan->lazy_append_error.empty() && planStillCurrent() &&
           !(cancel_check && cancel_check())) {
        const bool must_wait = queued_bytes <= kMaximumPendingLazyBytes
            ? plan->pending_lazy_bytes > kMaximumPendingLazyBytes - queued_bytes
            : plan->pending_lazy_bytes != 0;
        if (!must_wait) break;
        plan->pending_lazy_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (cancel_check && cancel_check()) {
        if (error) *error = "building projection loading was cancelled";
        return false;
    }
    if (!planStillCurrent()) {
        if (error) *error = "projection lazy plan was replaced or cleared";
        return false;
    }
    if (!plan->lazy_append_error.empty()) {
        if (error) *error = plan->lazy_append_error;
        return false;
    }
    if (queued_bytes > std::numeric_limits<size_t>::max() - plan->pending_lazy_bytes) {
        if (error) *error = "lazy projection queue accounting overflow";
        return false;
    }
    for (ProjectionGroup& group : groups) {
        plan->pending_lazy_groups.push_back(std::move(group));
    }
    plan->pending_lazy_bytes += queued_bytes;
    lock.unlock();
    plan->pending_lazy_cv.notify_all();
    return true;
}

void BuildProjectionRenderer::abortStreamingPlan() noexcept {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    g_stream_build.reset();
}

namespace {

bool clearPublishedPlan(uint64_t* clear_ticket) noexcept {
    BuildProjectionRenderer::instance().abortStreamingPlan();
    BuildProjectionRenderer::instance().clearReachabilityPreview();
    const uint64_t ticket =
        g_plan_clear_requested.fetch_add(1, std::memory_order_acq_rel) + 1U;
    if (clear_ticket) *clear_ticket = ticket;
    std::shared_ptr<ProjectionPlan> removed = std::atomic_exchange_explicit(
        &g_published_plan, std::shared_ptr<ProjectionPlan>{}, std::memory_order_acq_rel);
    if (removed) {
        std::lock_guard<std::mutex> lock(removed->pending_lazy_mutex);
        removed->pending_lazy_cv.notify_all();
    }
    return static_cast<bool>(removed);
}

}  // namespace

void BuildProjectionRenderer::clearPlan() noexcept {
    // Serialize ticket snapshots with clearPlanAndWaitForRender().  Without
    // this, two concurrent JNI clears can let a no-op clear acknowledge a
    // ticket belonging to a plan removed by the other caller.
    std::lock_guard<std::mutex> lock(g_plan_clear_mutex);
    (void)clearPublishedPlan(nullptr);
}

bool BuildProjectionRenderer::clearPlanAndWaitForRender(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(g_plan_clear_mutex);
    const uint64_t completed_before =
        g_plan_clear_completed.load(std::memory_order_acquire);
    const uint64_t requested_before =
        g_plan_clear_requested.load(std::memory_order_acquire);
    uint64_t ticket = 0;
    const bool removed_published_plan = clearPublishedPlan(&ticket);
    // An already-pending clear can still leave a render-thread plan alive
    // even when this call found no published plan of its own.
    if (!removed_published_plan && completed_before >= requested_before) {
        // clearPublishedPlan() deliberately creates a monotonic ticket even
        // when there was no plan.  The prior completed snapshot proves the
        // render thread had already retired every earlier plan, so acknowledge
        // this no-op ticket too; otherwise the next real load can wait for a
        // ticket which no render frame is required to observe.
        completePlanClearTicket(ticket);
        return true;
    }
    return g_plan_clear_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [ticket] {
            return g_plan_clear_completed.load(std::memory_order_acquire) >= ticket;
        });
}

void BuildProjectionRenderer::setEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) clearReachabilityPreview();
}

bool BuildProjectionRenderer::isEnabled() const noexcept {
    return g_enabled.load(std::memory_order_acquire);
}

bool BuildProjectionRenderer::setOutlineEnabled(bool enabled) noexcept {
    g_outline_enabled.store(enabled, std::memory_order_release);
    return g_outline_enabled.load(std::memory_order_acquire) == enabled;
}

void BuildProjectionRenderer::setFillAlpha(float alpha) {
    g_fill_alpha.store(std::max(0.05f, std::min(1.0f, alpha)),
                       std::memory_order_release);
}

void BuildProjectionRenderer::setRangeChunks(int32_t range_chunks) {
    g_range_chunks.store(std::max(kMinimumRangeChunks,
                                  std::min(kMaximumRangeChunks, range_chunks)),
                          std::memory_order_release);
}

int32_t BuildProjectionRenderer::rangeChunks() const noexcept {
    return g_range_chunks.load(std::memory_order_acquire);
}

void BuildProjectionRenderer::setLayerFilter(ProjectionLayerFilter filter) {
    if (filter.mode < ProjectionLayerMode::All ||
        filter.mode > ProjectionLayerMode::Range) {
        filter.mode = ProjectionLayerMode::All;
    }
    if (filter.space < ProjectionLayerSpace::Relative ||
        filter.space > ProjectionLayerSpace::World) {
        filter.space = ProjectionLayerSpace::Relative;
    }
    if (filter.mode == ProjectionLayerMode::Range &&
        filter.minimum_y > filter.maximum_y) {
        std::swap(filter.minimum_y, filter.maximum_y);
    }
    std::shared_ptr<const ProjectionLayerFilter> published =
        std::make_shared<const ProjectionLayerFilter>(filter);
    std::atomic_store_explicit(&g_layer_filter, std::move(published),
                               std::memory_order_release);
}

ProjectionLayerFilter BuildProjectionRenderer::layerFilter() const {
    const std::shared_ptr<const ProjectionLayerFilter> filter =
        std::atomic_load_explicit(&g_layer_filter, std::memory_order_acquire);
    return filter ? *filter : ProjectionLayerFilter{};
}

ProjectionDisplayScope BuildProjectionRenderer::displayScope(
        int32_t relative_origin_y) const noexcept {
    ProjectionDisplayScope scope;
    const int32_t range_chunks = g_range_chunks.load(std::memory_order_acquire);
    const std::shared_ptr<const ProjectionLayerFilter> filter =
        std::atomic_load_explicit(&g_layer_filter, std::memory_order_acquire);
    (void)makeProjectionDisplayScope(relative_origin_y, range_chunks,
                                     filter ? *filter : ProjectionLayerFilter{},
                                     &scope);
    return scope;
}

void BuildProjectionRenderer::setReachabilityPreviewEnabled(bool enabled) noexcept {
    g_reachability_preview_enabled.store(enabled, std::memory_order_release);
    if (!enabled) clearReachabilityPreview();
}

bool BuildProjectionRenderer::reachabilityPreviewEnabled() const noexcept {
    return g_reachability_preview_enabled.load(std::memory_order_acquire);
}

void BuildProjectionRenderer::clearReachabilityPreview() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_reachability_preview_mutex);
        std::atomic_store_explicit(&g_reachability_preview_snapshot,
                                   g_empty_reachability_preview_snapshot,
                                   std::memory_order_release);
    } catch (...) {
        // The renderer must fail open if a lifecycle teardown races a preview
        // publication; stale positions are never worth risking the game tick.
    }
}

void BuildProjectionRenderer::publishReachabilityPreview(
        uint64_t expected_plan_identity,
        std::vector<ProjectionReachabilityPreviewPosition> positions) {
    if (!reachabilityPreviewEnabled()) return;
    const std::shared_ptr<ProjectionPlan> plan =
        std::atomic_load_explicit(&g_published_plan, std::memory_order_acquire);
    if (!plan || expected_plan_identity == 0U ||
        plan->identity != expected_plan_identity) {
        // A concurrent projection replacement makes this snapshot stale, but
        // must not erase a newer plan's preview published by its own tick.
        return;
    }
    try {
        std::unordered_map<ProjectionWorldMatchGroupKey,
                           std::shared_ptr<ProjectionReachabilityPreviewGroupState>,
                           ProjectionWorldMatchGroupKeyHash> mutable_groups;
        mutable_groups.reserve(positions.size());
        for (const ProjectionReachabilityPreviewPosition& position : positions) {
            const ProjectionWorldMatchGroupKey key{
                reachabilityPreviewGroupCoordinate(position.x),
                reachabilityPreviewGroupCoordinate(position.y),
                reachabilityPreviewGroupCoordinate(position.z)};
            auto entry = mutable_groups.find(key);
            if (entry == mutable_groups.end()) {
                entry = mutable_groups.emplace(
                    key, std::make_shared<ProjectionReachabilityPreviewGroupState>()).first;
            }
            const uint8_t local_x = reachabilityPreviewLocalCoordinate(position.x, key.x);
            const uint8_t local_y = reachabilityPreviewLocalCoordinate(position.y, key.y);
            const uint8_t local_z = reachabilityPreviewLocalCoordinate(position.z, key.z);
            const size_t cell_index = reachabilityPreviewCellIndex(
                local_x, local_y, local_z);
            entry->second->cells[cell_index / 64U] |=
                UINT64_C(1) << (cell_index % 64U);
        }

        auto next = std::make_shared<ProjectionReachabilityPreviewSnapshot>();
        next->plan_identity = expected_plan_identity;
        next->groups.reserve(mutable_groups.size());
        for (auto& entry : mutable_groups) {
            if (!entry.second) continue;
            entry.second->fingerprint = reachabilityPreviewFingerprint(*entry.second);
            if (entry.second->fingerprint == 0U) continue;
            next->groups.emplace(entry.first,
                std::shared_ptr<const ProjectionReachabilityPreviewGroupState>(
                    std::move(entry.second)));
        }

        std::lock_guard<std::mutex> lock(g_reachability_preview_mutex);
        if (!g_reachability_preview_enabled.load(std::memory_order_acquire)) return;
        const std::shared_ptr<const ProjectionReachabilityPreviewSnapshot> current =
            std::atomic_load_explicit(&g_reachability_preview_snapshot,
                                      std::memory_order_acquire);
        if (current && sameReachabilityPreview(*current, *next)) return;
        ++g_reachability_preview_revision;
        if (g_reachability_preview_revision == 0U) {
            ++g_reachability_preview_revision;
        }
        next->revision = g_reachability_preview_revision;
        std::shared_ptr<const ProjectionReachabilityPreviewSnapshot> immutable = std::move(next);
        std::atomic_store_explicit(&g_reachability_preview_snapshot, std::move(immutable),
                                   std::memory_order_release);
    } catch (...) {
        // Presentation is optional. Keep a previous verified snapshot instead
        // of letting allocation pressure interrupt the verified player tick.
    }
}

std::vector<ProjectionMaterialRequest>
BuildProjectionRenderer::textureMaterialRequests() const {
    const std::shared_ptr<ProjectionPlan> plan =
        std::atomic_load_explicit(&g_published_plan, std::memory_order_acquire);
    return plan ? plan->material_requests : std::vector<ProjectionMaterialRequest>{};
}

bool BuildProjectionRenderer::installTexturePack(
        std::vector<std::string> material_keys,
        std::vector<int16_t> face_layers,
        int32_t tile_size,
        std::vector<uint8_t> rgba_layers,
        std::string* error) {
    const std::shared_ptr<ProjectionPlan> plan =
        std::atomic_load_explicit(&g_published_plan, std::memory_order_acquire);
    if (!plan) {
        if (error) *error = "cannot install textures without a published projection plan";
        return false;
    }
    if (tile_size < 1 || tile_size > 64) {
        if (error) *error = "projection texture tile size must be between 1 and 64";
        return false;
    }
    if (material_keys.size() != plan->material_requests.size()) {
        if (error) {
            *error = "projection texture materials do not match the current plan; "
                     "reload its material request list";
        }
        return false;
    }
    if (material_keys.size() > std::numeric_limits<size_t>::max() / 6U ||
        face_layers.size() != material_keys.size() * 6U) {
        if (error) *error = "projection texture pack must provide six faces per material";
        return false;
    }

    const size_t tile_bytes = static_cast<size_t>(tile_size) *
        static_cast<size_t>(tile_size) * 4U;
    if (rgba_layers.empty() || rgba_layers.size() % tile_bytes != 0) {
        if (error) {
            *error = "projection texture pixel buffer does not contain complete layers";
        }
        return false;
    }
    const size_t decoded_layer_count = rgba_layers.size() / tile_bytes;
    if (decoded_layer_count < 1U || decoded_layer_count > 2048U) {
        if (error) *error = "projection texture pack must contain between 1 and 2048 layers";
        return false;
    }
    const int32_t layer_count = static_cast<int32_t>(decoded_layer_count);
    for (int16_t layer : face_layers) {
        if (layer < -1 || layer >= layer_count) {
            if (error) *error = "projection texture face layer is outside the decoded layer range";
            return false;
        }
    }
    auto pack = std::make_shared<ProjectionTexturePack>();
    pack->material_face_layers.assign(
        (plan->material_requests.size() + 1U) * 6U, 0U);
    for (size_t index = 0; index < plan->material_requests.size(); ++index) {
        const ProjectionMaterialRequest& request = plan->material_requests[index];
        std::string expected_key;
        expected_key.reserve(request.name.size() + 10U);
        expected_key.append(request.name);
        expected_key.push_back('\t');
        expected_key.append(std::to_string(static_cast<unsigned int>(request.aux)));
        expected_key.push_back('\t');
        expected_key.append(std::to_string(
            static_cast<unsigned int>(request.rotation_quarters & 0x03U)));
        if (material_keys[index] != expected_key || request.material_id == 0 ||
            static_cast<size_t>(request.material_id) > plan->material_requests.size()) {
            if (error) {
                *error = "projection texture materials belong to a stale or different plan";
            }
            return false;
        }
        const size_t destination = static_cast<size_t>(request.material_id) * 6U;
        const size_t source = index * 6U;
        for (size_t face = 0; face < 6U; ++face) {
            const int16_t layer = face_layers[source + face];
            pack->material_face_layers[destination + face] =
                static_cast<uint16_t>(std::max<int16_t>(0, layer));
        }
    }

    // Android Bitmap rows arrive top-first while OpenGL's v=0 addresses the
    // bottom of a block face. Flip each tile once on the loader thread so the
    // render thread can upload contiguous layers without any repacking.
    const size_t row_bytes = static_cast<size_t>(tile_size) * 4U;
    for (int32_t layer = 0; layer < layer_count; ++layer) {
        uint8_t* layer_pixels = rgba_layers.data() +
            static_cast<size_t>(layer) * tile_bytes;
        for (int32_t row = 0; row < tile_size / 2; ++row) {
            uint8_t* top = layer_pixels + static_cast<size_t>(row) * row_bytes;
            uint8_t* bottom = layer_pixels +
                static_cast<size_t>(tile_size - 1 - row) * row_bytes;
            std::swap_ranges(top, top + row_bytes, bottom);
        }
    }
    pack->rgba_layers = std::move(rgba_layers);
    pack->tile_size = tile_size;
    pack->layer_count = layer_count;

    if (std::atomic_load_explicit(&g_published_plan,
                                  std::memory_order_acquire).get() != plan.get()) {
        if (error) *error = "projection plan changed while its textures were decoded";
        return false;
    }
    const std::shared_ptr<const ProjectionTexturePack> published_pack = std::move(pack);
    std::atomic_store_explicit(&plan->texture_pack, published_pack,
                               std::memory_order_release);
    if (std::atomic_load_explicit(&g_published_plan,
                                  std::memory_order_acquire).get() != plan.get()) {
        std::shared_ptr<const ProjectionTexturePack> expected = published_pack;
        std::atomic_compare_exchange_strong_explicit(
            &plan->texture_pack, &expected,
            std::shared_ptr<const ProjectionTexturePack>{},
            std::memory_order_acq_rel, std::memory_order_acquire);
        if (error) *error = "projection plan changed while its textures were installed";
        return false;
    }
    return true;
}

uint64_t BuildProjectionRenderer::publishedPlanIdentity() const noexcept {
    const std::shared_ptr<ProjectionPlan> plan =
        std::atomic_load_explicit(&g_published_plan, std::memory_order_acquire);
    return plan ? plan->identity : 0;
}

std::string BuildProjectionRenderer::lazyBuildError(
        uint64_t expected_plan_identity) const {
    const std::shared_ptr<ProjectionPlan> plan =
        std::atomic_load_explicit(&g_published_plan, std::memory_order_acquire);
    if (!plan || plan->identity != expected_plan_identity ||
        !plan->lazy_surface_streaming) {
        return "projection lazy plan was replaced or cleared";
    }
    std::lock_guard<std::mutex> lock(plan->pending_lazy_mutex);
    return plan->lazy_append_error;
}

void BuildProjectionRenderer::render() {
    const EGLContext current_context = eglGetCurrentContext();
    if (current_context == EGL_NO_CONTEXT) {
        clearProjectionWorldMatchInterest();
        setProjectionRenderDiagnostic(ProjectionRenderDiagnostic::NoEglContext);
        return;
    }
    const bool has_retained_gl_objects =
        g_program || g_cube_vao || g_cube_vertex_buffer || g_cube_index_buffer ||
        g_block_texture_array || g_material_face_texture;
    // Some drivers recycle both the EGLContext value and the Level-render
    // owner allocation after several leave/re-enter cycles.  Neither pointer
    // then changes even though the GL namespace did.  Verify the private
    // shader signature before using any retained numeric name; otherwise a
    // third (or later) projection can bind a game-owned buffer/program.
    const bool retained_objects_stale = has_retained_gl_objects &&
        !projectionGlObjectsBelongToCurrentContext();
    if (current_context != g_gl_context || retained_objects_stale) {
        // Programs, textures and buffers may survive in a share group even
        // though the new world owns a different local VAO namespace.  A
        // matching private shader signature lets us release only those shared
        // objects; releaseGlResourcesForCurrentContext() intentionally never
        // deletes a VAO from a different (or unverified) EGL context.
        if (projectionSharedGlObjectsBelongToCurrentNamespace()) {
            releasePlanGpu(g_render_plan);
            releaseGlResourcesForCurrentContext();
        } else {
            // The context either changed to an unrelated namespace or its
            // handle was recycled.  Never pass retained numeric names to
            // glDelete* in that case: they may now name game resources.
            if (retained_objects_stale) {
                LOGI("projection GL namespace was recreated; discarding retired handles");
            }
            invalidatePlanGpu(g_render_plan);
            invalidateGlResourcesForContextChange();
        }
        g_gl_context = current_context;
    }

    const std::shared_ptr<ProjectionPlan> published =
        std::atomic_load_explicit(&g_published_plan, std::memory_order_acquire);
    if (published.get() != g_render_plan.get()) {
        const GlStateSnapshot state;
        releasePlanGpu(g_render_plan);
        // A projection replacement is a hard renderer-generation boundary.
        // Keeping the cube VAO/program alive across loads looks efficient, but
        // RenderDragon may recreate the backing GL namespace without changing
        // its EGL handle.  Rebuild all module-owned objects for the next plan
        // instead of allowing the third or later load to reuse a stale name.
        // releaseGlResourcesForCurrentContext verifies the private shader
        // signature first; on a recycled namespace it only forgets our old
        // numeric names and never deletes a game resource.
        releaseGlResourcesForCurrentContext();
        g_render_plan = published;
        state.restore();
    }
    if (!published && !g_render_plan) {
        // All plan-owned buffers and textures were released above on this
        // exact EGL context. Background reloads may now safely create their
        // new disk and texture state without overlapping the retired plan.
        acknowledgePlanClearOnRenderThread();
    }
    if (!g_render_plan || !g_enabled.load(std::memory_order_acquire)) {
        clearProjectionWorldMatchInterest();
        if (g_gpu_texture_pack || g_block_texture_array || g_material_face_texture) {
            const GlStateSnapshot state;
            releaseTextureResourcesForCurrentContext();
            state.restore();
        }
        return;
    }
    ++g_render_frame;

    float mvp[16]{};
    Vec3 camera;
    if (!readCamera(mvp, &camera)) {
        clearProjectionWorldMatchInterest();
        return;
    }
    const Frustum frustum = extractFrustum(mvp);

    const GlStateSnapshot state;
    if (!initializeGlResources()) {
        clearProjectionWorldMatchInterest();
        state.restore();
        return;
    }
    synchronizeTextureResources(g_render_plan);
    // The worker has already performed the expensive surface extraction.  Do
    // only bounded spool/metadata adoption here, preserving render-thread
    // ownership of groups, LRU links and GLES resources.
    drainLazyGroups(g_render_plan.get());

    // World reads happen on the game-tick side.  Hold one immutable snapshot
    // for this frame so rendering never touches the game's block/level ABI.
    // A stale snapshot belongs to a previous projection and is deliberately
    // treated as the default (unmarked) view.
    const std::shared_ptr<const ProjectionWorldMatchSnapshot> world_match_snapshot =
        GetProjectionWorldMatchSnapshot();
    const ProjectionWorldMatchSnapshot* active_world_match_snapshot = nullptr;
    if (world_match_snapshot &&
        world_match_snapshot->plan_identity == g_render_plan->identity) {
        active_world_match_snapshot = world_match_snapshot.get();
    }
    const std::shared_ptr<const ProjectionReachabilityPreviewSnapshot>
        reachability_preview_snapshot = std::atomic_load_explicit(
            &g_reachability_preview_snapshot, std::memory_order_acquire);
    const ProjectionReachabilityPreviewSnapshot* active_reachability_preview = nullptr;
    if (g_reachability_preview_enabled.load(std::memory_order_acquire) &&
        reachability_preview_snapshot &&
        reachability_preview_snapshot->plan_identity == g_render_plan->identity) {
        active_reachability_preview = reachability_preview_snapshot.get();
    }

    const int32_t range_chunks = g_range_chunks.load(std::memory_order_acquire);
    const bool outline_enabled = g_outline_enabled.load(std::memory_order_acquire);
    const size_t maximum_fill_draw_calls = outline_enabled
        ? kMaximumFillDrawCallsPerFrame : kMaximumDrawCallsPerFrame;
    size_t upload_budget = kUploadBudgetPerFrame;
    size_t upload_calls = 0;
    size_t read_budget = kCpuReadBudgetPerFrame;
    size_t read_calls = 0;
    const auto read_start_deadline =
        std::chrono::steady_clock::now() + kCpuReadStartBudgetPerFrame;
    size_t frame_evictions = 0;
    size_t fill_instances = 0;
    struct CandidateGroupCache {
        uint64_t plan_identity = 0;
        uint64_t topology_revision = 0;
        std::vector<uint32_t> indices;
        int32_t camera_group_x = 0;
        int32_t camera_group_y = 0;
        int32_t camera_group_z = 0;
        int32_t scan_radius = -1;
        int64_t minimum_layer_y = 0;
        int64_t maximum_layer_y = 0;
        bool layer_cut_active = false;
    };
    thread_local std::vector<ProjectionDrawBatch> visible_groups;
    thread_local std::vector<ProjectionDrawBatch> outline_groups;
    thread_local std::vector<ProjectionWorldMatchInterestCandidate>
        world_match_interest_candidates;
    thread_local std::vector<std::pair<int32_t, int32_t>> column_offsets;
    thread_local CandidateGroupCache candidate_cache;
    thread_local ProjectionGroupUploadStaging upload_staging;
    thread_local int32_t cached_scan_radius = -1;
    visible_groups.clear();
    outline_groups.clear();
    world_match_interest_candidates.clear();
    if (visible_groups.capacity() < maximum_fill_draw_calls) {
        visible_groups.reserve(maximum_fill_draw_calls);
    }
    if (outline_enabled &&
        outline_groups.capacity() < kReservedOutlineDrawCallsPerFrame) {
        outline_groups.reserve(kReservedOutlineDrawCallsPerFrame);
    }
    if (world_match_interest_candidates.capacity() <
        kMaximumWorldMatchInterestCandidates) {
        try {
            world_match_interest_candidates.reserve(
                kMaximumWorldMatchInterestCandidates);
        } catch (...) {
            // Rendering remains valid without a fresh interest handoff.
        }
    }
    size_t outlined_instances = 0;
    const int32_t outline_range_chunks = std::min(
        range_chunks, kMaximumOutlineRangeChunks);
    const float render_range = static_cast<float>(range_chunks * kGroupSpan);
    const float render_range_squared = render_range * render_range;
    const float outline_range = static_cast<float>(outline_range_chunks * kGroupSpan);
    const float outline_range_squared = outline_range * outline_range;
    const std::shared_ptr<const ProjectionLayerFilter> layer_filter =
        std::atomic_load_explicit(&g_layer_filter, std::memory_order_acquire);
    int64_t minimum_layer_y = INT32_MIN;
    int64_t maximum_layer_y = INT32_MAX;
    if (!resolveProjectionWorldLayerRange(
            g_render_plan->relative_origin_y,
            layer_filter ? *layer_filter : ProjectionLayerFilter{},
            &minimum_layer_y, &maximum_layer_y)) {
        clearProjectionWorldMatchInterest();
        state.restore();
        return;
    }
    const bool layer_cut_active = layer_filter &&
        layer_filter->mode != ProjectionLayerMode::All;

    const auto blockCoordinate = [](float coordinate) {
        const double value = std::floor(static_cast<double>(coordinate));
        if (value <= static_cast<double>(INT32_MIN)) return INT32_MIN;
        if (value >= static_cast<double>(INT32_MAX)) return INT32_MAX;
        return static_cast<int32_t>(value);
    };
    const int32_t camera_group_x = floorDiv(blockCoordinate(camera.x), kGroupSpan);
    const int32_t camera_group_y = floorDiv(blockCoordinate(camera.y), kGroupSpan);
    const int32_t camera_group_z = floorDiv(blockCoordinate(camera.z), kGroupSpan);
    // A group can straddle the circular range boundary, so inspect one extra
    // 16-block column on each edge before applying the exact distance test.
    const int32_t scan_radius = range_chunks + 1;
    if (cached_scan_radius != scan_radius) {
        column_offsets.clear();
        const size_t diameter = static_cast<size_t>(scan_radius * 2 + 1);
        column_offsets.reserve(diameter * diameter);
        for (int32_t z = -scan_radius; z <= scan_radius; ++z) {
            for (int32_t x = -scan_radius; x <= scan_radius; ++x) {
                column_offsets.emplace_back(x, z);
            }
        }
        std::sort(column_offsets.begin(), column_offsets.end(), [](const auto& left,
                                                                   const auto& right) {
            const int64_t left_distance = static_cast<int64_t>(left.first) * left.first +
                static_cast<int64_t>(left.second) * left.second;
            const int64_t right_distance = static_cast<int64_t>(right.first) * right.first +
                static_cast<int64_t>(right.second) * right.second;
            return left_distance < right_distance;
        });
        cached_scan_radius = scan_radius;
    }

    const int64_t camera_origin_y = static_cast<int64_t>(camera_group_y) * kGroupSpan;
    const bool rebuild_candidates =
        candidate_cache.plan_identity != g_render_plan->identity ||
        candidate_cache.topology_revision != g_render_plan->topology_revision ||
        candidate_cache.camera_group_x != camera_group_x ||
        candidate_cache.camera_group_y != camera_group_y ||
        candidate_cache.camera_group_z != camera_group_z ||
        candidate_cache.scan_radius != scan_radius ||
        candidate_cache.minimum_layer_y != minimum_layer_y ||
        candidate_cache.maximum_layer_y != maximum_layer_y ||
        candidate_cache.layer_cut_active != layer_cut_active;
    if (rebuild_candidates) {
        candidate_cache.indices.clear();
        if (candidate_cache.indices.capacity() < kMaximumGroupsInspectedPerFrame) {
            candidate_cache.indices.reserve(kMaximumGroupsInspectedPerFrame);
        }
        for (const auto& offset : column_offsets) {
            if (candidate_cache.indices.size() >= kMaximumGroupsInspectedPerFrame) break;
            const int64_t group_x = static_cast<int64_t>(camera_group_x) + offset.first;
            const int64_t group_z = static_cast<int64_t>(camera_group_z) + offset.second;
            if (group_x < INT32_MIN || group_x > INT32_MAX ||
                group_z < INT32_MIN || group_z > INT32_MAX) continue;
            const auto column = g_render_plan->columns.find(columnKey(
                static_cast<int32_t>(group_x), static_cast<int32_t>(group_z)));
            if (column == g_render_plan->columns.end()) continue;

            const std::vector<uint32_t>& indices = layer_cut_active
                ? column->second.all_groups : column->second.surface_groups;
            if (indices.empty()) continue;
            auto allowed_begin = indices.begin();
            auto allowed_end = indices.end();
            if (minimum_layer_y != INT32_MIN) {
                const int64_t minimum_group_origin = minimum_layer_y - (kGroupSpan - 1);
                allowed_begin = std::lower_bound(
                    allowed_begin, allowed_end, minimum_group_origin,
                    [&](uint32_t index, int64_t origin_y) {
                        return static_cast<int64_t>(
                            g_render_plan->groups[index].origin_y) < origin_y;
                    });
            }
            if (maximum_layer_y != INT32_MAX) {
                allowed_end = std::upper_bound(
                    allowed_begin, allowed_end, maximum_layer_y,
                    [&](int64_t origin_y, uint32_t index) {
                        return origin_y < static_cast<int64_t>(
                            g_render_plan->groups[index].origin_y);
                    });
            }
            if (allowed_begin == allowed_end) continue;
            const auto middle = std::lower_bound(
                allowed_begin, allowed_end, camera_origin_y,
                [&](uint32_t index, int64_t origin_y) {
                    return static_cast<int64_t>(
                        g_render_plan->groups[index].origin_y) < origin_y;
                });
            size_t below = static_cast<size_t>(middle - indices.begin());
            size_t above = below;
            const size_t first_allowed = static_cast<size_t>(allowed_begin - indices.begin());
            const size_t one_past_last_allowed = static_cast<size_t>(allowed_end - indices.begin());
            size_t column_inspected = 0;
            while ((below > first_allowed || above < one_past_last_allowed) &&
                   column_inspected < kMaximumGroupsInspectedPerColumn &&
                   candidate_cache.indices.size() < kMaximumGroupsInspectedPerFrame) {
                uint32_t index = 0;
                if (below == first_allowed) {
                    index = indices[above++];
                } else if (above == one_past_last_allowed) {
                    index = indices[--below];
                } else {
                    const uint32_t lower_index = indices[below - 1];
                    const uint32_t upper_index = indices[above];
                    const int64_t lower_origin = g_render_plan->groups[lower_index].origin_y;
                    const int64_t upper_origin = g_render_plan->groups[upper_index].origin_y;
                    const int64_t lower_distance = camera_origin_y >= lower_origin
                        ? camera_origin_y - lower_origin : lower_origin - camera_origin_y;
                    const int64_t upper_distance = camera_origin_y >= upper_origin
                        ? camera_origin_y - upper_origin : upper_origin - camera_origin_y;
                    if (lower_distance <= upper_distance) {
                        index = lower_index;
                        --below;
                    } else {
                        index = upper_index;
                        ++above;
                    }
                }
                ++column_inspected;
                if (index < g_render_plan->groups.size()) {
                    candidate_cache.indices.push_back(index);
                }
            }
        }
        candidate_cache.plan_identity = g_render_plan->identity;
        candidate_cache.topology_revision = g_render_plan->topology_revision;
        candidate_cache.camera_group_x = camera_group_x;
        candidate_cache.camera_group_y = camera_group_y;
        candidate_cache.camera_group_z = camera_group_z;
        candidate_cache.scan_radius = scan_radius;
        candidate_cache.minimum_layer_y = minimum_layer_y;
        candidate_cache.maximum_layer_y = maximum_layer_y;
        candidate_cache.layer_cut_active = layer_cut_active;
    }

    bool draw_limit_reached = false;
    for (uint32_t compact_index : candidate_cache.indices) {
            if (draw_limit_reached) break;
            const size_t index = compact_index;
            ProjectionGroup& group = g_render_plan->groups[index];
            // Reject on the cheap camera tests before resolving layer ranges:
            // both are pure functions of the group bounds and the camera.
            const float horizontal_distance_squared = horizontalDistanceSquared(group, camera);
            if (horizontal_distance_squared > render_range_squared ||
                !intersectsFrustum(group, camera, frustum)) continue;
            std::array<ProjectionDrawBatch, 2> batches{};
            const size_t batch_count = drawBatchesForLayerRange(
                group, minimum_layer_y, maximum_layer_y, &batches);
            if (batch_count == 0) continue;
            const size_t draw_batch_count =
                group.flags.interior_only && batch_count == 2 ? 1U : batch_count;
            if (draw_batch_count >
                maximum_fill_draw_calls - visible_groups.size()) {
                draw_limit_reached = true;
                break;
            }
            if (fill_instances >= kMaximumFillInstancesPerFrame) {
                draw_limit_reached = true;
                break;
            }
            size_t group_instances = 0;
            for (size_t batch_index = 0; batch_index < batch_count; ++batch_index) {
                group_instances += batches[batch_index].instance_count;
            }
            if (group_instances > kMaximumFillInstancesPerFrame - fill_instances) continue;

            // Publish the source scope as soon as it has passed visibility,
            // layer, and draw-budget selection. Waiting for a VBO upload here
            // creates a feedback loop on large projections: a state change
            // evicts the VBO, upload throttling leaves this frame with no
            // candidates, and the matcher then clears the very snapshot that
            // requested the re-upload. The game-tick matcher never touches GPU
            // memory, so it is safe and more stable to use this selected source
            // scope even while its replacement VBO is deferred.
            if (world_match_interest_candidates.size() <
                kMaximumWorldMatchInterestCandidates) {
                for (size_t batch_index = 0; batch_index < batch_count &&
                     world_match_interest_candidates.size() <
                         kMaximumWorldMatchInterestCandidates;
                     ++batch_index) {
                    ProjectionWorldMatchInterestCandidate candidate;
                    if (!projectionWorldMatchInterestRegionForBatch(
                            group, batches[batch_index], &candidate)) {
                        continue;
                    }
                    try {
                        world_match_interest_candidates.push_back(candidate);
                    } catch (...) {
                        // Preserve previously collected visible regions if a
                        // transient allocation failure occurs on the frame.
                        break;
                    }
                }
            }
            const ProjectionWorldMatchGroupState* world_match_group =
                FindProjectionWorldMatchGroupState(
                    active_world_match_snapshot, group.origin_x, group.origin_y,
                    group.origin_z);
            const uint64_t world_match_fingerprint =
                GetProjectionWorldMatchGroupFingerprint(
                    active_world_match_snapshot, group.origin_x, group.origin_y,
                    group.origin_z);
            const ProjectionReachabilityPreviewGroupState* reachability_group =
                findReachabilityPreviewGroup(
                    active_reachability_preview, group.origin_x, group.origin_y,
                    group.origin_z);
            const uint64_t reachability_fingerprint =
                reachabilityPreviewGroupFingerprint(
                    active_reachability_preview, group.origin_x, group.origin_y,
                    group.origin_z);
            const uint64_t presentation_fingerprint = combinedPresentationFingerprint(
                world_match_fingerprint, reachability_fingerprint);

            std::array<ProjectionDrawBatch, 2> gpu_batches{};
            bool gpu_batches_ready = gpuBatchesForRequest(
                group, batches, batch_count, outline_enabled, &gpu_batches);
            const bool presentation_changed = group.instance_buffer &&
                group.gpu_presentation_fingerprint != presentation_fingerprint;
            if (group.instance_buffer && (!gpu_batches_ready || presentation_changed)) {
                // A layer-filter change, world-match update or reachability
                // update can select different transient CPU staging data. Retire
                // only a bounded number per frame and rebuild from pristine
                // source/spool data below.
                if (frame_evictions >= kMaximumEvictionsPerFrame) continue;
                evictResidentGroup(g_render_plan.get(), index);
                ++frame_evictions;
                gpu_batches_ready = false;
            }
            if (!group.instance_buffer) {
                const size_t minimum_byte_count = batchInstanceBytes(batches, batch_count);
                if (minimum_byte_count == 0) continue;
                if (upload_calls >= kMaximumUploadsPerFrame ||
                    minimum_byte_count > upload_budget) continue;
                if (!loadGroupBatchesFromSpool(
                        g_render_plan.get(), index, batches, batch_count,
                        &read_budget, &read_calls, &frame_evictions,
                        read_start_deadline)) continue;
                if (!prepareGroupUpload(
                        group, batches, batch_count, outline_enabled,
                        world_match_group,
                        reachability_group,
                        &upload_staging)) continue;
                const size_t byte_count = upload_staging.total_byte_count;
                if (byte_count > upload_budget) continue;
                if (!ensureGpuCapacity(g_render_plan.get(), byte_count, &frame_evictions)) continue;
                ++upload_calls;
                // Count attempted traffic too: a failed driver upload must not
                // allow later groups to exceed this frame's transfer budget.
                upload_budget -= byte_count;
                if (!uploadGroupBatches(
                        &group, batches, batch_count, upload_staging,
                        presentation_fingerprint, &gpu_batches)) continue;
                registerResidentGroup(g_render_plan.get(), index);
                gpu_batches_ready = true;
                // Disk-backed groups need CPU instances only as an upload
                // staging buffer. Releasing them here prevents the CPU cache
                // from duplicating every resident GPU group.
                if (group.spool_offset != kInvalidSpoolOffset &&
                    group.flags.in_cpu_lru) {
                    evictCpuGroup(g_render_plan.get(), index);
                }
            }
            if (!group.instance_buffer || group.instance_count <= 0 ||
                !gpu_batches_ready) continue;

            if (draw_batch_count == 1 && batch_count == 2) {
                const uint64_t first_end = static_cast<uint64_t>(
                    gpu_batches[0].first_instance) + gpu_batches[0].instance_count;
                if (first_end != gpu_batches[1].first_instance ||
                    gpu_batches[1].instance_count >
                        UINT32_MAX - gpu_batches[0].instance_count) {
                    continue;
                }
                gpu_batches[0].instance_count += gpu_batches[1].instance_count;
            }

            const float distance_squared = groupDistanceSquared(group, camera);
            for (size_t batch_index = 0; batch_index < draw_batch_count; ++batch_index) {
                gpu_batches[batch_index].group_index = static_cast<uint32_t>(index);
                gpu_batches[batch_index].distance_squared = distance_squared;
                visible_groups.push_back(gpu_batches[batch_index]);
            }
            touchResidentGroup(g_render_plan.get(), index);
            fill_instances += group_instances;
            if (outline_enabled && group.flags.has_outline != 0 &&
                horizontal_distance_squared <= outline_range_squared) {
                ProjectionDrawBatch outline_batch;
                if (compactOutlineBatchForGroup(
                        group, static_cast<uint32_t>(index), distance_squared,
                        &outline_batch) &&
                    outline_groups.size() < kReservedOutlineDrawCallsPerFrame &&
                    outline_batch.instance_count <=
                        kMaximumOutlinedInstancesPerFrame - outlined_instances) {
                    outline_groups.push_back(outline_batch);
                    outlined_instances += outline_batch.instance_count;
                }
            }
    }
    publishProjectionWorldMatchInterest(
        g_render_plan->identity, world_match_interest_candidates);
    if (visible_groups.empty()) {
        state.restore();
        return;
    }

    std::sort(visible_groups.begin(), visible_groups.end(), [&](const auto& left,
                                                                const auto& right) {
        return left.distance_squared > right.distance_squared;
    });
    const bool draw_outlines = outline_enabled && !outline_groups.empty();
    if (draw_outlines) {
        std::sort(outline_groups.begin(), outline_groups.end(), [&](const auto& left,
                                                                    const auto& right) {
            return left.distance_squared > right.distance_squared;
        });
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_program);
    glUniformMatrix4fv(g_mvp_location, 1, GL_FALSE, mvp);
    const bool has_layer_minimum = minimum_layer_y != INT32_MIN;
    const bool has_layer_maximum = maximum_layer_y != INT32_MAX;
    glUniform1i(g_has_layer_minimum_location, has_layer_minimum ? GL_TRUE : GL_FALSE);
    glUniform1i(g_has_layer_maximum_location, has_layer_maximum ? GL_TRUE : GL_FALSE);
    glUniform1i(g_layer_minimum_location, static_cast<GLint>(std::max<int64_t>(
        INT32_MIN, std::min<int64_t>(INT32_MAX, minimum_layer_y))));
    glUniform1i(g_layer_maximum_location, static_cast<GLint>(std::max<int64_t>(
        INT32_MIN, std::min<int64_t>(INT32_MAX, maximum_layer_y))));
    bindProjectionSamplers();
    glActiveTexture(kBlockTextureUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_block_texture_array);
    glActiveTexture(kMaterialLookupTextureUnit);
    glBindTexture(GL_TEXTURE_2D, g_material_face_texture);
    glUniform1i(g_material_lookup_width_location,
                std::max(1, g_material_lookup_width));
    glUniform1i(g_uploaded_texture_layers_location,
                g_uploaded_texture_layers);
    glUniform1i(g_textures_available_location,
                g_block_texture_array && g_material_face_texture &&
                g_uploaded_texture_layers > 1 ? GL_TRUE : GL_FALSE);
    glBindVertexArray(g_cube_vao);

    glUniform1f(g_alpha_location, g_fill_alpha.load(std::memory_order_acquire));
    glUniform1i(g_line_pass_location, GL_FALSE);
    for (const ProjectionDrawBatch& batch : visible_groups) {
        drawGroup(g_render_plan->groups[batch.group_index], camera, GL_TRIANGLES,
                  kTriangleIndexCount, 0, batch.first_instance, batch.instance_count);
    }

    if (draw_outlines) {
        // Keep the nearest projection surface in depth, then test outlines against
        // it without allowing the line pass to disturb subsequent depth values.
        glDepthMask(GL_FALSE);
        glUniform1f(g_alpha_location, 0.96f);
        glUniform1i(g_line_pass_location, GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(g_outline_line_width);
        const size_t line_offset = static_cast<size_t>(kTriangleIndexCount) * sizeof(GLushort);
        for (const ProjectionDrawBatch& batch : outline_groups) {
            drawGroup(g_render_plan->groups[batch.group_index], camera, GL_LINES,
                      kLineIndexCount, line_offset, batch.first_instance,
                      batch.instance_count);
        }
    }
    state.restore();
}

bool InitBuildProjectionHook(uintptr_t base_address) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (g_hook_installed.load(std::memory_order_acquire)) return true;
    uintptr_t target = 0;
    if (!ResolveMinecraftExecutableOffset(base_address, FunctionsAddress::Level_Render_Hook,
                                          16, &target)) {
        LOGE("Level::_render target is not executable in the Minecraft image: "
             "base=%p offset=%p", reinterpret_cast<void*>(base_address),
             reinterpret_cast<void*>(FunctionsAddress::Level_Render_Hook));
        return false;
    }
    const int result = DobbyHook(reinterpret_cast<void*>(target),
                                 reinterpret_cast<void*>(LevelRenderHook),
                                 &g_level_render_original);
    const void* original = __atomic_load_n(&g_level_render_original, __ATOMIC_ACQUIRE);
    if (result != 0 || !original) {
        LOGE("Level::_render hook failed: target=%p result=%d",
             reinterpret_cast<void*>(target), result);
        return false;
    }
    g_hook_installed.store(true, std::memory_order_release);
    LOGI("Level::_render hook installed at %p", reinterpret_cast<void*>(target));
    return true;
}

}  // namespace build_import
