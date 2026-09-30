#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_SILENT_ROTATION_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_SILENT_ROTATION_H

#include <cstdint>
#include <string>

namespace build_import {

// A one-shot server-side rotation override for printer placement.  It changes
// only a verified outgoing PlayerAuthInputPacket, never LocalPlayer or the
// rendered camera.  The caller must wait for isSynchronized() before sending
// the corresponding ItemUse, then finish() immediately afterwards.
class ProjectionPrinterSilentRotation {
public:
    using Ticket = uint64_t;

    static bool armYaw(float yaw, Ticket* ticket, std::string* error = nullptr);
    static bool isSynchronized(Ticket ticket) noexcept;
    static void finish(Ticket ticket) noexcept;
    static void cancel(Ticket ticket) noexcept;
    static void cancelAll() noexcept;

    // Called only by the existing LoopbackPacketSender hook.  The returned
    // ticket must be passed to onAfterOutgoingPacket after the original sender
    // returns, so "synchronized" means the packet has really been sent.
    static Ticket onBeforeOutgoingPacket(void* packet) noexcept;
    static void onAfterOutgoingPacket(Ticket ticket) noexcept;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_PRINTER_SILENT_ROTATION_H
