#include "../ExecuteCommandConverter.h"

#include <cassert>
#include <string>

using namespace build_import;

namespace {

void testBasicLegacyExecuteWithQuotedSelector() {
    std::string command =
        "execute @e[type=minecraft:armor_stand,name=\"two words\",scores={ticks=1..}] "
        "~ ~1 ~-2 tellraw @s {\"rawtext\":[{\"text\":\"[unchanged body]\"}]}";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @e[type=minecraft:armor_stand,name=\"two words\",scores={ticks=1..}] "
        "at @s positioned ~ ~1 ~-2 run tellraw @s "
        "{\"rawtext\":[{\"text\":\"[unchanged body]\"}]}");
}

void testLegacyDetectColoredBlock() {
    std::string command =
        "/execute @p ~ ~ ~ detect ~ ~-1 ~ minecraft:wool 0 effect @p minecraft:speed 10 5";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "/execute as @p at @s positioned ~ ~ ~ if block ~ ~-1 ~ minecraft:white_wool "
        "run effect @p minecraft:speed 10 5");
}

void testLegacyDetectWildcardBlockData() {
    std::string command =
        "execute @e[type=minecraft:zombie] ~ ~ ~ detect ~ ~-1 ~ minecraft:stone -1 say stone";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @e[type=minecraft:zombie] at @s positioned ~ ~ ~ if block ~ ~-1 ~ "
        "minecraft:stone run say stone");
}

void testNestedLegacyExecute() {
    std::string command =
        "execute @p ~ ~ ~ execute @e[type=minecraft:armor_stand] ~ ~ ~ say nested";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @p at @s positioned ~ ~ ~ run execute as "
        "@e[type=minecraft:armor_stand] at @s positioned ~ ~ ~ run say nested");
}

void testNestedLegacyExecuteWithSlash() {
    std::string command =
        "execute @p ~ ~ ~ /execute @e[type=minecraft:armor_stand] ~ ~ ~ say nested slash";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @p at @s positioned ~ ~ ~ run execute as "
        "@e[type=minecraft:armor_stand] at @s positioned ~ ~ ~ run say nested slash");
}

void testCompactBdxCoordinatesAttachToSelectorAndBody() {
    // BDX exports from older music-map tools preserve dispatcher-friendly
    // compact coordinates. In particular there is no separator after `]` or
    // after the last `~`, so a whitespace-only tokenizer would retain this
    // command unchanged.
    std::string command =
        "execute @a[scores={jf_time=0,jf_cl=9}] ~~~tellraw @s "
        "{\"rawtext\":[{\"text\":\"start\"}]}";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @a[scores={jf_time=0,jf_cl=9}] at @s positioned ~ ~ ~ run "
        "tellraw @s {\"rawtext\":[{\"text\":\"start\"}]}");

    command =
        "execute @a[scores={jf_time=0,jf_cl=9}]~~~scoreboard players add @s jf_time 6";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @a[scores={jf_time=0,jf_cl=9}] at @s positioned ~ ~ ~ run "
        "scoreboard players add @s jf_time 6");
}

void testCompactBdxNestedExecuteAndLocalCoordinates() {
    std::string command =
        "execute @e[type=armor_stand,tag=mm_music]~~~execute "
        "@a[scores={jf_time=6,jf_cl=9}] ^^^playsound note.harp @s ^^1^ 0.97 0.59 1";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @e[type=armor_stand,tag=mm_music] at @s positioned ~ ~ ~ run "
        "execute as @a[scores={jf_time=6,jf_cl=9}] at @s positioned ^ ^ ^ run "
        "playsound note.harp @s ^^1^ 0.97 0.59 1");
}

void testCompactBdxDetectCoordinates() {
    std::string command =
        "execute @p~~~detect~~~minecraft:wool 0 say compact detect";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @p at @s positioned ~ ~ ~ if block ~ ~ ~ minecraft:white_wool run "
        "say compact detect");
}

void testMalformedCompactLegacyExecuteIsPreserved() {
    std::string command = "execute @a[scores={jf_time=0}]~~~";
    const std::string original = command;
    assert(!normalizeLegacyExecuteCommand(&command));
    assert(command == original);

    command = "execute @a[scores={jf_time=0}~~~say incomplete selector";
    const std::string malformed_selector = command;
    assert(!normalizeLegacyExecuteCommand(&command));
    assert(command == malformed_selector);
}

void testLegacyDetectRetainsSupportedDataValue() {
    std::string command =
        "execute @p ~ ~ ~ detect ~ ~-1 ~ minecraft:log 4 say keep data";
    assert(normalizeLegacyExecuteCommand(&command));
    assert(command ==
        "execute as @p at @s positioned ~ ~ ~ if block ~ ~-1 ~ minecraft:log 4 "
        "run say keep data");
}

void testNestedLegacyFailurePreservesWholeCommand() {
    std::string command =
        "execute @p ~ ~ ~ execute @e[type=minecraft:zombie] ~ ~ ~ "
        "detect ~ ~-1 ~ minecraft:stone 16 say unsupported";
    const std::string original = command;
    assert(!normalizeLegacyExecuteCommand(&command));
    assert(command == original);
}

void testNestedLegacyDepthLimitPreservesWholeCommand() {
    std::string command = "say finished";
    // The root plus sixteen nested execute commands is the supported bound.
    // Add one more nested layer to verify the converter rejects the whole
    // command instead of leaving a legacy execute behind `run`.
    for (int index = 0; index < 18; ++index) {
        command = "execute @p ~ ~ ~ " + command;
    }
    const std::string original = command;
    assert(!normalizeLegacyExecuteCommand(&command));
    assert(command == original);
}

void testModernAndInvalidCommandsArePreserved() {
    std::string modern = "execute as @p at @s run say already new";
    assert(!normalizeLegacyExecuteCommand(&modern));
    assert(modern == "execute as @p at @s run say already new");

    std::string unsupported_detect =
        "execute @p ~ ~ ~ detect ~ ~-1 ~ minecraft:log 16 say preserve me";
    assert(!normalizeLegacyExecuteCommand(&unsupported_detect));
    assert(unsupported_detect ==
        "execute @p ~ ~ ~ detect ~ ~-1 ~ minecraft:log 16 say preserve me");

    std::string incomplete = "execute @p ~ ~ ~";
    assert(!normalizeLegacyExecuteCommand(&incomplete));
    assert(incomplete == "execute @p ~ ~ ~");
}

}  // namespace

int main() {
    testBasicLegacyExecuteWithQuotedSelector();
    testLegacyDetectColoredBlock();
    testLegacyDetectWildcardBlockData();
    testNestedLegacyExecute();
    testNestedLegacyExecuteWithSlash();
    testCompactBdxCoordinatesAttachToSelectorAndBody();
    testCompactBdxNestedExecuteAndLocalCoordinates();
    testCompactBdxDetectCoordinates();
    testMalformedCompactLegacyExecuteIsPreserved();
    testLegacyDetectRetainsSupportedDataValue();
    testNestedLegacyFailurePreservesWholeCommand();
    testNestedLegacyDepthLimitPreservesWholeCommand();
    testModernAndInvalidCommandsArePreserved();
    return 0;
}
