#ifndef INFINITE_TEXTURE_EXECUTE_COMMAND_CONVERTER_H
#define INFINITE_TEXTURE_EXECUTE_COMMAND_CONVERTER_H

#include <string>

namespace build_import {

// Rewrites only legacy execute forms whose meaning can be preserved on the
// current command dispatcher. Returns true when the supplied command changed.
// Unsupported legacy variants are intentionally left untouched.
bool normalizeLegacyExecuteCommand(std::string* command);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_EXECUTE_COMMAND_CONVERTER_H
