package com.vdl.kong520.ui

import android.content.Context
import android.content.res.AssetManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapRegionDecoder
import android.graphics.Rect
import android.os.Looper
import org.json.JSONArray
import org.json.JSONObject
import java.io.Closeable
import java.io.FilterInputStream
import java.io.InputStream
import java.io.InputStreamReader
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.zip.ZipFile
import kotlin.math.max

/**
 * Loads only the vanilla block textures used by a projection.
 *
 * Material request lines use `name<TAB>aux<TAB>rotation`. Rotation accepts
 * either quarter turns (0..3) or degrees divisible by 90. [faceLayers] stores
 * six entries per material in renderer order: north, south, west, east, down,
 * up. [layerPixels] is tightly packed, layer-major RGBA8 data.
 *
 * Layer zero is always an opaque magenta/black fallback. This keeps missing
 * resources visible and lets native rendering fall back without disappearing.
 */
object ProjectionTextureLoader {
    const val FACE_NORTH = 0
    const val FACE_SOUTH = 1
    const val FACE_WEST = 2
    const val FACE_EAST = 3
    const val FACE_DOWN = 4
    const val FACE_UP = 5
    const val FACE_COUNT = 6

    const val STATUS_OK = "ok"
    const val STATUS_PARTIAL = "partial"
    const val STATUS_ERROR = "error"

    // The host resource pack is nested below an assets/ directory inside the
    // injected asset tree, so AssetManager needs the full two-level path.
    private const val PACK_ASSET_ROOT = "assets/resource_packs/vanilla"
    private const val DEFAULT_TEXTURE_BUDGET = 64 * 1024 * 1024
    private const val MIN_TEXTURE_BUDGET = 1024 * 1024
    private const val MAX_TEXTURE_BUDGET = 256 * 1024 * 1024
    private const val MAX_REQUEST_TEXT_BYTES = 4 * 1024 * 1024
    // Native material id 0 is reserved for the fallback, leaving 1..65535.
    private const val MAX_MATERIALS = 65_535
    private const val MAX_JSON_BYTES = 32 * 1024 * 1024
    private const val MAX_TILE_SIZE = 32
    private const val MAX_TEXTURE_LAYERS = 2_048
    private const val MAX_IMAGE_WIDTH = 8_192
    private const val MAX_IMAGE_HEIGHT = 65_536
    // Bedrock still ships a handful of vanilla block textures as TGA only.
    // Keep the decoder bounded by the same maximum texture allocation used by
    // this loader: a projection tile is at most 32 px, so accepting a source
    // larger than this has no useful rendering benefit.
    private const val MAX_TGA_SOURCE_BYTES = MAX_TEXTURE_BUDGET
    private const val MAX_TGA_PIXELS = 64L * 1024L * 1024L
    private const val MAX_WARNINGS = 256

