#include "ContainerEntityCodec.h"

#include <cassert>
#include <string>
#include <vector>

using namespace build_import;

int main() {
    const std::string source =
        R"({"Items":[{"Slot":2,"Name":"STONE","Count":3,"Damage":5,"tag":{"display":{"Name":"ignored"}}},)"
        R"({"slot":7,"itemName":"minecraft:oak_planks","count":1,"auxValue":12,"enchants":[1]},)"
        R"({"Slot":8,"Name":"minecraft:diamond","Count":0,"Damage":0},)"
        R"({"Slot":9,"Name":"minecraft:bad id","Count":1,"Damage":0}]})";
    std::vector<ContainerItemRecord> records;
    std::string error;
    assert(parseContainerEntityJson(source, &records, &error));
    assert(error.empty());
    assert(records.size() == 2);
    assert(records[0].slot == 2 && records[0].count == 3 && records[0].aux == 5 &&
           records[0].item_id == "minecraft:stone" && records[0].enchantments.empty());
    assert(records[1].slot == 7 && records[1].count == 1 && records[1].aux == 12 &&
           records[1].item_id == "minecraft:oak_planks");

    std::string normalized;
    assert(normalizeContainerEntityJson(source, &normalized, &error));
    assert(normalized ==
           R"({"Items":[{"Slot":2,"Name":"minecraft:stone","Count":3,"Damage":5},{"Slot":7,"Name":"minecraft:oak_planks","Count":1,"Damage":12}]})");
    std::string captured;
    assert(encodeContainerItemsJson(records, &captured, &error));
    assert(captured == normalized);

    records.clear();
    assert(parseContainerEntityJson(
        R"({"items":[{"slotId":1,"newItemName":"apple","num":4,"newAuxValue":0}]})",
        &records, &error));
    assert(records.size() == 1 && records[0].item_id == "minecraft:apple");

    assert(!parseContainerEntityJson(
        R"({"Items":[{"Slot":4,"NumericId":-17,"Count":2,"Damage":31}]})",
        &records, &error));
    assert(error.find("not portable") != std::string::npos);

    assert(!parseContainerEntityJson("[]", &records, &error));
    assert(!parseContainerEntityJson(R"({"Items":{}})", &records, &error));

    ContainerItemRecord command_record;
    command_record.x = -12;
    command_record.y = 64;
    command_record.z = 30;
    command_record.slot = 7;
    command_record.item_id = "minecraft:diamond_sword";
    command_record.count = 1;
    command_record.aux = 23;
    assert(formatContainerTeleportCommand(command_record.x, command_record.y,
                                          command_record.z) ==
           "/tp @s -12 65 30");
    assert(formatContainerReplaceItemCommand(command_record) ==
           "/replaceitem block -12 64 30 slot.container 7 minecraft:diamond_sword 1 23");

    command_record.item_id = "-17";
    assert(formatContainerReplaceItemCommand(command_record).empty());
    return 0;
}
