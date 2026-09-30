#ifndef INFINITE_TEXTURE_INFINITY_FORMAT_KEY_H
#define INFINITE_TEXTURE_INFINITY_FORMAT_KEY_H

#include "InfinityCryptoCodec.h"

namespace build_import {

// Version-one files use a shared application key so an exported building can
// be imported on another device. This provides authenticated obfuscation, not
// device-bound secrecy: software that can decrypt automatically must carry the
// corresponding key material.
const InfinityCryptoCodec::Key& infinityFormatKeyV1();

}  // namespace build_import

#endif  // INFINITE_TEXTURE_INFINITY_FORMAT_KEY_H
