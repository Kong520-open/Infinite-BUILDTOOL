# Decoder-only Brotli build used by streamed structure import parsers.
#
# Keep this self-contained instead of importing Brotli's full upstream build
# system: the vendor drop intentionally contains only the decoder sources, and
# this target is consumed both by the Android shared library and the standalone
# host parser-test CMake project.
add_library(build_import_brotli_decoder STATIC
    c/common/constants.c
    c/common/context.c
    c/common/dictionary.c
    c/common/platform.c
    c/common/shared_dictionary.c
    c/common/transform.c
    c/dec/bit_reader.c
    c/dec/decode.c
    c/dec/huffman.c
    c/dec/state.c
)

# The BDX export path needs to create the same Brotli command stream that the
# parser consumes. Keep the encoder separate from the decoder target so import
#-only builds do not pull the substantially larger compression implementation.
file(GLOB BUILD_IMPORT_BROTLI_ENCODER_SOURCES CONFIGURE_DEPENDS
    c/enc/*.c
)
add_library(build_import_brotli_encoder STATIC
    c/common/constants.c
    c/common/context.c
    c/common/dictionary.c
    c/common/platform.c
    c/common/shared_dictionary.c
    c/common/transform.c
    ${BUILD_IMPORT_BROTLI_ENCODER_SOURCES}
)

set_target_properties(build_import_brotli_encoder PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    POSITION_INDEPENDENT_CODE ON
)

set_target_properties(build_import_brotli_decoder PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    POSITION_INDEPENDENT_CODE ON
)

# Consumers include only the stable public decoder header.  Decoder internals
# remain private to avoid exposing a second, conflicting include surface.
target_include_directories(build_import_brotli_decoder
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/c/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/c/common
        ${CMAKE_CURRENT_SOURCE_DIR}/c/dec
)

target_include_directories(build_import_brotli_encoder
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/c/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/c/common
        ${CMAKE_CURRENT_SOURCE_DIR}/c/enc
)
