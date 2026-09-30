#include "ExecuteCommandConverter.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace build_import {
namespace {

constexpr size_t kMaximumNestedExecuteConversions = 16;

struct TokenSpan {
    size_t begin = 0;
    size_t end = 0;
};

bool isAsciiWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}

bool isAsciiLetterOrDigit(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        char left_value = left[index];
        char right_value = right[index];
        if (left_value >= 'A' && left_value <= 'Z') {
            left_value = static_cast<char>(left_value - 'A' + 'a');
        }
        if (right_value >= 'A' && right_value <= 'Z') {
            right_value = static_cast<char>(right_value - 'A' + 'a');
        }
        if (left_value != right_value) return false;
    }
    return true;
}

void appendView(std::string* output, std::string_view value) {
    output->append(value.data(), value.size());
}

bool readToken(std::string_view command, size_t* cursor, TokenSpan* token) {
    if (!cursor || !token) return false;
    while (*cursor < command.size() && isAsciiWhitespace(command[*cursor])) ++*cursor;
    if (*cursor >= command.size()) return false;

    const size_t begin = *cursor;
    char quote = '\0';
    bool escaped = false;
    int bracket_depth = 0;
    int brace_depth = 0;
    int parenthesis_depth = 0;
    for (; *cursor < command.size(); ++*cursor) {
        const char value = command[*cursor];
        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == quote) {
                quote = '\0';
            }
            continue;
        }
        if (value == '\'' || value == '\"') {
            quote = value;
            continue;
        }
        if (value == '[') {
            ++bracket_depth;
            continue;
        }
        if (value == ']') {
            if (bracket_depth == 0) return false;
            --bracket_depth;
            continue;
        }
        if (value == '{') {
            ++brace_depth;
            continue;
        }
        if (value == '}') {
            if (brace_depth == 0) return false;
            --brace_depth;
            continue;
        }
        if (value == '(') {
            ++parenthesis_depth;
            continue;
        }
        if (value == ')') {
            if (parenthesis_depth == 0) return false;
            --parenthesis_depth;
            continue;
        }
        if (isAsciiWhitespace(value) && bracket_depth == 0 && brace_depth == 0 &&
            parenthesis_depth == 0) {
            token->begin = begin;
            token->end = *cursor;
            return true;
        }
    }
    if (quote != '\0' || bracket_depth != 0 || brace_depth != 0 || parenthesis_depth != 0) {
        return false;
    }
    token->begin = begin;
    token->end = *cursor;
    return true;
}

bool readTokenPrefix(std::string_view command, size_t count, std::vector<TokenSpan>* tokens) {
    if (!tokens) return false;
    tokens->clear();
    tokens->reserve(count);
    size_t cursor = 0;
    for (size_t index = 0; index < count; ++index) {
        TokenSpan token;
        if (!readToken(command, &cursor, &token)) return false;
        tokens->push_back(token);
    }
    return true;
}

std::string_view tokenText(std::string_view command, const TokenSpan& token) {
    return command.substr(token.begin, token.end - token.begin);
}

void skipAsciiWhitespace(std::string_view command, size_t* cursor) {
    if (!cursor) return;
    while (*cursor < command.size() && isAsciiWhitespace(command[*cursor])) ++*cursor;
}

