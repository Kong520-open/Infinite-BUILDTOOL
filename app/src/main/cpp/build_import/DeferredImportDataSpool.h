#ifndef INFINITE_TEXTURE_DEFERRED_IMPORT_DATA_SPOOL_H
#define INFINITE_TEXTURE_DEFERRED_IMPORT_DATA_SPOOL_H

#include "BuildImportTypes.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace build_import {

// Source blueprint NBT is deliberately reduced to the small set of fields the
// importer can safely recreate.  These records never contain arbitrary NBT,
// commands, nested inventories, UUIDs, AI data, or player data.
struct DeferredEnchantment {
    std::string id;
    uint8_t level = 0;
};

struct ContainerItemRecord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t slot = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    // The parser only records an item after the source voxel stream proves a
    // container shell exists at this location.  The runtime checks the loaded
    // target again before it sends /replaceitem.
    std::string expected_container_id;
    // Always a canonical namespaced command identifier. Session runtime IDs
    // are resolved through ItemRuntimeRegistry before a record is created.
    std::string item_id;
    std::vector<DeferredEnchantment> enchantments;
};

// Presence is tracked independently from the value so a missing source field
// is never rewritten as an explicit default during sign restoration.
struct SignTextRecord {
    bool present = false;
    bool has_text = false;
    std::string text;
    bool has_text_color = false;
    int32_t text_color = 0;
    bool has_ignore_lighting = false;
    bool ignore_lighting = false;
    bool has_persist_formatting = false;
    bool persist_formatting = false;
    bool has_hide_glow_outline = false;
    bool hide_glow_outline = false;
    // Kept in the v1 sidecar layout for bounded backward decoding only. This
    // is a legacy migration marker, not visual sign state, so codecs and the
    // packet sender deliberately do not export, replay, or verify it.
    bool has_text_ignore_legacy_bug_resolved = false;
    bool text_ignore_legacy_bug_resolved = false;
};

struct SignRecord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    // The block shell is authoritative for standing/wall/hanging type and
    // orientation. Import verifies both before applying text NBT. Older v1
    // sidecars omit expected_aux and remain readable without guessing it.
    std::string expected_sign_id;
    bool has_expected_aux = false;
    uint16_t expected_aux = 0;
    SignTextRecord front;
    SignTextRecord back;
    bool has_is_waxed = false;
    bool is_waxed = false;
};

struct EntityRecord {
    std::string entity_id;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    float yaw = 0.0F;
    float pitch = 0.0F;
    // This is a plain display name only.  JSON text, selectors, control
    // characters and unbounded source text are normalized before it reaches
    // the command formatter.
    std::string custom_name;
};

// Normalizers are shared by parsers so malformed or hostile source NBT cannot
// become a command token later in the post-import phase.
bool normalizeDeferredItemIdentifier(std::string* identifier);
bool parseDeferredNumericItemIdentifier(std::string_view identifier,
                                        int32_t* numeric_id = nullptr);
bool normalizeDeferredItemToken(std::string* identifier);
bool normalizeDeferredEntityIdentifier(std::string* identifier);
bool normalizeDeferredEnchantmentIdentifier(std::string* identifier);
bool normalizeDeferredEntityName(std::string* value);
bool normalizeDeferredEnchantment(std::string* identifier, int64_t level,
                                   DeferredEnchantment* output);
bool deferredContainerIdentifier(std::string_view identifier);
// True only for block containers that open a normal server-backed inventory
// window and can therefore be captured through ContainerOpen/InventoryContent.
bool packetCapturableContainerIdentifier(std::string_view identifier);
bool deferredSignIdentifier(std::string_view identifier);
bool deferredHangingSignIdentifier(std::string_view identifier);
bool deferredSignShellMatches(std::string_view expected_identifier,
                              std::string_view actual_identifier);

class ContainerItemSpoolWriter {
public:
    static constexpr const char* kFileName = "container_items.dis";
    static constexpr uint32_t kVersion = 1;
    static constexpr uint64_t kMaximumRecords = 2ULL * 1024ULL * 1024ULL;
    static constexpr uint64_t kMaximumDataBytes = 128ULL * 1024ULL * 1024ULL;

    explicit ContainerItemSpoolWriter(std::string directory,
                                      std::string file_name = kFileName);
    ~ContainerItemSpoolWriter();

    ContainerItemSpoolWriter(const ContainerItemSpoolWriter&) = delete;
    ContainerItemSpoolWriter& operator=(const ContainerItemSpoolWriter&) = delete;

    bool append(const ContainerItemRecord& record, std::string* error = nullptr);
    bool finish(std::string* spool_path, std::string* error = nullptr);
    void discard();
    uint64_t recordCount() const;

private:
    bool ensureOpenLocked(std::string* error);
    void discardLocked(bool remove_final);

    mutable std::mutex mutex_;
    std::string directory_;
    std::string spool_path_;
    std::string temporary_path_;
    std::ofstream output_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    bool opened_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

class ContainerItemSpoolReader {
public:
    explicit ContainerItemSpoolReader(std::string path);

