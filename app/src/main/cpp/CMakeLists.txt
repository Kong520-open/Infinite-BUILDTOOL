cmake_minimum_required(VERSION 3.10.2)
project("weibosdkcore" LANGUAGES C CXX ASM)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags to reduce .so size and fix Dobby relocation overflow
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections -Os -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -Os -fPIC")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -fPIC")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--as-needed -Wl,-z,max-page-size=4096 -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,--exclude-libs,ALL")

# 1. Force global static library generation
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Forcibly build static libraries" FORCE)

# 2. Dobby specific switch
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)

# 3. Symbol visibility configuration
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_ASM_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# 4. Load Dobby (with optimizations applied above)
add_subdirectory(Dobby)

# Decoder-only Brotli library for streamed BDX structure parsing.  It is kept
# separate from the game library so the same target can be reused by host tests.
add_subdirectory(third_party/brotli)

# Import precompiled OpenSSL static libraries
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libcrypto.a)

# OpenSSL include directory
set(OPENSSL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/openssl_ndk/include")
file(GLOB
        SRC
        ${CMAKE_SOURCE_DIR}/openssl_ndk/openssl/*.h
        #${CMAKE_SOURCE_DIR}/openssl_ndk/crypto/*.h
)

# 5. Generate target shared library
add_library(
        weibosdkcore
        SHARED
        native-lib.cpp
        Json/cJSON.c
         build_import/BlockMapper.cpp
         build_import/BlockCommandFormatter.cpp
         build_import/ExecuteCommandConverter.cpp
           build_import/ChunkSpoolWriter.cpp
           build_import/CommandBlockSpool.cpp
         build_import/DeferredImportDataSpool.cpp
         build_import/ContainerEntityCodec.cpp
         build_import/SignEntityCodec.cpp
         build_import/ContainerCaptureMailbox.cpp
         build_import/ItemExtraMapMetadata.cpp
         build_import/ProjectionPrinterInventoryMailbox.cpp
         build_import/MapTextureObservation.cpp
         build_import/MapManualOutboundTrace.cpp
         build_import/MapAnvilRecipeProbe.cpp
         build_import/MapAnvilRecipeProbeState.cpp
         build_import/MapAnvilCreatedOutputProbe.cpp
         build_import/MapAnvilNativeActionTrace.cpp
         build_import/MapNativeAnvilManagerProbe.cpp
          build_import/MapAnvilUiCloseBridge.cpp
          build_import/MapChestUiCloseBridge.cpp
         build_import/MapNativeAnvilNameBridge.cpp
         build_import/MapNativeAnvilResultGroupProbe.cpp
         build_import/MapNativeAnvilCraftPacket.cpp
         build_import/MapNativeAnvilResultActionBridge.cpp
         build_import/MapAnvilRenameSender.cpp
         build_import/MapAnvilRenameJournal.cpp
         build_import/MapAnvilWireDiagnostic.cpp
         build_import/MapAnvilDebugBridge.cpp
         build_import/MapChestPlacement.cpp
         build_import/MapChestNativeSurvey.cpp
         build_import/MapChestStorageSender.cpp
         build_import/MapChestWorldPlacement.cpp
         build_import/MapPairPlacementJournal.cpp
         build_import/MapExtraChestPlacementJournal.cpp
         build_import/MapChestTransferJournal.cpp
         build_import/MapChestStorageVerification.cpp
         build_import/MapChestWindowSession.cpp
         build_import/MapChestProductionPreflight.cpp
         build_import/MapVisibleAnvilWindowSession.cpp
         build_import/MapVisibleAnvilProductionPreflight.cpp
         build_import/MapStorageCoordinator.cpp
         build_import/MapStoragePipelineDriver.cpp
         build_import/MapStorageProductionActions.cpp
         build_import/MapStorageAnvilAdapter.cpp
         build_import/MapNativeHeldMapEvidence.cpp
          build_import/MapStorageChestCaptureAdapter.cpp
          build_import/MapChestClientSlotSync.cpp
          build_import/MapChestClientSyncDelivery.cpp
         build_import/MapStorageCursorCommit.cpp
         build_import/MapBlankSupplyJournal.cpp
         build_import/ProjectionPrinterInventoryClientSync.cpp
         build_import/MapAnvilClientSlotSync.cpp
         build_import/MapAnvilClientSyncDelivery.cpp
         build_import/ProjectionPrinterInventorySession.cpp
         build_import/ProjectionPrinterInventoryPresentationGate.cpp
         build_import/ProjectionPrinterNativeHotbarSelection.cpp
          build_import/ProjectionPrinterInventoryMover.cpp
          build_import/MapInventoryTransfer.cpp
          build_import/PlayerInventoryOpenPacketSender.cpp
         build_import/SignEditSessionMailbox.cpp
        build_import/ItemRuntimeRegistry.cpp
        build_import/ContainerClosePacketSender.cpp
        build_import/ContainerOpenPacketSender.cpp
        build_import/ContainerOpenPacketSenderThunks.S
          build_import/CommandBlockPacketSender.cpp
          build_import/SignBlockActorPacketSender.cpp
          build_import/CommandSpool.cpp
         build_import/PyRpcAckDecoder.cpp
        build_import/BdxParser.cpp
        build_import/BdxWriter.cpp
         build_import/LitematicParser.cpp
         build_import/McworldParser.cpp
         build_import/SchematicParser.cpp
        build_import/SchematicWriter.cpp
        build_import/InfiniteczBuildWriter.cpp
        build_import/InfiniteczBuildParser.cpp
        build_import/InfinityCompressionCodec.cpp
        build_import/InfinityCryptoCodec.cpp
        build_import/InfinityFormatKey.cpp
        build_import/BuildExportCheckpoint.cpp
        build_import/BuildExportPaths.cpp
         build_import/PixelArtParser.cpp
         build_import/CommandMusicParser.cpp
         build_import/MidiCommandMusicParser.cpp
         build_import/BuildExportRuntime.cpp
        build_import/BuildImportRuntime.cpp
         build_import/BuildProjectionRuntime.cpp
         build_import/ProjectionBlockIdentity.cpp
         build_import/ProjectionPrinterRuntime.cpp
         build_import/ProjectionPrinterSilentRotation.cpp
         build_import/ProjectionWorldMatchRuntime.cpp
         build_import/BuildProjectionRenderer.cpp
        build_import/NativeWorldAccess.cpp
        build_import/BuildImportCheckpoint.cpp
        build_import/BuildImportUndoStore.cpp
        build_import/BuildImportController.cpp
        tp/PythonUtils.cpp
        tp/TpModule.cpp
        tp/FunctionsAddress.cpp
        tp/MinecraftUpdateHook.cpp
        tp/LightningEffect.cpp
        tp/LoopbackPacketSenderCapture.cpp
        ${SRC}
)

# 6. Set include directories
target_include_directories(weibosdkcore PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Dobby/include
    ${CMAKE_SOURCE_DIR}/Json
    ${CMAKE_SOURCE_DIR}/openssl_ndk
    #${OPENSSL_INCLUDE_DIR}
)

find_library(log-lib log)
find_library(z-lib z)
find_library(glesv3-lib GLESv3)
find_library(egl-lib EGL)

# 7. Link libraries
target_link_libraries(
        weibosdkcore
        build_import_brotli_decoder
        build_import_brotli_encoder
        crypto
        dobby
         ${z-lib}
         ${glesv3-lib}
         ${egl-lib}
         ${log-lib}
)

# Keep the map/anvil transport and its diagnostics identical in both APK
# variants while the automatic storage flow is being validated. The runtime
# opt-in and same-window safety checks still govern whether it may dispatch.
target_compile_definitions(weibosdkcore PRIVATE MAP_ANVIL_DEBUG_TRACE=1)
