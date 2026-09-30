#include "SignBlockActorPacketSender.h"

#include "SignBlockActorPacketAbiProfile.h"

#if defined(__aarch64__)
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace build_import {
namespace {

bool fail(std::string* error, std::string detail) {
    if (error) *error = std::move(detail);
    return false;
}

bool validUtf8(std::string_view value) {
    size_t cursor = 0;
    while (cursor < value.size()) {
        const uint8_t first = static_cast<uint8_t>(value[cursor]);
        if (first == 0U) return false;
        if (first <= 0x7FU) {
            ++cursor;
            continue;
        }
        const auto continuation = [&value](size_t index) {
            return index < value.size() &&
                (static_cast<uint8_t>(value[index]) & 0xC0U) == 0x80U;
        };
        if (first >= 0xC2U && first <= 0xDFU) {
            if (!continuation(cursor + 1U)) return false;
            cursor += 2U;
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU) {
            if (!continuation(cursor + 1U) || !continuation(cursor + 2U)) return false;
            const uint8_t second = static_cast<uint8_t>(value[cursor + 1U]);
            if ((first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second >= 0xA0U)) return false;
            cursor += 3U;
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U) {
            if (!continuation(cursor + 1U) || !continuation(cursor + 2U) ||
                !continuation(cursor + 3U)) return false;
            const uint8_t second = static_cast<uint8_t>(value[cursor + 1U]);
            if ((first == 0xF0U && second < 0x90U) ||
                (first == 0xF4U && second >= 0x90U)) return false;
            cursor += 4U;
            continue;
        }
        return false;
    }
    return true;
}

bool validFace(const SignTextRecord& face, size_t* text_bytes) {
    if (!face.present) {
        return !face.has_text && face.text.empty() && !face.has_text_color &&
            face.text_color == 0 && !face.has_ignore_lighting &&
            !face.ignore_lighting && !face.has_persist_formatting &&
            !face.persist_formatting && !face.has_hide_glow_outline &&
            !face.hide_glow_outline &&
            !face.has_text_ignore_legacy_bug_resolved &&
            !face.text_ignore_legacy_bug_resolved;
    }
    if (!face.has_text && !face.text.empty()) return false;
    if (face.has_text &&
        (face.text.size() > SignBlockActorPacketSender::kMaximumFaceTextBytes ||
         !validUtf8(face.text))) return false;
    if ((!face.has_text_color && face.text_color != 0) ||
        (!face.has_ignore_lighting && face.ignore_lighting) ||
        (!face.has_persist_formatting && face.persist_formatting) ||
        (!face.has_hide_glow_outline && face.hide_glow_outline) ||
        (!face.has_text_ignore_legacy_bug_resolved &&
         face.text_ignore_legacy_bug_resolved)) return false;
    if (text_bytes && face.has_text) *text_bytes += face.text.size();
    return true;
}

#if defined(__aarch64__)

constexpr size_t kBlockPositionOffset = 0x30U;
constexpr size_t kBlockPositionPaddingOffset = 0x3CU;
constexpr size_t kCompoundTagOffset = 0x40U;
constexpr uint32_t kBlockActorDataPacketId = 0x38U;

struct WireBlockPos {
    int32_t x;
    int32_t y;
    int32_t z;
};

static_assert(sizeof(WireBlockPos) == 12U, "unexpected BlockPos layout");
static_assert(kCompoundTagOffset + 0x20U <= 0x68U,
              "BlockActorDataPacket layout exceeds allocation");
static_assert(sizeof(std::string) == 0x18U,
              "unexpected arm64 std::string layout for native CompoundTag");

enum NativeFunctionIndex : size_t {
    kPacketConstructor,
    kPacketCompleteDestructor,
    kPacketDeletingDestructor,
    kPacketGetId,
    kPacketWrite,
    kPacketRead,
    kCompoundConstructor,
    kCompoundCompleteDestructor,
    kCompoundDeletingDestructor,
    kCompoundPutInt,
    kCompoundPutString,
    kCompoundPutCompound,
    kCompoundPutByte,
    kSender,
    kNativeFunctionCount,
};

struct ResolvedAbi {
    uintptr_t module_base = 0;
    uintptr_t packet_vtable = 0;
    uintptr_t compound_vtable = 0;
    std::array<uintptr_t, kNativeFunctionCount> functions{};
};

using PacketConstructor = void (*)(void* packet);
using NativeDeletingDestructor = void (*)(void* object);
using PacketGetId = uint32_t (*)(const void* packet);
using CompoundConstructor = void (*)(void* tag);
using CompoundPutInt = void* (*)(void* tag, std::string* key, int32_t value);
using CompoundPutString = void* (*)(void* tag, std::string* key,
                                    std::string* value);
using CompoundPutCompound = void* (*)(void* tag, std::string* key,
                                      void** compound_holder);
using CompoundPutByte = void* (*)(void* tag, std::string* key, uint8_t value);
using SendToServer = void (*)(void* sender, void* packet);

template <size_t N>
bool matchesInstructionFingerprint(uintptr_t address,
                                   const std::array<uint8_t, N>& expected) {
    return address != 0 && std::memcmp(reinterpret_cast<const void*>(address),
                                       expected.data(), expected.size()) == 0;
}

bool checkedAddressAdd(uintptr_t base, uintptr_t offset, uintptr_t* output) {
    if (!output || base == 0 || offset > std::numeric_limits<uintptr_t>::max() - base) {
        return false;
    }
    *output = base + offset;
    return true;
}

bool resolveAbi(uintptr_t module_base, ResolvedAbi* output, std::string* error) {
    if (!output) return fail(error, "sign packet ABI output is missing");
    *output = {};
    const SignBlockActorPacketAbiProfile& profile =
        kSignBlockActorPacketAbiProfile;
    const std::array<const NativeFunctionFingerprint*, kNativeFunctionCount>
        fingerprints{{
            &profile.packet_constructor,
            &profile.packet_complete_destructor,
            &profile.packet_deleting_destructor,
            &profile.packet_get_id,
            &profile.packet_write,
            &profile.packet_read,
            &profile.compound_constructor,
            &profile.compound_complete_destructor,
            &profile.compound_deleting_destructor,
            &profile.compound_put_int,
            &profile.compound_put_string,
            &profile.compound_put_compound,
            &profile.compound_put_byte,
            &profile.sender,
        }};

    ResolvedAbi resolved;
    resolved.module_base = module_base;
    if (!checkedAddressAdd(module_base, profile.packet_vtable_rva,
                           &resolved.packet_vtable) ||
        !checkedAddressAdd(module_base, profile.compound_vtable_rva,
                           &resolved.compound_vtable)) {
        return fail(error, "invalid libminecraftpe.so base address");
    }

    for (size_t index = 0; index < fingerprints.size(); ++index) {
        const NativeFunctionFingerprint& fingerprint = *fingerprints[index];
        uintptr_t address = 0;
        if (!ResolveMinecraftExecutableOffset(module_base, fingerprint.rva,
                                              fingerprint.prologue.size(),
                                              &address)) {
            return fail(error, std::string("sign packet ABI address is unavailable: ") +
                               fingerprint.name);
        }
        const bool matches = matchesInstructionFingerprint(address,
                                                            fingerprint.prologue);
        const bool captured_sender_matches = index == kSender &&
            CapturedLoopbackPacketSenderOriginalPrologueMatches(
                fingerprint.prologue.data(), fingerprint.prologue.size());
        if (!matches && !captured_sender_matches) {
            return fail(error, std::string("sign packet ABI fingerprint mismatch: ") +
                               fingerprint.name);
        }
        resolved.functions[index] = address;
    }

    uintptr_t packet_typeinfo = 0;
    if (!checkedAddressAdd(module_base, profile.packet_typeinfo_rva,
                           &packet_typeinfo) ||
        resolved.packet_vtable < sizeof(uintptr_t) ||
        !IsMemoryReadable(
            reinterpret_cast<const void*>(resolved.packet_vtable - sizeof(uintptr_t)),
            19U * sizeof(uintptr_t)) ||
        resolved.compound_vtable < sizeof(uintptr_t) ||
        !IsMemoryReadable(
            reinterpret_cast<const void*>(resolved.compound_vtable - sizeof(uintptr_t)),
            3U * sizeof(uintptr_t))) {
        return fail(error, "sign packet ABI vtable is unreadable");
    }

    const auto* packet_vtable =
        reinterpret_cast<const uintptr_t*>(resolved.packet_vtable);
    const auto* compound_vtable =
        reinterpret_cast<const uintptr_t*>(resolved.compound_vtable);
    if (packet_vtable[-1] != packet_typeinfo ||
        packet_vtable[0] != resolved.functions[kPacketCompleteDestructor] ||
        packet_vtable[1] != resolved.functions[kPacketDeletingDestructor] ||
        packet_vtable[2] != resolved.functions[kPacketGetId] ||
        packet_vtable[6] != resolved.functions[kPacketWrite] ||
        packet_vtable[17] != resolved.functions[kPacketRead] ||
        compound_vtable[0] != resolved.functions[kCompoundCompleteDestructor] ||
        compound_vtable[1] != resolved.functions[kCompoundDeletingDestructor]) {
        return fail(error, "sign packet ABI vtable slots do not match protocol 859");
    }
    *output = resolved;
    return true;
}

class NativeObjectLifetime {
public:
    NativeObjectLifetime(void* object, NativeDeletingDestructor destroy)
        : object_(object), destroy_(destroy) {}

    ~NativeObjectLifetime() {
        if (object_) destroy_(object_);
    }

    NativeObjectLifetime(const NativeObjectLifetime&) = delete;
    NativeObjectLifetime& operator=(const NativeObjectLifetime&) = delete;

private:
    void* object_ = nullptr;
    NativeDeletingDestructor destroy_ = nullptr;
};

void* constructNativeObject(size_t size, void (*constructor)(void*)) {
    void* const object = ::operator new(size, std::nothrow);
    if (!object) return nullptr;
    try {
        constructor(object);
    } catch (...) {
        ::operator delete(object);
        throw;
    }
    return object;
}

class CompoundWriter {
public:
    explicit CompoundWriter(const ResolvedAbi& abi)
        : put_int_(reinterpret_cast<CompoundPutInt>(abi.functions[kCompoundPutInt])),
          put_string_(reinterpret_cast<CompoundPutString>(
              abi.functions[kCompoundPutString])),
          put_compound_(reinterpret_cast<CompoundPutCompound>(
              abi.functions[kCompoundPutCompound])),
          put_byte_(reinterpret_cast<CompoundPutByte>(abi.functions[kCompoundPutByte])) {}

    void putInt(void* tag, const char* name, int32_t value) const {
        std::string key(name);
        put_int_(tag, &key, value);
    }

    void putString(void* tag, const char* name, const std::string& value) const {
        std::string key(name);
        std::string mutable_value(value);
        put_string_(tag, &key, &mutable_value);
    }

    void putByte(void* tag, const char* name, bool value) const {
        std::string key(name);
        put_byte_(tag, &key, value ? 1U : 0U);
    }

    void putCompound(void* tag, const char* name, void* compound) const {
        std::string key(name);
        void* holder = compound;
        put_compound_(tag, &key, &holder);
        // Protocol 859's verified putCompound implementation never consumes
        // this holder pointer. It move-constructs the inserted tag from the
        // pointed CompoundTag's internal tree, leaving that source object
        // allocated but empty. The surrounding NativeObjectLifetime therefore
        // deletes only the moved-from temporary, not the inserted tag.
        if (holder != compound) {
            throw std::runtime_error("CompoundTag holder ownership changed");
        }
    }

    void writeFace(void* tag, const SignTextRecord& face) const {
        // This order and these names match SignBlockActor's protocol-859 save
        // function. TextOwner, FilteredText and editing locks are session data.
        if (face.has_ignore_lighting) {
            putByte(tag, "IgnoreLighting", face.ignore_lighting);
        }
        if (face.has_hide_glow_outline) {
            putByte(tag, "HideGlowOutline", face.hide_glow_outline);
        }
        if (face.has_text_color) {
            putInt(tag, "SignTextColor", face.text_color);
        }
        if (face.has_persist_formatting) {
            putByte(tag, "PersistFormatting", face.persist_formatting);
        }
        if (face.has_text) {
            putString(tag, "Text", face.text);
        }
    }

private:
    CompoundPutInt put_int_ = nullptr;
    CompoundPutString put_string_ = nullptr;
    CompoundPutCompound put_compound_ = nullptr;
    CompoundPutByte put_byte_ = nullptr;
};

#endif  // defined(__aarch64__)

}  // namespace

bool SignBlockActorPacketSender::validate(const SignRecord& record,
                                          std::string* error) {
    if (error) error->clear();
    try {
        std::string normalized_id = record.expected_sign_id;
        if (!normalizeDeferredItemIdentifier(&normalized_id) ||
            normalized_id != record.expected_sign_id ||
            !deferredSignIdentifier(normalized_id)) {
            return fail(error, "sign packet target identifier is invalid");
        }
        size_t text_bytes = 0;
        if (!validFace(record.front, &text_bytes) ||
            !validFace(record.back, &text_bytes)) {
            return fail(error, "sign packet face data is invalid");
        }
        if (text_bytes > kMaximumTotalTextBytes) {
            return fail(error, "sign packet text payload is too large");
        }
        if (!record.has_is_waxed && record.is_waxed) {
            return fail(error, "sign packet waxed value has no presence flag");
        }
        if (!record.front.present && !record.back.present &&
            !record.has_is_waxed) {
            return fail(error, "sign packet has no persistent payload");
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory to validate sign packet data");
    }
    return true;
}

bool SignBlockActorPacketSender::send(const SignRecord& record,
                                      std::string* error) {
#if !defined(__aarch64__)
    (void)record;
    return fail(error, "sign packet sending is supported only on arm64-v8a");
#else
    if (!validate(record, error)) return false;

    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");

    ResolvedAbi abi;
    if (!resolveAbi(module_base, &abi, error)) return false;

    void* const sender_instance = GetCapturedLoopbackPacketSender();
    if (!sender_instance) {
        return fail(error,
                    "LoopbackPacketSender is not ready; wait for a live game packet");
    }

    try {
        void* const packet = constructNativeObject(
            kSignBlockActorPacketAbiProfile.packet_size,
            reinterpret_cast<PacketConstructor>(abi.functions[kPacketConstructor]));
        if (!packet) return fail(error, "unable to allocate BlockActorDataPacket");

        const uintptr_t actual_vtable =
            *reinterpret_cast<const uintptr_t*>(packet);
        if (actual_vtable != abi.packet_vtable) {
            // No tag has received dynamic data yet, so raw release is safe and
            // avoids invoking an unexpected virtual destructor.
            ::operator delete(packet);
            return fail(error,
                        "BlockActorDataPacket vtable does not match protocol 859");
        }

        NativeObjectLifetime packet_lifetime(
            packet,
            reinterpret_cast<NativeDeletingDestructor>(
                abi.functions[kPacketDeletingDestructor]));
        if (reinterpret_cast<PacketGetId>(abi.functions[kPacketGetId])(packet) !=
            kBlockActorDataPacketId) {
            return fail(error, "BlockActorDataPacket native ID is not 0x38");
        }

        auto* const bytes = static_cast<uint8_t*>(packet);
        void* const root_tag = bytes + kCompoundTagOffset;
        if (*reinterpret_cast<const uintptr_t*>(root_tag) != abi.compound_vtable) {
            return fail(error, "BlockActorDataPacket CompoundTag layout is invalid");
        }

        *reinterpret_cast<WireBlockPos*>(bytes + kBlockPositionOffset) =
            {record.x, record.y, record.z};
        std::memset(bytes + kBlockPositionPaddingOffset, 0, 4U);

        const CompoundWriter writer(abi);
        writer.putString(root_tag, "id",
                         deferredHangingSignIdentifier(record.expected_sign_id)
                             ? std::string("HangingSign")
                             : std::string("Sign"));
        writer.putInt(root_tag, "x", record.x);
        writer.putInt(root_tag, "y", record.y);
        writer.putInt(root_tag, "z", record.z);

        const auto compound_constructor = reinterpret_cast<CompoundConstructor>(
            abi.functions[kCompoundConstructor]);
        const auto compound_deleting_destructor =
            reinterpret_cast<NativeDeletingDestructor>(
                abi.functions[kCompoundDeletingDestructor]);

        if (record.front.present) {
            void* const front = constructNativeObject(
                kSignBlockActorPacketAbiProfile.compound_size,
                compound_constructor);
            if (!front) return fail(error, "unable to allocate FrontText CompoundTag");
            if (*reinterpret_cast<const uintptr_t*>(front) != abi.compound_vtable) {
                ::operator delete(front);
                return fail(error, "FrontText CompoundTag layout is invalid");
            }
            NativeObjectLifetime front_lifetime(front,
                                                 compound_deleting_destructor);
            writer.writeFace(front, record.front);
            writer.putCompound(root_tag, "FrontText", front);
        }

        if (record.back.present) {
            void* const back = constructNativeObject(
                kSignBlockActorPacketAbiProfile.compound_size,
                compound_constructor);
            if (!back) return fail(error, "unable to allocate BackText CompoundTag");
            if (*reinterpret_cast<const uintptr_t*>(back) != abi.compound_vtable) {
                ::operator delete(back);
                return fail(error, "BackText CompoundTag layout is invalid");
            }
            NativeObjectLifetime back_lifetime(back,
                                                compound_deleting_destructor);
            writer.writeFace(back, record.back);
            writer.putCompound(root_tag, "BackText", back);
        }

        if (record.has_is_waxed) {
            writer.putByte(root_tag, "IsWaxed", record.is_waxed);
        }

        reinterpret_cast<SendToServer>(abi.functions[kSender])(sender_instance,
                                                               packet);
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory to build sign BlockActorDataPacket");
    } catch (...) {
        return fail(error, "native sign BlockActorDataPacket construction failed");
    }
    return true;
#endif
}

bool SignBlockActorPacketSender::sendFace(const SignRecord& record, Face face,
                                          bool include_waxed,
                                          std::string* error) {
    if (error) error->clear();
    try {
        if (!validate(record, error)) return false;

        if (face != Face::Front && face != Face::Back) {
            return fail(error, "sign packet face selector is invalid");
        }
        const SignTextRecord& selected = face == Face::Front
            ? record.front : record.back;
        if (!selected.present) {
            return fail(error, face == Face::Front
                ? "sign packet FrontText payload is absent"
                : "sign packet BackText payload is absent");
        }

        SignRecord face_record = record;
        if (face == Face::Front) {
            face_record.back = {};
        } else {
            face_record.front = {};
        }
        if (!include_waxed) {
            face_record.has_is_waxed = false;
            face_record.is_waxed = false;
        }
        return send(face_record, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory to select sign face data");
    } catch (...) {
        return fail(error, "sign face packet selection failed");
    }
}

}  // namespace build_import