    bool valid() const { return valid_; }
    bool failed() const { return failed_; }
    uint64_t recordCount() const { return record_count_; }
    uint64_t cursor() const { return cursor_; }
    bool seekRecord(uint64_t record_index);
    std::optional<ContainerItemRecord> next();

private:
    bool readHeaderLocked();
    bool readRecordLocked(ContainerItemRecord* record);
    bool skipRecordLocked();
    bool readBytesLocked(void* output, size_t count);
    bool skipBytesLocked(uint64_t count);
    bool resetLocked();
    void failLocked();

    mutable std::mutex mutex_;
    std::string path_;
    std::ifstream input_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    uint64_t consumed_bytes_ = 0;
    uint64_t cursor_ = 0;
    std::streampos data_start_{};
    bool valid_ = false;
    bool failed_ = false;
};

class SignSpoolWriter {
public:
    static constexpr const char* kFileName = "signs.dis";
    static constexpr uint32_t kVersion = 1;
    static constexpr uint64_t kMaximumRecords = 512ULL * 1024ULL;
    static constexpr uint64_t kMaximumDataBytes = 64ULL * 1024ULL * 1024ULL;
    static constexpr size_t kMaximumTextBytes = 64U * 1024U;

    explicit SignSpoolWriter(std::string directory,
                             std::string file_name = kFileName);
    ~SignSpoolWriter();

    SignSpoolWriter(const SignSpoolWriter&) = delete;
    SignSpoolWriter& operator=(const SignSpoolWriter&) = delete;

    bool append(const SignRecord& record, std::string* error = nullptr);
    bool finish(std::string* spool_path, std::string* error = nullptr);
    void discard();
    uint64_t recordCount() const;

private:
    bool ensureOpenLocked(std::string* error);
    void discardLocked(bool remove_final);

    mutable std::mutex mutex_;
    std::string directory_;
    std::string spool_path_;
    std::string temporary_path_;
    std::ofstream output_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    bool opened_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

class SignSpoolReader {
public:
    explicit SignSpoolReader(std::string path);

    bool valid() const { return valid_; }
    bool failed() const { return failed_; }
    uint64_t recordCount() const { return record_count_; }
    uint64_t cursor() const { return cursor_; }
    bool seekRecord(uint64_t record_index);
    std::optional<SignRecord> next();

private:
    bool readHeaderLocked();
    bool readRecordLocked(SignRecord* record);
    bool skipRecordLocked();
    bool readBytesLocked(void* output, size_t count);
    bool resetLocked();
    void failLocked();

    mutable std::mutex mutex_;
    std::string path_;
    std::ifstream input_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    uint64_t consumed_bytes_ = 0;
    uint64_t cursor_ = 0;
    std::streampos data_start_{};
    bool valid_ = false;
    bool failed_ = false;
};

class EntitySpoolWriter {
public:
    static constexpr const char* kFileName = "entities.dis";
    static constexpr uint32_t kVersion = 1;
    static constexpr uint64_t kMaximumRecords = 512ULL * 1024ULL;
    static constexpr uint64_t kMaximumDataBytes = 64ULL * 1024ULL * 1024ULL;

    explicit EntitySpoolWriter(std::string directory, std::string file_name = kFileName);
    ~EntitySpoolWriter();

    EntitySpoolWriter(const EntitySpoolWriter&) = delete;
    EntitySpoolWriter& operator=(const EntitySpoolWriter&) = delete;

    bool append(const EntityRecord& record, std::string* error = nullptr);
    bool finish(std::string* spool_path, std::string* error = nullptr);
    void discard();
    uint64_t recordCount() const;

private:
    bool ensureOpenLocked(std::string* error);
    void discardLocked(bool remove_final);

    mutable std::mutex mutex_;
    std::string directory_;
    std::string spool_path_;
    std::string temporary_path_;
    std::ofstream output_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    bool opened_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

class EntitySpoolReader {
public:
    explicit EntitySpoolReader(std::string path);

    bool valid() const { return valid_; }
    bool failed() const { return failed_; }
    uint64_t recordCount() const { return record_count_; }
    uint64_t cursor() const { return cursor_; }
    bool seekRecord(uint64_t record_index);
    std::optional<EntityRecord> next();

private:
    bool readHeaderLocked();
    bool readRecordLocked(EntityRecord* record);
    bool skipRecordLocked();
    bool readBytesLocked(void* output, size_t count);
    bool skipBytesLocked(uint64_t count);
    bool resetLocked();
    void failLocked();

    mutable std::mutex mutex_;
    std::string path_;
    std::ifstream input_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    uint64_t consumed_bytes_ = 0;
    uint64_t cursor_ = 0;
    std::streampos data_start_{};
    bool valid_ = false;
    bool failed_ = false;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_DEFERRED_IMPORT_DATA_SPOOL_H
