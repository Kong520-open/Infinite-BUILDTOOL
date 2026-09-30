#ifndef INFINITE_TEXTURE_BUILD_EXPORT_PATHS_H
#define INFINITE_TEXTURE_BUILD_EXPORT_PATHS_H

#include <string>
#include <vector>

namespace build_import {

struct BuildExportPathResolution {
    std::string output_stem;
    // The caller-provided path stays first so extension-bearing checkpoints
    // from the previous release remain discoverable before the normalized key.
    std::vector<std::string> checkpoint_keys;
};

bool resolveBuildExportPath(const std::string& requested_path,
                            BuildExportPathResolution* resolution);

std::string buildExportPublicationPath(const std::string& output_stem,
                                       bool use_bdx);

// Canonical lossless Infinitecz publication. The extension is owned by the
// exporter and is never taken from user input.
std::string buildExportNativePublicationPath(const std::string& output_stem);

// Called only after the new destination has been atomically published.
// Removes older managed formats for the same logical filename.
bool removeStaleBuildExportPublications(const std::string& output_stem,
                                        const std::string& published_path,
                                        std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_EXPORT_PATHS_H
