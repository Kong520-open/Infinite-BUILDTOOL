#include "ContainerEntityCodec.h"

#include "ItemRuntimeRegistry.h"

#include "../Json/cJSON.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace build_import {
namespace {

constexpr size_t kMaximumEntityJsonBytes = 64U * 1024U * 1024U;
constexpr size_t kMaximumItemsPerEntity = 256U;

struct JsonDeleter {
    void operator()(cJSON* value) const noexcept {
        if (value) cJSON_Delete(value);
    }
};

using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

cJSON* firstObjectItem(cJSON* object, const char* const* names, size_t count) {
    if (!object || !cJSON_IsObject(object)) return nullptr;
    for (size_t index = 0; index < count; ++index) {
        cJSON* value = cJSON_GetObjectItemCaseSensitive(object, names[index]);
        if (value) return value;
    }
    return nullptr;
}

bool readInteger(cJSON* value, int64_t minimum, int64_t maximum, int64_t* output) {
    if (!value || !output || !cJSON_IsNumber(value) ||
        !std::isfinite(value->valuedouble)) return false;
    const double number = value->valuedouble;
    if (std::floor(number) != number || number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum)) return false;
    *output = static_cast<int64_t>(number);
    return true;
}

bool readItem(cJSON* value, ContainerItemRecord* output,
              bool* unsupported_numeric_id) {
    if (unsupported_numeric_id) *unsupported_numeric_id = false;
    if (!value || !output || !cJSON_IsObject(value)) return false;
    static constexpr const char* kSlotNames[] = {"Slot", "slot", "slotId"};
    static constexpr const char* kCountNames[] = {"Count", "count", "num"};
    static constexpr const char* kAuxNames[] = {
        "Damage", "damage", "aux", "Aux", "auxValue", "newAuxValue", "data"};
    static constexpr const char* kIdNames[] = {
        "Name", "name", "itemName", "newItemName", "id", "Id"};
    static constexpr const char* kNumericIdNames[] = {
        "NumericId", "numericId", "NetworkId", "networkId"};
    cJSON* slot_value = firstObjectItem(value, kSlotNames, sizeof(kSlotNames) / sizeof(*kSlotNames));
    cJSON* count_value = firstObjectItem(value, kCountNames,
                                         sizeof(kCountNames) / sizeof(*kCountNames));
    cJSON* id_value = firstObjectItem(value, kIdNames, sizeof(kIdNames) / sizeof(*kIdNames));
    cJSON* numeric_id_value = firstObjectItem(
        value, kNumericIdNames, sizeof(kNumericIdNames) / sizeof(*kNumericIdNames));

    int64_t slot = 0;
    int64_t count = 0;
    if (!readInteger(slot_value, 0, 255, &slot) ||
        !readInteger(count_value, 1, 64, &count)) return false;

    int64_t aux = 0;
    cJSON* aux_value = firstObjectItem(value, kAuxNames,
                                       sizeof(kAuxNames) / sizeof(*kAuxNames));
    if (aux_value && !readInteger(aux_value, 0, UINT16_MAX, &aux)) return false;

    std::string item_id;
    if (cJSON_IsString(id_value) && id_value->valuestring) {
        item_id = id_value->valuestring;
        if (!normalizeDeferredItemIdentifier(&item_id)) item_id.clear();
    }
    if (item_id.empty()) {
        if (numeric_id_value && unsupported_numeric_id) {
            *unsupported_numeric_id = true;
        }
        return false;
    }
    output->slot = static_cast<uint16_t>(slot);
    output->count = static_cast<uint16_t>(count);
    output->aux = static_cast<uint16_t>(aux);
    output->item_id = std::move(item_id);
    output->enchantments.clear();
    return true;
}

