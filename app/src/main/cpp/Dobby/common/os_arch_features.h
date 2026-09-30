#pragma once

#include <sys/types.h>
#include <stddef.h>
#include "pac_kit.h"

namespace features {

template <typename T> inline T arm_thumb_fix_addr(T &addr) {
#if defined(__arm__) || defined(__aarch64__)
  addr = (T)((uintptr_t)addr & ~1);
#endif
  return addr;
}

namespace apple {
template <typename T> inline T arm64e_pac_strip(T &addr) {
  return addr;
}

template <typename T> inline T arm64e_pac_sign(T &addr) {
  return addr;
}

template <typename T> inline T arm64e_pac_strip_and_sign(T &addr) {
  return addr;
}
} // namespace apple

namespace android {
void make_memory_readable(void *address, size_t size);
} // namespace android
} // namespace features