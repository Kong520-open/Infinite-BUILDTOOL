#include "../SignBlockActorPacketAbiProfile.h"
#include "../SignBlockActorPacketSender.h"

#if defined(__aarch64__)
#include "../../main.h"
#include "../../tp/LoopbackPacketSenderCapture.h"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace build_import;

#if defined(__aarch64__)
// The cross-compiled parser-test binary is not injected into Minecraft. These
// stubs keep the production arm64 sender compiled and linked while ensuring a
// standalone test can never resolve or call a live game ABI.
std::atomic<uintptr_t> Main::baseAddress{0};

bool ResolveMinecraftExecutableOffset(uintptr_t, uintptr_t, size_t, uintptr_t*) {
    return false;
}

void* GetCapturedLoopbackPacketSender() {
    return nullptr;
}

bool CapturedLoopbackPacketSenderOriginalPrologueMatches(const uint8_t*, size_t) {
    return false;
}

bool IsMemoryReadable(const void*, size_t) {
    return false;
}
#endif

namespace {

SignRecord validRecord() {
    SignRecord record;
    record.x = -12;
    record.y = 64;
    record.z = 31;
    record.expected_sign_id = "minecraft:oak_sign";
    record.front.present = true;
    record.front.has_text = true;
    record.front.text = std::string("front\n") + u8"\u4f60\u597d";
    record.front.has_text_color = true;
    record.front.text_color = -16711936;
    record.front.has_ignore_lighting = true;
    record.front.ignore_lighting = false;
    record.front.has_hide_glow_outline = true;
    record.front.hide_glow_outline = true;
    record.front.has_persist_formatting = true;
    record.front.persist_formatting = true;
    record.front.has_text_ignore_legacy_bug_resolved = true;
    record.front.text_ignore_legacy_bug_resolved = true;
    record.back.present = true;
    record.back.has_text = true;
    record.back.text = "back";
    record.has_is_waxed = true;
    record.is_waxed = false;
    return record;
}

void testValidationAndIdentifiers() {
    SignRecord record = validRecord();
    std::string error = "stale";
    assert(SignBlockActorPacketSender::validate(record, &error));
    assert(error.empty());

    record.expected_sign_id = "minecraft:oak_hanging_sign";
    assert(SignBlockActorPacketSender::validate(record, &error));
    assert(deferredHangingSignIdentifier(record.expected_sign_id));
    assert(deferredSignShellMatches(record.expected_sign_id,
                                    "minecraft:wall_hanging_sign"));
    assert(!deferredSignShellMatches(record.expected_sign_id,
                                     "minecraft:standing_sign"));
    record.expected_sign_id = "minecraft:wall_sign";
    assert(SignBlockActorPacketSender::validate(record, &error));
    assert(!deferredHangingSignIdentifier(record.expected_sign_id));
    assert(deferredSignShellMatches(record.expected_sign_id,
                                    "minecraft:oak_sign[ground_sign_direction=2]"));
    record.expected_sign_id = "minecraft:pale_oak_hanging_sign";
    assert(!deferredSignShellMatches(record.expected_sign_id,
                                     "minecraft:birch_hanging_sign"));
    assert(deferredSignShellMatches(record.expected_sign_id,
                                    "minecraft:wall_hanging_sign"));
    record.expected_sign_id = "minecraft:spruce_standing_sign";
    assert(!deferredSignShellMatches(record.expected_sign_id,
                                     "minecraft:oak_standing_sign"));

    record.expected_sign_id = "minecraft:chest";
    assert(!SignBlockActorPacketSender::validate(record, &error));
    assert(!error.empty());
    record.expected_sign_id = "Minecraft:oak_sign";
    assert(!SignBlockActorPacketSender::validate(record, &error));
    record.expected_sign_id = "minecraft:oak_sign extra";
    assert(!SignBlockActorPacketSender::validate(record, &error));
}

void testTextAndPresenceBounds() {
    SignRecord record = validRecord();
    std::string error;

    record.front.text.assign(
        SignBlockActorPacketSender::kMaximumFaceTextBytes, 'a');
    record.back.text.assign(
        SignBlockActorPacketSender::kMaximumTotalTextBytes -
            SignBlockActorPacketSender::kMaximumFaceTextBytes,
        'b');
    assert(SignBlockActorPacketSender::validate(record, &error));

    record.back.text.push_back('b');
    assert(!SignBlockActorPacketSender::validate(record, &error));

    record = validRecord();
    record.front.text.assign(
        SignBlockActorPacketSender::kMaximumFaceTextBytes + 1U, 'a');
    assert(!SignBlockActorPacketSender::validate(record, &error));

    record = validRecord();
    record.front.text.assign("bad\0text", 8U);
    assert(!SignBlockActorPacketSender::validate(record, &error));
    record.front.text.assign(1U, static_cast<char>(0xC0U));
    assert(!SignBlockActorPacketSender::validate(record, &error));

    record = validRecord();
    record.front.present = false;
    assert(!SignBlockActorPacketSender::validate(record, &error));

    record = validRecord();
    record.front.has_text_ignore_legacy_bug_resolved = false;
    assert(!SignBlockActorPacketSender::validate(record, &error));

    record = {};
    record.expected_sign_id = "minecraft:standing_sign";
    assert(!SignBlockActorPacketSender::validate(record, &error));

    record.has_is_waxed = false;
    record.is_waxed = true;
    assert(!SignBlockActorPacketSender::validate(record, &error));
}

void testProtocol859AbiProfile() {
    const SignBlockActorPacketAbiProfile& profile =
        kSignBlockActorPacketAbiProfile;
    assert(profile.protocol_version == 859U);
    assert(profile.packet_size == 0x68U);
    assert(profile.compound_size == 0x20U);
    assert(profile.packet_vtable_rva == 0x128EDF30ULL);
    assert(profile.packet_typeinfo_rva == 0x128EEBE8ULL);
    assert(profile.packet_get_id.rva == 0x0A5258F0ULL);
    assert(profile.packet_get_id.prologue[0] == 0x00U);
    assert(profile.packet_get_id.prologue[1] == 0x07U);
    assert(profile.packet_get_id.prologue[2] == 0x80U);
    assert(profile.packet_get_id.prologue[3] == 0x52U);

    const std::array<const NativeFunctionFingerprint*, 14U> functions{{
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
    for (size_t index = 0; index < functions.size(); ++index) {
        assert(functions[index]->name[0] != '\0');
        assert(functions[index]->rva != 0U);
        bool any_byte = false;
        for (uint8_t byte : functions[index]->prologue) any_byte |= byte != 0U;
        assert(any_byte);
        for (size_t other = index + 1U; other < functions.size(); ++other) {
            assert(functions[index]->rva != functions[other]->rva);
        }
    }
}

void testUnsupportedHostFailsClosed() {
#if !defined(__aarch64__)
    SignRecord record = validRecord();
    std::string error;
    assert(!SignBlockActorPacketSender::send(record, &error));
    assert(error.find("arm64-v8a") != std::string::npos);
#endif
}

void testFaceSelectionFailsClosed() {
    SignRecord record = validRecord();
    std::string error;

    record.back = {};
    assert(!SignBlockActorPacketSender::sendFace(
        record, SignBlockActorPacketSender::Face::Back, false, &error));
    assert(error.find("BackText") != std::string::npos);

    record = validRecord();
    assert(!SignBlockActorPacketSender::sendFace(
        record, static_cast<SignBlockActorPacketSender::Face>(2), false,
        &error));
    assert(error.find("selector") != std::string::npos);
}

}  // namespace

int main() {
    testValidationAndIdentifiers();
    testTextAndPresenceBounds();
    testProtocol859AbiProfile();
    testUnsupportedHostFailsClosed();
    testFaceSelectionFailsClosed();
    return 0;
}