// Older BDX writers commonly compact coordinates into forms such as
// `@e[...]~~~` and `^^^playsound`. The command dispatcher used by those
// worlds can find the coordinate boundaries without spaces, but the modern
// command form requires them. Keep the selector parser separate from the
// normal whitespace tokenizer so its closing bracket remains a hard boundary.
bool readLegacyEntityArgument(std::string_view command, size_t* cursor, TokenSpan* token) {
    if (!cursor || !token) return false;
    skipAsciiWhitespace(command, cursor);
    if (*cursor >= command.size()) return false;
    if (command[*cursor] != '@') return readToken(command, cursor, token);

    const size_t begin = *cursor;
    ++*cursor;
    const size_t selector_name_begin = *cursor;
    while (*cursor < command.size() &&
           (isAsciiLetterOrDigit(command[*cursor]) || command[*cursor] == '_')) {
        ++*cursor;
    }
    if (*cursor == selector_name_begin) return false;
    if (*cursor >= command.size() || command[*cursor] != '[') {
        token->begin = begin;
        token->end = *cursor;
        return true;
    }

    int bracket_depth = 0;
    int brace_depth = 0;
    int parenthesis_depth = 0;
    char quote = '\0';
    bool escaped = false;
    for (; *cursor < command.size(); ++*cursor) {
        const char value = command[*cursor];
        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == quote) {
                quote = '\0';
            }
            continue;
        }
        if (value == '\'' || value == '\"') {
            quote = value;
            continue;
        }
        if (value == '[') {
            ++bracket_depth;
            continue;
        }
        if (value == '{') {
            ++brace_depth;
            continue;
        }
        if (value == '(') {
            ++parenthesis_depth;
            continue;
        }
        if (value == '}') {
            if (brace_depth == 0) return false;
            --brace_depth;
            continue;
        }
        if (value == ')') {
            if (parenthesis_depth == 0) return false;
            --parenthesis_depth;
            continue;
        }
        if (value != ']') continue;
        if (bracket_depth == 0) return false;
        --bracket_depth;
        if (bracket_depth == 0) {
            if (brace_depth != 0 || parenthesis_depth != 0) return false;
            ++*cursor;
            token->begin = begin;
            token->end = *cursor;
            return true;
        }
    }
    return false;
}

bool consumeFiniteNumberPrefix(std::string_view command, size_t* cursor) {
    if (!cursor || *cursor >= command.size()) return false;
    const char first = command[*cursor];
    if (first != '+' && first != '-' && first != '.' &&
        (first < '0' || first > '9')) {
        return false;
    }
    std::string temporary(command.substr(*cursor));
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(temporary.c_str(), &end);
    if (end == temporary.c_str() || errno == ERANGE || !std::isfinite(value)) return false;
    *cursor += static_cast<size_t>(end - temporary.c_str());
    return true;
}

bool readLegacyCoordinate(std::string_view command, size_t* cursor,
                          TokenSpan* token, bool allow_attached_body) {
    if (!cursor || !token) return false;
    skipAsciiWhitespace(command, cursor);
    if (*cursor >= command.size()) return false;

    const size_t begin = *cursor;
    const char prefix = command[*cursor];
    if (prefix == '~' || prefix == '^') {
        ++*cursor;
        // Relative and local coordinates may omit their numeric suffix. When
        // another coordinate marker follows immediately, that marker starts
        // the next coordinate instead of being part of this one.
        if (*cursor < command.size()) {
            const char next = command[*cursor];
            if (next == '+' || next == '-' || next == '.' ||
                (next >= '0' && next <= '9')) {
                if (!consumeFiniteNumberPrefix(command, cursor)) return false;
            }
        }
    } else {
        if (!consumeFiniteNumberPrefix(command, cursor)) return false;
        if (!allow_attached_body && *cursor < command.size() &&
            !isAsciiWhitespace(command[*cursor]) && command[*cursor] != '~' &&
            command[*cursor] != '^') {
            return false;
        }
    }
    token->begin = begin;
    token->end = *cursor;
    return true;
}

bool readLegacyCoordinateTriple(std::string_view command, size_t* cursor,
                                std::array<TokenSpan, 3>* coordinates) {
    if (!coordinates) return false;
    for (size_t index = 0; index < coordinates->size(); ++index) {
        if (!readLegacyCoordinate(command, cursor, &(*coordinates)[index], index == 2U)) {
            return false;
        }
    }
    return true;
}

bool startsLegacyDetectKeyword(std::string_view command, size_t cursor, size_t* next) {
    skipAsciiWhitespace(command, &cursor);
    constexpr std::string_view kDetect = "detect";
    if (command.size() - cursor < kDetect.size() ||
        !equalsIgnoreCase(command.substr(cursor, kDetect.size()), kDetect)) {
        return false;
    }
    const size_t end = cursor + kDetect.size();
    if (end < command.size() && !isAsciiWhitespace(command[end]) &&
        command[end] != '~' && command[end] != '^') {
        return false;
    }
    if (next) *next = end;
    return true;
}

bool isNewExecuteSubcommand(std::string_view token) {
    constexpr std::array<std::string_view, 14> kSubcommands{{
        "align", "anchored", "as", "at", "facing", "if", "in", "on", "positioned",
        "rotated", "run", "store", "summon", "unless",
    }};
    for (const std::string_view subcommand : kSubcommands) {
        if (equalsIgnoreCase(token, subcommand)) return true;
    }
    return false;
}

