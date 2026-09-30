package com.vdl.kong520.ui

import android.util.Base64
import java.io.ByteArrayInputStream
import java.util.Locale
import java.util.zip.GZIPInputStream

/**
 * Generated from the user's Minecraft Bedrock 1.21.120 zh_CN.lang.
 *
 * The compact payload contains every tile display name plus item display names
 * from that file. Entity display names are kept as their own generated table
 * below so the HUD can translate RTTI type names without relying on the
 * developer-machine source file at runtime.
 */
internal object MinecraftZhCnTranslations {
    private val encodedEntries = arrayOf(
        "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"
    )

    private val entries: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        val decoded = Base64.decode(encodedEntries.joinToString(separator = ""), Base64.NO_WRAP)
        val names = HashMap<String, String>(1_800)
        GZIPInputStream(ByteArrayInputStream(decoded))
            .bufferedReader(Charsets.UTF_8)
            .useLines { lines ->
                lines.forEach { line ->
                    val separator = line.indexOf('\t')
                    if (separator > 2 && separator < line.lastIndex) {
                        // A few recently-added lang entries carry an English
                        // developer comment after the translated value.  Keep
                        // the value itself exactly as supplied, but never let
                        // its separator whitespace widen the HUD card.
                        names[line.substring(0, separator)] =
                            line.substring(separator + 1).substringBefore('\t').trimEnd()
                    }
                }
            }
        names
    }

    private val blockNames: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        entries.asSequence()
            .filter { (key, _) -> key.startsWith("b:") }
            .associate { (key, value) -> key.removePrefix("b:") to value }
    }

    private val itemNames: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        entries.asSequence()
            .filter { (key, _) -> key.startsWith("i:") }
            .associate { (key, value) -> key.removePrefix("i:") to value }
    }

    // Generated from every `entity.*.name` entry in the supplied zh_CN.lang.
    // Keep this separate from the compressed block/item payload: entity keys
    // use a different, stable namespace and are looked up from native RTTI.
    private val entityNames = mapOf(
        "area_effect_cloud" to "区域效果云雾",
        "armadillo" to "犰狳",
        "armor_stand" to "盔甲架",
        "arrow" to "箭",
        "bat" to "蝙蝠",
        "bee" to "蜜蜂",
        "blaze" to "烈焰人",
        "boat" to "船",
        "bogged" to "沼骸",
        "breeze" to "旋风人",
        "breeze_wind_charge_projectile" to "风弹",
        "cat" to "猫",
        "cave_spider" to "洞穴蜘蛛",
        "chicken" to "鸡",
        "cow" to "牛",
        "creaking" to "嘎枝",
        "creeper" to "苦力怕",
        "dolphin" to "海豚",
        "goat" to "山羊",
        "panda" to "熊猫",
        "donkey" to "驴",
        "dragon_fireball" to "末影龙火球",
        "drowned" to "溺尸",
        "egg" to "鸡蛋",
        "elder_guardian" to "远古守卫者",
        "ender_crystal" to "末影水晶",
        "ender_dragon" to "末影龙",
        "enderman" to "末影人",
        "endermite" to "末影螨",
        "ender_pearl" to "末影珍珠",
        "evocation_illager" to "唤魔者",
        "evocation_fang" to "唤魔者尖牙",
        "eye_of_ender_signal" to "末影之眼",
        "falling_block" to "下落的方块",
        "fireball" to "火球",
        "fireworks_rocket" to "焰火火箭",
        "fishing_hook" to "鱼钩",
        "fish.clownfish" to "海葵鱼",
        "fox" to "狐狸",
        "cod" to "鳕鱼",
        "pufferfish" to "河豚",
        "salmon" to "鲑鱼",
        "tropicalfish" to "热带鱼",
        "axolotl" to "美西螈",
        "ghast" to "恶魂",
        "glow_squid" to "发光鱿鱼",
        "piglin_brute" to "猪灵蛮兵",
        "guardian" to "守卫者",
        "hoglin" to "疣猪兽",
        "horse" to "马",
        "husk" to "尸壳",
        "ravager" to "劫掠兽",
        "iron_golem" to "铁傀儡",
        "item" to "物品",
        "leash_knot" to "拴绳结",
        "lightning_bolt" to "闪电",
        "lingering_potion" to "滞留药水",
        "llama" to "羊驼",
        "trader_llama" to "行商羊驼",
        "llama_spit" to "羊驼口水",
        "magma_cube" to "岩浆怪",
        "minecart" to "矿车",
        "chest_minecart" to "运输矿车",
        "command_block_minecart" to "命令方块矿车",
        "furnace_minecart" to "动力矿车",
        "hopper_minecart" to "漏斗矿车",
        "tnt_minecart" to "TNT 矿车",
        "mule" to "骡",
        "mooshroom" to "哞菇",
        "moving_block" to "移动中的方块",
        "ocelot" to "豹猫",
        "painting" to "画",
        "parrot" to "鹦鹉",
        "phantom" to "幻翼",
        "pig" to "猪",
        "piglin" to "猪灵",
        "pillager" to "掠夺者",
        "polar_bear" to "北极熊",
        "rabbit" to "兔子",
        "sheep" to "羊",
        "shulker" to "潜影贝",
        "shulker_bullet" to "潜影贝子弹",
        "silverfish" to "蠹虫",
        "skeleton" to "骷髅",
        "skeleton_horse" to "骷髅马",
        "stray" to "流浪者",
        "slime" to "史莱姆",
        "small_fireball" to "小火球",
        "sniffer" to "嗅探兽",
        "snowball" to "雪球",
        "snow_golem" to "雪傀儡",
        "spider" to "蜘蛛",
        "splash_potion" to "药水",
        "squid" to "鱿鱼",
        "strider" to "炽足兽",
        "tnt" to "TNT 方块",
        "thrown_trident" to "三叉戟",
        "tripod_camera" to "三脚架摄像机",
        "turtle" to "海龟",
        "unknown" to "未知",
        "vex" to "恼鬼",
        "villager" to "村民",
        "villager_v2" to "村民",
        "vindicator" to "卫道士",
        "wandering_trader" to "流浪商人",
        "wind_charge_projectile" to "风弹",
        "witch" to "女巫",
        "wither" to "凋灵",
        "wither_skeleton" to "凋灵骷髅",
        "wither_skull" to "凋灵头颅",
        "wither_skull_dangerous" to "凋灵头颅",
        "wolf" to "狼",
        "xp_orb" to "经验球",
        "xp_bottle" to "附魔之瓶",
        "zoglin" to "僵尸疣猪兽",
        "zombie" to "僵尸",
        "zombie_horse" to "僵尸马",
        "zombie_pigman" to "僵尸猪灵",
        "zombie_villager" to "僵尸村民",
        "zombie_villager_v2" to "怪人村民",
        "frog" to "青蛙",
        "tadpole" to "蝌蚪",
        "warden" to "监守者",
        "allay" to "悦灵",
        "chest_boat" to "运输船",
        "camel" to "骆驼",
        "chest_raft" to "带宝箱的的竹筏",
        "happy_ghast" to "善念恶魂",
        "copper_golem" to "铜傀儡"
    )

    /**
     * Entity identifiers are normally flat Bedrock ids, while a few entries in
     * the supplied language file retain legacy separators (for example
     * `fish.clownfish`).  Native callers can also arrive with an underscore
     * form.  Build a collision-safe separator-insensitive index from the same
     * zh_CN.lang-derived table rather than duplicating any display strings.
     */
    private val canonicalEntityNames: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        val result = HashMap<String, String>(entityNames.size)
        val ambiguous = HashSet<String>()
        entityNames.forEach { (key, value) ->
            val canonical = entityCanonicalSignature(key)
            if (canonical.isBlank()) return@forEach
            val previous = result.putIfAbsent(canonical, value)
            if (previous != null && previous != value) ambiguous += canonical
        }
        ambiguous.forEach { result.remove(it) }
        result
    }

    private val blockSignatureNames: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        signatureIndex(blockNames)
    }

    private val itemSignatureNames: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        signatureIndex(itemNames)
    }

    /**
     * Bedrock keeps several historical language-key families (for example
     * `stone.andesite` and `red_flower.allium`) while the runtime now reports
     * flat identifiers (`andesite`, `allium`).  This index is built entirely
     * from the supplied zh_CN.lang values; it contains no hand-written Chinese
     * display text.  Ambiguous canonical keys are intentionally discarded.
     */
    private val canonicalBlockNames: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        canonicalIndex(blockNames)
    }

    private val canonicalItemNames: Map<String, String> by lazy(LazyThreadSafetyMode.PUBLICATION) {
        canonicalIndex(itemNames)
    }

    fun blockName(key: String): String? {
        return lookupName(key, blockNames, canonicalBlockNames, blockSignatureNames)
    }

    fun itemName(key: String): String? {
        return lookupName(key, itemNames, canonicalItemNames, itemSignatureNames)
    }

    /** Looks up a canonical Bedrock entity id (without `entity.` / `.name`). */
    fun entityName(key: String): String? {
        val normalized = normalizeEntityKey(key)
        if (normalized.isBlank()) return null
        return entityNames[normalized]
            // The language file still contains a small dotted legacy family
            // (`fish.clownfish`); allow native snake_case input for it too.
            ?: entityNames[normalized.replace('_', '.')]
            ?: canonicalEntityNames[entityCanonicalSignature(normalized)]
    }

    /**
     * Many modern placeable blocks only have an item translation in this
     * version's language file (doors and hanging signs are common examples).
     * Prefer the block wording when present, then safely use that item wording.
     */
    fun blockOrItemName(key: String): String? = blockName(key) ?: itemName(key)

    private fun lookupName(
        key: String,
        names: Map<String, String>,
        canonicalNames: Map<String, String>,
        signatureNames: Map<String, String>
    ): String? {
        val normalized = normalizeKey(key)
        if (normalized.isBlank()) return null
        return names[normalized]
            ?: canonicalNames[canonicalSignature(normalized)]
            ?: signatureNames[tokenSignature(normalized)]
    }

    private fun signatureIndex(names: Map<String, String>): Map<String, String> {
        val result = HashMap<String, String>(names.size)
        val ambiguous = HashSet<String>()
        names.forEach { (key, value) ->
            val signature = tokenSignature(key)
            if (signature.isBlank()) return@forEach
            val previous = result.putIfAbsent(signature, value)
            // Reordered legacy keys are useful only when their source wording
            // agrees.  Do not let an unrelated entry with the same tokens turn
            // an unknown block into an incorrect label.
            if (previous != null && previous != value) ambiguous += signature
        }
        ambiguous.forEach { result.remove(it) }
        return result
    }

    private fun canonicalIndex(names: Map<String, String>): Map<String, String> {
        val result = HashMap<String, String>(names.size)
        val ambiguous = HashSet<String>()
        names.forEach { (key, value) ->
            val canonical = canonicalSignature(key)
            if (canonical.isBlank()) return@forEach
            val previous = result.putIfAbsent(canonical, value)
            if (previous != null && previous != value) ambiguous += canonical
        }
        ambiguous.forEach { result.remove(it) }
        return result
    }

    /**
     * English-only aliases used to normalize *keys*, never display values.
     * Every returned value still comes from an entry in zh_CN.lang.
     */
    private val canonicalExactAliases = mapOf(
        // Older BDX records keep the pre-flattening slab keys.  The targets
        // below are language-table keys, so the displayed Chinese still comes
        // exclusively from the version-matched zh_CN payload.
        "brick_slab" to "stone_slab.brick",
        "brick_double_slab" to "double_stone_slab.brick",
        "cobblestone_slab" to "stone_slab.cobble",
        "cobblestone_double_slab" to "double_stone_slab.cobble",
        "nether_brick_slab" to "stone_slab.nether_brick",
        "nether_brick_double_slab" to "double_stone_slab.nether_brick",
        "quartz_slab" to "stone_slab.quartz",
        "quartz_double_slab" to "double_stone_slab.quartz",
        "sandstone_slab" to "stone_slab.sand",
        "sandstone_double_slab" to "double_stone_slab.sand",
        "smooth_stone_slab" to "stone_slab.stone",
        "smooth_stone_double_slab" to "double_stone_slab.stone",
        "grass" to "grass_block",
        "grass_path" to "dirt_path",
        "dirt_with_roots" to "rooted_dirt",
        "waterlily" to "lily_pad",
        "deadbush" to "dead_bush",
        "reeds" to "sugar_cane",
        "noteblock" to "note_block",
        "brick_block" to "bricks",
        "stonebrick" to "stone_bricks",
        "stonebrick_default" to "stone_bricks",
        "stonebrick_mossy" to "mossy_stone_bricks",
        "stonebrick_chiseled" to "chiseled_stone_bricks",
        "stonebrick_cracked" to "cracked_stone_bricks",
        "end_bricks" to "end_stone_bricks",
        "anvil_intact" to "anvil",
        "anvil_slightly_damaged" to "chipped_anvil",
        "anvil_very_damaged" to "damaged_anvil",
        "deprecated_anvil" to "anvil",
        "purpur_block_default" to "purpur_block",
        "purpur_block_lines" to "purpur_pillar",
        "purpur_block_chiseled" to "chiseled_purpur_block",
        "daylight_detector_inverted" to "daylight_detector",
        "torchflower_crop" to "torchflower",
        "pitcher_crop" to "pitcher_plant",
        "tallgrass_grass" to "short_grass",
        "tallgrass_fern" to "fern",
        "double_plant_grass" to "tall_grass",
        "double_plant_fern" to "large_fern",
        "double_plant_paeonia" to "peony",
        "double_plant_rose" to "rose_bush",
        "double_plant_sunflower" to "sunflower",
        "double_plant_syringa" to "lilac",
        "red_flower_allium" to "allium",
        "red_flower_blue_orchid" to "blue_orchid",
        "red_flower_cornflower" to "cornflower",
        "red_flower_houstonia" to "azure_bluet",
        "red_flower_lily_of_the_valley" to "lily_of_the_valley",
        "red_flower_oxeye_daisy" to "oxeye_daisy",
        "red_flower_poppy" to "poppy",
        "red_flower_tulip_orange" to "orange_tulip",
        "red_flower_tulip_pink" to "pink_tulip",
        "red_flower_tulip_red" to "red_tulip",
        "red_flower_tulip_white" to "white_tulip",
        "yellow_flower_dandelion" to "dandelion",
        "stone_stone" to "stone",
        "stone_andesite" to "andesite",
        "stone_andesite_smooth" to "polished_andesite",
        "stone_diorite" to "diorite",
        "stone_diorite_smooth" to "polished_diorite",
        "stone_granite" to "granite",
        "stone_granite_smooth" to "polished_granite",
        "cobblestone_wall_normal" to "cobblestone_wall",
        "cobblestone_wall_mossy" to "mossy_cobblestone_wall",
        "cobblestone_wall_stone_brick" to "stone_brick_wall",
        "cobblestone_wall_mossy_stone_brick" to "mossy_stone_brick_wall",
        "cobblestone_wall_end_brick" to "end_stone_brick_wall",
        "cobblestone_wall_brick" to "brick_wall",
        "cobblestone_wall_red_sandstone" to "red_sandstone_wall",
        "cobblestone_wall_red_nether_brick" to "red_nether_brick_wall",
        "cobblestone_wall_prismarine" to "prismarine_wall",
        "cobblestone_wall_sandstone" to "sandstone_wall",
        "cobblestone_wall_granite" to "granite_wall",
        "cobblestone_wall_diorite" to "diorite_wall",
        "cobblestone_wall_andesite" to "andesite_wall",
        "monster_egg_stone" to "infested_stone",
        "monster_egg_cobble" to "infested_cobblestone",
        "monster_egg_brick" to "infested_stone_bricks",
        "monster_egg_mossybrick" to "infested_mossy_stone_bricks",
        "monster_egg_crackedbrick" to "infested_cracked_stone_bricks",
        "monster_egg_chiseledbrick" to "infested_chiseled_stone_bricks",
        "crimson_roots_crimson_roots" to "crimson_roots",
        "warped_roots_warped_roots" to "warped_roots",
        "brown_mushroom_block_stem" to "mushroom_stem",
        "brown_mushroom_block_cap" to "brown_mushroom_block",
        "skull_creeper" to "creeper_head",
        "skull_dragon" to "dragon_head",
        "skull_player" to "player_head",
        "skull_skeleton" to "skeleton_skull",
        "skull_wither" to "wither_skeleton_skull",
        "skull_zombie" to "zombie_head",
        "skull_piglin" to "piglin_head",
        "powered_comparator" to "comparator",
        "unpowered_comparator" to "comparator",
        "powered_repeater" to "repeater",
        "unpowered_repeater" to "repeater",
        "seagrass_seagrass" to "seagrass",
        "sponge_dry" to "sponge",
        "quartz_block_lines" to "quartz_pillar",
        "quartz_block_smooth" to "smooth_quartz"
    )

    private val coralColorNames = mapOf(
        "blue" to "tube",
        "pink" to "brain",
        "purple" to "bubble",
        "red" to "fire",
        "yellow" to "horn"
    )

    private fun canonicalSignature(key: String): String {
        var name = normalizeKey(key).replace('.', '_')
        if (name.isBlank()) return ""
        name = normalizeLegacyColor(name)
        if (name.startsWith("stained_hardened_clay_")) {
            name = name.removePrefix("stained_hardened_clay_") + "_terracotta"
        } else if (name == "hardened_clay") {
            name = "terracotta"
        }
        name = canonicalCoralName(name)
        name = canonicalWoodName(name)
        if (name.startsWith("darkoak_")) name = "dark_oak_" + name.removePrefix("darkoak_")
        if (name.startsWith("big_oak_")) name = "dark_oak_" + name.removePrefix("big_oak_")
        if (name.endsWith("_wall_fan")) name = name.removeSuffix("_wall_fan") + "_fan"
        if (name.endsWith("_wall_sign")) name = name.removeSuffix("_wall_sign") + "_standing_sign"
        if (name == "wall_sign") name = "standing_sign"
        // Newer wood families often only expose their sign translation through
        // the corresponding item key (for example `bamboo_sign`).
        if (name.endsWith("_standing_sign")) name = name.removeSuffix("_standing_sign") + "_sign"
        if (name.endsWith("_double_slab")) {
            // Retain a known legacy double-slab mapping when one exists; other
            // families safely fall back to the matching single-slab language key.
            name = canonicalExactAliases[name]
                ?: (name.removeSuffix("_double_slab") + "_slab")
        }
        if (name.startsWith("lit_")) name = name.removePrefix("lit_")
        name = canonicalExactAliases[name] ?: name
        return tokenSignature(name)
    }

    private fun normalizeLegacyColor(name: String): String =
        name.split('_').joinToString(separator = "_") { token ->
            if (token == "silver") "light_gray" else token
        }

    private fun canonicalCoralName(name: String): String {
        fun coralType(color: String): String = coralColorNames[color] ?: color
        return when {
            name.startsWith("coral_fan_dead_") -> {
                val color = name.removePrefix("coral_fan_dead_").removeSuffix("_fan")
                "dead_${coralType(color)}_coral_fan"
            }
            name.startsWith("coral_fan_") -> {
                val color = name.removePrefix("coral_fan_").removeSuffix("_fan")
                "${coralType(color)}_coral_fan"
            }
            name.startsWith("coral_block_") -> {
                val state = name.removePrefix("coral_block_")
                val dead = state.endsWith("_dead")
                val color = state.removeSuffix("_dead")
                (if (dead) "dead_" else "") + coralType(color) + "_coral_block"
            }
            name.startsWith("coral_") -> {
                val state = name.removePrefix("coral_")
                val dead = state.endsWith("_dead")
                val color = state.removeSuffix("_dead")
                (if (dead) "dead_" else "") + coralType(color) + "_coral"
            }
            else -> name
        }
    }

    private fun canonicalWoodName(name: String): String {
        fun woodVariant(value: String): String =
            if (value == "big_oak") "dark_oak" else value
        return when {
            name.startsWith("sapling_") -> woodVariant(name.removePrefix("sapling_")) + "_sapling"
            name.startsWith("planks_") -> woodVariant(name.removePrefix("planks_")) + "_planks"
            name.startsWith("leaves2_") -> woodVariant(name.removePrefix("leaves2_")) + "_leaves"
            name.startsWith("leaves_") -> woodVariant(name.removePrefix("leaves_")) + "_leaves"
            name.startsWith("wooden_slab_") -> woodVariant(name.removePrefix("wooden_slab_")) + "_slab"
            name.startsWith("log_") -> woodVariant(name.removePrefix("log_")) + "_log"
            name.startsWith("wood_stripped_") ->
                "stripped_" + woodVariant(name.removePrefix("wood_stripped_")) + "_wood"
            name.startsWith("wood_") -> woodVariant(name.removePrefix("wood_")) + "_wood"
            else -> name
        }
    }

    private fun tokenSignature(key: String): String =
        key.split('.', '_')
            .filter { it.isNotBlank() }
            .sorted()
            .joinToString(separator = "_")

    private fun normalizeKey(key: String): String =
        key.trim()
            .replace(Regex("([a-z0-9])([A-Z])"), "$1_$2")
            .replace('-', '_')
            .lowercase(Locale.ROOT)

    private fun normalizeEntityKey(key: String): String =
        key.trim()
            .removePrefix("minecraft:")
            .removePrefix("entity.")
            .removeSuffix(".name")
            .replace(Regex("([A-Z]+)([A-Z][a-z])"), "$1_$2")
            .replace(Regex("([a-z0-9])([A-Z])"), "$1_$2")
            .replace('-', '_')
            .lowercase(Locale.ROOT)

    private fun entityCanonicalSignature(key: String): String =
        normalizeEntityKey(key).replace(Regex("[^a-z0-9]+"), "")
}
