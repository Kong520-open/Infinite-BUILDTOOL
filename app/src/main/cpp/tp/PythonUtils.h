#ifndef BUILD_TOOL_PYTHON_UTILS_H
#define BUILD_TOOL_PYTHON_UTILS_H

#include "string"
#include "FunctionUtils.h"
#include "FunctionsAddress.h"
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>

typedef
enum { PyGILState_LOCKED1, PyGILState_UNLOCKED1 }
        PyGILState_STATE1;
class PyObject1;
class PyCompilerFlags1;
constexpr auto Py_file_input1 = 257;


class PythonUtils{
public:

static bool PyEvalUtf8(const std::string& statements, const std::string& expression,
                       std::string* output) {
    if (!output) return false;
    output->clear();
    if (Main::getBaseAddress() == 0 ||
        FunctionsAddress::PythonUtils_setExecLocked == 0 ||
        FunctionsAddress::PythonUtils_PyGILState_EnsureMC == 0 ||
        FunctionsAddress::PythonUtils_PyImport_AddModuleMC == 0 ||
        FunctionsAddress::PythonUtils_PyModule_GetDictMC == 0 ||
        FunctionsAddress::PythonUtils_PyRun_StringFlagsMC == 0 ||
        FunctionsAddress::PythonUtils_PyGILState_ReleaseMC == 0) {
        return false;
    }

    // The Provider loader can place Minecraft's embedded Python in a linker
    // namespace where RTLD_DEFAULT cannot see PyString_AsString/PyErr_Clear.
    // Marshal the result and exception status from Python itself instead of
    // making those optional exported symbols part of correctness.
    std::lock_guard<std::mutex> lock(executionMutex());
    constexpr const char* kStatus = "INFINITECZ_PYEVAL_STATUS";
    constexpr const char* kValue = "INFINITECZ_PYEVAL_VALUE";
    setenv(kStatus, "0", 1);
    setenv(kValue, "", 1);
    setExecLocked(false);
    const PyGILState_STATE1 state = PyGILState_EnsureMC();
    PyObject1* module = PyImport_AddModuleMC("__main__");
    PyObject1* dict = module ? PyModule_GetDictMC(module) : nullptr;
    if (!dict) {
        reportPythonError();
        PyGILState_ReleaseMC(state);
        return false;
    }

    std::string bootstrap =
        "def __infinitecz_checked_eval():\n"
        "  import os, sys, traceback, binascii\n"
        "  __ok = '0'\n"
        "  __value = ''\n"
        "  try:\n"
        "    exec compile(" + pythonBytesLiteral(statements) +
            ", '<infinitecz-eval-setup>', 'exec') in globals(), globals()\n"
        "    __result = eval(compile(" + pythonBytesLiteral(expression) +
            ", '<infinitecz-eval>', 'eval'), globals(), globals())\n"
        "    try:\n"
        "      if isinstance(__result, unicode):\n"
        "        __result = __result.encode('utf-8')\n"
        "      else:\n"
        "        __result = str(__result)\n"
        "    except NameError:\n"
        "      __result = str(__result)\n"
        "    __value = binascii.hexlify(__result)\n"
        "    __ok = '1'\n"
        "  except BaseException:\n"
        "    try:\n"
        "      traceback.print_exc()\n"
        "    except BaseException:\n"
        "      pass\n"
        "  finally:\n"
        "    try:\n"
        "      sys.exc_clear()\n"
        "    except BaseException:\n"
        "      pass\n"
        "    try:\n"
        "      os.environ['INFINITECZ_PYEVAL_VALUE'] = __value\n"
        "      os.environ['INFINITECZ_PYEVAL_STATUS'] = __ok\n"
        "    except BaseException:\n"
        "      pass\n"
        "__infinitecz_checked_eval()\n"
        "try:\n"
        "  del __infinitecz_checked_eval\n"
        "except BaseException:\n"
        "  pass\n";
    PyObject1* result = PyRun_StringFlagsMC(
            bootstrap.c_str(), Py_file_input1, dict, dict, nullptr);
    if (!result) {
        reportPythonError();
    }
    releaseObject(result);
    const char* status = std::getenv(kStatus);
    const char* encoded = std::getenv(kValue);
    const bool success = result != nullptr && status && status[0] == '1' &&
        status[1] == '\0' && encoded && decodeHex(encoded, output);
    PyGILState_ReleaseMC(state);
    return success;
}

static bool PyExecChecked(std::string& str, bool noMessage = false) {
    (void)noMessage;
    if (Main::getBaseAddress() == 0 ||
        FunctionsAddress::PythonUtils_setExecLocked == 0 ||
        FunctionsAddress::PythonUtils_PyGILState_EnsureMC == 0 ||
        FunctionsAddress::PythonUtils_PyImport_AddModuleMC == 0 ||
        FunctionsAddress::PythonUtils_PyModule_GetDictMC == 0 ||
        FunctionsAddress::PythonUtils_PyRun_StringFlagsMC == 0 ||
        FunctionsAddress::PythonUtils_PyGILState_ReleaseMC == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(executionMutex());
    constexpr const char* kStatus = "INFINITECZ_PYEXEC_STATUS";
    setenv(kStatus, "0", 1);
    setExecLocked();
    PyGILState_STATE1 State = PyGILState_EnsureMC();

    PyObject1* Module = PyImport_AddModuleMC("__main__");
    if (!Module) {
        reportPythonError();
        PyGILState_ReleaseMC(State);
        return false;
    }
    PyObject1* Dict = PyModule_GetDictMC(Module);
    if (!Dict) {
        reportPythonError();
        PyGILState_ReleaseMC(State);
        return false;
    }
    std::string bootstrap =
        "def __infinitecz_checked_exec():\n"
        "  import os, sys, traceback\n"
        "  __ok = '0'\n"
        "  try:\n"
        "    exec compile(" + pythonBytesLiteral(str) +
            ", '<infinitecz-exec>', 'exec') in globals(), globals()\n"
        "    __ok = '1'\n"
        "  except BaseException:\n"
        "    try:\n"
        "      traceback.print_exc()\n"
        "    except BaseException:\n"
        "      pass\n"
        "  finally:\n"
        "    try:\n"
        "      sys.exc_clear()\n"
        "    except BaseException:\n"
        "      pass\n"
        "    try:\n"
        "      os.environ['INFINITECZ_PYEXEC_STATUS'] = __ok\n"
        "    except BaseException:\n"
        "      pass\n"
        "__infinitecz_checked_exec()\n"
        "try:\n"
        "  del __infinitecz_checked_exec\n"
        "except BaseException:\n"
        "  pass\n";
    PyObject1* result = PyRun_StringFlagsMC(
            bootstrap.c_str(), Py_file_input1, Dict, Dict, 0);
    if (!result) {
        // Only a failure in the fixed bootstrap itself reaches this path. User
        // code exceptions are caught and explicitly cleared by sys.exc_clear().
        reportPythonError();
    }
    releaseObject(result);
    const char* status = std::getenv(kStatus);
    const bool success = result != nullptr && status && status[0] == '1' && status[1] == '\0';
    PyGILState_ReleaseMC(State);
    return success;
}

// Executes trusted, internally generated source directly. Unlike
// PyExecChecked this avoids constructing and compiling a second Python
// bootstrap around every call. Callers must expose their detailed result via
// their own return channel; a non-null result only means the source itself did
// not raise an uncaught exception.
static bool PyExecDirectChecked(const std::string& str) {
    if (Main::getBaseAddress() == 0 ||
        FunctionsAddress::PythonUtils_setExecLocked == 0 ||
        FunctionsAddress::PythonUtils_PyGILState_EnsureMC == 0 ||
        FunctionsAddress::PythonUtils_PyImport_AddModuleMC == 0 ||
        FunctionsAddress::PythonUtils_PyModule_GetDictMC == 0 ||
        FunctionsAddress::PythonUtils_PyRun_StringFlagsMC == 0 ||
        FunctionsAddress::PythonUtils_PyGILState_ReleaseMC == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(executionMutex());
    setExecLocked();
    const PyGILState_STATE1 state = PyGILState_EnsureMC();
    PyObject1* module = PyImport_AddModuleMC("__main__");
    PyObject1* dict = module ? PyModule_GetDictMC(module) : nullptr;
    if (!dict) {
        reportPythonError();
        PyGILState_ReleaseMC(state);
        return false;
    }
    PyObject1* result = PyRun_StringFlagsMC(
        str.c_str(), Py_file_input1, dict, dict, nullptr);
    if (!result) reportPythonError();
    releaseObject(result);
    PyGILState_ReleaseMC(state);
    return result != nullptr;
}

static void setExecLocked(bool isLock = false) {
    const uintptr_t base_address = Main::getBaseAddress();
    const uintptr_t boolPtr = base_address + FunctionsAddress::PythonUtils_setExecLocked;
    bool* isExecLocked = (bool*)boolPtr;
    *isExecLocked = isLock;
}

static PyGILState_STATE1 PyGILState_EnsureMC() {
    const uintptr_t base_address = Main::getBaseAddress();
    return FunctionUtils::callFunc<PyGILState_STATE1>(base_address + FunctionsAddress::PythonUtils_PyGILState_EnsureMC);
}

static PyObject1* PyImport_AddModuleMC(const char* str) {
    const uintptr_t base_address = Main::getBaseAddress();
    return FunctionUtils::callFunc<PyObject1*, const char*>(base_address + FunctionsAddress::PythonUtils_PyImport_AddModuleMC, str);
}

static PyObject1* PyModule_GetDictMC(PyObject1* obj) {
    const uintptr_t base_address = Main::getBaseAddress();
    return FunctionUtils::callFunc<PyObject1*, PyObject1*>(base_address + FunctionsAddress::PythonUtils_PyModule_GetDictMC, obj);
}

static PyObject1* PyRun_StringFlagsMC(const char* str, int start, PyObject1* globals, PyObject1* locals, PyCompilerFlags1* flags) {
    const uintptr_t base_address = Main::getBaseAddress();
    return FunctionUtils::callFunc<PyObject1*, const char*, int, PyObject1*, PyObject1*, PyCompilerFlags1*>(base_address + FunctionsAddress::PythonUtils_PyRun_StringFlagsMC, str, start, globals, locals, flags);
}

static void PyGILState_ReleaseMC(PyGILState_STATE1 oldstate) {
    const uintptr_t base_address = Main::getBaseAddress();
    return FunctionUtils::callFunc<void, PyGILState_STATE1>(base_address + FunctionsAddress::PythonUtils_PyGILState_ReleaseMC, oldstate);
}

private:
static std::mutex& executionMutex() {
    static std::mutex mutex;
    return mutex;
}

static std::string pythonBytesLiteral(const std::string& value) {
    static const char hex[] = "0123456789abcdef";
    std::string literal;
    literal.reserve(value.size() * 2 + 2);
    literal.push_back('\'');
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': literal += "\\\\"; break;
            case '\'': literal += "\\\'"; break;
            case '\n': literal += "\\n"; break;
            case '\r': literal += "\\r"; break;
            case '\t': literal += "\\t"; break;
            default:
                if (ch >= 0x20 && ch <= 0x7e) {
                    literal.push_back(static_cast<char>(ch));
                } else {
                    literal += "\\x";
                    literal.push_back(hex[(ch >> 4) & 0x0f]);
                    literal.push_back(hex[ch & 0x0f]);
                }
        }
    }
    literal.push_back('\'');
    return literal;
}

static int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool decodeHex(const char* encoded, std::string* output) {
    if (!encoded || !output) return false;
    const std::string value(encoded);
    if ((value.size() & 1U) != 0) return false;
    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        const int high = hexValue(value[i]);
        const int low = hexValue(value[i + 1]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    *output = std::move(decoded);
    return true;
}

static void releaseObject(PyObject1* object) {
    if (!object) return;
    using PyDecRef = void (*)(PyObject1*);
    static PyDecRef decref = reinterpret_cast<PyDecRef>(dlsym(RTLD_DEFAULT, "Py_DecRef"));
    if (decref) decref(object);
}

static void reportPythonError() {
    using PyErrPrint = void (*)();
    using PyErrClear = void (*)();
    static PyErrPrint print_error =
        reinterpret_cast<PyErrPrint>(dlsym(RTLD_DEFAULT, "PyErr_Print"));
    static PyErrClear clear_error =
        reinterpret_cast<PyErrClear>(dlsym(RTLD_DEFAULT, "PyErr_Clear"));
    if (print_error) print_error();
    else if (clear_error) clear_error();
}

};


#endif //BUILD_TOOL_PYTHON_UTILS_H
