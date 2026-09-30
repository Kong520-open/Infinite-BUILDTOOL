#ifndef INFINITE_TEXTURE_BLOCK_COMMAND_FORMATTER_H
#define INFINITE_TEXTURE_BLOCK_COMMAND_FORMATTER_H

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

std::string formatPlacementBlockArgument(std::string_view name, uint16_t aux);
std::string formatVerificationBlockArgument(std::string_view name, uint16_t aux,
                                            bool ignore_aux);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BLOCK_COMMAND_FORMATTER_H