    private val executor: ExecutorService = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "projection-texture-loader").apply { priority = Thread.NORM_PRIORITY - 1 }
    }

    class Result(
        @JvmField val materialKeys: Array<String>,
        @JvmField val faceLayers: ShortArray,
        @JvmField val tileSize: Int,
        @JvmField val layerCount: Int,
        @JvmField val layerPixels: ByteArray,
        @JvmField val warnings: Array<String>,
        @JvmField val status: String
    )

    fun interface Callback {
        /** Called on the loader thread, never on the Android main thread. */
        fun onComplete(result: Result)
    }

    /**
     * Runs a serialized background load. The callback is invoked on the same
     * worker so callers can hand the arrays directly to JNI without a UI copy.
     */
    @JvmStatic
    @JvmOverloads
    fun loadAsync(
        context: Context,
        materialRequests: String,
        maxTextureBytes: Int = DEFAULT_TEXTURE_BUDGET,
        callback: Callback
    ) {
        val appContext = context.applicationContext ?: context
        executor.execute {
            val result = try {
                loadInternal(appContext, materialRequests, maxTextureBytes)
            } catch (error: OutOfMemoryError) {
                errorResult("not enough memory to decode projection textures")
            } catch (error: Throwable) {
                errorResult(error.message ?: "projection texture loading failed")
            }
            callback.onComplete(result)
        }
    }

    /** Must be called off the Android main thread. */
    @JvmStatic
    @JvmOverloads
    fun load(
        context: Context,
        materialRequests: String,
        maxTextureBytes: Int = DEFAULT_TEXTURE_BUDGET
    ): Result {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return errorResult("projection textures must be loaded off the main thread")
        }
        return try {
            loadInternal(context.applicationContext ?: context, materialRequests, maxTextureBytes)
        } catch (error: OutOfMemoryError) {
            errorResult("not enough memory to decode projection textures")
        } catch (error: Throwable) {
            errorResult(error.message ?: "projection texture loading failed")
        }
    }

    private data class MaterialRequest(
        val key: String,
        val name: String,
        val leaf: String,
        val aux: Int,
        val rotationQuarters: Int
    )

    private enum class ImageFormat {
        BITMAP,
        TGA
    }

    private data class ImageInfo(
        val path: String,
        val width: Int,
        val height: Int,
        val frameWidth: Int,
        val frameHeight: Int,
        val format: ImageFormat
    )

    private data class TgaHeader(
        val imageType: Int,
        val idLength: Int,
        val width: Int,
        val height: Int,
        val bytesPerPixel: Int,
        val descriptor: Int
    )

    private enum class JsonReadState {
        NORMAL,
        SLASH,
        STRING,
        STRING_ESCAPE,
        LINE_COMMENT,
        BLOCK_COMMENT,
        BLOCK_COMMENT_STAR
    }

    private class SizeLimitedInputStream(
        input: InputStream,
        private val maximumBytes: Int
    ) : FilterInputStream(input) {
        private var consumedBytes = 0

        private fun account(read: Int): Int {
            if (read > 0) {
                if (read > maximumBytes - consumedBytes) {
                    throw IllegalArgumentException("resource exceeds size limit")
                }
                consumedBytes += read
            }
            return read
        }

        override fun read(): Int {
            val value = super.read()
            if (value >= 0) account(1)
            return value
        }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int =
            account(super.read(buffer, offset, length))
    }

    private class WarningCollector {
        private val messages = ArrayList<String>()
        private var omitted = 0

        fun add(message: String) {
            if (messages.size < MAX_WARNINGS) messages.add(message) else omitted++
        }

        fun toArray(): Array<String> {
            if (omitted > 0 && messages.size < MAX_WARNINGS + 1) {
                messages.add("$omitted additional texture warnings omitted")
            }
            return messages.toTypedArray()
        }

        fun isEmpty(): Boolean = messages.isEmpty() && omitted == 0
    }

    private fun loadInternal(
        context: Context,
        materialRequests: String,
        requestedBudget: Int
    ): Result {
        val warnings = WarningCollector()
        val requests = parseRequests(materialRequests, warnings)
        if (requests.isEmpty()) {
            return errorResult("projection did not provide any valid material requests", warnings)
        }
        if (Thread.currentThread().isInterrupted) {
            return errorResult("projection texture loading was cancelled", warnings)
        }

        ResourcePack(context).use { pack ->
            val blocks = loadJson(pack, "blocks.json", warnings)
            val terrainRoot = loadJson(pack, "textures/terrain_texture.json", warnings)
            val blockTable = blocks?.optJSONObject("blocks") ?: blocks
            val textureData = terrainRoot?.optJSONObject("texture_data")
            if (blockTable == null) warnings.add("vanilla blocks.json is unavailable")
            if (textureData == null) warnings.add("vanilla terrain_texture.json is unavailable")

            val resolver = TextureResolver(pack, blockTable, textureData)
            val materialFaces = ArrayList<Array<String?>>(requests.size)
            for ((index, request) in requests.withIndex()) {
                if ((index and 0xff) == 0 && Thread.currentThread().isInterrupted) {
                    return errorResult("projection texture loading was cancelled", warnings)
                }
                val faces = resolver.resolve(request)
                val missing = ArrayList<String>(FACE_COUNT)
                for (face in 0 until FACE_COUNT) {
                    if (faces[face] == null) missing.add(FACE_LABELS[face])
                }
                if (missing.isNotEmpty()) {
                    warnings.add("${request.name}: missing ${missing.joinToString(", ")} texture")
                }
                materialFaces.add(faces)
            }

            val tileSize = chooseTileSize(materialFaces, resolver)
            val bytesPerLayer = tileSize * tileSize * 4
            val budget = requestedBudget.coerceIn(MIN_TEXTURE_BUDGET, MAX_TEXTURE_BUDGET)
            val maximumLayers = minOf(
                MAX_TEXTURE_LAYERS,
                max(1, budget / bytesPerLayer)
            )

            val selectedPaths = LinkedHashSet<String>()
            for (faces in materialFaces) {
                for (path in faces) {
                    if (path != null && selectedPaths.size + 1 < maximumLayers) {
                        selectedPaths.add(path)
                    }
                }
            }
            val allPathCount = materialFaces.asSequence()
                .flatMap { it.asSequence() }
                .filterNotNull()
                .distinct()
                .count()
            if (selectedPaths.size < allPathCount) {
                warnings.add(
                    "texture budget kept ${selectedPaths.size} of $allPathCount unique vanilla textures"
                )
            }

            val allocatedLayerCount = selectedPaths.size + 1
            val byteCount = allocatedLayerCount.toLong() * bytesPerLayer.toLong()
            if (byteCount > Int.MAX_VALUE) {
                return errorResult("projection texture array is too large", warnings)
            }
            val layerPixels = ByteArray(byteCount.toInt())
            writeFallbackLayer(layerPixels, tileSize)
            val pathToLayer = HashMap<String, Int>(selectedPaths.size * 2 + 1)
            var nextLayer = 1
            for (path in selectedPaths) {
                if (Thread.currentThread().isInterrupted) {
                    return errorResult("projection texture loading was cancelled", warnings)
                }
                val info = resolver.imageInfo(path)
                val rgba = info?.let { decodeFirstFrame(pack, it, tileSize) }
                if (rgba == null) {
                    warnings.add("failed to decode vanilla texture $path")
                    continue
                }
                rgba.copyInto(layerPixels, nextLayer * bytesPerLayer)
                pathToLayer[path] = nextLayer
                nextLayer++
            }

            val faceLayers = ShortArray(requests.size * FACE_COUNT)
            for (materialIndex in requests.indices) {
                val faces = materialFaces[materialIndex]
                val offset = materialIndex * FACE_COUNT
                for (face in 0 until FACE_COUNT) {
                    faceLayers[offset + face] =
                        (faces[face]?.let { pathToLayer[it] } ?: 0).toShort()
                }
            }
            val warningArray = warnings.toArray()
            return Result(
                requests.map { it.key }.toTypedArray(),
                faceLayers,
                tileSize,
                nextLayer,
                layerPixels,
                warningArray,
                if (warningArray.isEmpty()) STATUS_OK else STATUS_PARTIAL
            )
        }
    }

    private fun parseRequests(text: String, warnings: WarningCollector): List<MaterialRequest> {
        if (exceedsUtf8Limit(text, MAX_REQUEST_TEXT_BYTES)) {
            warnings.add("material request list exceeds the 4 MiB safety limit")
            return emptyList()
        }
        val result = LinkedHashMap<String, MaterialRequest>()
        for (rawLine in text.lineSequence()) {
            val line = rawLine.trimEnd('\r')
            if (line.isBlank()) continue
            if (result.size >= MAX_MATERIALS) {
                warnings.add("material request list was truncated at $MAX_MATERIALS entries")
                break
            }
            val fields = line.split('\t', limit = 4)
            val name = canonicalBlockName(fields.getOrNull(0).orEmpty())
            val aux = fields.getOrNull(1)?.trim()?.toIntOrNull() ?: 0
            val rotationValue = fields.getOrNull(2)?.trim()?.toIntOrNull() ?: 0
            if (name == null || aux !in 0..255) {
                warnings.add("ignored invalid projection material request")
                continue
            }
            val rotation = normalizeRotation(rotationValue)
            if (rotation == null) {
                warnings.add("$name: ignored unsupported material rotation $rotationValue")
                continue
            }
            val key = "$name\t$aux\t$rotation"
            result.putIfAbsent(
                key,
                MaterialRequest(key, name, name.substringAfter(':'), aux, rotation)
            )
        }
        return result.values.toList()
    }

    private fun canonicalBlockName(raw: String): String? {
        var name = raw.trim().substringBefore('[').lowercase(Locale.ROOT)
        if (name.isEmpty()) return null
        if (':' !in name) name = "minecraft:$name"
        if (name.any { !(it in 'a'..'z' || it in '0'..'9' || it == '_' || it == ':' || it == '-' || it == '.' || it == '/') }) {
            return null
        }
        return name
    }

    private fun normalizeRotation(value: Int): Int? = when {
        value in 0..3 -> value
        value % 90 == 0 -> ((value / 90) % 4 + 4) % 4
        else -> null
    }

    private class TextureResolver(
        private val pack: ResourcePack,
        blockTable: JSONObject?,
        textureData: JSONObject?
    ) {
        private val blocks = JsonIndex(blockTable)
        private val terrain = JsonIndex(textureData)
        private val imageInfoCache = HashMap<String, ImageInfo?>()

        fun imageInfo(path: String): ImageInfo? = imageInfoCache.getOrPut(path) {
            probeImage(pack, path)
        }

        fun resolve(request: MaterialRequest): Array<String?> {
            val heuristic = heuristicAssignments(request)
            val definition = findBlockDefinition(request)
            val assignment = definition?.let { selectBlockTextureAssignment(it, request) }
            val definedFaces = assignment?.let { expandFaces(it, request) }
            val resolved = arrayOfNulls<String>(FACE_COUNT)
            for (face in 0 until FACE_COUNT) {
                val values = ArrayList<Any?>(8)
                definedFaces?.get(face)?.let { values.add(it) }
                values.addAll(heuristic[face])
                resolved[face] = resolveFirst(values, request)
            }
            applyLogAxis(request, resolved)
            rotateHorizontalFaces(resolved, intrinsicRotation(request) + request.rotationQuarters)
            return resolved
        }

        private fun findBlockDefinition(request: MaterialRequest): Any? {
            for (key in blockKeyCandidates(request.leaf, request.name)) {
                blocks[key]?.let { return it }
            }
            return null
        }

        private fun selectBlockTextureAssignment(definition: Any, request: MaterialRequest): Any? {
            if (definition !is JSONObject) return definition
            val regular = definition.optValue("textures")
            val carried = definition.optValue("carried_textures")
            return if (request.leaf in FIXED_TINT_BLOCKS && carried != null) carried
            else regular ?: carried
        }

        private fun expandFaces(value: Any, request: MaterialRequest): Array<Any?> {
            if (value !is JSONObject || !value.hasAnyKey(FACE_OBJECT_KEYS)) {
                val selected = selectVariant(value, request, "")
                return arrayOf(selected, selected, selected, selected, selected, selected)
            }
            return arrayOf(
                value.firstValue("north", "front", "side", "all", "*"),
                value.firstValue("south", "back", "side", "all", "*"),
                value.firstValue("west", "side", "all", "*"),
                value.firstValue("east", "side", "all", "*"),
                value.firstValue("down", "bottom", "side", "all", "*"),
                value.firstValue("up", "top", "side", "all", "*")
            )
        }

        private fun resolveFirst(values: List<Any?>, request: MaterialRequest): String? {
            val candidates = LinkedHashSet<String>()
            for (value in values) collectPaths(value, request, candidates, 0)
            for (candidate in candidates) {
                val path = normalizeTexturePath(candidate) ?: continue
                if (imageInfo(path) != null) return path
            }
            return null
        }

        private fun collectPaths(
            value: Any?,
            request: MaterialRequest,
            output: LinkedHashSet<String>,
            depth: Int
        ) {
            if (value == null || value === JSONObject.NULL || depth > 6) return
            when (value) {
                is String -> collectStringPaths(value, request, output, depth)
                is JSONArray -> {
                    for (entry in variantValues(value, request, "")) {
                        collectPaths(entry, request, output, depth + 1)
                    }
                }
                is JSONObject -> {
                    val path = value.optValue("path")
                    val textures = value.optValue("textures")
                    val texture = value.optValue("texture")
                    if (path != null) collectPaths(path, request, output, depth + 1)
                    if (textures != null) collectPaths(textures, request, output, depth + 1)
                    if (texture != null) collectPaths(texture, request, output, depth + 1)
                }
            }
        }

        private fun collectStringPaths(
            raw: String,
            request: MaterialRequest,
            output: LinkedHashSet<String>,
            depth: Int
        ) {
            val token = raw.trim().removeSuffix(".png")
            if (token.isEmpty()) return
            val terrainKeys = textureKeyCandidates(token, request.leaf)
            var foundTerrainEntry = false
            val directPath = token.startsWith("textures/") || '/' in token
            if (!directPath) {
                for (key in terrainKeys) {
                    val entry = terrain[key] ?: continue
                    foundTerrainEntry = true
                    val textureValue = if (entry is JSONObject) {
                        entry.optValue("textures") ?: entry.optValue("path") ?: entry
                    } else entry
                    when (textureValue) {
                        is JSONArray -> for (candidate in variantValues(textureValue, request, key)) {
                            collectPaths(candidate, request, output, depth + 1)
                        }
                        else -> collectPaths(textureValue, request, output, depth + 1)
                    }
                }
            }
            if (!foundTerrainEntry || directPath) {
                output.add(token)
            }
            if (!foundTerrainEntry && !directPath) {
                for (key in terrainKeys) output.add("textures/blocks/$key")
            }
        }

        private fun selectVariant(value: Any, request: MaterialRequest, key: String): Any? {
            if (value !is JSONArray) return value
            return variantValues(value, request, key).firstOrNull()
        }

        private fun variantValues(
            array: JSONArray,
            request: MaterialRequest,
            key: String
        ): List<Any?> {
            if (array.length() == 0) return emptyList()
            val index = variantIndex(request, key, array.length())
            if (index == 0) return listOf(array.opt(0))
            return listOf(array.opt(index), array.opt(0))
        }

        private fun variantIndex(request: MaterialRequest, key: String, count: Int): Int {
            if (count <= 1) return 0
            val leaf = request.leaf
            val generic = leaf in LEGACY_VARIANT_BLOCKS || key in LEGACY_VARIANT_BLOCKS
            if (!generic) return 0
            val raw = when (leaf) {
                "log", "leaves" -> request.aux and 3
                "log2", "leaves2" -> request.aux and 1
                else -> request.aux and 15
            }
            return if (raw in 0 until count) raw else 0
        }

        /**
         * Compatibility faces for the part of the vanilla pack whose file
         * names predate current block identifiers.  `blocks.json` and
         * `terrain_texture.json`, when exposed by the host, still win because
         * their values are inserted before these candidates in [resolve].
         *
         * These rules deliberately name only files confirmed in the extracted
         * reference pack.  Runtime reads still come from the host game.
         * Unknown names retain the normal fallback rather than
         * selecting an unrelated texture by substring similarity.
         */
        private fun compatibilityAssignments(request: MaterialRequest): Array<List<Any?>>? {
            fun typed(values: List<String>): List<Any?> = values.map { it as Any? }
            fun all(vararg values: String): Array<List<Any?>> =
                Array(FACE_COUNT) { typed(values.toList()) }
            fun sided(
                horizontal: List<String>,
                down: List<String> = horizontal,
                up: List<String> = horizontal
            ): Array<List<Any?>> = arrayOf(
                typed(horizontal), typed(horizontal), typed(horizontal), typed(horizontal),
                typed(down), typed(up)
            )
            fun fronted(
                horizontal: List<String>,
                front: List<String>,
                back: List<String> = horizontal,
                down: List<String> = horizontal,
                up: List<String> = horizontal
            ): Array<List<Any?>> = sided(horizontal, down, up).also { faces ->
                faces[FACE_NORTH] = typed(front)
                faces[FACE_SOUTH] = typed(back)
            }

            val leaf = request.leaf
            val aux = request.aux
            return when {
                leaf == "water" || leaf == "flowing_water" || leaf == "bubble_column" ->
                    sided(
                        horizontal = listOf("water_flow", "water_still"),
                        down = listOf("water_still"),
                        up = listOf("water_still")
                    )
                leaf == "lava" || leaf == "flowing_lava" ->
                    sided(
                        horizontal = listOf("lava_flow", "lava_still"),
                        down = listOf("lava_still"),
                        up = listOf("lava_still")
                    )
                leaf == "fire" -> all("fire_0", "fire_1")
                leaf == "soul_fire" -> all("soul_fire_0", "soul_fire_1")
                leaf == "torch" || leaf == "wall_torch" -> all("torch_on")
                leaf == "soul_torch" || leaf == "soul_wall_torch" -> all("soul_torch")
                leaf == "redstone_torch" || leaf == "redstone_wall_torch" ->
                    all("redstone_torch_on")
                leaf == "unlit_redstone_torch" -> all("redstone_torch_off")
                leaf == "chain" -> all("chain1", "chain2")
                leaf == "rail" -> all("rail_normal", "rail_normal_turned")
                leaf == "golden_rail" || leaf == "powered_rail" ->
                    all("rail_golden", "rail_golden_powered")
                leaf == "detector_rail" -> all("rail_detector", "rail_detector_powered")
                leaf == "activator_rail" -> all("rail_activator", "rail_activator_powered")
                leaf == "redstone_wire" -> all("redstone_dust_cross", "redstone_dust_line")
                leaf == "repeater" || leaf == "unpowered_repeater" -> all("repeater_off", "repeater_on")
                leaf == "powered_repeater" -> all("repeater_on", "repeater_off")
                leaf == "comparator" || leaf == "unpowered_comparator" ->
                    all("comparator_off", "comparator_on")
                leaf == "powered_comparator" -> all("comparator_on", "comparator_off")
                leaf == "planks" || leaf == "wooden_slab" || leaf == "double_wooden_slab" ->
                    all("planks_${legacyWoodTextureName(aux and 7)}")
                leaf == "stone_slab" || leaf == "double_stone_slab" ||
                    leaf == "stone_slab2" || leaf == "double_stone_slab2" ->
                    all(legacyStoneSlabTextureName(leaf, aux))
                leaf == "fence" || leaf == "fence_gate" || leaf == "wooden_button" ||
                    leaf == "wooden_pressure_plate" -> all("planks_oak")
                leaf == "log" || leaf == "log2" -> {
                    val wood = if (leaf == "log") legacyLogTextureName(aux and 3)
                    else legacySecondLogTextureName(aux and 1)
                    sided(
                        horizontal = listOf("log_$wood"),
                        down = listOf("log_${wood}_top", "log_$wood"),
                        up = listOf("log_${wood}_top", "log_$wood")
                    )
                }
                leaf == "leaves" || leaf == "leaves2" -> {
                    val wood = if (leaf == "leaves") legacyLogTextureName(aux and 3)
                    else legacySecondLogTextureName(aux and 1)
                    all("leaves_${wood}_opaque", "leaves_$wood")
                }
                leaf == "sapling" -> all("sapling_${legacySaplingTextureName(aux and 7)}")
                leaf == "wool" || leaf == "carpet" ->
                    all("wool_colored_${legacyDyeTextureName(aux)}")
                leaf == "concrete" -> all("concrete_${legacyDyeTextureName(aux)}")
                leaf == "concrete_powder" -> all("concrete_powder_${legacyDyeTextureName(aux)}")
                leaf == "stained_hardened_clay" ->
                    all("hardened_clay_stained_${legacyDyeTextureName(aux)}")
                leaf == "stained_glass" -> all("glass_${legacyDyeTextureName(aux)}")
                leaf == "stained_glass_pane" -> all(
                    "glass_pane_top_${legacyDyeTextureName(aux)}",
                    "glass_${legacyDyeTextureName(aux)}"
                )
                leaf == "shulker_box" -> all("shulker_top_${legacyDyeTextureName(aux)}")
                leaf == "undyed_shulker_box" -> all("shulker_top_undyed")
                leaf == "yellow_flower" -> all("flower_dandelion")
                leaf == "red_flower" -> all(legacyRedFlowerTextureName(aux))
                leaf == "short_grass" || leaf == "tallgrass" ->
                    all(if ((aux and 7) == 2) "fern" else "tallgrass")
                leaf == "fern" -> all("fern")
                leaf == "sugar_cane" || leaf == "reeds" -> all("reeds")
                leaf == "kelp" -> all("kelp_a")
                leaf == "seagrass" -> all("seagrass")
                leaf == "grass_path" || leaf == "dirt_path" -> sided(
                    horizontal = listOf("grass_path_side", "dirt"),
                    down = listOf("dirt"),
                    up = listOf("grass_path_top", "grass_path_side")
                )
                leaf == "farmland" -> all(if ((aux and 7) == 0) "farmland_dry" else "farmland_wet")
                leaf == "frosted_ice" -> all("frosted_ice_${(aux and 3).coerceAtMost(3)}")
                leaf == "wheat" -> all("wheat_stage_${(aux and 7).coerceAtMost(7)}")
                leaf == "carrots" -> all("carrots_stage_${(aux and 7).coerceAtMost(3)}")
                leaf == "potatoes" -> all("potatoes_stage_${(aux and 7).coerceAtMost(3)}")
                leaf == "beetroot" -> all("beetroots_stage_${(aux and 3).coerceAtMost(3)}")
                leaf == "nether_wart" -> all("nether_wart_stage_${(aux and 3).coerceAtMost(2)}")
                leaf == "cocoa" -> all("cocoa_stage_${((aux ushr 2) and 3).coerceAtMost(2)}")
                leaf == "melon_stem" -> all("melon_stem_disconnected")
                leaf == "pumpkin_stem" -> all("pumpkin_stem_disconnected")
                leaf == "cave_vines" -> all("cave_vines_body")
                leaf == "cave_vines_body_with_berries" -> all("cave_vines_body_berries")
                leaf == "cave_vines_head_with_berries" -> all("cave_vines_head_berries")
                leaf == "weeping_vines" -> all("weeping_vines")
                leaf == "twisting_vines" -> all("twisting_vines_bottom")
                leaf == "double_plant" -> {
                    val variant = LEGACY_DOUBLE_PLANT_TEXTURES.getOrElse(aux and 7) {
                        LEGACY_DOUBLE_PLANT_TEXTURES.first()
                    }
                    val half = if ((aux and 8) != 0) "top" else "bottom"
                    if (variant == "sunflower" && half == "bottom") {
                        fronted(
                            horizontal = listOf("double_plant_sunflower_bottom"),
                            front = listOf(
                                "double_plant_sunflower_front",
                                "double_plant_sunflower_bottom"
                            ),
                            back = listOf(
                                "double_plant_sunflower_back",
                                "double_plant_sunflower_bottom"
                            ),
                            down = listOf("double_plant_sunflower_bottom"),
                            up = listOf("double_plant_sunflower_bottom")
                        )
                    } else {
                        all("double_plant_${variant}_$half")
                    }
                }
                leaf == "bed" -> {
                    val piece = if ((aux and 8) != 0) "bed_head" else "bed_feet"
                    fronted(
                        horizontal = listOf("${piece}_side"),
                        front = listOf("${piece}_end", "${piece}_side"),
                        down = listOf("${piece}_side"),
                        up = listOf("${piece}_top", "${piece}_side")
                    )
                }
                leaf == "anvil" -> sided(
                    horizontal = listOf("anvil_base"),
                    down = listOf("anvil_base"),
                    up = listOf("anvil_top_damaged_${anvilDamageIndex(aux)}", "anvil_base")
                )
                leaf == "cake" -> sided(
                    horizontal = listOf("cake_side"),
                    down = listOf("cake_bottom", "cake_side"),
                    up = listOf("cake_top", "cake_side")
                )
                leaf == "cauldron" -> sided(
                    horizontal = listOf("cauldron_side"),
                    down = listOf("cauldron_bottom", "cauldron_side"),
                    up = listOf("cauldron_top", "cauldron_inner", "cauldron_side")
                )
                leaf == "enchanting_table" -> sided(
                    horizontal = listOf("enchanting_table_side"),
                    down = listOf("enchanting_table_bottom", "enchanting_table_side"),
                    up = listOf("enchanting_table_top", "enchanting_table_side")
                )
                leaf == "observer" -> fronted(
                    horizontal = listOf("observer_side"),
                    front = listOf("observer_front", "observer_side"),
                    back = listOf("observer_back", "observer_back_lit", "observer_side"),
                    down = listOf("observer_side"),
                    up = listOf("observer_top", "observer_side")
                )
                leaf == "dispenser" -> all("dispenser_front_horizontal", "dispenser_front_vertical")
                leaf == "dropper" -> all("dropper_front_horizontal", "dropper_front_vertical")
                leaf == "glass_pane" -> all("glass_pane_top", "glass")
                leaf == "lit_redstone_lamp" -> all("redstone_lamp_on")
                leaf == "redstone_lamp" -> all("redstone_lamp_off")
                leaf == "lit_redstone_ore" || leaf == "redstone_ore" -> all("redstone_ore")
                leaf == "mossy_cobblestone" -> all("cobblestone_mossy")
                leaf == "packed_ice" -> all("ice_packed")
                leaf == "brown_mushroom" -> all("mushroom_brown")
                leaf == "red_mushroom" -> all("mushroom_red")
                leaf == "brown_mushroom_block" -> all("mushroom_block_skin_brown", "mushroom_block_inside")
                leaf == "red_mushroom_block" -> all("mushroom_block_skin_red", "mushroom_block_inside")
                leaf == "melon_block" -> sided(
                    horizontal = listOf("melon_side"),
                    down = listOf("melon_side"),
                    up = listOf("melon_top", "melon_side")
                )
                leaf == "crimson_stem" -> sided(
                    horizontal = listOf("crimson_log_side"),
                    down = listOf("crimson_log_top", "crimson_log_side"),
                    up = listOf("crimson_log_top", "crimson_log_side")
                )
                leaf == "warped_stem" -> sided(
                    horizontal = listOf("warped_stem_side"),
                    down = listOf("warped_stem_top", "warped_stem_side"),
                    up = listOf("warped_stem_top", "warped_stem_side")
                )
                leaf == "dirt" -> when (aux and 3) {
                    1 -> all("coarse_dirt")
                    2 -> sided(
                        horizontal = listOf("dirt_podzol_side", "dirt"),
                        down = listOf("dirt"),
                        up = listOf("dirt_podzol_top", "dirt_podzol_side")
                    )
                    else -> all("dirt")
                }
                leaf == "podzol" -> sided(
                    horizontal = listOf("dirt_podzol_side", "dirt"),
                    down = listOf("dirt"),
                    up = listOf("dirt_podzol_top", "dirt_podzol_side")
                )
                leaf == "sponge" -> all(if ((aux and 1) == 0) "sponge" else "sponge_wet")
                leaf == "prismarine" -> all(
                    when (aux and 3) {
                        1 -> "prismarine_bricks"
                        2 -> "prismarine_dark"
                        else -> "prismarine_rough"
                    }
                )
                leaf == "stonebrick" -> all(
                    when (aux and 3) {
                        1 -> "stonebrick_mossy"
                        2 -> "stonebrick_cracked"
                        3 -> "stonebrick_carved"
                        else -> "stonebrick"
                    }
                )
                leaf == "sandstone" || leaf == "red_sandstone" -> {
                    val style = when (aux and 3) {
                        1 -> "carved"
                        2 -> "smooth"
                        else -> "normal"
                    }
                    sided(
                        horizontal = listOf("${leaf}_$style"),
                        down = listOf("${leaf}_bottom", "${leaf}_$style"),
                        up = listOf("${leaf}_top", "${leaf}_$style")
                    )
                }
                leaf == "scaffolding" -> sided(
                    horizontal = listOf("scaffolding_side"),
                    down = listOf("scaffolding_bottom", "scaffolding_side"),
                    up = listOf("scaffolding_top", "scaffolding_side")
                )
                leaf == "barrel" -> sided(
                    horizontal = listOf("barrel_side"),
                    down = listOf("barrel_bottom", "barrel_side"),
                    up = listOf("barrel_top", "barrel_side")
                )
                leaf == "hopper" -> sided(
                    horizontal = listOf("hopper_outside", "hopper_top"),
                    down = listOf("hopper_outside"),
                    up = listOf("hopper_top", "hopper_inside")
                )
                leaf == "chest" || leaf == "trapped_chest" || leaf == "ender_chest" -> {
                    val base = leaf
                    fronted(
                        horizontal = listOf("${base}_side", "chest_side"),
                        front = listOf("${base}_front", "chest_front"),
                        down = listOf("${base}_side", "chest_side"),
                        up = listOf("${base}_top", "chest_top", "chest_side")
                    )
                }
                leaf == "command_block" || leaf == "repeating_command_block" ||
                    leaf == "chain_command_block" -> fronted(
                    horizontal = listOf("${leaf}_side", leaf),
                    front = listOf("${leaf}_front", "${leaf}_side", leaf),
                    back = listOf("${leaf}_back", "${leaf}_side", leaf)
                )
                leaf == "piston" || leaf == "sticky_piston" -> sided(
                    horizontal = listOf("piston_side"),
                    down = listOf("piston_bottom", "piston_side"),
                    up = listOf(if (leaf == "sticky_piston") "piston_top_sticky" else "piston_top_normal", "piston_side")
                )
                leaf == "bone_block" -> sided(
                    horizontal = listOf("bone_block_side"),
                    down = listOf("bone_block_top", "bone_block_side"),
                    up = listOf("bone_block_top", "bone_block_side")
                )
                leaf == "quartz_block" || leaf == "quartz_pillar" -> {
                    when (if (leaf == "quartz_pillar") 2 else aux and 3) {
                        1 -> sided(
                            horizontal = listOf("quartz_block_chiseled", "quartz_block_side"),
                            down = listOf("quartz_block_bottom", "quartz_block_chiseled"),
                            up = listOf("quartz_block_chiseled_top", "quartz_block_chiseled")
                        )
                        2 -> sided(
                            horizontal = listOf("quartz_block_lines", "quartz_block_side"),
                            down = listOf("quartz_block_lines_top", "quartz_block_lines"),
                            up = listOf("quartz_block_lines_top", "quartz_block_lines")
                        )
                        else -> sided(
                            horizontal = listOf("quartz_block_side"),
                            down = listOf("quartz_block_bottom", "quartz_block_side"),
                            up = listOf("quartz_block_top", "quartz_block_side")
                        )
                    }
                }
                else -> null
            }
        }

        private fun heuristicAssignments(request: MaterialRequest): Array<List<Any?>> {
            compatibilityAssignments(request)?.let { return it }
            val leaf = request.leaf
            val common = textureKeyCandidates(leaf, leaf).map { it as Any? }
            val result: Array<MutableList<Any?>> = Array(FACE_COUNT) { face ->
                val faceCandidates = when (face) {
                    FACE_DOWN -> listOf("${leaf}_bottom", "${leaf}_side")
                    FACE_UP -> listOf("${leaf}_top", "${leaf}_side")
                    else -> listOf("${leaf}_side", "${leaf}_front", "${leaf}_back")
                }
                ArrayList<Any?>(faceCandidates.size + common.size).apply {
                    addAll(faceCandidates)
                    addAll(common)
                }
            }

            when {
                leaf == "grass" || leaf == "grass_block" -> {
                    setHorizontal(result, listOf("grass_side_carried", "grass_side"))
                    result[FACE_DOWN] = mutableListOf("dirt")
                    result[FACE_UP] = mutableListOf("grass_carried", "grass_top")
                }
                leaf.endsWith("_log") || leaf == "log" || leaf == "log2" -> {
                    val wood = leaf.removeSuffix("_log")
                    val side = if (leaf == "log" || leaf == "log2") listOf(leaf)
                    else listOf("${leaf}_side", leaf, "log_$wood")
                    val top = if (leaf == "log" || leaf == "log2") {
                        listOf("${leaf}_top", leaf)
                    } else {
                        listOf("${leaf}_top", "log_${wood}_top", leaf)
                    }
                    setAll(result, side)
                    result[FACE_DOWN] = top.toMutableList()
                    result[FACE_UP] = top.toMutableList()
                }
                leaf.endsWith("_wood") || leaf.endsWith("_hyphae") -> {
                    setAll(result, common)
                }
                leaf == "sandstone" || leaf == "red_sandstone" -> {
                    setHorizontal(result, listOf("${leaf}_normal", leaf))
                    result[FACE_DOWN] = mutableListOf("${leaf}_bottom", leaf)
                    result[FACE_UP] = mutableListOf("${leaf}_top", leaf)
                }
                leaf == "crafting_table" -> {
                    setHorizontal(result, listOf("crafting_table_side", leaf))
                    result[FACE_NORTH] = mutableListOf("crafting_table_front", "crafting_table_side")
                    result[FACE_SOUTH] = mutableListOf("crafting_table_front", "crafting_table_side")
                    result[FACE_DOWN] = mutableListOf("planks_oak", "oak_planks")
                    result[FACE_UP] = mutableListOf("crafting_table_top", leaf)
                }
                leaf == "furnace" || leaf == "lit_furnace" || leaf == "blast_furnace" ||
                    leaf == "lit_blast_furnace" || leaf == "smoker" || leaf == "lit_smoker" -> {
                    val base = leaf.removePrefix("lit_")
                    val active = leaf.startsWith("lit_")
                    setHorizontal(result, listOf("${base}_side", base))
                    result[FACE_NORTH] = mutableListOf(
                        if (active) "${base}_front_on" else "${base}_front_off",
                        "${base}_front",
                        base
                    )
                    result[FACE_DOWN] = mutableListOf("${base}_top", "${base}_side", base)
                    result[FACE_UP] = mutableListOf("${base}_top", base)
                }
                leaf == "tnt" -> {
                    setHorizontal(result, listOf("tnt_side", leaf))
                    result[FACE_DOWN] = mutableListOf("tnt_bottom", leaf)
                    result[FACE_UP] = mutableListOf("tnt_top", leaf)
                }
                leaf == "bookshelf" -> {
                    setHorizontal(result, listOf("bookshelf", leaf))
                    result[FACE_DOWN] = mutableListOf("planks_oak", "oak_planks")
                    result[FACE_UP] = mutableListOf("planks_oak", "oak_planks")
                }
                leaf == "cactus" -> {
                    setHorizontal(result, listOf("cactus_side", leaf))
                    result[FACE_DOWN] = mutableListOf("cactus_bottom", leaf)
                    result[FACE_UP] = mutableListOf("cactus_top", leaf)
                }
                leaf == "pumpkin" || leaf == "lit_pumpkin" || leaf == "jack_o_lantern" -> {
                    setHorizontal(result, listOf("pumpkin_side", leaf))
                    result[FACE_NORTH] = mutableListOf(
                        if (leaf == "pumpkin") "pumpkin_face_off" else "pumpkin_face_on",
                        "pumpkin_side"
                    )
                    result[FACE_DOWN] = mutableListOf("pumpkin_top", leaf)
                    result[FACE_UP] = mutableListOf("pumpkin_top", leaf)
                }
                leaf == "hay_block" || leaf.endsWith("_stem") || leaf.endsWith("_pillar") -> {
                    val top = listOf<Any?>("${leaf}_top", leaf)
                    result[FACE_DOWN] = top.toMutableList()
                    result[FACE_UP] = top.toMutableList()
                }
            }
            return Array(FACE_COUNT) { result[it].toList() }
        }

        private fun applyLogAxis(request: MaterialRequest, faces: Array<String?>) {
            val leaf = request.leaf
            val axisAware = leaf == "log" || leaf == "log2" || leaf == "hay_block" ||
                leaf == "bone_block" || leaf == "quartz_block" || leaf == "quartz_pillar"
            if (!axisAware) return
            when (request.aux and 0x0c) {
                0x04 -> {
                    val cap = faces[FACE_UP]
                    val side = faces[FACE_NORTH]
                    faces[FACE_DOWN] = side
                    faces[FACE_UP] = side
                    faces[FACE_WEST] = cap
                    faces[FACE_EAST] = cap
                }
                0x08 -> {
                    val cap = faces[FACE_UP]
                    val side = faces[FACE_WEST]
                    faces[FACE_DOWN] = side
                    faces[FACE_UP] = side
                    faces[FACE_NORTH] = cap
                    faces[FACE_SOUTH] = cap
                }
            }
        }

        private fun intrinsicRotation(request: MaterialRequest): Int {
            val leaf = request.leaf
            if (leaf in FACING_2_TO_5_BLOCKS) {
                return when (request.aux and 7) {
                    5 -> 1
                    3 -> 2
                    4 -> 3
                    else -> 0
                }
            }
            if (leaf == "pumpkin" || leaf == "lit_pumpkin" || leaf == "jack_o_lantern" ||
                leaf == "bed") {
                return when (request.aux and 3) {
                    3 -> 1
                    0 -> 2
                    1 -> 3
                    else -> 0
                }
            }
            return 0
        }
    }

    private class JsonIndex(source: JSONObject?) {
        private val values = HashMap<String, Any?>()

        init {
            if (source != null) {
                val keys = source.keys()
                while (keys.hasNext()) {
                    val key = keys.next()
                    values[key.lowercase(Locale.ROOT)] = source.opt(key)
                }
            }
        }

        operator fun get(key: String): Any? = values[key.lowercase(Locale.ROOT)]
    }

    private class ResourcePack(context: Context) : Closeable {
        private val hostAssets = context.assets
        private val zipFiles = ArrayList<ZipFile>()
        // This index is built from the target game's own resource pack only.
        // It covers the few vanilla textures stored in subfolders while keeping
        // all bytes and runtime lookups inside the host application.
        private val hostTextureIndex: Map<String, String> by lazy(LazyThreadSafetyMode.SYNCHRONIZED) {
            buildHostTextureIndex()
        }

        init {
            val paths = LinkedHashSet<String>()
            context.applicationInfo.sourceDir?.let(paths::add)
            context.applicationInfo.splitSourceDirs?.forEach(paths::add)
            for (path in paths) {
                try {
                    zipFiles.add(ZipFile(path))
                } catch (_: Exception) {
                    // AssetManager may still expose the resource.
                }
            }
        }

        fun open(pathWithinPack: String): InputStream? {
            val clean = sanitizePackPath(pathWithinPack) ?: return null
            openHostExact(clean)?.let { return it }
            if (!clean.startsWith("textures/blocks/") ||
                (!clean.endsWith(".png", ignoreCase = true) &&
                    !clean.endsWith(".tga", ignoreCase = true))) {
                return null
            }
            // A few valid vanilla files are nested (for example
            // huge_fungus/warped_stem_side.png).  Resolve a unique basename
            // against the game's pack, never against this module's assets.
            val indexed = hostTextureIndex[clean.substringAfterLast('/').lowercase(Locale.ROOT)]
                ?: return null
            return if (indexed == clean) null else openHostExact(indexed)
        }

        private fun openHostExact(clean: String): InputStream? {
            val assetPath = "$PACK_ASSET_ROOT/$clean"
            try {
                return hostAssets.open(assetPath, AssetManager.ACCESS_STREAMING)
            } catch (_: Exception) {
                // Some injected contexts require reading the target APK itself.
            }
            val zipPaths = arrayOf("assets/$assetPath", assetPath)
            for (zip in zipFiles) {
                for (zipPath in zipPaths) {
                    try {
                        val entry = zip.getEntry(zipPath) ?: continue
                        return zip.getInputStream(entry)
                    } catch (_: Exception) {
                        // Try the next location or split APK.
                    }
                }
            }
            return null
        }

        private fun buildHostTextureIndex(): Map<String, String> {
            val result = LinkedHashMap<String, String>()
            val ambiguous = HashSet<String>()
            fun remember(clean: String) {
                val key = clean.substringAfterLast('/').lowercase(Locale.ROOT)
                if (!key.endsWith(".png") && !key.endsWith(".tga")) return
                val previous = result[key]
                when {
                    previous == null -> result[key] = clean
                    previous != clean -> {
                        result.remove(key)
                        ambiguous.add(key)
                    }
                }
            }
            fun visit(assetDirectory: String, cleanDirectory: String, depth: Int) {
                if (depth > 8) return
                val entries = try {
                    hostAssets.list(assetDirectory) ?: emptyArray()
                } catch (_: Exception) {
                    emptyArray()
                }
                for (entry in entries) {
                    if (entry.isBlank()) continue
                    val assetPath = "$assetDirectory/$entry"
                    val cleanPath = "$cleanDirectory/$entry"
                    if (entry.endsWith(".png", ignoreCase = true) ||
                        entry.endsWith(".tga", ignoreCase = true)) {
                        remember(cleanPath)
                    } else {
                        visit(assetPath, cleanPath, depth + 1)
                    }
                }
            }
            visit("$PACK_ASSET_ROOT/textures/blocks", "textures/blocks", 0)

            val zipPrefixes = arrayOf(
                "assets/$PACK_ASSET_ROOT/textures/blocks/",
                "$PACK_ASSET_ROOT/textures/blocks/"
            )
            for (zip in zipFiles) {
                try {
                    val entries = zip.entries()
                    while (entries.hasMoreElements()) {
                        val entry = entries.nextElement()
                        if (entry.isDirectory) continue
                        val prefix = zipPrefixes.firstOrNull { entry.name.startsWith(it) } ?: continue
                        val clean = "textures/blocks/" + entry.name.removePrefix(prefix)
                        remember(clean)
                    }
                } catch (_: Exception) {
                    // The AssetManager path remains available on hosts that
                    // deny directory enumeration from a split APK.
                }
            }
            for (key in ambiguous) result.remove(key)
            return result
        }

        override fun close() {
            for (zip in zipFiles) {
                try {
                    zip.close()
                } catch (_: Exception) {
                }
            }
            zipFiles.clear()
        }
    }

    private fun loadJson(
        pack: ResourcePack,
        path: String,
        warnings: WarningCollector
    ): JSONObject? {
        val stream = pack.open(path) ?: return null
        return try {
            JSONObject(readJsonWithoutComments(stream, MAX_JSON_BYTES))
        } catch (error: Exception) {
            warnings.add("failed to parse vanilla $path: ${error.message ?: "invalid JSON"}")
            null
        } finally {
            runCatching { stream.close() }
        }
    }

    private fun readJsonWithoutComments(stream: InputStream, limit: Int): String {
        val output = StringBuilder(minOf(64 * 1024, limit))
        val buffer = CharArray(16 * 1024)
        var state = JsonReadState.NORMAL
        var firstCharacter = true
        InputStreamReader(
            SizeLimitedInputStream(stream, limit),
            StandardCharsets.UTF_8
        ).use { reader ->
            while (true) {
                val count = reader.read(buffer)
                if (count < 0) break
                for (index in 0 until count) {
                    val character = buffer[index]
                    if (firstCharacter) {
                        firstCharacter = false
                        if (character == '\uFEFF') continue
                    }
                    state = when (state) {
                        JsonReadState.NORMAL -> when (character) {
                            '/' -> JsonReadState.SLASH
                            '"' -> {
                                output.append(character)
                                JsonReadState.STRING
                            }
                            else -> {
                                output.append(character)
                                JsonReadState.NORMAL
                            }
                        }
                        JsonReadState.SLASH -> when (character) {
                            '/' -> JsonReadState.LINE_COMMENT
                            '*' -> JsonReadState.BLOCK_COMMENT
                            '"' -> {
                                output.append('/').append(character)
                                JsonReadState.STRING
                            }
                            else -> {
                                output.append('/').append(character)
                                JsonReadState.NORMAL
                            }
                        }
                        JsonReadState.STRING -> {
                            output.append(character)
                            when (character) {
                                '\\' -> JsonReadState.STRING_ESCAPE
                                '"' -> JsonReadState.NORMAL
                                else -> JsonReadState.STRING
                            }
                        }
                        JsonReadState.STRING_ESCAPE -> {
                            output.append(character)
                            JsonReadState.STRING
                        }
                        JsonReadState.LINE_COMMENT -> {
                            if (character == '\n' || character == '\r') {
                                output.append(character)
                                JsonReadState.NORMAL
                            } else {
                                JsonReadState.LINE_COMMENT
                            }
                        }
                        JsonReadState.BLOCK_COMMENT -> {
                            if (character == '\n' || character == '\r') output.append(character)
                            if (character == '*') {
                                JsonReadState.BLOCK_COMMENT_STAR
                            } else {
                                JsonReadState.BLOCK_COMMENT
                            }
                        }
                        JsonReadState.BLOCK_COMMENT_STAR -> when (character) {
                            '/' -> JsonReadState.NORMAL
                            '*' -> JsonReadState.BLOCK_COMMENT_STAR
                            else -> {
                                if (character == '\n' || character == '\r') output.append(character)
                                JsonReadState.BLOCK_COMMENT
                            }
                        }
                    }
                }
            }
        }
        if (state == JsonReadState.SLASH) output.append('/')
        return output.toString()
    }

    private fun exceedsUtf8Limit(text: String, limit: Int): Boolean {
        var bytes = 0
        var index = 0
        while (index < text.length) {
            val character = text[index]
            val encodedBytes = when {
                character.code <= 0x7f -> 1
                character.code <= 0x7ff -> 2
                Character.isHighSurrogate(character) && index + 1 < text.length &&
                    Character.isLowSurrogate(text[index + 1]) -> {
                    index++
                    4
                }
                else -> 3
            }
            if (encodedBytes > limit - bytes) return true
            bytes += encodedBytes
            index++
        }
        return false
    }

    private fun probeImage(pack: ResourcePack, path: String): ImageInfo? {
        // Vanilla packs use PNG for most terrain, but several otherwise normal
        // blocks (for example cactus, reeds and the legacy leaf textures) are
        // supplied only as TGA. Keep the normal PNG path first so a pack with
        // both formats always follows the game's PNG resource.
        for (candidate in imageSourceCandidates(path)) {
            val info = if (candidate.endsWith(".tga", ignoreCase = true)) {
                probeTga(pack, candidate)
            } else {
                probeBitmap(pack, candidate)
            }
            if (info != null) return info
        }
        return null
    }

    private fun imageSourceCandidates(path: String): List<String> {
        val stem = when {
            path.endsWith(".png", ignoreCase = true) -> path.dropLast(4)
            path.endsWith(".tga", ignoreCase = true) -> path.dropLast(4)
            else -> return listOf(path)
        }
        return listOf("$stem.png", "$stem.tga")
    }

    private fun probeBitmap(pack: ResourcePack, path: String): ImageInfo? {
        val stream = pack.open(path) ?: return null
        val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        try {
            stream.use { BitmapFactory.decodeStream(it, null, options) }
        } catch (_: Exception) {
            return null
        }
        return imageInfo(path, options.outWidth, options.outHeight, ImageFormat.BITMAP)
    }

    private fun probeTga(pack: ResourcePack, path: String): ImageInfo? {
        val stream = pack.open(path) ?: return null
        return try {
            stream.use { raw ->
                val input = SizeLimitedInputStream(raw, MAX_TGA_SOURCE_BYTES)
                val bytes = ByteArray(18)
                if (!readFully(input, bytes)) return null
                val header = parseTgaHeader(bytes) ?: return null
                if (header.width.toLong() * header.height.toLong() > MAX_TGA_PIXELS) {
                    return null
                }
                imageInfo(path, header.width, header.height, ImageFormat.TGA)
            }
        } catch (_: Exception) {
            null
        }
    }

    private fun imageInfo(
        path: String,
        width: Int,
        height: Int,
        format: ImageFormat
    ): ImageInfo? {
        if (width <= 0 || height <= 0 || width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
            return null
        }
        val frameWidth: Int
        val frameHeight: Int
        if (height > width && height % width == 0) {
            frameWidth = width
            frameHeight = width
        } else if (width > height && width % height == 0) {
            frameWidth = height
            frameHeight = height
        } else {
            frameWidth = width
            frameHeight = height
        }
        return ImageInfo(path, width, height, frameWidth, frameHeight, format)
    }

    private fun parseTgaHeader(bytes: ByteArray): TgaHeader? {
        if (bytes.size != 18) return null
        val colorMapType = bytes[1].toInt() and 0xff
        val imageType = bytes[2].toInt() and 0xff
        val width = littleEndianU16(bytes, 12)
        val height = littleEndianU16(bytes, 14)
        val pixelDepth = bytes[16].toInt() and 0xff
        if (colorMapType != 0 || imageType !in setOf(2, 10) ||
            pixelDepth !in setOf(24, 32) || width <= 0 || height <= 0) {
            return null
        }
        return TgaHeader(
            imageType = imageType,
            idLength = bytes[0].toInt() and 0xff,
            width = width,
            height = height,
            bytesPerPixel = pixelDepth / 8,
            descriptor = bytes[17].toInt() and 0xff
        )
    }

    private fun littleEndianU16(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8)

    private fun readFully(input: InputStream, destination: ByteArray): Boolean {
        var offset = 0
        while (offset < destination.size) {
            val read = input.read(destination, offset, destination.size - offset)
            if (read <= 0) return false
            offset += read
        }
        return true
    }

    private fun skipFully(input: InputStream, count: Int): Boolean {
        var remaining = count
        while (remaining > 0) {
            val skipped = input.skip(remaining.toLong())
            if (skipped > 0L) {
                remaining -= skipped.toInt()
            } else if (input.read() >= 0) {
                remaining--
            } else {
                return false
            }
        }
        return true
    }

    private fun decodeTgaFirstFrame(pack: ResourcePack, info: ImageInfo): Bitmap? {
        val stream = pack.open(info.path) ?: return null
        return try {
            stream.use { raw ->
                val input = SizeLimitedInputStream(raw, MAX_TGA_SOURCE_BYTES)
                val headerBytes = ByteArray(18)
                if (!readFully(input, headerBytes)) return null
                val header = parseTgaHeader(headerBytes) ?: return null
                if (header.width != info.width || header.height != info.height ||
                    header.width.toLong() * header.height.toLong() > MAX_TGA_PIXELS ||
                    !skipFully(input, header.idLength)) {
                    return null
                }

                val sourcePixelCount = header.width * header.height
                val framePixelCount = info.frameWidth.toLong() * info.frameHeight.toLong()
                if (framePixelCount <= 0L || framePixelCount > Int.MAX_VALUE.toLong()) {
                    return null
                }
                val pixels = IntArray(framePixelCount.toInt())
                val encodedPixel = ByteArray(header.bytesPerPixel)
                var sourceIndex = 0

                fun storePixel() {
                    val sourceX = sourceIndex % header.width
                    val sourceY = sourceIndex / header.width
                    val x = if ((header.descriptor and 0x10) != 0) {
                        header.width - 1 - sourceX
                    } else {
                        sourceX
                    }
                    val y = if ((header.descriptor and 0x20) != 0) {
                        sourceY
                    } else {
                        header.height - 1 - sourceY
                    }
                    if (x < info.frameWidth && y < info.frameHeight) {
                        val blue = encodedPixel[0].toInt() and 0xff
                        val green = encodedPixel[1].toInt() and 0xff
                        val red = encodedPixel[2].toInt() and 0xff
                        val alpha = if (header.bytesPerPixel == 4) {
                            encodedPixel[3].toInt() and 0xff
                        } else {
                            0xff
                        }
                        pixels[y * info.frameWidth + x] =
                            (alpha shl 24) or (red shl 16) or (green shl 8) or blue
                    }
                    sourceIndex++
                }

                while (sourceIndex < sourcePixelCount) {
                    if (header.imageType == 2) {
                        if (!readFully(input, encodedPixel)) return null
                        storePixel()
                        continue
                    }

                    val packet = input.read()
                    if (packet < 0) return null
                    val packetPixels = (packet and 0x7f) + 1
                    if (packetPixels > sourcePixelCount - sourceIndex) return null
                    if ((packet and 0x80) != 0) {
                        if (!readFully(input, encodedPixel)) return null
                        var remaining = packetPixels
                        while (remaining-- > 0) storePixel()
                    } else {
                        var remaining = packetPixels
                        while (remaining-- > 0) {
                            if (!readFully(input, encodedPixel)) return null
                            storePixel()
                        }
                    }
                }

                Bitmap.createBitmap(
                    pixels,
                    info.frameWidth,
                    info.frameHeight,
                    Bitmap.Config.ARGB_8888
                )
            }
        } catch (_: Exception) {
            null
        }
    }

    @Suppress("DEPRECATION")
    private fun decodeFirstFrame(
        pack: ResourcePack,
        info: ImageInfo,
        tileSize: Int
    ): ByteArray? {
        var decoded: Bitmap? = null
        var scaled: Bitmap? = null
        try {
            if (info.format == ImageFormat.TGA) {
                decoded = decodeTgaFirstFrame(pack, info)
            } else {
                val sampleSize = calculateSampleSize(info.frameWidth, info.frameHeight, tileSize)
                val options = BitmapFactory.Options().apply {
                    inPreferredConfig = Bitmap.Config.ARGB_8888
                    inSampleSize = sampleSize
                }
                val stream = pack.open(info.path) ?: return null
                stream.use {
                    val decoder = BitmapRegionDecoder.newInstance(it, false) ?: return null
                    try {
                        decoded = decoder.decodeRegion(
                            Rect(0, 0, info.frameWidth, info.frameHeight),
                            options
                        )
                    } finally {
                        decoder.recycle()
                    }
                }
            }
            val source = decoded ?: return null
            scaled = if (source.width == tileSize && source.height == tileSize) {
                source
            } else {
                Bitmap.createScaledBitmap(source, tileSize, tileSize, false)
            }
            val pixels = IntArray(tileSize * tileSize)
            scaled!!.getPixels(pixels, 0, tileSize, 0, 0, tileSize, tileSize)
            val rgba = ByteArray(tileSize * tileSize * 4)
            var destination = 0
            for (color in pixels) {
                rgba[destination++] = ((color ushr 16) and 0xff).toByte()
                rgba[destination++] = ((color ushr 8) and 0xff).toByte()
                rgba[destination++] = (color and 0xff).toByte()
                rgba[destination++] = ((color ushr 24) and 0xff).toByte()
            }
            return rgba
        } catch (_: Exception) {
            return null
        } catch (_: OutOfMemoryError) {
            return null
        } finally {
            if (scaled != null && scaled !== decoded && !scaled!!.isRecycled) scaled!!.recycle()
            if (decoded != null && !decoded!!.isRecycled) decoded!!.recycle()
        }
    }

    private fun calculateSampleSize(width: Int, height: Int, target: Int): Int {
        var sample = 1
        while (width / (sample * 2) >= target && height / (sample * 2) >= target) {
            sample *= 2
        }
        return sample
    }

    private fun chooseTileSize(
        materialFaces: List<Array<String?>>,
        resolver: TextureResolver
    ): Int {
        var maximumNaturalSize = 1
        val seen = HashSet<String>()
        for (faces in materialFaces) {
            for (path in faces) {
                if (path == null || !seen.add(path)) continue
                val info = resolver.imageInfo(path) ?: continue
                maximumNaturalSize = max(maximumNaturalSize, max(info.frameWidth, info.frameHeight))
            }
        }
        return when {
            maximumNaturalSize <= 16 -> 16
            maximumNaturalSize <= 32 -> 32
            else -> MAX_TILE_SIZE
        }
    }

    private fun writeFallbackLayer(destination: ByteArray, tileSize: Int) {
        var offset = 0
        for (y in 0 until tileSize) {
            for (x in 0 until tileSize) {
                val magenta = ((x / max(1, tileSize / 4)) + (y / max(1, tileSize / 4))) % 2 == 0
                destination[offset++] = if (magenta) 0xff.toByte() else 0
                destination[offset++] = 0
                destination[offset++] = if (magenta) 0xff.toByte() else 0
                destination[offset++] = 0xff.toByte()
            }
        }
    }

    private fun errorResult(message: String, warnings: WarningCollector? = null): Result {
        val tileSize = 16
        val fallback = ByteArray(tileSize * tileSize * 4)
        writeFallbackLayer(fallback, tileSize)
        val allWarnings = warnings ?: WarningCollector()
        allWarnings.add(message)
        return Result(
            emptyArray(),
            ShortArray(0),
            tileSize,
            1,
            fallback,
            allWarnings.toArray(),
            STATUS_ERROR
        )
    }

    private fun sanitizePackPath(raw: String): String? {
        val path = raw.replace('\\', '/').trimStart('/')
        if (path.isEmpty() || path.split('/').any { it.isEmpty() || it == "." || it == ".." }) {
            return null
        }
        return path
    }

    private fun normalizeTexturePath(raw: String): String? {
        var path = raw.trim().replace('\\', '/').trimStart('/')
        path = path.removePrefix("$PACK_ASSET_ROOT/")
        path = path.removePrefix("assets/$PACK_ASSET_ROOT/")
        if (path.startsWith("resource_packs/vanilla/")) {
            path = path.removePrefix("resource_packs/vanilla/")
        }
        if (!path.startsWith("textures/")) path = "textures/blocks/$path"
        if (!path.endsWith(".png", ignoreCase = true) &&
            !path.endsWith(".tga", ignoreCase = true)) {
            if ('.' in path.substringAfterLast('/')) return null
            path += ".png"
        }
        return sanitizePackPath(path)
    }

    private fun legacyWoodTextureName(index: Int): String = LEGACY_WOOD_TEXTURES.getOrElse(index) {
        LEGACY_WOOD_TEXTURES.first()
    }

    private fun legacyLogTextureName(index: Int): String = LEGACY_LOG_TEXTURES.getOrElse(index) {
        LEGACY_LOG_TEXTURES.first()
    }

    private fun legacySecondLogTextureName(index: Int): String = LEGACY_SECOND_LOG_TEXTURES.getOrElse(index) {
        LEGACY_SECOND_LOG_TEXTURES.first()
    }

    private fun legacySaplingTextureName(index: Int): String = LEGACY_SAPLING_TEXTURES.getOrElse(index) {
        LEGACY_SAPLING_TEXTURES.first()
    }

    private fun legacyDyeTextureName(aux: Int): String = LEGACY_DYE_TEXTURES[aux and 15]

    private fun legacyRedFlowerTextureName(aux: Int): String =
        LEGACY_RED_FLOWER_TEXTURES.getOrElse(aux and 15) { LEGACY_RED_FLOWER_TEXTURES.first() }

    private fun legacyStoneSlabTextureName(leaf: String, aux: Int): String {
        return when (leaf) {
            "stone_slab", "double_stone_slab" -> when (aux and 7) {
                0 -> "stone_slab_top"
                1 -> "sandstone_normal"
                2 -> "planks_oak"
                3 -> "cobblestone"
                4 -> "brick"
                5 -> "stonebrick"
                6 -> "nether_brick"
                else -> "quartz_block_side"
            }
            else -> when (aux and 7) {
                0 -> "red_sandstone_normal"
                1 -> "purpur_block"
                2 -> "prismarine_rough"
                3 -> "prismarine_bricks"
                4 -> "prismarine_dark"
                5 -> "red_nether_brick"
                else -> "end_bricks"
            }
        }
    }

    private fun anvilDamageIndex(aux: Int): Int = ((aux ushr 2) and 3).coerceAtMost(2)

    private fun resourceDyeTextureName(color: String): String =
        if (color == "light_gray") "silver" else color

    private fun blockKeyCandidates(leaf: String, fullName: String): List<String> {
        val result = LinkedHashSet<String>()
        result.add(fullName)
        result.add(leaf)
        BLOCK_ALIASES[leaf]?.let(result::addAll)
        return result.toList()
    }

    private fun textureKeyCandidates(token: String, leaf: String): List<String> {
        var clean = token.trim().lowercase(Locale.ROOT).removePrefix("minecraft:")
        clean = clean.removePrefix("textures/blocks/").removeSuffix(".png").removeSuffix(".tga")
        val result = LinkedHashSet<String>()
        result.add(clean)
        BLOCK_ALIASES[clean]?.let(result::addAll)
        HOST_TEXTURE_STEM_ALIASES[clean]?.let(result::addAll)
        if (clean == leaf) BLOCK_ALIASES[leaf]?.let(result::addAll)
        if (clean == leaf) HOST_TEXTURE_STEM_ALIASES[leaf]?.let(result::addAll)

        val reorderSuffixes = listOf(
            "planks", "log", "wood", "leaves", "sapling", "wool", "concrete",
            "terracotta", "glass"
        )
        for (suffix in reorderSuffixes) {
            val marker = "_$suffix"
            if (clean.endsWith(marker)) {
                val prefix = clean.removeSuffix(marker)
                result.add("${suffix}_$prefix")
            }
        }
        for (suffix in SHAPE_SUFFIXES) {
            if (clean.endsWith(suffix)) {
                val base = clean.removeSuffix(suffix)
                result.add(base)
                result.add("${base}_planks")
                result.add("planks_$base")
                if (base == "dark_oak") result.add("planks_big_oak")
            }
        }
        if (clean.endsWith("_leaves")) {
            val wood = clean.removeSuffix("_leaves").let {
                if (it == "dark_oak") "big_oak" else it
            }
            result.add("leaves_${wood}_opaque")
            result.add("leaves_$wood")
        }
        if (clean.endsWith("_door")) {
            val wood = clean.removeSuffix("_door")
            val oldWood = if (wood == "oak" || wood == "wooden") "wood" else wood
            result.add("door_${oldWood}_lower")
            result.add("${wood}_door_lower")
            result.add("${wood}_door_bottom")
        }
        val color = DYE_COLORS.firstOrNull { clean.startsWith("${it}_") }
        if (color != null) {
            val resourceColor = resourceDyeTextureName(color)
            when {
                clean.endsWith("_wool") -> result.add("wool_colored_$resourceColor")
                clean.endsWith("_carpet") -> result.add("wool_colored_$resourceColor")
                clean.endsWith("_concrete_powder") -> result.add("concrete_powder_$resourceColor")
                clean.endsWith("_concrete") -> result.add("concrete_$resourceColor")
                clean.endsWith("_glazed_terracotta") -> result.add("glazed_terracotta_$resourceColor")
                clean.endsWith("_stained_glass_pane") -> result.add("glass_pane_top_$resourceColor")
                clean.endsWith("_terracotta") -> result.add("hardened_clay_stained_$resourceColor")
                clean.endsWith("_stained_glass") -> result.add("glass_$resourceColor")
                clean.endsWith("_shulker_box") -> result.add("shulker_top_$resourceColor")
            }
        }
        return result.toList()
    }

    private fun setHorizontal(target: Array<MutableList<Any?>>, values: List<Any?>) {
        for (face in FACE_NORTH..FACE_EAST) target[face] = values.toMutableList()
    }

    private fun setAll(target: Array<MutableList<Any?>>, values: List<Any?>) {
        for (face in 0 until FACE_COUNT) target[face] = values.toMutableList()
    }

    private fun rotateHorizontalFaces(faces: Array<String?>, quarterTurns: Int) {
        repeat(((quarterTurns % 4) + 4) % 4) {
            val north = faces[FACE_NORTH]
            val south = faces[FACE_SOUTH]
            val west = faces[FACE_WEST]
            val east = faces[FACE_EAST]
            faces[FACE_NORTH] = west
            faces[FACE_EAST] = north
            faces[FACE_SOUTH] = east
            faces[FACE_WEST] = south
        }
    }

    private fun JSONObject.optValue(key: String): Any? = opt(key).takeUnless { it === JSONObject.NULL }

    private fun JSONObject.firstValue(vararg keys: String): Any? {
        for (key in keys) optValue(key)?.let { return it }
        return null
    }

    private fun JSONObject.hasAnyKey(keys: Set<String>): Boolean = keys.any(::has)

    private val FACE_LABELS = arrayOf("north", "south", "west", "east", "down", "up")
    private val FACE_OBJECT_KEYS = setOf(
        "north", "south", "west", "east", "front", "back", "side",
        "down", "up", "bottom", "top", "all", "*"
    )
    private val DYE_COLORS = listOf(
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "silver", "cyan", "purple", "blue", "brown", "green", "red", "black"
    )
    // Legacy metadata uses the sixteen-colour sequence below.  Bedrock's
    // resource files call old light-gray "silver", while modern block IDs may
    // still spell it light_gray; keep those concerns separate.
    private val LEGACY_DYE_TEXTURES = listOf(
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "silver", "cyan", "purple", "blue", "brown", "green", "red", "black"
    )
    private val LEGACY_WOOD_TEXTURES = listOf(
        "oak", "spruce", "birch", "jungle", "acacia", "big_oak"
    )
    private val LEGACY_LOG_TEXTURES = listOf("oak", "spruce", "birch", "jungle")
    private val LEGACY_SECOND_LOG_TEXTURES = listOf("acacia", "big_oak")
    private val LEGACY_SAPLING_TEXTURES = listOf(
        "oak", "spruce", "birch", "jungle", "acacia", "roofed_oak"
    )
    private val LEGACY_RED_FLOWER_TEXTURES = listOf(
        "flower_rose", "flower_blue_orchid", "flower_allium", "flower_houstonia",
        "flower_tulip_red", "flower_tulip_orange", "flower_tulip_white",
        "flower_tulip_pink", "flower_oxeye_daisy"
    )
    private val LEGACY_DOUBLE_PLANT_TEXTURES = listOf(
        "sunflower", "syringa", "grass", "fern", "rose", "paeonia"
    )
    private val SHAPE_SUFFIXES = listOf(
        "_stairs", "_double_slab", "_slab", "_wall", "_fence_gate", "_fence", "_button",
        "_pressure_plate", "_trapdoor"
    )
    private val FIXED_TINT_BLOCKS = setOf(
        "grass", "grass_block", "leaves", "leaves2", "oak_leaves", "spruce_leaves",
        "birch_leaves", "jungle_leaves", "acacia_leaves", "dark_oak_leaves", "vine"
    )
    private val LEGACY_VARIANT_BLOCKS = setOf(
        "stone", "dirt", "planks", "sapling", "sand", "log", "log2", "leaves", "leaves2",
        "sponge", "sandstone", "wool", "red_flower", "yellow_flower", "stone_slab",
        "stone_slab2", "double_stone_slab", "double_stone_slab2", "monster_egg",
        "stonebrick", "cobblestone_wall", "quartz_block", "stained_hardened_clay",
        "stained_glass", "stained_glass_pane", "carpet", "concrete", "concrete_powder"
    )
    private val FACING_2_TO_5_BLOCKS = setOf(
        "furnace", "lit_furnace", "blast_furnace", "smoker", "dispenser", "dropper",
        "observer", "piston", "sticky_piston", "chest", "trapped_chest", "ender_chest",
        "command_block", "repeating_command_block", "chain_command_block"
    )
    // These are texture-file stems, not block aliases.  They only cover
    // confirmed vanilla names from the extracted reference pack and are intentionally
    // conservative: anything unknown stays visibly unresolved instead of
    // being assigned a misleading nearby texture.
    private val HOST_TEXTURE_STEM_ALIASES = mapOf(
        "water" to listOf("water_still"),
        "flowing_water" to listOf("water_flow"),
        "bubble_column" to listOf("water_still"),
        "lava" to listOf("lava_still"),
        "flowing_lava" to listOf("lava_flow"),
        "torch" to listOf("torch_on"),
        "wall_torch" to listOf("torch_on"),
        "soul_torch" to listOf("soul_torch"),
        "soul_wall_torch" to listOf("soul_torch"),
        "redstone_torch" to listOf("redstone_torch_on"),
        "redstone_wall_torch" to listOf("redstone_torch_on"),
        "unlit_redstone_torch" to listOf("redstone_torch_off"),
        "fire" to listOf("fire_0"),
        "soul_fire" to listOf("soul_fire_0"),
        "chain" to listOf("chain1"),
        "rail" to listOf("rail_normal"),
        "golden_rail" to listOf("rail_golden"),
        "powered_rail" to listOf("rail_golden"),
        "detector_rail" to listOf("rail_detector"),
        "activator_rail" to listOf("rail_activator"),
        "redstone_wire" to listOf("redstone_dust_cross"),
        "brick_block" to listOf("brick"),
        "bricks" to listOf("brick"),
        "stone_bricks" to listOf("stonebrick"),
        "mossy_stone_bricks" to listOf("stonebrick_mossy"),
        "cracked_stone_bricks" to listOf("stonebrick_cracked"),
        "chiseled_stone_bricks" to listOf("stonebrick_carved"),
        "mossy_cobblestone" to listOf("cobblestone_mossy"),
        "packed_ice" to listOf("ice_packed"),
        "azalea_leaves_flowered" to listOf("azalea_leaves_flowers"),
        "brown_mushroom" to listOf("mushroom_brown"),
        "red_mushroom" to listOf("mushroom_red"),
        "redstone_lamp" to listOf("redstone_lamp_off"),
        "lit_redstone_lamp" to listOf("redstone_lamp_on"),
        "lit_redstone_ore" to listOf("redstone_ore"),
        "tripwire" to listOf("trip_wire"),
        "tripwire_hook" to listOf("trip_wire_source"),
        "snow_layer" to listOf("snow")
    )
    private val BLOCK_ALIASES = mapOf(
        "grass_block" to listOf("grass"),
        "dirt_path" to listOf("grass_path"),
        "cobweb" to listOf("web"),
        "lily_pad" to listOf("waterlily"),
        "bricks" to listOf("brick"),
        "brick_block" to listOf("brick"),
        "nether_bricks" to listOf("nether_brick"),
        "end_stone_bricks" to listOf("end_bricks"),
        "spawner" to listOf("mob_spawner"),
        "note_block" to listOf("noteblock"),
        "jack_o_lantern" to listOf("lit_pumpkin"),
        "redstone_torch" to listOf("redstone_torch_on"),
        "redstone_wall_torch" to listOf("redstone_torch_on"),
        "snow_block" to listOf("snow"),
        "terracotta" to listOf("hardened_clay"),
        "mossy_stone_bricks" to listOf("stonebrick_mossy"),
        "cracked_stone_bricks" to listOf("stonebrick_cracked"),
        "chiseled_stone_bricks" to listOf("stonebrick_carved"),
        "smooth_stone" to listOf("stone_slab_top"),
        "short_grass" to listOf("tallgrass"),
        "fern" to listOf("tallgrass"),
        "sugar_cane" to listOf("reeds"),
        "nether_quartz_ore" to listOf("quartz_ore"),
        "mossy_cobblestone" to listOf("cobblestone_mossy"),
        "packed_ice" to listOf("ice_packed"),
        "azalea_leaves_flowered" to listOf("azalea_leaves_flowers"),
        "brown_mushroom" to listOf("mushroom_brown"),
        "red_mushroom" to listOf("mushroom_red"),
        "tripwire" to listOf("trip_wire"),
        "tripwire_hook" to listOf("trip_wire_source"),
        "snow_layer" to listOf("snow"),
        "purpur" to listOf("purpur_block"),
        "quartz" to listOf("quartz_block_side"),
        "stone_brick" to listOf("stonebrick"),
        "sandstone" to listOf("sandstone_normal"),
        "red_sandstone" to listOf("red_sandstone_normal")
    )
}