bool hasLeadingExecuteSlash(std::string_view command) {
    constexpr std::string_view kExecute = "execute";
    return command.size() >= kExecute.size() + 1U && command.front() == '/' &&
           equalsIgnoreCase(command.substr(1, kExecute.size()), kExecute) &&
           (command.size() == kExecute.size() + 1U ||
            isAsciiWhitespace(command[kExecute.size() + 1U]));
}

// A nested legacy execute must be converted as a unit. Partially rewriting
// only its parent would leave a legacy command behind a modern `run`, where it
// can no longer be dispatched by current game versions.
bool startsLegacyExecuteCandidate(std::string_view command) {
    std::vector<TokenSpan> tokens;
    if (!readTokenPrefix(command, 1, &tokens)) return false;
    const std::string_view first = tokenText(command, tokens[0]);
    if (!equalsIgnoreCase(first, "execute")) return false;

    // A truncated execute is still a legacy candidate: preserving the whole
    // source command is safer than producing a misleading partial migration.
    if (!readTokenPrefix(command, 2, &tokens)) return true;
    return !isNewExecuteSubcommand(tokenText(command, tokens[1]));
}

bool isLegacyEntityArgument(std::string_view token) {
    if (token.size() >= 2 && token.front() == '@') {
        return !isAsciiWhitespace(token[1]);
    }
    if (token.empty() || token.size() > 64) return false;
    for (const char value : token) {
        if (!isAsciiLetterOrDigit(value) && value != '_' && value != '-') return false;
    }
    return true;
}

bool parseLegacyDataValue(std::string_view token, int32_t* output) {
    if (!output || token.empty()) return false;
    size_t cursor = 0;
    bool negative = false;
    if (token[cursor] == '+' || token[cursor] == '-') {
        negative = token[cursor] == '-';
        ++cursor;
    }
    if (cursor == token.size()) return false;

    const uint64_t positive_limit = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    const uint64_t limit = negative ? positive_limit + 1U : positive_limit;
    uint64_t value = 0;
    for (; cursor < token.size(); ++cursor) {
        const char digit = token[cursor];
        if (digit < '0' || digit > '9') return false;
        const uint64_t next = static_cast<uint64_t>(digit - '0');
        if (value > (limit - next) / 10U) return false;
        value = value * 10U + next;
    }
    if (negative) {
        if (value == positive_limit + 1U) {
            *output = std::numeric_limits<int32_t>::min();
        } else {
            *output = -static_cast<int32_t>(value);
        }
    } else {
        *output = static_cast<int32_t>(value);
    }
    return true;
}

bool isNamespaceCharacter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
           value == '_' || value == '.' || value == '-';
}

bool isLegacyBlockIdentifier(std::string_view identifier) {
    if (identifier.empty()) return false;
    const size_t separator = identifier.find(':');
    if (separator != std::string_view::npos &&
        identifier.find(':', separator + 1) != std::string_view::npos) {
        return false;
    }
    const std::string_view name = separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1);
    if (name.empty()) return false;
    if (separator != std::string_view::npos) {
        const std::string_view name_space = identifier.substr(0, separator);
        if (name_space.empty()) return false;
        for (const char value : name_space) {
            if (!isNamespaceCharacter(value)) return false;
        }
    }
    for (const char value : name) {
        if (!isNamespaceCharacter(value) && value != '/') return false;
    }
    return true;
}

bool isLegacyMinecraftName(std::string_view identifier, std::string_view name) {
    return identifier == name || (identifier.size() == name.size() + 10U &&
                                  identifier.substr(0, 10) == "minecraft:" &&
                                  identifier.substr(10) == name);
}

bool convertLegacyDetectBlock(std::string_view identifier, int32_t data,
                              std::string_view data_token, std::string* converted) {
    if (!converted || !isLegacyBlockIdentifier(identifier)) return false;
    if (data == -1) {
        converted->assign(identifier.data(), identifier.size());
        return true;
    }
    if (data < 0 || data > 15) return false;

    constexpr std::array<std::string_view, 16> kDyeColors{{
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
    }};
    const auto convertColored = [&](std::string_view legacy_name,
                                    std::string_view suffix) -> bool {
        if (!isLegacyMinecraftName(identifier, legacy_name)) return false;
        converted->assign("minecraft:");
        converted->append(kDyeColors[static_cast<size_t>(data)].data(),
                          kDyeColors[static_cast<size_t>(data)].size());
        converted->append(suffix.data(), suffix.size());
        return true;
    };
    if (convertColored("wool", "_wool") ||
        convertColored("stained_glass", "_stained_glass") ||
        convertColored("stained_glass_pane", "_stained_glass_pane") ||
        convertColored("stained_hardened_clay", "_terracotta") ||
        convertColored("concrete", "_concrete") ||
        convertColored("concrete_powder", "_concrete_powder") ||
        convertColored("carpet", "_carpet")) {
        return true;
    }
    converted->assign(identifier.data(), identifier.size());
    converted->push_back(' ');
    appendView(converted, data_token);
    return true;
}

