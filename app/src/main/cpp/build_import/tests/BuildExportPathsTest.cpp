#include "BuildExportPaths.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

using namespace build_import;

int main() {
    BuildExportPathResolution paths;
    assert(resolveBuildExportPath("C:\\exports\\castle.SCHEM", &paths));
    assert(paths.output_stem == "C:\\exports\\castle");
    assert(paths.checkpoint_keys.size() == 2);
    assert(paths.checkpoint_keys[0] == "C:\\exports\\castle.SCHEM");
    assert(paths.checkpoint_keys[1] == "C:\\exports\\castle");

    assert(resolveBuildExportPath("/exports/castle", &paths));
    assert(paths.output_stem == "/exports/castle");
    assert(paths.checkpoint_keys.size() == 1);
    assert(paths.checkpoint_keys.front() == paths.output_stem);
    assert(buildExportPublicationPath(paths.output_stem, false) ==
           "/exports/castle.schem");
    assert(buildExportPublicationPath(paths.output_stem, true) ==
           "/exports/castle.bdx");
    assert(resolveBuildExportPath("/exports/castle.infinity", &paths));
    assert(paths.output_stem == "/exports/castle");
    assert(buildExportNativePublicationPath(paths.output_stem) ==
           "/exports/castle.infinity");
    assert(resolveBuildExportPath("/exports/castle.INFINITY", &paths));
    assert(paths.output_stem == "/exports/castle");

    // Extension-bearing paths from the .IBuild release remain valid
    // checkpoint lookup keys, but are never used for a new publication.
    assert(resolveBuildExportPath("/exports/castle.IBuild", &paths));
    assert(paths.output_stem == "/exports/castle");
    assert(resolveBuildExportPath("/exports/castle.ibuild", &paths));
    assert(paths.output_stem == "/exports/castle");
    assert(resolveBuildExportPath("/exports/castle.IBUILD", &paths));
    assert(paths.output_stem == "/exports/castle");

    assert(!resolveBuildExportPath("/exports/castle.zip", &paths));
    assert(!resolveBuildExportPath("/exports/.schem", &paths));
    assert(!resolveBuildExportPath("/exports/", &paths));

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "infinitecz_export_paths_test";
    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
    assert(std::filesystem::create_directories(directory));
    const std::string stem = (directory / "building").string();
    const std::string published = stem + ".bdx";
    for (const std::string& path : {stem, stem + ".schem", stem + ".schematic",
                                    stem + ".infinity", stem + ".IBuild",
                                    stem + ".ibuild", published}) {
        std::ofstream output(path, std::ios::binary);
        assert(output.good());
        output << "test";
    }
    std::string cleanup_error;
    assert(removeStaleBuildExportPublications(stem, published, &cleanup_error));
    assert(cleanup_error.empty());
    assert(!std::filesystem::exists(stem));
    assert(!std::filesystem::exists(stem + ".schem"));
    assert(!std::filesystem::exists(stem + ".schematic"));
    assert(!std::filesystem::exists(stem + ".infinity"));
    assert(!std::filesystem::exists(stem + ".IBuild"));
    assert(!std::filesystem::exists(stem + ".ibuild"));
    assert(std::filesystem::is_regular_file(published));

    const std::string native_stem = (directory / "native").string();
    const std::string native_published = native_stem + ".infinity";
    {
        std::ofstream output(native_published, std::ios::binary);
        assert(output.good());
        output << "native";
    }
    assert(removeStaleBuildExportPublications(
        native_stem, native_published, &cleanup_error));
    assert(cleanup_error.empty());
    assert(std::filesystem::is_regular_file(native_published));

    const std::filesystem::path protected_directory = stem + ".schem";
    assert(std::filesystem::create_directory(protected_directory));
    assert(!removeStaleBuildExportPublications(stem, published, &cleanup_error));
    assert(!cleanup_error.empty());
    assert(std::filesystem::is_directory(protected_directory));
    assert(std::filesystem::is_regular_file(published));

    std::filesystem::remove_all(directory, remove_error);
    return 0;
}
