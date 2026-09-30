#ifndef MINECRAFT_FUNCTIONSADDRESS_H
#define MINECRAFT_FUNCTIONSADDRESS_H

#include <cstdint>

class FunctionsAddress {
public:
    /*Class : PythonUtils*/
    static uintptr_t PythonUtils_setExecLocked;
    static uintptr_t PythonUtils_PyGILState_EnsureMC;
    static uintptr_t PythonUtils_PyImport_AddModuleMC;
    static uintptr_t PythonUtils_PyModule_GetDictMC;
    static uintptr_t PythonUtils_PyRun_StringFlagsMC;
    static uintptr_t PythonUtils_PyGILState_ReleaseMC;
    static uintptr_t Actor_getClientInstance;
    static uintptr_t Actor_getDimension;

    static uintptr_t Minecraft_update_Hook;
    static uintptr_t Level_Render_Hook;
    static uintptr_t ClientInstance_getCamera;
    static uintptr_t newReceivePacket_Hook;
};

#endif // MINECRAFT_FUNCTIONSADDRESS_H
