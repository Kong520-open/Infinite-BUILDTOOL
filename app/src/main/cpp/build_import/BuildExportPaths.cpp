#include "BuildExportPaths.h"

#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

bool hasSuffixCaseInsensitive(const std::string& path, std::string_view suffix) {
    if (path.size() < suffix.size()) return false;
    const size_t start = path.size() - suffix.size();
    for (size_t index = 0; index < suffix.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(path[start + index]);
        const unsigned char expected = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(character) != std::tolower(expected)) return false;
    }
    return true;
}

bool removeRegularFileIfPresent(const std::string& path, std::string* error) {
#if defined(_WIN32)
    struct _stat64 info {};
    if (_stat64(path.c_str(), &info) != 0) {
#else
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) {
#endif
        if (errno == ENOENT) return true;
        if (error) {
            *error = "cannot inspect stale export " + path + ": " +
                std::string(std::strerror(errno));
        }
        return false;
    }
#if defined(_WIN32)
    const bool removable = (info.st_mode & _S_IFMT) == _S_IFREG;
#else
    const bool removable = S_ISREG(info.st_mode) || S_ISLNK(info.st_mode);
#endif
    if (!removable) {
        if (error) *error = "stale export path is not a regular file: " + path;
        return false;
    }
    errno = 0;
    if (std::remove(path.c_str()) == 0 || errno == ENOENT) return true;
    if (error) {
        *error = "cannot remove stale export " + path + ": " +
            std::string(std::strerror(errno));
    }
    return false;
}

}  // namespace

bool resolveBuildExportPath(const std::string& requested_path,
                            BuildExportPathResolution* resolution) {
    if (!resolution || requested_path.empty()) return false;
    const size_t separator = requested_path.find_last_of("/\\");
    const size_t basename = separator == std::string::npos ? 0 : separator + 1;
    if (basename >= requested_path.size()) return false;

    std::string stem = requested_path;
    for (const std::string_view extension : {std::string_view(".schematic"),
                                              std::string_view(".schem"),
                                              std::string_view(".bdx"),
                                              std::string_view(".infinity"),
                                              std::string_view(".IBuild")}) {
        if (hasSuffixCaseInsensitive(requested_path, extension) &&
            requested_path.size() - extension.size() >= basename) {
            stem.resize(requested_path.size() - extension.size());
            break;
        }
    }
    if (stem.size() <= basename) return false;
    const std::string_view leaf(stem.data() + basename, stem.size() - basename);
    if (leaf.find('.') != std::string_view::npos) return false;

    try {
        BuildExportPathResolution result;
        result.output_stem = std::move(stem);
        result.checkpoint_keys.push_back(requested_path);
        if (result.output_stem != requested_path) {
            result.checkpoint_keys.push_back(result.output_stem);
        }
        *resolution = std::move(result);
    } catch (...) {
        return false;
    }
    return true;
}

std::string buildExportPublicationPath(const std::string& output_stem,
                                       bool use_bdx) {
    return output_stem + (use_bdx ? ".bdx" : ".schem");
}

std::string buildExportNativePublicationPath(const std::string& output_stem) {
    return output_stem + ".infinity";
}

bool removeStaleBuildExportPublications(const std::string& output_stem,
                                        const std::string& published_path,
                                        std::string* error) {
    const std::array<std::string, 7> candidates = {
        output_stem,
        output_stem + ".schem",
        output_stem + ".schematic",
        output_stem + ".bdx",
        output_stem + ".infinity",
        output_stem + ".IBuild",
        output_stem + ".ibuild",
    };
    for (const std::string& candidate : candidates) {
        if (candidate == published_path) continue;
        if (!removeRegularFileIfPresent(candidate, error)) return false;
    }
    if (error) error->clear();
    return true;
}

}  // namespace build_import
