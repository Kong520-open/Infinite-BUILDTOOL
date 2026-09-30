#pragma once

#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace build_import {

#if !defined(_WIN32)
// A newly prepared journal must never replace an existing transaction. Android
// SELinux denies hard links in app data, so use the kernel's atomic no-replace
// rename there. If unsupported, fail closed before any game packet is sent.
inline bool CommitMapJournalNoReplace(const std::string& temporary,
                                      const std::string& path) noexcept {
#if defined(__ANDROID__)
    if (syscall(__NR_renameat2, AT_FDCWD, temporary.c_str(),
                AT_FDCWD, path.c_str(), RENAME_NOREPLACE) == 0) {
        return true;
    }
    const int failure = errno;
    __android_log_print(ANDROID_LOG_WARN, "Infinitecz_MapJournal",
                        "no-replace rename failed errno=%d", failure);
    return false;
#else
    return link(temporary.c_str(), path.c_str()) == 0 &&
           unlink(temporary.c_str()) == 0;
#endif
}
#endif

}  // namespace build_import
