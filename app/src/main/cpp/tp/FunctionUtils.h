#ifndef BUILD_TOOL_FUNCTION_UTILS_H
#define BUILD_TOOL_FUNCTION_UTILS_H

#include <cstdint>
#include <main.h>

class FunctionUtils {
public:
    template<typename Result, typename... Args>
    static Result callFunc(uintptr_t address, Args... args) {
        using Function = Result (*)(Args...);
        return reinterpret_cast<Function>(address)(args...);
    }
};

#endif // BUILD_TOOL_FUNCTION_UTILS_H
