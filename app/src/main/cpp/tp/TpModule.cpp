#include "TpModule.h"
#include "MinecraftUpdateHook.h"
#include "BuildPacketReceiveHook.h"
#include "../build_import/BuildExportRuntime.h"
#include "../build_import/BuildImportRuntime.h"
#include "../build_import/BuildProjectionRuntime.h"
#include "../build_import/BuildProjectionRenderer.h"
#include "../build_import/ProjectionBlockIdentity.h"
#include "../build_import/ProjectionPrinterRuntime.h"
#include "../build_import/ProjectionWorldMatchRuntime.h"
#include "../main.h"
#include "../log_control.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <limits>
#include <mutex>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#define LOG_TAG "TInfinitecz_C_pModule"

static std::string jstringToStdString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* utf8 = env->GetStringUTFChars(value, nullptr);
    if (!utf8) return {};
    std::string result(utf8);
    env->ReleaseStringUTFChars(value, utf8);
    return result;
}

namespace {

class ScopedJniLocalRef {
public:
    ScopedJniLocalRef(JNIEnv* env, jobject value) : env_(env), value_(value) {}
    ~ScopedJniLocalRef() {
        if (env_ && value_) env_->DeleteLocalRef(value_);
    }

    ScopedJniLocalRef(const ScopedJniLocalRef&) = delete;
    ScopedJniLocalRef& operator=(const ScopedJniLocalRef&) = delete;

private:
    JNIEnv* env_;
    jobject value_;
};

class ScopedJniUtfChars {
public:
    ScopedJniUtfChars(JNIEnv* env, jstring value, const char* chars)
        : env_(env), value_(value), chars_(chars) {}
    ~ScopedJniUtfChars() {
        if (env_ && value_ && chars_) env_->ReleaseStringUTFChars(value_, chars_);
    }

    ScopedJniUtfChars(const ScopedJniUtfChars&) = delete;
    ScopedJniUtfChars& operator=(const ScopedJniUtfChars&) = delete;

private:
    JNIEnv* env_;
    jstring value_;
    const char* chars_;
};

constexpr size_t kProjectionTextureFaceCount = 6U;
constexpr size_t kMaximumProjectionTextureMaterials = 65'535U;
constexpr size_t kMaximumProjectionTextureLayers = 2'048U;
constexpr size_t kMaximumProjectionTextureBytes = 32U * 1024U * 1024U;
constexpr size_t kMaximumProjectionMaterialKeyBytes = 4U * 1024U * 1024U;
constexpr int32_t kMaximumProjectionTextureTileSize = 64;
constexpr int32_t kBuildExportFirstActiveState = 1;
constexpr int32_t kBuildExportLastContiguousActiveState = 4;
constexpr int32_t kBuildExportWaitingForPlayerState = 8;
constexpr int32_t kBuildExportCapturingContainersState = 9;
constexpr int32_t kBuildExportFailedState = 6;

std::mutex g_buildOperationAdmissionMutex;

bool requireNoActiveBuildExport(const char* operation) {
    const int32_t state = static_cast<int32_t>(
        build_import::BuildExportRuntime::instance().state());
    const bool active =
        (state >= kBuildExportFirstActiveState &&
         state <= kBuildExportLastContiguousActiveState) ||
        state == kBuildExportWaitingForPlayerState ||
        state == kBuildExportCapturingContainersState;
    if (!active) {
        return true;
    }
    LOGE("%s denied: build export is active (state=%d)", operation, state);
    return false;
}

bool requireBuildImportReceiveHook(const char* operation) {
    const uintptr_t base_address = Main::getBaseAddress();
    const bool ready = BuildPacketReceiveHook::isReceiveHookReady() ||
        (base_address != 0 && BuildPacketReceiveHook::init(base_address));
    LOGI("[import-preflight] operation=%s receive_hook_ready=%d base_ready=%d",
         operation, ready ? 1 : 0, base_address != 0 ? 1 : 0);
    if (!ready) {
        LOGE("%s denied: aiCommand result receive hook is unavailable", operation);
    }
    return ready;
}

bool requireNoActiveBuildImport(const char* operation) {
    const build_import::ImportState state =
        build_import::BuildImportRuntime::instance().state();
    if (state != build_import::ImportState::Planning &&
        state != build_import::ImportState::Running &&
        state != build_import::ImportState::Paused &&
        state != build_import::ImportState::ClosedForContextChange &&
        state != build_import::ImportState::Verifying) {
        return true;
    }
    LOGE("%s denied: build import is active (state=%d)", operation,
         static_cast<int32_t>(state));
    return false;
}

constexpr jint kMaximumProjectionMaterialPreviewPageEntries = 256;

const char* projectionMaterialSummaryStatusWireName(
        build_import::ProjectionMaterialSummaryPageStatus status) noexcept {
    switch (status) {
        case build_import::ProjectionMaterialSummaryPageStatus::NoProjection:
            return "no_projection";
        case build_import::ProjectionMaterialSummaryPageStatus::Pending:
            return "pending";
        case build_import::ProjectionMaterialSummaryPageStatus::Ready:
        case build_import::ProjectionMaterialSummaryPageStatus::Complete:
            return "ready";
        case build_import::ProjectionMaterialSummaryPageStatus::InvalidQuery:
            return "invalid";
        case build_import::ProjectionMaterialSummaryPageStatus::SourceUnavailable:
            return "unavailable";
    }
    return "unavailable";
}

// The injected Kotlin UI deliberately uses a tiny line-oriented response so it
// does not depend on a host application's JSON implementation.  Imported block
// identifiers are expected to be ordinary names, but preserve wire framing if a
// malformed input happened to contain a separator.
void appendProjectionMaterialWireField(std::string* output, std::string_view value) {
    if (!output) return;
    for (const char character : value) {
        switch (character) {
            case '\t':
            case '\r':
            case '\n':
                output->push_back(' ');
                break;
            default:
                output->push_back(character);
                break;
        }
    }
}

uint64_t projectionMaterialPreviewPageCount(uint64_t total_materials,
                                             uint64_t page_size) noexcept {
    if (total_materials == 0 || page_size == 0) return 1;
    uint64_t pages = total_materials / page_size;
    if (total_materials % page_size != 0) ++pages;
    return std::min<uint64_t>(
        pages, static_cast<uint64_t>(std::numeric_limits<jint>::max()));
}

// Material summary records are already flattened to their inventory identity.
// Still use the shared projection-to-live matcher here rather than comparing
// text directly: legacy aux values, slab halves and modern item aliases are
// intentionally handled in one place with the printer itself.
uint64_t projectionMaterialInventoryCount(
        const build_import::ProjectionMaterialSummaryEntry& required,
        const build_import::ProjectionPrinterMaterialInventorySnapshot& inventory) {
    if (inventory.status !=
        build_import::ProjectionPrinterMaterialInventoryStatus::Ready) {
        return std::numeric_limits<uint64_t>::max();
    }
    uint64_t total = 0U;
    for (const build_import::ProjectionPrinterMaterialInventoryEntry& held :
         inventory.entries) {
        if (held.item_name.empty() || held.count == 0U ||
            !build_import::ProjectionCanonicalMaterialMatchesLive(
                required.item_name, required.item_aux, held.item_name, held.item_aux)) {
            continue;
        }
        if (held.count > std::numeric_limits<uint64_t>::max() - total) {
            return std::numeric_limits<uint64_t>::max() - 1U;
        }
        total += held.count;
    }
    return total;
}

std::string projectionMaterialSummaryWireResponse(
        const build_import::ProjectionMaterialSummaryPage& page,
        uint64_t requested_page, uint64_t page_size,
        const build_import::ProjectionPrinterMaterialInventorySnapshot& inventory) {
    std::string response;
    response.reserve(128U + page.entries.size() * 40U);
    response.append("v1\t");
    response.append(projectionMaterialSummaryStatusWireName(page.status));
    response.push_back('\t');
    response.append(std::to_string(page.generation));
    response.push_back('\t');
    response.append(std::to_string(page.plan_identity));
    response.push_back('\t');
    response.append(std::to_string(page.total_block_count));
    response.push_back('\t');
    response.append(std::to_string(page.total_material_count));
    response.push_back('\t');
    response.append(std::to_string(requested_page));
    response.push_back('\t');
    response.append(std::to_string(
        projectionMaterialPreviewPageCount(page.total_material_count, page_size)));
    response.push_back('\n');
    for (const build_import::ProjectionMaterialSummaryEntry& entry : page.entries) {
        appendProjectionMaterialWireField(&response, entry.item_name);
        response.push_back('\t');
        response.append(std::to_string(entry.item_aux));
        response.push_back('\t');
        response.append(std::to_string(entry.count));
        response.push_back('\t');
        const uint64_t available = projectionMaterialInventoryCount(entry, inventory);
        // -1 means the game-tick inventory snapshot is still being refreshed;
        // the Kotlin panel deliberately leaves such rows unmarked rather than
        // showing a stale green result.
        if (available == std::numeric_limits<uint64_t>::max()) {
            response.append("-1");
        } else {
            response.append(std::to_string(available));
        }
        response.push_back('\n');
    }
    return response;
}

std::string projectionMaterialSummaryWireError(const char* status) {
    std::string response = "v1\t";
    response.append(status ? status : "unavailable");
    response.append("\t0\t0\t0\t0\t0\t1\n");
    return response;
}

}  // namespace

