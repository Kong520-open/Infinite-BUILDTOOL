#ifndef INFINITE_TEXTURE_CONTAINER_ENTITY_CODEC_H
#define INFINITE_TEXTURE_CONTAINER_ENTITY_CODEC_H

#include "ContainerCaptureMailbox.h"
#include "DeferredImportDataSpool.h"

#include <string>
#include <string_view>
#include <vector>

namespace build_import {

// Reads the inventory portion of a Bedrock block-entity JSON snapshot.  The
// source may contain arbitrary NBT-like fields, but only these four fields are
// retained in the deferred import record. Invalid item entries are ignored;
// malformed JSON, a non-object root, or a non-array Items field is rejected.
bool parseContainerEntityJson(std::string_view entity_json,
                              std::vector<ContainerItemRecord>* output,
                              std::string* error = nullptr);

// Produces the canonical minimal representation used by .infinity files.
// The result contains only an `Items` array. Every item uses its canonical
// namespaced Name plus Slot, Count and Damage; runtime IDs are never persisted.
bool normalizeContainerEntityJson(std::string_view entity_json,
                                  std::string* normalized_json,
                                  std::string* error = nullptr);

// Serializes already validated capture records into the same minimal payload
// used by normalizeContainerEntityJson(). Coordinate and import-only fields in
// ContainerItemRecord are deliberately ignored.
bool encodeContainerItemsJson(const std::vector<ContainerItemRecord>& items,
                               std::string* entity_json,
                               std::string* error = nullptr);

// Resolves and serializes records captured directly from InventoryContent.
// The current ItemRuntimeRegistry must have been observed before this call.
bool encodeCapturedContainerItemsJson(const ContainerCaptureResult& capture,
                                      std::string* entity_json,
                                      std::string* error = nullptr);

// Command formatting is kept beside the normalized record contract so the
// exact Bedrock syntax can be covered by host/NDK tests independently of the
// game-thread runtime.
std::string formatContainerTeleportCommand(int32_t x, int32_t y, int32_t z);
std::string formatContainerReplaceItemCommand(const ContainerItemRecord& record);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_CONTAINER_ENTITY_CODEC_H
