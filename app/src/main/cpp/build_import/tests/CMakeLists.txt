cmake_minimum_required(VERSION 3.10.2)
project(build_import_parser_tests LANGUAGES C CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(BUILD_IMPORT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(build_import_test_json STATIC
    ${BUILD_IMPORT_DIRECTORY}/../Json/cJSON.c
)

# This project is also configured directly for host parser tests, rather than
# through the Android root CMakeLists.  Load the same vendored decoder target
# here when it has not already been provided by a parent build.
if(NOT TARGET build_import_brotli_decoder)
    add_subdirectory(
        "${BUILD_IMPORT_DIRECTORY}/../third_party/brotli"
        "${CMAKE_CURRENT_BINARY_DIR}/brotli_decoder"
    )
endif()

add_executable(
    PixelArtParserTest
    PixelArtParserTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/PixelArtParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
)
target_include_directories(PixelArtParserTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    BuildImportControllerTest
    BuildImportControllerTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BuildImportController.cpp
    ${BUILD_IMPORT_DIRECTORY}/BuildImportCheckpoint.cpp
)
target_include_directories(BuildImportControllerTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    BuildImportUndoStoreTest
    BuildImportUndoStoreTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BuildImportUndoStore.cpp
)
target_include_directories(BuildImportUndoStoreTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    CommandSpoolTest
    CommandSpoolTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/CommandSpool.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
)
target_include_directories(CommandSpoolTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(CommandSpoolTest PRIVATE Threads::Threads)

add_executable(
    CommandBlockSpoolTest
    CommandBlockSpoolTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/CommandBlockSpool.cpp
)
target_include_directories(CommandBlockSpoolTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(CommandBlockSpoolTest PRIVATE Threads::Threads)

add_executable(
    CommandMusicParserTest
    CommandMusicParserTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/CommandMusicParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
)
target_include_directories(CommandMusicParserTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(CommandMusicParserTest PRIVATE Threads::Threads)

add_executable(
    MidiCommandMusicParserTest
    MidiCommandMusicParserTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MidiCommandMusicParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/CommandMusicParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
)
target_include_directories(MidiCommandMusicParserTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MidiCommandMusicParserTest PRIVATE Threads::Threads)

add_executable(
    ExecuteCommandConverterTest
    ExecuteCommandConverterTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ExecuteCommandConverter.cpp
)
target_include_directories(ExecuteCommandConverterTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    BlockCommandFormatterTest
    BlockCommandFormatterTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockCommandFormatter.cpp
)
target_include_directories(BlockCommandFormatterTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    DeferredImportDataSpoolTest
    DeferredImportDataSpoolTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(DeferredImportDataSpoolTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(DeferredImportDataSpoolTest PRIVATE Threads::Threads)

add_executable(
    ContainerEntityCodecTest
    ContainerEntityCodecTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ContainerEntityCodec.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemRuntimeRegistry.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
    ${BUILD_IMPORT_DIRECTORY}/../Json/cJSON.c
)
target_include_directories(ContainerEntityCodecTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(ContainerEntityCodecTest PRIVATE Threads::Threads)

add_executable(
    SignEntityCodecTest
    SignEntityCodecTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/SignEntityCodec.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
    ${BUILD_IMPORT_DIRECTORY}/../Json/cJSON.c
)
target_include_directories(SignEntityCodecTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(SignEntityCodecTest PRIVATE Threads::Threads)

add_executable(
    SignBlockActorPacketSenderTest
    SignBlockActorPacketSenderTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/SignBlockActorPacketSender.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(SignBlockActorPacketSenderTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(SignBlockActorPacketSenderTest PRIVATE Threads::Threads)

add_executable(
    SignEditSessionMailboxTest
    SignEditSessionMailboxTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/SignEditSessionMailbox.cpp
)
target_include_directories(SignEditSessionMailboxTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(SignEditSessionMailboxTest PRIVATE Threads::Threads)

add_executable(
    ContainerCaptureMailboxTest
    ContainerCaptureMailboxTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ContainerCaptureMailbox.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestUiCloseBridge.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(ContainerCaptureMailboxTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(ContainerCaptureMailboxTest PRIVATE Threads::Threads)

add_executable(
    ItemExtraMapMetadataTest
    ItemExtraMapMetadataTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(ItemExtraMapMetadataTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
if(MSVC)
    target_compile_options(ItemExtraMapMetadataTest PRIVATE /W4 /WX /UNDEBUG)
else()
    target_compile_options(ItemExtraMapMetadataTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

add_executable(
    MapAnvilWireDiagnosticTest
    MapAnvilWireDiagnosticTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilWireDiagnostic.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(MapAnvilWireDiagnosticTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapAnvilWireDiagnosticTest PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(MapAnvilWireDiagnosticTest PRIVATE /W4 /WX /UNDEBUG)
else()
    target_compile_options(MapAnvilWireDiagnosticTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

add_executable(
    MapChestStoragePreflightTest
    MapChestStoragePreflightTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestStorageSender.cpp
    ${BUILD_IMPORT_DIRECTORY}/ContainerCaptureMailbox.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(MapChestStoragePreflightTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapChestStoragePreflightTest PRIVATE Threads::Threads)

add_executable(
    MapChestProductionPreflightTest
    MapChestProductionPreflightTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestProductionPreflight.cpp
)
target_include_directories(MapChestProductionPreflightTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapVisibleAnvilProductionPreflightTest
    MapVisibleAnvilProductionPreflightTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapVisibleAnvilProductionPreflight.cpp
)
target_include_directories(MapVisibleAnvilProductionPreflightTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
if(MSVC)
    target_compile_options(MapChestProductionPreflightTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapVisibleAnvilProductionPreflightTest PRIVATE /W4 /WX /UNDEBUG)
else()
    target_compile_options(MapChestProductionPreflightTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapVisibleAnvilProductionPreflightTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

add_executable(
    MapAnvilRenameSenderTest
    MapAnvilRenameSenderTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRenameSender.cpp
)
target_include_directories(MapAnvilRenameSenderTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
if(MSVC)
    target_compile_options(MapAnvilRenameSenderTest PRIVATE /W4 /WX /UNDEBUG)
else()
    target_compile_options(MapAnvilRenameSenderTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

add_executable(
    ItemRuntimeRegistryTest
    ItemRuntimeRegistryTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemRuntimeRegistry.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(ItemRuntimeRegistryTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(ItemRuntimeRegistryTest PRIVATE Threads::Threads)

add_executable(
    ProjectionPrinterInventoryMailboxTest
    ProjectionPrinterInventoryMailboxTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionPrinterInventoryMailbox.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemRuntimeRegistry.cpp
)
target_include_directories(ProjectionPrinterInventoryMailboxTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(ProjectionPrinterInventoryMailboxTest PRIVATE Threads::Threads)

add_executable(
    MapTextureObservationTest
    MapTextureObservationTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapTextureObservation.cpp
)
target_include_directories(MapTextureObservationTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapTextureObservationTest PRIVATE Threads::Threads)

add_executable(
    MapChestPlacementTest
    MapChestPlacementTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestPlacement.cpp
)
target_include_directories(MapChestPlacementTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapChestNativeSurveyTest
    MapChestNativeSurveyTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestNativeSurvey.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestPlacement.cpp
)
target_include_directories(MapChestNativeSurveyTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapChestWorldPlacementTest
    MapChestWorldPlacementTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestWorldPlacement.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestNativeSurvey.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestPlacement.cpp
)
target_include_directories(MapChestWorldPlacementTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapChestTransferJournalTest
    MapChestTransferJournalTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestTransferJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestStorageVerification.cpp
)
target_include_directories(MapChestTransferJournalTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapAnvilRenameJournalTest
    MapAnvilRenameJournalTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRenameJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestTransferJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestStorageVerification.cpp
)
target_include_directories(MapAnvilRenameJournalTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapStorageAnvilAdapterTest
    MapStorageAnvilAdapterTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapStorageAnvilAdapter.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRenameSender.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRenameJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestTransferJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestStorageVerification.cpp
)
target_include_directories(MapStorageAnvilAdapterTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapPairPlacementJournalTest
    MapPairPlacementJournalTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapPairPlacementJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestWorldPlacement.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestNativeSurvey.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestPlacement.cpp
)
target_include_directories(MapPairPlacementJournalTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapExtraChestPlacementJournalTest
    MapExtraChestPlacementJournalTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapExtraChestPlacementJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestWorldPlacement.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestNativeSurvey.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestPlacement.cpp
)
target_include_directories(MapExtraChestPlacementJournalTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapStorageProductionActionsTest
    MapStorageProductionActionsTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapStorageProductionActions.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapStoragePipelineDriver.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapStorageCoordinator.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapStorageAnvilAdapter.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRenameSender.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRenameJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestTransferJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestStorageVerification.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapPairPlacementJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapExtraChestPlacementJournal.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestWorldPlacement.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestNativeSurvey.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestPlacement.cpp
    ${BUILD_IMPORT_DIRECTORY}/ContainerCaptureMailbox.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(MapStorageProductionActionsTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapStorageProductionActionsTest PRIVATE Threads::Threads)

add_executable(
    MapChestStorageVerificationTest
    MapChestStorageVerificationTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestStorageVerification.cpp
)
target_include_directories(MapChestStorageVerificationTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapChestWindowSessionTest
    MapChestWindowSessionTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestWindowSession.cpp
)
target_include_directories(MapChestWindowSessionTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapStorageChestCaptureAdapterTest
    MapStorageChestCaptureAdapterTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapStorageChestCaptureAdapter.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestWindowSession.cpp
)
target_include_directories(MapStorageChestCaptureAdapterTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

add_executable(MapTileNamingTest MapTileNamingTest.cpp)
target_include_directories(MapTileNamingTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(MapBlankSupplyJournalTest
    MapBlankSupplyJournalTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapBlankSupplyJournal.cpp)
target_include_directories(MapBlankSupplyJournalTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapAnvilRecipeProbeStateTest
    MapAnvilRecipeProbeStateTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRecipeProbeState.cpp
)
target_include_directories(MapAnvilRecipeProbeStateTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapVisibleAnvilWindowSessionTest
    MapVisibleAnvilWindowSessionTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapVisibleAnvilWindowSession.cpp
)
target_include_directories(MapVisibleAnvilWindowSessionTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

add_executable(MapNativeAnvilManagerProbeRearmTest
               MapNativeAnvilManagerProbeRearmTest.cpp)
target_include_directories(MapNativeAnvilManagerProbeRearmTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

add_executable(
    MapAnvilCreatedOutputProbeTest
    MapAnvilCreatedOutputProbeTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilCreatedOutputProbe.cpp
)
target_include_directories(MapAnvilCreatedOutputProbeTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

add_executable(
    ProjectionPrinterInventoryClientSyncTest
    ProjectionPrinterInventoryClientSyncTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionPrinterInventoryClientSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(ProjectionPrinterInventoryClientSyncTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(ProjectionPrinterInventoryClientSyncTest PRIVATE Threads::Threads)

add_executable(
    MapAnvilClientSlotSyncTest
    MapAnvilClientSlotSyncTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilClientSlotSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionPrinterInventoryClientSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(MapAnvilClientSlotSyncTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapAnvilClientSlotSyncTest PRIVATE Threads::Threads)

add_executable(
    MapChestClientSlotSyncTest
    MapChestClientSlotSyncTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestClientSlotSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionPrinterInventoryClientSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(MapChestClientSlotSyncTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapChestClientSlotSyncTest PRIVATE Threads::Threads)

add_executable(
    MapAnvilClientSyncDeliveryTest
    MapAnvilClientSyncDeliveryTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilClientSyncDelivery.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilClientSlotSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapAnvilRenameSender.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionPrinterInventoryClientSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(MapAnvilClientSyncDeliveryTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapAnvilClientSyncDeliveryTest PRIVATE Threads::Threads)

add_executable(
    MapChestClientSyncDeliveryTest
    MapChestClientSyncDeliveryTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestClientSyncDelivery.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapChestClientSlotSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionPrinterInventoryClientSync.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
)
target_include_directories(MapChestClientSyncDeliveryTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(MapChestClientSyncDeliveryTest PRIVATE Threads::Threads)

add_executable(
    ProjectionPrinterInventoryMoveEvidenceTest
    ProjectionPrinterInventoryMoveEvidenceTest.cpp
)
target_include_directories(ProjectionPrinterInventoryMoveEvidenceTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

add_executable(
    ProjectionPrinterInventorySessionTest
    ProjectionPrinterInventorySessionTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionPrinterInventorySession.cpp
)
target_include_directories(ProjectionPrinterInventorySessionTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(ProjectionPrinterInventorySessionTest PRIVATE Threads::Threads)

add_executable(
    ProjectionBlockIdentityTest
    ProjectionBlockIdentityTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ProjectionBlockIdentity.cpp
)
target_include_directories(ProjectionBlockIdentityTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

add_executable(
    ContainerPacketPipelineTest
    ContainerPacketPipelineTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/ContainerCaptureMailbox.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemExtraMapMetadata.cpp
    ${BUILD_IMPORT_DIRECTORY}/ContainerEntityCodec.cpp
    ${BUILD_IMPORT_DIRECTORY}/ItemRuntimeRegistry.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
    ${BUILD_IMPORT_DIRECTORY}/../Json/cJSON.c
)
target_include_directories(ContainerPacketPipelineTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(ContainerPacketPipelineTest PRIVATE Threads::Threads)

add_executable(
    BuildExportRuntimeContainerCaptureTest
    BuildExportRuntimeContainerCaptureTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BuildExportRuntime.cpp
)
target_include_directories(BuildExportRuntimeContainerCaptureTest PRIVATE
    ${BUILD_IMPORT_DIRECTORY}
    ${CMAKE_CURRENT_LIST_DIR}/host_stubs
)
target_compile_definitions(BuildExportRuntimeContainerCaptureTest PRIVATE
    BUILD_IMPORT_RUNTIME_TESTS=1
)
target_link_libraries(BuildExportRuntimeContainerCaptureTest PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(BuildExportRuntimeContainerCaptureTest PRIVATE /Gy)
    target_link_options(BuildExportRuntimeContainerCaptureTest PRIVATE /OPT:REF)
else()
    target_compile_options(BuildExportRuntimeContainerCaptureTest PRIVATE
        -ffunction-sections -fdata-sections
    )
    if(NOT APPLE)
        target_link_options(BuildExportRuntimeContainerCaptureTest PRIVATE
            -Wl,--gc-sections
        )
    endif()
endif()

add_executable(
    BlockMapperTest
    BlockMapperTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
)
target_include_directories(BlockMapperTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    SchematicParserTest
    SchematicParserTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/SchematicParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(SchematicParserTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(SchematicParserTest PRIVATE Threads::Threads)

add_executable(
    SchematicWriterTest
    SchematicWriterTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/SchematicWriter.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
)
target_include_directories(SchematicWriterTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    BuildExportCheckpointTest
    BuildExportCheckpointTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BuildExportCheckpoint.cpp
)
target_include_directories(BuildExportCheckpointTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    BuildExportPathsTest
    BuildExportPathsTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BuildExportPaths.cpp
)
target_include_directories(BuildExportPathsTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    LitematicParserTest
    LitematicParserTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/LitematicParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
    ${BUILD_IMPORT_DIRECTORY}/CommandSpool.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(LitematicParserTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(LitematicParserTest PRIVATE Threads::Threads)

add_executable(
    BdxParserTest
    BdxParserTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BdxParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(BdxParserTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(BdxParserTest PRIVATE Threads::Threads build_import_brotli_decoder)

add_executable(
    BdxWriterTest
    BdxWriterTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/BdxWriter.cpp
    ${BUILD_IMPORT_DIRECTORY}/BdxParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(BdxWriterTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(BdxWriterTest PRIVATE Threads::Threads
                                             build_import_brotli_decoder
                                             build_import_brotli_encoder)

add_executable(
    InfinityCompressionCodecTest
    InfinityCompressionCodecTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/InfinityCompressionCodec.cpp
)
target_include_directories(InfinityCompressionCodecTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(InfinityCompressionCodecTest PRIVATE build_import_brotli_decoder
                                                            build_import_brotli_encoder)

# Android uses the same vendored OpenSSL archive as the app. Native host tests
# are enabled only when the host has a discoverable OpenSSL development package.
set(INFINITY_CRYPTO_TEST_AVAILABLE OFF)
if(ANDROID)
    set(INFINITY_CRYPTO_ARCHIVE
        "${BUILD_IMPORT_DIRECTORY}/../libs/${ANDROID_ABI}/libcrypto.a")
    if(EXISTS "${INFINITY_CRYPTO_ARCHIVE}")
        if(NOT TARGET build_import_test_crypto)
            add_library(build_import_test_crypto STATIC IMPORTED)
            set_target_properties(build_import_test_crypto PROPERTIES
                IMPORTED_LOCATION "${INFINITY_CRYPTO_ARCHIVE}")
        endif()
        set(INFINITY_CRYPTO_TEST_AVAILABLE ON)
    endif()
else()
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND AND TARGET OpenSSL::Crypto)
        set(INFINITY_CRYPTO_TEST_AVAILABLE ON)
    endif()
endif()

if(INFINITY_CRYPTO_TEST_AVAILABLE)
    add_executable(
        InfiniteczBuildTest
        InfiniteczBuildTest.cpp
        ${BUILD_IMPORT_DIRECTORY}/InfiniteczBuildWriter.cpp
        ${BUILD_IMPORT_DIRECTORY}/InfiniteczBuildParser.cpp
        ${BUILD_IMPORT_DIRECTORY}/InfinityCompressionCodec.cpp
        ${BUILD_IMPORT_DIRECTORY}/ContainerEntityCodec.cpp
        ${BUILD_IMPORT_DIRECTORY}/SignEntityCodec.cpp
        ${BUILD_IMPORT_DIRECTORY}/ItemRuntimeRegistry.cpp
        ${BUILD_IMPORT_DIRECTORY}/InfinityCryptoCodec.cpp
        ${BUILD_IMPORT_DIRECTORY}/InfinityFormatKey.cpp
        ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
        ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
        ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
        ${BUILD_IMPORT_DIRECTORY}/../Json/cJSON.c
    )
    target_include_directories(InfiniteczBuildTest PRIVATE
        ${BUILD_IMPORT_DIRECTORY}
        "${BUILD_IMPORT_DIRECTORY}/../openssl_ndk"
    )
    target_link_libraries(InfiniteczBuildTest PRIVATE Threads::Threads
                                                     build_import_brotli_decoder
                                                     build_import_brotli_encoder)

    add_executable(
        InfinityCryptoCodecTest
        InfinityCryptoCodecTest.cpp
        ${BUILD_IMPORT_DIRECTORY}/InfinityCryptoCodec.cpp
    )
    target_include_directories(InfinityCryptoCodecTest PRIVATE
        ${BUILD_IMPORT_DIRECTORY}
        "${BUILD_IMPORT_DIRECTORY}/../openssl_ndk"
    )
    target_link_libraries(InfinityCryptoCodecTest PRIVATE Threads::Threads)
    if(ANDROID)
        target_link_libraries(InfiniteczBuildTest PRIVATE build_import_test_crypto)
        target_link_libraries(InfinityCryptoCodecTest PRIVATE build_import_test_crypto)
    else()
        target_link_libraries(InfiniteczBuildTest PRIVATE OpenSSL::Crypto)
        target_link_libraries(InfinityCryptoCodecTest PRIVATE OpenSSL::Crypto)
    endif()
endif()

add_executable(
    McworldParserTest
    McworldParserTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/McworldParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
)
target_include_directories(McworldParserTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_link_libraries(McworldParserTest PRIVATE Threads::Threads)

add_executable(
    PyRpcAckDecoderTest
    PyRpcAckDecoderTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/PyRpcAckDecoder.cpp
)
target_include_directories(PyRpcAckDecoderTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    PyRpcPointerBridgeTest
    PyRpcPointerBridgeTest.cpp
)
target_include_directories(PyRpcPointerBridgeTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    BuildImportThroughputTest
    BuildImportThroughputTest.cpp
)
target_include_directories(BuildImportThroughputTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    CommandFeedbackGateTest
    CommandFeedbackGateTest.cpp
)
target_include_directories(CommandFeedbackGateTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    LevelChunkEvidenceCacheTest
    LevelChunkEvidenceCacheTest.cpp
)
target_include_directories(LevelChunkEvidenceCacheTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    VerificationRepairTrackerTest
    VerificationRepairTrackerTest.cpp
)
target_include_directories(VerificationRepairTrackerTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(
    BuildProjectionPartitionTest
    BuildProjectionPartitionTest.cpp
    BuildProjectionRendererStub.cpp
    ${BUILD_IMPORT_DIRECTORY}/BuildProjectionRuntime.cpp
    ${BUILD_IMPORT_DIRECTORY}/BlockMapper.cpp
    ${BUILD_IMPORT_DIRECTORY}/ChunkSpoolWriter.cpp
    ${BUILD_IMPORT_DIRECTORY}/CommandSpool.cpp
    ${BUILD_IMPORT_DIRECTORY}/BdxParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/LitematicParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/McworldParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/PixelArtParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/SchematicParser.cpp
    ${BUILD_IMPORT_DIRECTORY}/DeferredImportDataSpool.cpp
)
target_include_directories(BuildProjectionPartitionTest PRIVATE ${BUILD_IMPORT_DIRECTORY})
target_compile_definitions(BuildProjectionPartitionTest PRIVATE BUILD_IMPORT_PROJECTION_TESTS=1)
target_link_libraries(BuildProjectionPartitionTest PRIVATE Threads::Threads build_import_brotli_decoder)

add_executable(
    BuildProjectionOutlineTest
    BuildProjectionOutlineTest.cpp
)
target_include_directories(BuildProjectionOutlineTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(MapNativeAnvilCraftPacketTest
    MapNativeAnvilCraftPacketTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapNativeAnvilCraftPacket.cpp)
target_include_directories(MapNativeAnvilCraftPacketTest PRIVATE ${BUILD_IMPORT_DIRECTORY})

add_executable(MapNativeAnvilResultActionGateTest
    MapNativeAnvilResultActionGateTest.cpp
    ${BUILD_IMPORT_DIRECTORY}/MapNativeAnvilResultActionBridge.cpp)
target_include_directories(MapNativeAnvilResultActionGateTest PRIVATE
                           ${BUILD_IMPORT_DIRECTORY})

set(BUILD_IMPORT_TEST_TARGETS
    PixelArtParserTest
    BuildImportControllerTest
    BuildImportUndoStoreTest
    CommandSpoolTest
    CommandBlockSpoolTest
    CommandMusicParserTest
    MidiCommandMusicParserTest
    ExecuteCommandConverterTest
    BlockCommandFormatterTest
    DeferredImportDataSpoolTest
    ContainerEntityCodecTest
    SignEntityCodecTest
    SignBlockActorPacketSenderTest
    SignEditSessionMailboxTest
    ContainerCaptureMailboxTest
    ItemExtraMapMetadataTest
    MapChestStoragePreflightTest
    MapChestProductionPreflightTest
    MapVisibleAnvilProductionPreflightTest
    MapAnvilRenameSenderTest
    ItemRuntimeRegistryTest
    ProjectionPrinterInventoryMailboxTest
    MapTextureObservationTest
    MapChestPlacementTest
    MapChestNativeSurveyTest
    MapChestWorldPlacementTest
    MapChestTransferJournalTest
    MapAnvilRenameJournalTest
    MapStorageAnvilAdapterTest
    MapPairPlacementJournalTest
    MapExtraChestPlacementJournalTest
    MapStorageProductionActionsTest
    MapChestStorageVerificationTest
    MapChestWindowSessionTest
    MapStorageChestCaptureAdapterTest
    MapTileNamingTest
    MapBlankSupplyJournalTest
    MapAnvilRecipeProbeStateTest
    MapAnvilCreatedOutputProbeTest
    ProjectionPrinterInventoryClientSyncTest
    MapAnvilClientSlotSyncTest
    MapChestClientSlotSyncTest
    MapAnvilClientSyncDeliveryTest
    MapChestClientSyncDeliveryTest
    ProjectionPrinterInventorySessionTest
    ProjectionPrinterInventoryMoveEvidenceTest
    ProjectionBlockIdentityTest
    ContainerPacketPipelineTest
    BuildExportRuntimeContainerCaptureTest
    BlockMapperTest
    SchematicParserTest
    SchematicWriterTest
    BuildExportCheckpointTest
    BuildExportPathsTest
    LitematicParserTest
    BdxParserTest
    BdxWriterTest
    InfinityCompressionCodecTest
    McworldParserTest
    PyRpcAckDecoderTest
    PyRpcPointerBridgeTest
    BuildImportThroughputTest
    CommandFeedbackGateTest
    LevelChunkEvidenceCacheTest
    VerificationRepairTrackerTest
    BuildProjectionPartitionTest
    BuildProjectionOutlineTest
    MapNativeAnvilCraftPacketTest
    MapNativeAnvilResultActionGateTest
)

# BlockMapper now consumes the exporter's native state JSON snapshot. Keep its
# single parser dependency attached to every test that compiles the mapper.
target_link_libraries(CommandSpoolTest PRIVATE build_import_test_json)
target_link_libraries(BlockMapperTest PRIVATE build_import_test_json)
target_link_libraries(SchematicParserTest PRIVATE build_import_test_json)
target_link_libraries(SchematicWriterTest PRIVATE build_import_test_json)
target_link_libraries(LitematicParserTest PRIVATE build_import_test_json)
target_link_libraries(BdxParserTest PRIVATE build_import_test_json)
target_link_libraries(BdxWriterTest PRIVATE build_import_test_json)
target_link_libraries(McworldParserTest PRIVATE build_import_test_json)
target_link_libraries(BuildProjectionPartitionTest PRIVATE build_import_test_json)
if(INFINITY_CRYPTO_TEST_AVAILABLE)
    list(APPEND BUILD_IMPORT_TEST_TARGETS InfiniteczBuildTest InfinityCryptoCodecTest)
endif()

# CTest does not consistently preserve the caller's MinGW runtime search path
# between test processes on Windows. Keep the required DLLs beside each host
# executable so a full test run is deterministic and does not depend on PATH.
if(MINGW)
    get_filename_component(MINGW_RUNTIME_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB MINGW_RUNTIME_LIBRARIES
        "${MINGW_RUNTIME_DIRECTORY}/libgcc_s_*.dll"
        "${MINGW_RUNTIME_DIRECTORY}/libstdc++-6.dll"
        "${MINGW_RUNTIME_DIRECTORY}/libwinpthread-1.dll"
        "${MINGW_RUNTIME_DIRECTORY}/zlib1.dll"
    )
    foreach(TEST_TARGET IN LISTS BUILD_IMPORT_TEST_TARGETS)
        foreach(RUNTIME_LIBRARY IN LISTS MINGW_RUNTIME_LIBRARIES)
            add_custom_command(TARGET ${TEST_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${RUNTIME_LIBRARY}" "$<TARGET_FILE_DIR:${TEST_TARGET}>"
            )
        endforeach()
    endforeach()
endif()

if(MSVC)
    target_compile_options(PixelArtParserTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildImportControllerTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildImportUndoStoreTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(CommandSpoolTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(CommandBlockSpoolTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(CommandMusicParserTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MidiCommandMusicParserTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ExecuteCommandConverterTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BlockCommandFormatterTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(DeferredImportDataSpoolTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ContainerEntityCodecTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(SignEntityCodecTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(SignBlockActorPacketSenderTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(SignEditSessionMailboxTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ContainerCaptureMailboxTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestStoragePreflightTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ItemRuntimeRegistryTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ProjectionPrinterInventoryMailboxTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapTextureObservationTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestPlacementTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestNativeSurveyTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestWorldPlacementTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestTransferJournalTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapAnvilRenameJournalTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapPairPlacementJournalTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapExtraChestPlacementJournalTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapStorageProductionActionsTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestStorageVerificationTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestWindowSessionTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapStorageChestCaptureAdapterTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapTileNamingTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapBlankSupplyJournalTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapAnvilRecipeProbeStateTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapAnvilCreatedOutputProbeTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ProjectionPrinterInventoryClientSyncTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapAnvilClientSlotSyncTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestClientSlotSyncTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapAnvilClientSyncDeliveryTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(MapChestClientSyncDeliveryTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ProjectionPrinterInventoryMoveEvidenceTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ProjectionPrinterInventorySessionTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ProjectionBlockIdentityTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(ContainerPacketPipelineTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildExportRuntimeContainerCaptureTest PRIVATE
        /W4 /WX /UNDEBUG)
    target_compile_options(BlockMapperTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(SchematicParserTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(SchematicWriterTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildExportCheckpointTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildExportPathsTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(LitematicParserTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BdxParserTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BdxWriterTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(InfinityCompressionCodecTest PRIVATE /W4 /WX /UNDEBUG)
    if(INFINITY_CRYPTO_TEST_AVAILABLE)
        target_compile_options(InfiniteczBuildTest PRIVATE /W4 /WX /UNDEBUG)
        target_compile_options(InfinityCryptoCodecTest PRIVATE /W4 /WX /UNDEBUG)
    endif()
    target_compile_options(McworldParserTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(PyRpcAckDecoderTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(PyRpcPointerBridgeTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildImportThroughputTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(CommandFeedbackGateTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(LevelChunkEvidenceCacheTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(VerificationRepairTrackerTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildProjectionPartitionTest PRIVATE /W4 /WX /UNDEBUG)
    target_compile_options(BuildProjectionOutlineTest PRIVATE /W4 /WX /UNDEBUG)
else()
    target_compile_options(PixelArtParserTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(BuildImportControllerTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                               -UNDEBUG)
    target_compile_options(BuildImportUndoStoreTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                              -UNDEBUG)
    target_compile_options(CommandSpoolTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(CommandBlockSpoolTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(CommandMusicParserTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MidiCommandMusicParserTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                              -UNDEBUG)
    target_compile_options(ExecuteCommandConverterTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                               -UNDEBUG)
    target_compile_options(BlockCommandFormatterTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                              -UNDEBUG)
    target_compile_options(DeferredImportDataSpoolTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ContainerEntityCodecTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(SignEntityCodecTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(SignBlockActorPacketSenderTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(SignEditSessionMailboxTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ContainerCaptureMailboxTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                               -UNDEBUG)
    target_compile_options(MapChestStoragePreflightTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ItemRuntimeRegistryTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                           -UNDEBUG)
    target_compile_options(ProjectionPrinterInventoryMailboxTest PRIVATE
                            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapTextureObservationTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestPlacementTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestNativeSurveyTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestWorldPlacementTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestTransferJournalTest PRIVATE
                            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapAnvilRenameJournalTest PRIVATE
                            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapPairPlacementJournalTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapExtraChestPlacementJournalTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapStorageProductionActionsTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestStorageVerificationTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestWindowSessionTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapStorageChestCaptureAdapterTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapTileNamingTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapBlankSupplyJournalTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapAnvilRecipeProbeStateTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapAnvilCreatedOutputProbeTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ProjectionPrinterInventoryClientSyncTest PRIVATE
                            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapAnvilClientSlotSyncTest PRIVATE
                            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestClientSlotSyncTest PRIVATE
                            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapAnvilClientSyncDeliveryTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(MapChestClientSyncDeliveryTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ProjectionPrinterInventoryMoveEvidenceTest PRIVATE
                            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ProjectionPrinterInventorySessionTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ProjectionBlockIdentityTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(ContainerPacketPipelineTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                              -UNDEBUG)
    target_compile_options(BuildExportRuntimeContainerCaptureTest PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(BlockMapperTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(SchematicParserTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(SchematicWriterTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(BuildExportCheckpointTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                               -UNDEBUG)
    target_compile_options(BuildExportPathsTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(LitematicParserTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(BdxParserTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(BdxWriterTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(InfinityCompressionCodecTest PRIVATE
                           -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    if(INFINITY_CRYPTO_TEST_AVAILABLE)
        target_compile_options(InfiniteczBuildTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                        -UNDEBUG)
        target_compile_options(InfinityCryptoCodecTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                               -UNDEBUG)
    endif()
    target_compile_options(McworldParserTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(PyRpcAckDecoderTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(PyRpcPointerBridgeTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(BuildImportThroughputTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                               -UNDEBUG)
    target_compile_options(CommandFeedbackGateTest PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_compile_options(LevelChunkEvidenceCacheTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                                    -UNDEBUG)
    target_compile_options(VerificationRepairTrackerTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                             -UNDEBUG)
    target_compile_options(BuildProjectionPartitionTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                                  -UNDEBUG)
    target_compile_options(BuildProjectionOutlineTest PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                                -UNDEBUG)
endif()

if(ANDROID)
    find_library(ZLIB_LIBRARY z)
    if(NOT ZLIB_LIBRARY)
        message(FATAL_ERROR "Android zlib library was not found")
    endif()
    target_link_libraries(PixelArtParserTest PRIVATE ${ZLIB_LIBRARY})
    target_link_libraries(SchematicParserTest PRIVATE ${ZLIB_LIBRARY})
    target_link_libraries(SchematicWriterTest PRIVATE ${ZLIB_LIBRARY})
    target_link_libraries(LitematicParserTest PRIVATE ${ZLIB_LIBRARY})
    target_link_libraries(McworldParserTest PRIVATE ${ZLIB_LIBRARY})
    target_link_libraries(BuildProjectionPartitionTest PRIVATE ${ZLIB_LIBRARY})
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(PixelArtParserTest PRIVATE ZLIB::ZLIB)
    target_link_libraries(SchematicParserTest PRIVATE ZLIB::ZLIB)
    target_link_libraries(SchematicWriterTest PRIVATE ZLIB::ZLIB)
    target_link_libraries(LitematicParserTest PRIVATE ZLIB::ZLIB)
    target_link_libraries(McworldParserTest PRIVATE ZLIB::ZLIB)
    target_link_libraries(BuildProjectionPartitionTest PRIVATE ZLIB::ZLIB)
endif()

enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)
add_test(
    NAME NativeWorldAccessSafetyTest
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_FILE=${BUILD_IMPORT_DIRECTORY}/NativeWorldAccess.cpp"
        -P ${CMAKE_CURRENT_LIST_DIR}/NativeWorldAccessSafetyTest.cmake
)
add_test(
    NAME ContainerRestoreFlowSafetyTest
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_FILE=${BUILD_IMPORT_DIRECTORY}/BuildImportRuntime.cpp"
        -P ${CMAKE_CURRENT_LIST_DIR}/ContainerRestoreFlowSafetyTest.cmake
)
add_test(
    NAME SignRestoreFlowSafetyTest
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_FILE=${BUILD_IMPORT_DIRECTORY}/BuildImportRuntime.cpp"
        -P ${CMAKE_CURRENT_LIST_DIR}/SignRestoreFlowSafetyTest.cmake
)
add_test(
    NAME MapBlankSupplyFlowSafetyTest
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_FILE=${BUILD_IMPORT_DIRECTORY}/BuildImportRuntime.cpp"
        -P ${CMAKE_CURRENT_LIST_DIR}/MapBlankSupplyFlowSafetyTest.cmake
)
add_test(NAME PixelArtParserTest COMMAND PixelArtParserTest)
add_test(NAME BuildImportControllerTest COMMAND BuildImportControllerTest)
add_test(NAME BuildImportUndoStoreTest COMMAND BuildImportUndoStoreTest)
add_test(NAME CommandSpoolTest COMMAND CommandSpoolTest)
add_test(NAME CommandBlockSpoolTest COMMAND CommandBlockSpoolTest)
add_test(NAME CommandMusicParserTest COMMAND CommandMusicParserTest)
add_test(NAME MidiCommandMusicParserTest COMMAND MidiCommandMusicParserTest)
add_test(NAME ExecuteCommandConverterTest COMMAND ExecuteCommandConverterTest)
add_test(NAME BlockCommandFormatterTest COMMAND BlockCommandFormatterTest)
add_test(NAME DeferredImportDataSpoolTest COMMAND DeferredImportDataSpoolTest)
add_test(NAME ContainerEntityCodecTest COMMAND ContainerEntityCodecTest)
add_test(NAME SignEntityCodecTest COMMAND SignEntityCodecTest)
add_test(NAME SignBlockActorPacketSenderTest COMMAND SignBlockActorPacketSenderTest)
add_test(NAME SignEditSessionMailboxTest COMMAND SignEditSessionMailboxTest)
add_test(NAME ContainerCaptureMailboxTest COMMAND ContainerCaptureMailboxTest)
add_test(NAME ItemExtraMapMetadataTest COMMAND ItemExtraMapMetadataTest)
add_test(NAME MapAnvilWireDiagnosticTest COMMAND MapAnvilWireDiagnosticTest)
add_test(NAME MapChestStoragePreflightTest COMMAND MapChestStoragePreflightTest)
add_test(NAME MapChestProductionPreflightTest COMMAND MapChestProductionPreflightTest)
add_test(NAME MapVisibleAnvilProductionPreflightTest COMMAND MapVisibleAnvilProductionPreflightTest)
add_test(NAME MapAnvilRenameSenderTest COMMAND MapAnvilRenameSenderTest)
add_test(NAME ItemRuntimeRegistryTest COMMAND ItemRuntimeRegistryTest)
add_test(NAME ProjectionPrinterInventoryMailboxTest COMMAND ProjectionPrinterInventoryMailboxTest)
add_test(NAME MapTextureObservationTest COMMAND MapTextureObservationTest)
add_test(NAME MapChestPlacementTest COMMAND MapChestPlacementTest)
add_test(NAME MapChestNativeSurveyTest COMMAND MapChestNativeSurveyTest)
add_test(NAME MapChestWorldPlacementTest COMMAND MapChestWorldPlacementTest)
add_test(NAME MapChestTransferJournalTest COMMAND MapChestTransferJournalTest)
add_test(NAME MapAnvilRenameJournalTest COMMAND MapAnvilRenameJournalTest)
add_test(NAME MapStorageAnvilAdapterTest COMMAND MapStorageAnvilAdapterTest)
add_test(NAME MapPairPlacementJournalTest COMMAND MapPairPlacementJournalTest)
add_test(NAME MapExtraChestPlacementJournalTest COMMAND MapExtraChestPlacementJournalTest)
add_test(NAME MapStorageProductionActionsTest
         COMMAND MapStorageProductionActionsTest "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME MapChestStorageVerificationTest COMMAND MapChestStorageVerificationTest)
add_test(NAME MapChestWindowSessionTest COMMAND MapChestWindowSessionTest)
add_test(NAME MapStorageChestCaptureAdapterTest COMMAND MapStorageChestCaptureAdapterTest)
add_test(NAME MapTileNamingTest COMMAND MapTileNamingTest)
add_test(NAME MapBlankSupplyJournalTest COMMAND MapBlankSupplyJournalTest)
add_test(NAME MapAnvilRecipeProbeStateTest COMMAND MapAnvilRecipeProbeStateTest)
add_test(NAME MapVisibleAnvilWindowSessionTest COMMAND MapVisibleAnvilWindowSessionTest)
add_test(NAME MapNativeAnvilManagerProbeRearmTest COMMAND MapNativeAnvilManagerProbeRearmTest)
add_test(NAME MapAnvilCreatedOutputProbeTest COMMAND MapAnvilCreatedOutputProbeTest)
add_test(NAME ProjectionPrinterInventoryClientSyncTest COMMAND ProjectionPrinterInventoryClientSyncTest)
add_test(NAME MapAnvilClientSlotSyncTest COMMAND MapAnvilClientSlotSyncTest)
add_test(NAME MapChestClientSlotSyncTest COMMAND MapChestClientSlotSyncTest)
add_test(NAME MapAnvilClientSyncDeliveryTest COMMAND MapAnvilClientSyncDeliveryTest)
add_test(NAME MapChestClientSyncDeliveryTest COMMAND MapChestClientSyncDeliveryTest)
add_test(NAME ProjectionPrinterInventoryMoveEvidenceTest COMMAND ProjectionPrinterInventoryMoveEvidenceTest)
add_test(NAME ProjectionPrinterInventorySessionTest COMMAND ProjectionPrinterInventorySessionTest)
add_test(NAME ProjectionBlockIdentityTest COMMAND ProjectionBlockIdentityTest)
add_test(NAME ContainerPacketPipelineTest COMMAND ContainerPacketPipelineTest)
add_test(NAME BuildExportRuntimeContainerCaptureTest COMMAND BuildExportRuntimeContainerCaptureTest)
add_test(NAME BlockMapperTest COMMAND BlockMapperTest)
add_test(NAME SchematicParserTest COMMAND SchematicParserTest)
add_test(NAME SchematicWriterTest COMMAND SchematicWriterTest)
add_test(NAME BuildExportCheckpointTest COMMAND BuildExportCheckpointTest)
add_test(NAME BuildExportPathsTest COMMAND BuildExportPathsTest)
add_test(NAME LitematicParserTest COMMAND LitematicParserTest)
add_test(NAME BdxParserTest COMMAND BdxParserTest)
add_test(NAME BdxWriterTest COMMAND BdxWriterTest)
add_test(NAME InfinityCompressionCodecTest COMMAND InfinityCompressionCodecTest)
if(INFINITY_CRYPTO_TEST_AVAILABLE)
    add_test(NAME InfiniteczBuildTest COMMAND InfiniteczBuildTest)
    add_test(NAME InfinityCryptoCodecTest COMMAND InfinityCryptoCodecTest)
endif()
add_test(NAME McworldParserTest COMMAND McworldParserTest)
add_test(NAME PyRpcAckDecoderTest COMMAND PyRpcAckDecoderTest)
add_test(NAME PyRpcPointerBridgeTest COMMAND PyRpcPointerBridgeTest)
add_test(NAME BuildImportThroughputTest COMMAND BuildImportThroughputTest)
add_test(NAME CommandFeedbackGateTest COMMAND CommandFeedbackGateTest)
add_test(NAME LevelChunkEvidenceCacheTest COMMAND LevelChunkEvidenceCacheTest)
add_test(NAME VerificationRepairTrackerTest COMMAND VerificationRepairTrackerTest)
add_test(NAME BuildProjectionPartitionTest COMMAND BuildProjectionPartitionTest)
add_test(NAME BuildProjectionOutlineTest COMMAND BuildProjectionOutlineTest)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME PyRpcEnvelopeBridgeTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/PyRpcEnvelopeBridgeTest.py
    )
endif()