bool convertLegacyExecute(std::string_view command, size_t depth, std::string* converted) {
    if (!converted) return false;
    std::vector<TokenSpan> tokens;
    if (!readTokenPrefix(command, 1, &tokens)) return false;

    const std::string_view command_token = tokenText(command, tokens[0]);
    const bool leading_slash = command_token.size() > 1 && command_token.front() == '/';
    const std::string_view command_name = leading_slash ? command_token.substr(1) : command_token;
    if (!equalsIgnoreCase(command_name, "execute")) return false;

    size_t cursor = tokens[0].end;
    TokenSpan entity_token;
    if (!readLegacyEntityArgument(command, &cursor, &entity_token)) return false;
    const std::string_view entity = tokenText(command, entity_token);
    if (isNewExecuteSubcommand(entity) || !isLegacyEntityArgument(entity)) {
        return false;
    }
    std::array<TokenSpan, 3> coordinates{};
    if (!readLegacyCoordinateTriple(command, &cursor, &coordinates)) return false;

    size_t detect_body_cursor = 0;
    bool has_detect = startsLegacyDetectKeyword(command, cursor, &detect_body_cursor);
    std::string converted_detect_block;
    std::array<TokenSpan, 3> detect_coordinates{};
    size_t body_start = cursor;
    if (has_detect) {
        cursor = detect_body_cursor;
        if (!readLegacyCoordinateTriple(command, &cursor, &detect_coordinates)) {
            return false;
        }
        TokenSpan block_token;
        TokenSpan data_token;
        if (!readToken(command, &cursor, &block_token) ||
            !readToken(command, &cursor, &data_token)) return false;
        int32_t data = 0;
        if (!parseLegacyDataValue(tokenText(command, data_token), &data) ||
            !convertLegacyDetectBlock(tokenText(command, block_token), data,
                                       tokenText(command, data_token),
                                       &converted_detect_block)) {
            return false;
        }
        skipAsciiWhitespace(command, &cursor);
        body_start = cursor;
    }

    skipAsciiWhitespace(command, &body_start);
    std::string_view body = command.substr(body_start);
    if (body.empty()) return false;
    if (hasLeadingExecuteSlash(body)) body.remove_prefix(1);
    std::string converted_body;
    if (startsLegacyExecuteCandidate(body)) {
        if (depth >= kMaximumNestedExecuteConversions ||
            !convertLegacyExecute(body, depth + 1U, &converted_body)) {
            return false;
        }
        body = converted_body;
    }

    converted->clear();
    converted->reserve(command.size() + 64U);
    appendView(converted, command.substr(0, tokens[0].begin));
    if (leading_slash) converted->push_back('/');
    converted->append("execute as ");
    appendView(converted, entity);
    converted->append(" at @s positioned ");
    appendView(converted, tokenText(command, coordinates[0]));
    converted->push_back(' ');
    appendView(converted, tokenText(command, coordinates[1]));
    converted->push_back(' ');
    appendView(converted, tokenText(command, coordinates[2]));
    if (has_detect) {
        converted->append(" if block ");
        appendView(converted, tokenText(command, detect_coordinates[0]));
        converted->push_back(' ');
        appendView(converted, tokenText(command, detect_coordinates[1]));
        converted->push_back(' ');
        appendView(converted, tokenText(command, detect_coordinates[2]));
        converted->push_back(' ');
        converted->append(converted_detect_block);
    }
    converted->append(" run ");
    appendView(converted, body);
    return true;
}

}  // namespace

bool normalizeLegacyExecuteCommand(std::string* command) {
    if (!command || command->empty()) return false;
    std::string converted;
    if (!convertLegacyExecute(*command, 0, &converted)) return false;
    *command = std::move(converted);
    return true;
}

}  // namespace build_import