JsonPtr parseJson(std::string_view entity_json, std::string* error) {
    if (entity_json.empty() || entity_json.size() > kMaximumEntityJsonBytes ||
        entity_json.find('\0') != std::string_view::npos) {
        if (error) *error = "container entity JSON is empty, oversized, or contains NUL";
        return {};
    }
    // cJSON requires a NUL-terminated input. Keep the copy local so callers
    // can pass a view into a larger payload without modifying it.
    std::string input(entity_json);
    const char* parse_end = nullptr;
    JsonPtr root(cJSON_ParseWithLengthOpts(input.c_str(), input.size() + 1U,
                                           &parse_end, 1));
    if (!root || !cJSON_IsObject(root.get()) ||
        (parse_end && *parse_end != '\0')) {
        if (error) *error = "container entity JSON is not an object";
        return {};
    }
    return root;
}

}  // namespace

bool parseContainerEntityJson(std::string_view entity_json,
                              std::vector<ContainerItemRecord>* output,
                              std::string* error) {
    if (error) error->clear();
    if (!output) return fail(error, "container item output is missing");
    output->clear();
    try {
        JsonPtr root = parseJson(entity_json, error);
        if (!root) return false;
        static constexpr const char* kItemsNames[] = {"Items", "items"};
        cJSON* items = firstObjectItem(root.get(), kItemsNames,
                                       sizeof(kItemsNames) / sizeof(*kItemsNames));
        if (!items) return true;
        if (!cJSON_IsArray(items)) return fail(error, "container Items field is not an array");
        const int item_count = cJSON_GetArraySize(items);
        if (item_count < 0 || static_cast<size_t>(item_count) > kMaximumItemsPerEntity) {
            return fail(error, "container Items array is too large");
        }
        output->reserve(static_cast<size_t>(item_count));
        for (int index = 0; index < item_count; ++index) {
            ContainerItemRecord record;
            bool unsupported_numeric_id = false;
            if (readItem(cJSON_GetArrayItem(items, index), &record,
                         &unsupported_numeric_id)) {
                output->push_back(std::move(record));
            } else if (unsupported_numeric_id) {
                output->clear();
                return fail(error,
                    "numeric-only container items are not portable; re-export with an item registry");
            }
        }
    } catch (const std::bad_alloc&) {
        output->clear();
        return fail(error, "not enough memory to parse container entity JSON");
    }
    return true;
}

bool normalizeContainerEntityJson(std::string_view entity_json,
                                  std::string* normalized_json,
                                  std::string* error) {
    if (error) error->clear();
    if (!normalized_json) return fail(error, "normalized container JSON output is missing");
    normalized_json->clear();
    std::vector<ContainerItemRecord> items;
    if (!parseContainerEntityJson(entity_json, &items, error)) return false;

    return encodeContainerItemsJson(items, normalized_json, error);
}

bool encodeContainerItemsJson(const std::vector<ContainerItemRecord>& items,
                              std::string* entity_json,
                              std::string* error) {
    if (error) error->clear();
    if (!entity_json) return fail(error, "container entity JSON output is missing");
    entity_json->clear();
    if (items.size() > kMaximumItemsPerEntity) {
        return fail(error, "container Items array is too large");
    }

    JsonPtr root(cJSON_CreateObject());
    cJSON* array = root ? cJSON_CreateArray() : nullptr;
    if (!root || !array) {
        if (array) cJSON_Delete(array);
        return fail(error, "cannot create normalized container JSON");
    }
    if (!cJSON_AddItemToObject(root.get(), "Items", array)) {
        cJSON_Delete(array);
        return fail(error, "cannot create normalized container JSON");
    }
    for (const ContainerItemRecord& item : items) {
        cJSON* value = cJSON_CreateObject();
        std::string item_id = item.item_id;
        int32_t numeric_id = 0;
        if (parseDeferredNumericItemIdentifier(item_id, &numeric_id) ||
            !normalizeDeferredItemIdentifier(&item_id)) {
            if (value) cJSON_Delete(value);
            return fail(error, "container item has no canonical namespaced identifier");
        }
        if (!value || !cJSON_AddNumberToObject(value, "Slot", item.slot)) {
            if (value) cJSON_Delete(value);
            return fail(error, "cannot create normalized container item JSON");
        }
        const bool id_added =
            cJSON_AddStringToObject(value, "Name", item_id.c_str()) != nullptr;
        if (!id_added || !cJSON_AddNumberToObject(value, "Count", item.count) ||
            !cJSON_AddNumberToObject(value, "Damage", item.aux) ||
            !cJSON_AddItemToArray(array, value)) {
            if (value) cJSON_Delete(value);
            return fail(error, "cannot create normalized container item JSON");
        }
    }
    char* printed = cJSON_PrintUnformatted(root.get());
    if (!printed) return fail(error, "cannot serialize normalized container JSON");
    try {
        entity_json->assign(printed);
    } catch (...) {
        cJSON_free(printed);
        return fail(error, "not enough memory for normalized container JSON");
    }
    cJSON_free(printed);
    return true;
}