// ==================== JNI 函数实现 ====================

extern "C" JNIEXPORT jstring JNICALL Java_com_vdl_kong520_TpModule_getWorldId(JNIEnv* env, jclass) {
    std::string wid = "unknown";
    if (!QueryWorldContextOnGameThread(&wid, 5000)) {
        LOGE("getWorldId: game-thread client API query timed out or returned no world");
        wid = "unknown";
    }
    return env->NewStringUTF(wid.c_str());
}

static bool hasFileExtension(const std::string& path, const char* extension) {
    if (!extension) return false;
    if (path.size() < std::strlen(extension)) return false;
    const size_t start = path.size() - std::strlen(extension);
    for (size_t i = 0; i < std::strlen(extension); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(path[start + i]);
        const unsigned char rhs = static_cast<unsigned char>(extension[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_startBuildImport(
        JNIEnv* env, jclass, jstring sourcePath, jstring spoolDirectory, jstring jobId,
        jint baseX, jint baseY, jint baseZ, jint blocksPerSecond, jboolean clearExisting,
        jboolean placeDenyLayer, jboolean verifyAfterImport, jint verificationPrecision,
        jint simulationChunkRange, jstring worldId, jint dimensionId,
        jboolean) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildExport("startBuildImport")) return JNI_FALSE;
    if (verificationPrecision < static_cast<jint>(build_import::VerificationPrecision::Fast) ||
        verificationPrecision > static_cast<jint>(build_import::VerificationPrecision::Thorough)) {
        LOGE("startBuildImport rejected invalid verification precision: %d", verificationPrecision);
        return JNI_FALSE;
    }
    if (!build_import::isValidSimulationChunkRange(simulationChunkRange)) {
        LOGE("startBuildImport rejected invalid simulation chunk range: %d", simulationChunkRange);
        return JNI_FALSE;
    }
    build_import::BuildImportStartRequest request;
    request.parse.source_path = jstringToStdString(env, sourcePath);
    // MP3 was a best-effort PCM transcription route.  Command music is now
    // intentionally MIDI-only so note timing, velocity, channels and program
    // changes are preserved instead of being inferred from compressed audio.
    if (hasFileExtension(request.parse.source_path, ".mp3")) {
        LOGE("startBuildImport rejected removed MP3 command-music source");
        return JNI_FALSE;
    }
    request.source_type = (hasFileExtension(request.parse.source_path, ".mid") ||
                           hasFileExtension(request.parse.source_path, ".midi"))
        ? build_import::ImportSourceType::CommandMusicMidi
        : hasFileExtension(request.parse.source_path, ".bdx")
            ? build_import::ImportSourceType::Bdx
            : hasFileExtension(request.parse.source_path, ".litematic")
                ? build_import::ImportSourceType::Litematic
                : hasFileExtension(request.parse.source_path, ".mcworld")
                    ? build_import::ImportSourceType::Mcworld
                    : (hasFileExtension(request.parse.source_path, ".infinity") ||
                       hasFileExtension(request.parse.source_path, ".IBuild"))
                        ? build_import::ImportSourceType::InfiniteczBuild
                        : build_import::ImportSourceType::Schematic;
    request.parse.spool_directory = jstringToStdString(env, spoolDirectory);
    request.parse.base_x = baseX;
    request.parse.base_y = baseY;
    request.parse.base_z = baseZ;
    request.identity.job_id = jstringToStdString(env, jobId);
    request.identity.source_file = request.parse.source_path;
    request.identity.source_hash = request.parse.source_path;
    request.world.world_id = jstringToStdString(env, worldId);
    request.world.dimension_id = dimensionId;
    request.config.overwrite_policy = clearExisting ? build_import::OverwritePolicy::ClearImportedBounds
                                                    : build_import::OverwritePolicy::PreserveExisting;
    request.config.place_deny_layer = placeDenyLayer == JNI_TRUE;
    request.config.blocks_per_second = blocksPerSecond;
    // The UI expresses this setting in native Minecraft chunks.  Parse on the
    // same 16-block grid so values such as 5 or 7 do not get rounded up by the
    // former 32-block logical partition size.
    request.config.chunk_size = build_import::ImportConfig::kVanillaChunkSize;
    request.config.simulation_chunk_range = simulationChunkRange;
    request.parse.chunk_size = request.config.chunk_size;
    request.config.verify_after_import = verifyAfterImport == JNI_TRUE;
    request.config.suppress_command_feedback = true;
    request.config.verification_precision =
        static_cast<build_import::VerificationPrecision>(verificationPrecision);
    request.config.checkpoint_path = request.parse.spool_directory + "/checkpoint.bin";
    if (!requireBuildImportReceiveHook("startBuildImport")) return JNI_FALSE;
    std::string error;
    const bool ok = build_import::BuildImportRuntime::instance().start(std::move(request), &error);
    if (!ok) LOGE("startBuildImport failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("startBuildImport C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("startBuildImport unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_startPixelArtImport(
        JNIEnv* env, jclass, jstring sourcePath, jstring spoolDirectory, jstring jobId,
        jint baseX, jint baseY, jint baseZ, jint targetWidth, jint blocksPerSecond, jboolean clearExisting,
        jboolean placeDenyLayer, jboolean verifyAfterImport, jint verificationPrecision,
        jint simulationChunkRange, jstring worldId, jint dimensionId,
        jboolean createMapsAfterImport, jboolean) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildExport("startPixelArtImport")) return JNI_FALSE;
    if (verificationPrecision < static_cast<jint>(build_import::VerificationPrecision::Fast) ||
        verificationPrecision > static_cast<jint>(build_import::VerificationPrecision::Thorough)) {
        LOGE("startPixelArtImport rejected invalid verification precision: %d", verificationPrecision);
        return JNI_FALSE;
    }
    if (!build_import::isValidSimulationChunkRange(simulationChunkRange)) {
        LOGE("startPixelArtImport rejected invalid simulation chunk range: %d", simulationChunkRange);
        return JNI_FALSE;
    }
    build_import::BuildImportStartRequest request;
    request.source_type = build_import::ImportSourceType::PixelArtPng;
    request.pixel_art.source_path = jstringToStdString(env, sourcePath);
    request.pixel_art.spool_directory = jstringToStdString(env, spoolDirectory);
    request.pixel_art.base_x = baseX;
    request.pixel_art.base_y = baseY;
    request.pixel_art.base_z = baseZ;
    request.pixel_art.target_width = targetWidth;
    request.pixel_art.create_maps_after_import = createMapsAfterImport == JNI_TRUE;
    request.identity.job_id = jstringToStdString(env, jobId);
    request.identity.source_file = request.pixel_art.source_path;
    request.identity.source_hash = request.pixel_art.source_path;
    request.world.world_id = jstringToStdString(env, worldId);
    request.world.dimension_id = dimensionId;
    request.config.overwrite_policy = clearExisting ? build_import::OverwritePolicy::ClearImportedBounds
                                                    : build_import::OverwritePolicy::PreserveExisting;
    request.config.place_deny_layer = placeDenyLayer == JNI_TRUE;
    request.config.blocks_per_second = blocksPerSecond;
    request.config.chunk_size = build_import::ImportConfig::kVanillaChunkSize;
    request.config.simulation_chunk_range = simulationChunkRange;
    request.pixel_art.chunk_size = request.config.chunk_size;
    request.config.verify_after_import = verifyAfterImport == JNI_TRUE;
    request.config.suppress_command_feedback = true;
    request.config.verification_precision =
        static_cast<build_import::VerificationPrecision>(verificationPrecision);
    request.config.checkpoint_path = request.pixel_art.spool_directory + "/checkpoint.bin";
    if (!requireBuildImportReceiveHook("startPixelArtImport")) return JNI_FALSE;
    std::string error;
    const bool ok = build_import::BuildImportRuntime::instance().start(std::move(request), &error);
    if (!ok) LOGE("startPixelArtImport failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("startPixelArtImport C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("startPixelArtImport unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_restoreBuildImport(
        JNIEnv* env, jclass, jstring spoolDirectory, jstring worldId, jint dimensionId) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildExport("restoreBuildImport")) return JNI_FALSE;
    if (!requireBuildImportReceiveHook("restoreBuildImport")) return JNI_FALSE;
    std::string error;
    const bool ok = build_import::BuildImportRuntime::instance().restore(
            jstringToStdString(env, spoolDirectory), {jstringToStdString(env, worldId), dimensionId},
            &error);
    if (!ok) LOGE("restoreBuildImport failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("restoreBuildImport C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("restoreBuildImport unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_undoLastBuildImport(
        JNIEnv* env, jclass, jstring storageDirectory, jstring spoolDirectory,
        jstring worldId, jint dimensionId) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildExport("undoLastBuildImport") ||
        !requireNoActiveBuildImport("undoLastBuildImport")) {
        return JNI_FALSE;
    }
    if (!requireBuildImportReceiveHook("undoLastBuildImport")) return JNI_FALSE;
    std::string error;
    const bool ok = build_import::BuildImportRuntime::instance().undoLastImport(
        jstringToStdString(env, storageDirectory),
        jstringToStdString(env, spoolDirectory),
        {jstringToStdString(env, worldId), dimensionId}, &error);
    if (!ok) LOGE("undoLastBuildImport failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("undoLastBuildImport C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("undoLastBuildImport unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_discardBuildImportUndoClaim(
        JNIEnv* env, jclass, jstring storageDirectory, jstring spoolDirectory) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildExport("discardBuildImportUndoClaim") ||
        !requireNoActiveBuildImport("discardBuildImportUndoClaim")) {
        return JNI_FALSE;
    }
    std::string error;
    const bool ok = build_import::BuildImportRuntime::instance().discardUndoClaim(
        jstringToStdString(env, storageDirectory),
        jstringToStdString(env, spoolDirectory), &error);
    if (!ok) LOGE("discardBuildImportUndoClaim failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("discardBuildImportUndoClaim C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("discardBuildImportUndoClaim unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_pauseBuildImport(JNIEnv*, jclass) try {
    build_import::BuildImportRuntime::instance().pause();
} catch (const std::exception& exception) {
    LOGE("pauseBuildImport C++ exception: %s", exception.what());
} catch (...) {
    LOGE("pauseBuildImport unknown C++ exception");
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_resumeBuildImport(
        JNIEnv* env, jclass, jstring worldId, jint dimensionId) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildExport("resumeBuildImport")) return JNI_FALSE;
    if (!requireBuildImportReceiveHook("resumeBuildImport")) return JNI_FALSE;
    std::string error;
    const bool ok = build_import::BuildImportRuntime::instance().resume(
            {jstringToStdString(env, worldId), dimensionId}, &error);
    if (!ok) LOGE("resumeBuildImport failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("resumeBuildImport C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("resumeBuildImport unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_cancelBuildImport(JNIEnv*, jclass) try {
    build_import::BuildImportRuntime::instance().cancel();
} catch (const std::exception& exception) {
    LOGE("cancelBuildImport C++ exception: %s", exception.what());
} catch (...) {
    LOGE("cancelBuildImport unknown C++ exception");
}

extern "C" JNIEXPORT jint JNICALL Java_com_vdl_kong520_TpModule_getBuildImportState(JNIEnv*, jclass) try {
    return static_cast<jint>(build_import::BuildImportRuntime::instance().state());
} catch (const std::exception& exception) {
    LOGE("getBuildImportState C++ exception: %s", exception.what());
    return static_cast<jint>(build_import::ImportState::Failed);
} catch (...) {
    LOGE("getBuildImportState unknown C++ exception");
    return static_cast<jint>(build_import::ImportState::Failed);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_vdl_kong520_TpModule_getBuildImportStatus(JNIEnv* env, jclass) try {
    const std::string status = build_import::BuildImportRuntime::instance().status();
    return env->NewStringUTF(status.c_str());
} catch (const std::exception& exception) {
    LOGE("getBuildImportStatus C++ exception: %s", exception.what());
    return env->NewStringUTF("native status unavailable");
} catch (...) {
    LOGE("getBuildImportStatus unknown C++ exception");
    return env->NewStringUTF("native status unavailable");
}

extern "C" JNIEXPORT jlong JNICALL Java_com_vdl_kong520_TpModule_getBuildImportTotalBlocks(JNIEnv*, jclass) try {
    return static_cast<jlong>(build_import::BuildImportRuntime::instance().totalBlockCount());
} catch (const std::exception& exception) {
    LOGE("getBuildImportTotalBlocks C++ exception: %s", exception.what());
    return 0;
} catch (...) {
    LOGE("getBuildImportTotalBlocks unknown C++ exception");
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_vdl_kong520_TpModule_getBuildImportImportedBlocks(JNIEnv*, jclass) try {
    return static_cast<jlong>(build_import::BuildImportRuntime::instance().importedBlockCount());
} catch (const std::exception& exception) {
    LOGE("getBuildImportImportedBlocks C++ exception: %s", exception.what());
    return 0;
} catch (...) {
    LOGE("getBuildImportImportedBlocks unknown C++ exception");
    return 0;
}

extern "C" JNIEXPORT jintArray JNICALL Java_com_vdl_kong520_TpModule_getBuildExportPlayerBlockPosition(
        JNIEnv* env, jclass) try {
    if (!env) {
        return nullptr;
    }
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    // The idle export form no longer polls Python/ModSDK continuously. Ask the
    // game thread for one bounded refresh when the user explicitly captures a
    // corner, keeping JNI worker threads away from engine-owned state.
    if (!build_import::BuildExportRuntime::instance().awaitPlayerBlockPosition(
            &x, &y, &z, std::chrono::milliseconds(400))) {
        return nullptr;
    }
    jintArray result = env->NewIntArray(3);
    if (!result) return nullptr;
    const jint values[3] = {
        static_cast<jint>(x), static_cast<jint>(y), static_cast<jint>(z)
    };
    env->SetIntArrayRegion(result, 0, 3, values);
    if (env->ExceptionCheck()) return nullptr;
    return result;
} catch (const std::exception& exception) {
    LOGE("getBuildExportPlayerBlockPosition C++ exception: %s", exception.what());
    return nullptr;
} catch (...) {
    LOGE("getBuildExportPlayerBlockPosition unknown C++ exception");
    return nullptr;
}

// Limited building import needs a dedicated coordinate capture control. This
// endpoint only returns the local position through the game-thread cache.
extern "C" JNIEXPORT jintArray JNICALL Java_com_vdl_kong520_TpModule_getBuildImportPlayerBlockPosition(
        JNIEnv* env, jclass) try {
    if (!env) {
        return nullptr;
    }
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    // The import panel can be opened and queried before the next idle export
    // tick has populated the shared position cache.  Ask the game thread for
    // one refresh and wait briefly rather than invoking world/Python access
    // from this JNI worker thread.
    if (!build_import::BuildExportRuntime::instance().awaitPlayerBlockPosition(
            &x, &y, &z, std::chrono::milliseconds(400))) {
        return nullptr;
    }
    jintArray result = env->NewIntArray(3);
    if (!result) return nullptr;
    const jint values[3] = {
        static_cast<jint>(x), static_cast<jint>(y), static_cast<jint>(z)
    };
    env->SetIntArrayRegion(result, 0, 3, values);
    return env->ExceptionCheck() ? nullptr : result;
} catch (const std::exception& exception) {
    LOGE("getBuildImportPlayerBlockPosition C++ exception: %s", exception.what());
    return nullptr;
} catch (...) {
    LOGE("getBuildImportPlayerBlockPosition unknown C++ exception");
    return nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_startBuildExport(
        JNIEnv* env, jclass, jstring outputPath,
        jint firstX, jint firstY, jint firstZ,
        jint secondX, jint secondY, jint secondZ,
        jint teleportMode, jstring worldId, jint dimensionId,
        jboolean replaceCheckpoint, jint simulationChunkRange,
        jboolean exportContainerItems) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildImport("startBuildExport")) return JNI_FALSE;
    if (!build_import::isValidSimulationChunkRange(simulationChunkRange)) {
        LOGE("startBuildExport rejected invalid simulation chunk range: %d", simulationChunkRange);
        return JNI_FALSE;
    }
    if (!build_import::isValidBuildExportTravelMode(teleportMode)) {
        LOGE("startBuildExport rejected invalid teleport mode: %d", teleportMode);
        return JNI_FALSE;
    }

    build_import::BuildExportStartRequest request;
    request.output_path = jstringToStdString(env, outputPath);
    request.first_x = firstX;
    request.first_y = firstY;
    request.first_z = firstZ;
    request.second_x = secondX;
    request.second_y = secondY;
    request.second_z = secondZ;
    request.travel_mode = static_cast<build_import::BuildExportTravelMode>(teleportMode);
    if (request.travel_mode == build_import::BuildExportTravelMode::Disabled) {
        CancelBuildExportTeleport();
    } else if (request.travel_mode == build_import::BuildExportTravelMode::Automatic) {
        LOGI("startBuildExport: automatic teleport requested; first region will verify "
             "TP from the observed local-player position");
    } else {
        LOGI("startBuildExport: semi-automatic teleport selected; waiting for per-region confirmation");
    }
    request.world_id = jstringToStdString(env, worldId);
    request.dimension_id = dimensionId;
    request.replace_checkpoint = replaceCheckpoint == JNI_TRUE;
    request.simulation_chunk_range = simulationChunkRange;
    request.export_container_items = exportContainerItems == JNI_TRUE;
    std::string error;
    const bool ok = build_import::BuildExportRuntime::instance().start(
        std::move(request), &error);
    if (!ok) LOGE("startBuildExport failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("startBuildExport C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("startBuildExport unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_hasBuildExportCheckpoint(
        JNIEnv* env, jclass, jstring outputPath) try {
    return build_import::BuildExportRuntime::instance().hasCheckpoint(
        jstringToStdString(env, outputPath)) ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("hasBuildExportCheckpoint C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("hasBuildExportCheckpoint unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_resumeBuildExport(
        JNIEnv* env, jclass, jstring outputPath, jint teleportMode,
        jstring worldId, jint dimensionId) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    if (!requireNoActiveBuildImport("resumeBuildExport")) return JNI_FALSE;
    std::string error;
    if (!build_import::isValidBuildExportTravelMode(teleportMode)) {
        LOGE("resumeBuildExport rejected invalid teleport mode: %d", teleportMode);
        return JNI_FALSE;
    }
    const auto travel_mode = static_cast<build_import::BuildExportTravelMode>(teleportMode);
    if (travel_mode == build_import::BuildExportTravelMode::Disabled) {
        CancelBuildExportTeleport();
    } else if (travel_mode == build_import::BuildExportTravelMode::Automatic) {
        LOGI("resumeBuildExport: automatic teleport requested; first restored region will verify "
             "TP from the observed local-player position");
    } else {
        LOGI("resumeBuildExport: semi-automatic teleport selected; waiting for per-region confirmation");
    }
    const bool ok = build_import::BuildExportRuntime::instance().resume(
        jstringToStdString(env, outputPath), jstringToStdString(env, worldId),
        dimensionId, travel_mode, &error);
    if (!ok) LOGE("resumeBuildExport failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("resumeBuildExport C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("resumeBuildExport unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_discardBuildExportCheckpoint(
        JNIEnv* env, jclass, jstring outputPath) try {
    std::lock_guard<std::mutex> admission_lock(g_buildOperationAdmissionMutex);
    std::string error;
    const bool ok = build_import::BuildExportRuntime::instance().discardCheckpoint(
        jstringToStdString(env, outputPath), &error);
    if (!ok) LOGE("discardBuildExportCheckpoint failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("discardBuildExportCheckpoint C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("discardBuildExportCheckpoint unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_cancelBuildExport(
        JNIEnv*, jclass) try {
    build_import::BuildExportRuntime::instance().cancel();
} catch (const std::exception& exception) {
    LOGE("cancelBuildExport C++ exception: %s", exception.what());
} catch (...) {
    LOGE("cancelBuildExport unknown C++ exception");
}

extern "C" JNIEXPORT jint JNICALL Java_com_vdl_kong520_TpModule_getBuildExportState(
        JNIEnv*, jclass) try {
    return static_cast<jint>(build_import::BuildExportRuntime::instance().state());
} catch (const std::exception& exception) {
    LOGE("getBuildExportState C++ exception: %s", exception.what());
    return kBuildExportFailedState;
} catch (...) {
    LOGE("getBuildExportState unknown C++ exception");
    return kBuildExportFailedState;
}

extern "C" JNIEXPORT jstring JNICALL Java_com_vdl_kong520_TpModule_getBuildExportStatus(
        JNIEnv* env, jclass) try {
    const std::string status = build_import::BuildExportRuntime::instance().status();
    return env->NewStringUTF(status.c_str());
} catch (const std::exception& exception) {
    LOGE("getBuildExportStatus C++ exception: %s", exception.what());
    return env->NewStringUTF("native export status unavailable");
} catch (...) {
    LOGE("getBuildExportStatus unknown C++ exception");
    return env->NewStringUTF("native export status unavailable");
}

extern "C" JNIEXPORT jlong JNICALL Java_com_vdl_kong520_TpModule_getBuildExportTotalBlocks(
        JNIEnv*, jclass) try {
    return static_cast<jlong>(
        build_import::BuildExportRuntime::instance().totalBlockCount());
} catch (const std::exception& exception) {
    LOGE("getBuildExportTotalBlocks C++ exception: %s", exception.what());
    return 0;
} catch (...) {
    LOGE("getBuildExportTotalBlocks unknown C++ exception");
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_vdl_kong520_TpModule_getBuildExportProcessedBlocks(
        JNIEnv*, jclass) try {
    return static_cast<jlong>(
        build_import::BuildExportRuntime::instance().processedBlockCount());
} catch (const std::exception& exception) {
    LOGE("getBuildExportProcessedBlocks C++ exception: %s", exception.what());
    return 0;
} catch (...) {
    LOGE("getBuildExportProcessedBlocks unknown C++ exception");
    return 0;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_isBuildExportTeleportAllowed(
        JNIEnv*, jclass) try {
    return build_import::BuildExportRuntime::instance().teleportAllowed()
        ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("isBuildExportTeleportAllowed C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("isBuildExportTeleportAllowed unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL Java_com_vdl_kong520_TpModule_getBuildExportTeleportMode(
        JNIEnv*, jclass) try {
    return static_cast<jint>(build_import::BuildExportRuntime::instance().travelMode());
} catch (const std::exception& exception) {
    LOGE("getBuildExportTeleportMode C++ exception: %s", exception.what());
    return static_cast<jint>(build_import::BuildExportTravelMode::Disabled);
} catch (...) {
    LOGE("getBuildExportTeleportMode unknown C++ exception");
    return static_cast<jint>(build_import::BuildExportTravelMode::Disabled);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_requestBuildExportNextRegion(
        JNIEnv*, jclass) try {
    return build_import::BuildExportRuntime::instance().requestNextRegionTeleport()
        ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("requestBuildExportNextRegion C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("requestBuildExportNextRegion unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jintArray JNICALL Java_com_vdl_kong520_TpModule_getBuildExportTravelTarget(
        JNIEnv* env, jclass) try {
    if (!env) {
        return nullptr;
    }
    build_import::BuildExportTravelTarget target;
    if (!build_import::BuildExportRuntime::instance().travelTarget(&target) ||
        !target.valid) {
        return nullptr;
    }
    jintArray result = env->NewIntArray(7);
    if (!result) return nullptr;
    const jint values[7] = {
        static_cast<jint>(target.x),
        static_cast<jint>(target.y),
        static_cast<jint>(target.z),
        static_cast<jint>(target.distance_blocks),
        target.waiting ? 1 : 0,
        static_cast<jint>(target.batch_index),
        static_cast<jint>(target.batch_count),
    };
    env->SetIntArrayRegion(result, 0, 7, values);
    if (env->ExceptionCheck()) return nullptr;
    return result;
} catch (const std::exception& exception) {
    LOGE("getBuildExportTravelTarget C++ exception: %s", exception.what());
    return nullptr;
} catch (...) {
    LOGE("getBuildExportTravelTarget unknown C++ exception");
    return nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_loadBuildProjection(
        JNIEnv* env, jclass, jstring sourcePath, jstring workDirectory,
        jint baseX, jint baseY, jint baseZ, jint rotationDegrees,
        jint pixelArtWidth) try {
    if (!requireNoActiveBuildExport("loadBuildProjection")) return JNI_FALSE;
    // Ensure the game hooks are ready before loading a projection.
    // Retry here so a transient early-loading failure cannot leave a valid
    // projection permanently invisible for the rest of the process.
    if (!EnsureBuildProjectionHooksReady()) {
        LOGE("loadBuildProjection failed: projection render hooks are unavailable");
        return JNI_FALSE;
    }
    // A new source must never inherit a pending placement from a prior plan.
    build_import::ProjectionPrinterRuntime::instance().clear();
    build_import::ProjectionWorldMatchRuntime::instance().clear();
    build_import::BuildProjectionLoadRequest request;
    request.source_path = jstringToStdString(env, sourcePath);
    request.work_directory = jstringToStdString(env, workDirectory);
    request.base_x = baseX;
    request.base_y = baseY;
    request.base_z = baseZ;
    request.rotation_degrees = rotationDegrees;
    request.pixel_art_width = pixelArtWidth;

    std::string error;
    const bool ok = build_import::BuildProjectionRuntime::instance().load(
            std::move(request), &error);
    if (!ok) LOGE("loadBuildProjection failed: %s", error.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("loadBuildProjection C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("loadBuildProjection unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_clearBuildProjection(
        JNIEnv*, jclass) try {
    build_import::ProjectionPrinterRuntime::instance().clear();
    build_import::ProjectionWorldMatchRuntime::instance().clear();
    build_import::BuildProjectionRuntime::instance().clear();
} catch (const std::exception& exception) {
    LOGE("clearBuildProjection C++ exception: %s", exception.what());
} catch (...) {
    LOGE("clearBuildProjection unknown C++ exception");
}

extern "C" JNIEXPORT jstring JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionStatus(
        JNIEnv* env, jclass) try {
    const std::string status = build_import::BuildProjectionRuntime::instance().status();
    return env->NewStringUTF(status.c_str());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionStatus C++ exception: %s", exception.what());
    return env->NewStringUTF("native projection status unavailable");
} catch (...) {
    LOGE("getBuildProjectionStatus unknown C++ exception");
    return env->NewStringUTF("native projection status unavailable");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_vdl_kong520_TpModule_getBuildProjectionMaterialSummaryPage(
        JNIEnv* env, jclass, jint pageIndex, jint pageSize) try {
    if (!env) return nullptr;
    if (pageIndex < 0 || pageSize <= 0 ||
        pageSize > kMaximumProjectionMaterialPreviewPageEntries) {
        const std::string response = projectionMaterialSummaryWireError("invalid");
        return env->NewStringUTF(response.c_str());
    }

    // Native material-summary cursors are stable entry offsets.  The UI has a
    // conventional zero-based page index, so this conversion stays O(1) even
    // when an unusually large building has many material kinds.
    const uint64_t requested_page = static_cast<uint64_t>(pageIndex);
    const uint64_t page_size = static_cast<uint64_t>(pageSize);
    const uint64_t cursor = requested_page * page_size;
    build_import::ProjectionMaterialSummaryPage page;
    std::string error;
    const bool queried = build_import::BuildProjectionRuntime::instance()
        .queryProjectionMaterialSummaryPage(cursor, static_cast<size_t>(page_size),
                                            &page, &error);
    if (!queried && !error.empty()) {
        LOGE("getBuildProjectionMaterialSummaryPage: %s", error.c_str());
    }
    build_import::ProjectionPrinterMaterialInventorySnapshot inventory;
    build_import::ProjectionPrinterRuntime::instance().queryMaterialInventorySnapshot(
        &inventory);
    const std::string response = projectionMaterialSummaryWireResponse(
        page, requested_page, page_size, inventory);
    return env->NewStringUTF(response.c_str());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionMaterialSummaryPage C++ exception: %s", exception.what());
    if (!env) return nullptr;
    const std::string response = projectionMaterialSummaryWireError("unavailable");
    return env->NewStringUTF(response.c_str());
} catch (...) {
    LOGE("getBuildProjectionMaterialSummaryPage unknown C++ exception");
    if (!env) return nullptr;
    const std::string response = projectionMaterialSummaryWireError("unavailable");
    return env->NewStringUTF(response.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_setBuildProjectionEnabled(
        JNIEnv*, jclass, jboolean enabled) {
    const bool requested = enabled == JNI_TRUE;
    build_import::BuildProjectionRenderer::instance().setEnabled(requested);
    // Printing is defined by the current rendered projection scope. Hiding
    // that projection also stops its printer rather than leaving an invisible
    // automation running in the world.
    if (!requested) build_import::ProjectionPrinterRuntime::instance().setEnabled(false);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_setBuildProjectionOutlineEnabled(
        JNIEnv*, jclass, jboolean enabled) {
    const bool requested = enabled == JNI_TRUE;
    const bool applied =
        build_import::BuildProjectionRenderer::instance().setOutlineEnabled(requested);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "projection outline requested=%d applied=%d",
                        requested ? 1 : 0, applied ? 1 : 0);
    return applied ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_setBuildProjectionAlpha(
        JNIEnv*, jclass, jfloat alpha) {
    build_import::BuildProjectionRenderer::instance().setFillAlpha(alpha);
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_setBuildProjectionRange(
        JNIEnv*, jclass, jint rangeChunks) {
    build_import::BuildProjectionRenderer::instance().setRangeChunks(rangeChunks);
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_setBuildProjectionLayerFilter(
        JNIEnv*, jclass, jint mode, jboolean worldSpace,
        jint minimumY, jint maximumY) try {
    build_import::ProjectionLayerFilter filter;
    const int bounded_mode = std::max(0, std::min(4, static_cast<int>(mode)));
    filter.mode = static_cast<build_import::ProjectionLayerMode>(bounded_mode);
    filter.space = worldSpace == JNI_TRUE
        ? build_import::ProjectionLayerSpace::World
        : build_import::ProjectionLayerSpace::Relative;
    filter.minimum_y = static_cast<int32_t>(minimumY);
    filter.maximum_y = static_cast<int32_t>(maximumY);
    if (filter.mode == build_import::ProjectionLayerMode::Single) {
        filter.maximum_y = filter.minimum_y;
    } else if (filter.mode == build_import::ProjectionLayerMode::Range &&
               filter.minimum_y > filter.maximum_y) {
        std::swap(filter.minimum_y, filter.maximum_y);
    }
    build_import::BuildProjectionRenderer::instance().setLayerFilter(filter);
} catch (const std::exception& exception) {
    LOGE("setBuildProjectionLayerFilter C++ exception: %s", exception.what());
} catch (...) {
    LOGE("setBuildProjectionLayerFilter unknown C++ exception");
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_setBuildProjectionPrinterEnabled(
        JNIEnv*, jclass, jboolean enabled) try {
    const bool requested = enabled == JNI_TRUE;
    build_import::ProjectionPrinterRuntime::instance().setEnabled(requested);
} catch (const std::exception& exception) {
    LOGE("setBuildProjectionPrinterEnabled C++ exception: %s", exception.what());
} catch (...) {
    LOGE("setBuildProjectionPrinterEnabled unknown C++ exception");
}

extern "C" JNIEXPORT void JNICALL
Java_com_vdl_kong520_TpModule_setBuildProjectionReachabilityPreviewEnabled(
        JNIEnv*, jclass, jboolean enabled) try {
    const bool requested = enabled == JNI_TRUE;
    build_import::ProjectionPrinterRuntime::instance().setReachabilityPreviewEnabled(requested);
} catch (const std::exception& exception) {
    LOGE("setBuildProjectionReachabilityPreviewEnabled C++ exception: %s", exception.what());
} catch (...) {
    LOGE("setBuildProjectionReachabilityPreviewEnabled unknown C++ exception");
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_isBuildProjectionPrinterEnabled(
        JNIEnv*, jclass) try {
    return build_import::ProjectionPrinterRuntime::instance().enabled() ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("isBuildProjectionPrinterEnabled C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("isBuildProjectionPrinterEnabled unknown C++ exception");
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_vdl_kong520_TpModule_setBuildProjectionPrinterRate(
        JNIEnv*, jclass, jint blocksPerSecond) try {
    build_import::ProjectionPrinterRuntime::instance().setRate(static_cast<int32_t>(blocksPerSecond));
} catch (const std::exception& exception) {
    LOGE("setBuildProjectionPrinterRate C++ exception: %s", exception.what());
} catch (...) {
    LOGE("setBuildProjectionPrinterRate unknown C++ exception");
}

extern "C" JNIEXPORT jint JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterRate(
        JNIEnv*, jclass) try {
    return static_cast<jint>(build_import::ProjectionPrinterRuntime::instance().rate());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionPrinterRate C++ exception: %s", exception.what());
    return 4;
} catch (...) {
    LOGE("getBuildProjectionPrinterRate unknown C++ exception");
    return 4;
}

extern "C" JNIEXPORT jint JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterState(
        JNIEnv*, jclass) try {
    return static_cast<jint>(build_import::ProjectionPrinterRuntime::instance().state());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionPrinterState C++ exception: %s", exception.what());
    return static_cast<jint>(build_import::ProjectionPrinterState::Error);
} catch (...) {
    LOGE("getBuildProjectionPrinterState unknown C++ exception");
    return static_cast<jint>(build_import::ProjectionPrinterState::Error);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterStatus(
        JNIEnv* env, jclass) try {
    if (!env) return nullptr;
    const std::string status = build_import::ProjectionPrinterRuntime::instance().status();
    return env->NewStringUTF(status.c_str());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionPrinterStatus C++ exception: %s", exception.what());
    return env ? env->NewStringUTF("打印机状态不可用") : nullptr;
} catch (...) {
    LOGE("getBuildProjectionPrinterStatus unknown C++ exception");
    return env ? env->NewStringUTF("打印机状态不可用") : nullptr;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterTotalBlocks(
        JNIEnv*, jclass) try {
    return static_cast<jlong>(build_import::ProjectionPrinterRuntime::instance().totalBlocks());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionPrinterTotalBlocks C++ exception: %s", exception.what());
    return 0;
} catch (...) {
    LOGE("getBuildProjectionPrinterTotalBlocks unknown C++ exception");
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterPlacedBlocks(
        JNIEnv*, jclass) try {
    return static_cast<jlong>(build_import::ProjectionPrinterRuntime::instance().placedBlocks());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionPrinterPlacedBlocks C++ exception: %s", exception.what());
    return 0;
} catch (...) {
    LOGE("getBuildProjectionPrinterPlacedBlocks unknown C++ exception");
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterSkippedBlocks(
        JNIEnv*, jclass) try {
    return static_cast<jlong>(build_import::ProjectionPrinterRuntime::instance().skippedBlocks());
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionPrinterSkippedBlocks C++ exception: %s", exception.what());
    return 0;
} catch (...) {
    LOGE("getBuildProjectionPrinterSkippedBlocks unknown C++ exception");
    return 0;
}

extern "C" JNIEXPORT jstring JNICALL Java_com_vdl_kong520_TpModule_getBuildProjectionTextureRequests(
        JNIEnv* env, jclass) try {
    if (!env) {
        LOGE("getBuildProjectionTextureRequests rejected null JNIEnv");
        return nullptr;
    }
    const std::vector<build_import::ProjectionMaterialRequest> requests =
        build_import::BuildProjectionRenderer::instance().textureMaterialRequests();
    std::string output;
    if (requests.size() <= std::numeric_limits<size_t>::max() / 48U) {
        output.reserve(requests.size() * 48U);
    }
    for (const build_import::ProjectionMaterialRequest& request : requests) {
        output.append(request.name);
        output.push_back('\t');
        output.append(std::to_string(static_cast<unsigned int>(request.aux)));
        output.push_back('\t');
        output.append(std::to_string(
            static_cast<unsigned int>(request.rotation_quarters)));
        output.push_back('\n');
    }
    jstring result = env->NewStringUTF(output.c_str());
    if (!result && !env->ExceptionCheck()) {
        LOGE("getBuildProjectionTextureRequests failed to allocate Java string");
    }
    return result;
} catch (const std::exception& exception) {
    LOGE("getBuildProjectionTextureRequests C++ exception: %s", exception.what());
    return env && !env->ExceptionCheck() ? env->NewStringUTF("") : nullptr;
} catch (...) {
    LOGE("getBuildProjectionTextureRequests unknown C++ exception");
    return env && !env->ExceptionCheck() ? env->NewStringUTF("") : nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_vdl_kong520_TpModule_installBuildProjectionTexturePack(
        JNIEnv* env, jclass, jobjectArray materialKeys, jshortArray faceLayers,
        jint tileSize, jint layerCount, jbyteArray layerPixels) try {
    if (!env || !materialKeys || !faceLayers || !layerPixels) {
        LOGE("installBuildProjectionTexturePack rejected null argument");
        return JNI_FALSE;
    }
    if (tileSize < 1 || tileSize > kMaximumProjectionTextureTileSize) {
        LOGE("installBuildProjectionTexturePack rejected tile size: %d", tileSize);
        return JNI_FALSE;
    }

    const jsize key_count_jni = env->GetArrayLength(materialKeys);
    const jsize face_count_jni = env->GetArrayLength(faceLayers);
    const jsize pixel_byte_count_jni = env->GetArrayLength(layerPixels);
    if (env->ExceptionCheck()) {
        LOGE("installBuildProjectionTexturePack failed to read Java array lengths");
        return JNI_FALSE;
    }
    if (key_count_jni < 0 || face_count_jni < 0 || pixel_byte_count_jni < 0) {
        LOGE("installBuildProjectionTexturePack rejected negative array length");
        return JNI_FALSE;
    }

    const size_t key_count = static_cast<size_t>(key_count_jni);
    const size_t face_count = static_cast<size_t>(face_count_jni);
    const size_t pixel_byte_count = static_cast<size_t>(pixel_byte_count_jni);
    if (key_count > kMaximumProjectionTextureMaterials) {
        LOGE("installBuildProjectionTexturePack rejected %zu material keys", key_count);
        return JNI_FALSE;
    }
    if (key_count > std::numeric_limits<size_t>::max() / kProjectionTextureFaceCount) {
        LOGE("installBuildProjectionTexturePack material face count overflow");
        return JNI_FALSE;
    }
    const size_t expected_face_count = key_count * kProjectionTextureFaceCount;
    if (face_count != expected_face_count) {
        LOGE("installBuildProjectionTexturePack rejected %zu faces for %zu materials",
             face_count, key_count);
        return JNI_FALSE;
    }

    const size_t tile_size = static_cast<size_t>(tileSize);
    if (tile_size > std::numeric_limits<size_t>::max() / tile_size) {
        LOGE("installBuildProjectionTexturePack tile area overflow");
        return JNI_FALSE;
    }
    const size_t tile_area = tile_size * tile_size;
    if (tile_area > std::numeric_limits<size_t>::max() / 4U) {
        LOGE("installBuildProjectionTexturePack layer byte count overflow");
        return JNI_FALSE;
    }
    const size_t bytes_per_layer = tile_area * 4U;
    if (layerCount < 1 ||
        static_cast<size_t>(layerCount) > kMaximumProjectionTextureLayers) {
        LOGE("installBuildProjectionTexturePack rejected %d texture layers", layerCount);
        return JNI_FALSE;
    }
    const size_t layer_count = static_cast<size_t>(layerCount);
    if (layer_count > std::numeric_limits<size_t>::max() / bytes_per_layer) {
        LOGE("installBuildProjectionTexturePack pixel prefix size overflow");
        return JNI_FALSE;
    }
    const size_t required_pixel_byte_count = layer_count * bytes_per_layer;
    if (pixel_byte_count < required_pixel_byte_count ||
        pixel_byte_count > kMaximumProjectionTextureBytes ||
        pixel_byte_count % bytes_per_layer != 0U) {
        LOGE("installBuildProjectionTexturePack rejected %zu pixel bytes for tile size %d",
             pixel_byte_count, tileSize);
        return JNI_FALSE;
    }

    std::vector<std::string> material_keys;
    material_keys.reserve(key_count);
    size_t total_key_bytes = 0U;
    jclass string_class = env->FindClass("java/lang/String");
    ScopedJniLocalRef string_class_ref(env, string_class);
    if (!string_class || env->ExceptionCheck()) {
        LOGE("installBuildProjectionTexturePack could not resolve java.lang.String");
        return JNI_FALSE;
    }
    for (jsize index = 0; index < key_count_jni; ++index) {
        jobject element = env->GetObjectArrayElement(materialKeys, index);
        ScopedJniLocalRef element_ref(env, element);
        if (env->ExceptionCheck()) {
            LOGE("installBuildProjectionTexturePack failed to read material key %d", index);
            return JNI_FALSE;
        }
        if (!element) {
            LOGE("installBuildProjectionTexturePack rejected material key %d", index);
            return JNI_FALSE;
        }
        const jboolean is_string = env->IsInstanceOf(element, string_class);
        if (env->ExceptionCheck()) {
            LOGE("installBuildProjectionTexturePack failed to validate material key %d", index);
            return JNI_FALSE;
        }
        if (is_string != JNI_TRUE) {
            LOGE("installBuildProjectionTexturePack rejected non-string material key %d", index);
            return JNI_FALSE;
        }

        jstring key = static_cast<jstring>(element);
        const jsize utf8_length_jni = env->GetStringUTFLength(key);
        if (env->ExceptionCheck() || utf8_length_jni <= 0) {
            LOGE("installBuildProjectionTexturePack rejected empty material key %d", index);
            return JNI_FALSE;
        }
        const size_t utf8_length = static_cast<size_t>(utf8_length_jni);
        if (total_key_bytes >= kMaximumProjectionMaterialKeyBytes ||
            utf8_length > kMaximumProjectionMaterialKeyBytes - total_key_bytes - 1U) {
            LOGE("installBuildProjectionTexturePack material keys exceed 4 MiB");
            return JNI_FALSE;
        }
        const char* utf8 = env->GetStringUTFChars(key, nullptr);
        ScopedJniUtfChars utf8_ref(env, key, utf8);
        if (!utf8 || env->ExceptionCheck()) {
            LOGE("installBuildProjectionTexturePack failed to copy material key %d", index);
            return JNI_FALSE;
        }
        material_keys.emplace_back(utf8, utf8_length);
        total_key_bytes += utf8_length + 1U;
    }

    std::vector<int16_t> face_layers(face_count);
    if (face_count_jni > 0) {
        static_assert(sizeof(jshort) == sizeof(int16_t),
                      "JNI short must be a 16-bit integer");
        env->GetShortArrayRegion(faceLayers, 0, face_count_jni,
                                 reinterpret_cast<jshort*>(face_layers.data()));
        if (env->ExceptionCheck()) {
            LOGE("installBuildProjectionTexturePack failed to copy face layers");
            return JNI_FALSE;
        }
    }
    for (size_t index = 0; index < face_layers.size(); ++index) {
        const int16_t layer = face_layers[index];
        if (layer < -1 || (layer >= 0 && static_cast<size_t>(layer) >= layer_count)) {
            LOGE("installBuildProjectionTexturePack rejected face layer %d at %zu",
                 static_cast<int>(layer), index);
            return JNI_FALSE;
        }
    }

    std::vector<uint8_t> rgba_layers(required_pixel_byte_count);
    env->GetByteArrayRegion(layerPixels, 0,
                            static_cast<jsize>(required_pixel_byte_count),
                            reinterpret_cast<jbyte*>(rgba_layers.data()));
    if (env->ExceptionCheck()) {
        LOGE("installBuildProjectionTexturePack failed to copy texture pixels");
        return JNI_FALSE;
    }

    std::string error;
    const bool installed = build_import::BuildProjectionRenderer::instance().installTexturePack(
        std::move(material_keys), std::move(face_layers), static_cast<int32_t>(tileSize),
        std::move(rgba_layers), &error);
    if (!installed) {
        LOGE("installBuildProjectionTexturePack failed: %s", error.c_str());
    }
    return installed ? JNI_TRUE : JNI_FALSE;
} catch (const std::exception& exception) {
    LOGE("installBuildProjectionTexturePack C++ exception: %s", exception.what());
    return JNI_FALSE;
} catch (...) {
    LOGE("installBuildProjectionTexturePack unknown C++ exception");
    return JNI_FALSE;
}

bool RegisterTpModuleNatives(JNIEnv* env) noexcept {
    if (!env) return false;
    static const JNINativeMethod methods[] = {
        {"getWorldId", "()Ljava/lang/String;", (void*)Java_com_vdl_kong520_TpModule_getWorldId},
        {"startBuildImport", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIIIZZZIILjava/lang/String;IZ)Z", (void*)Java_com_vdl_kong520_TpModule_startBuildImport},
        {"startPixelArtImport", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIIIIZZZIILjava/lang/String;IZZ)Z", (void*)Java_com_vdl_kong520_TpModule_startPixelArtImport},
        {"restoreBuildImport", "(Ljava/lang/String;Ljava/lang/String;I)Z", (void*)Java_com_vdl_kong520_TpModule_restoreBuildImport},
        {"undoLastBuildImport", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Z", (void*)Java_com_vdl_kong520_TpModule_undoLastBuildImport},
        {"discardBuildImportUndoClaim", "(Ljava/lang/String;Ljava/lang/String;)Z", (void*)Java_com_vdl_kong520_TpModule_discardBuildImportUndoClaim},
        {"pauseBuildImport", "()V", (void*)Java_com_vdl_kong520_TpModule_pauseBuildImport},
        {"resumeBuildImport", "(Ljava/lang/String;I)Z", (void*)Java_com_vdl_kong520_TpModule_resumeBuildImport},
        {"cancelBuildImport", "()V", (void*)Java_com_vdl_kong520_TpModule_cancelBuildImport},
        {"getBuildImportState", "()I", (void*)Java_com_vdl_kong520_TpModule_getBuildImportState},
        {"getBuildImportStatus", "()Ljava/lang/String;", (void*)Java_com_vdl_kong520_TpModule_getBuildImportStatus},
        {"getBuildImportTotalBlocks", "()J", (void*)Java_com_vdl_kong520_TpModule_getBuildImportTotalBlocks},
        {"getBuildImportImportedBlocks", "()J", (void*)Java_com_vdl_kong520_TpModule_getBuildImportImportedBlocks},
        {"getBuildImportPlayerBlockPosition", "()[I", (void*)Java_com_vdl_kong520_TpModule_getBuildImportPlayerBlockPosition},
        {"getBuildExportPlayerBlockPosition", "()[I", (void*)Java_com_vdl_kong520_TpModule_getBuildExportPlayerBlockPosition},
        {"startBuildExport", "(Ljava/lang/String;IIIIIIILjava/lang/String;IZIZ)Z", (void*)Java_com_vdl_kong520_TpModule_startBuildExport},
        {"hasBuildExportCheckpoint", "(Ljava/lang/String;)Z", (void*)Java_com_vdl_kong520_TpModule_hasBuildExportCheckpoint},
        {"resumeBuildExport", "(Ljava/lang/String;ILjava/lang/String;I)Z", (void*)Java_com_vdl_kong520_TpModule_resumeBuildExport},
        {"discardBuildExportCheckpoint", "(Ljava/lang/String;)Z", (void*)Java_com_vdl_kong520_TpModule_discardBuildExportCheckpoint},
        {"cancelBuildExport", "()V", (void*)Java_com_vdl_kong520_TpModule_cancelBuildExport},
        {"getBuildExportState", "()I", (void*)Java_com_vdl_kong520_TpModule_getBuildExportState},
        {"getBuildExportStatus", "()Ljava/lang/String;", (void*)Java_com_vdl_kong520_TpModule_getBuildExportStatus},
        {"getBuildExportTotalBlocks", "()J", (void*)Java_com_vdl_kong520_TpModule_getBuildExportTotalBlocks},
        {"getBuildExportProcessedBlocks", "()J", (void*)Java_com_vdl_kong520_TpModule_getBuildExportProcessedBlocks},
        {"isBuildExportTeleportAllowed", "()Z", (void*)Java_com_vdl_kong520_TpModule_isBuildExportTeleportAllowed},
        {"getBuildExportTeleportMode", "()I", (void*)Java_com_vdl_kong520_TpModule_getBuildExportTeleportMode},
        {"requestBuildExportNextRegion", "()Z", (void*)Java_com_vdl_kong520_TpModule_requestBuildExportNextRegion},
        {"getBuildExportTravelTarget", "()[I", (void*)Java_com_vdl_kong520_TpModule_getBuildExportTravelTarget},
        {"loadBuildProjection", "(Ljava/lang/String;Ljava/lang/String;IIIII)Z", (void*)Java_com_vdl_kong520_TpModule_loadBuildProjection},
        {"clearBuildProjection", "()V", (void*)Java_com_vdl_kong520_TpModule_clearBuildProjection},
        {"getBuildProjectionStatus", "()Ljava/lang/String;", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionStatus},
        {"getBuildProjectionMaterialSummaryPage", "(II)Ljava/lang/String;", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionMaterialSummaryPage},
        {"setBuildProjectionEnabled", "(Z)V", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionEnabled},
        {"setBuildProjectionOutlineEnabled", "(Z)Z", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionOutlineEnabled},
        {"setBuildProjectionAlpha", "(F)V", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionAlpha},
        {"setBuildProjectionRange", "(I)V", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionRange},
        {"setBuildProjectionLayerFilter", "(IZII)V", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionLayerFilter},
        {"setBuildProjectionPrinterEnabled", "(Z)V", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionPrinterEnabled},
        {"setBuildProjectionReachabilityPreviewEnabled", "(Z)V", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionReachabilityPreviewEnabled},
        {"isBuildProjectionPrinterEnabled", "()Z", (void*)Java_com_vdl_kong520_TpModule_isBuildProjectionPrinterEnabled},
        {"setBuildProjectionPrinterRate", "(I)V", (void*)Java_com_vdl_kong520_TpModule_setBuildProjectionPrinterRate},
        {"getBuildProjectionPrinterRate", "()I", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterRate},
        {"getBuildProjectionPrinterState", "()I", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterState},
        {"getBuildProjectionPrinterStatus", "()Ljava/lang/String;", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterStatus},
        {"getBuildProjectionPrinterTotalBlocks", "()J", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterTotalBlocks},
        {"getBuildProjectionPrinterPlacedBlocks", "()J", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterPlacedBlocks},
        {"getBuildProjectionPrinterSkippedBlocks", "()J", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionPrinterSkippedBlocks},
        {"getBuildProjectionTextureRequests", "()Ljava/lang/String;", (void*)Java_com_vdl_kong520_TpModule_getBuildProjectionTextureRequests},
        {"installBuildProjectionTexturePack", "([Ljava/lang/String;[SII[B)Z", (void*)Java_com_vdl_kong520_TpModule_installBuildProjectionTexturePack},
    };
    jclass clazz = env->FindClass("com/vdl/kong520/TpModule");
    if (!clazz) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("JNI registration could not resolve TpModule");
        return false;
    }
    const jint result = env->RegisterNatives(
        clazz, methods, static_cast<jint>(sizeof(methods) / sizeof(methods[0])));
    env->DeleteLocalRef(clazz);
    if (result != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("JNI registration failed for TpModule");
        return false;
    }
    return true;
}