bool encodeCapturedContainerItemsJson(const ContainerCaptureResult& capture,
                                      std::string* entity_json,
                                      std::string* error) {
    if (error) error->clear();
    if (capture.slot_count == 0U ||
        capture.slot_count > kMaximumCapturedContainerSlots) {
        return fail(error, "container capture has an invalid slot count");
    }
    if (capture.slot_count == 54U) {
        return fail(error,
                    "double-chest capture is not supported yet because its 54 slots "
                    "cannot be assigned safely to one chest half");
    }

    std::array<bool, kMaximumCapturedContainerSlots> seen_slots{};
    std::vector<ContainerItemRecord> items;
    try {
        items.reserve(capture.items.size());
        for (const CapturedContainerItem& captured : capture.items) {
            if (captured.slot >= capture.slot_count || seen_slots[captured.slot] ||
                captured.numeric_id == 0 || captured.count == 0U ||
                captured.count > 64U) {
                return fail(error,
                            "container capture contains an invalid item slot, ID, or count");
            }
            seen_slots[captured.slot] = true;
            ContainerItemRecord item;
            item.slot = captured.slot;
            item.count = captured.count;
            item.aux = captured.aux;
            const ItemRuntimeResolveStatus resolve_status = ResolveItemRuntimeId(
                captured.numeric_id, &item.item_id);
            if (resolve_status == ItemRuntimeResolveStatus::RegistryUnavailable) {
                return fail(error,
                    "item registry packet is unavailable; leave and re-enter the world before resuming export");
            }
            if (resolve_status == ItemRuntimeResolveStatus::UnknownRuntimeId) {
                if (error) {
                    *error = "item runtime ID " + std::to_string(captured.numeric_id) +
                        " is absent from the current item registry";
                }
                return false;
            }
            items.push_back(std::move(item));
        }
        std::sort(items.begin(), items.end(),
                  [](const ContainerItemRecord& left,
                     const ContainerItemRecord& right) {
                      return left.slot < right.slot;
                  });
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for captured container items");
    }
    return encodeContainerItemsJson(items, entity_json, error);
}

std::string formatContainerTeleportCommand(int32_t x, int32_t y, int32_t z) {
    const int32_t above_y = y == std::numeric_limits<int32_t>::max() ? y : y + 1;
    return "/tp @s " + std::to_string(x) + " " + std::to_string(above_y) + " " +
        std::to_string(z);
}

std::string formatContainerReplaceItemCommand(const ContainerItemRecord& record) {
    std::string item_id = record.item_id;
    int32_t numeric_id = 0;
    if (parseDeferredNumericItemIdentifier(item_id, &numeric_id) ||
        !normalizeDeferredItemIdentifier(&item_id) || item_id != record.item_id) {
        return {};
    }
    return "/replaceitem block " + std::to_string(record.x) + " " +
        std::to_string(record.y) + " " + std::to_string(record.z) +
        " slot.container " + std::to_string(record.slot) + " " + item_id + " " +
        std::to_string(record.count) + " " + std::to_string(record.aux);
}

}  // namespace build_import
