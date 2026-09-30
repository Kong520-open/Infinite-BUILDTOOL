#include "FunctionsAddress.h"

/*Class : PythonUtils*/
uintptr_t FunctionsAddress::PythonUtils_setExecLocked                  = 0x1351D350; //U
uintptr_t FunctionsAddress::PythonUtils_PyGILState_EnsureMC            = 0x123552F8; //U
uintptr_t FunctionsAddress::PythonUtils_PyImport_AddModuleMC           = 0x122D4960; //U
uintptr_t FunctionsAddress::PythonUtils_PyModule_GetDictMC             = 0x12249110; //U
uintptr_t FunctionsAddress::PythonUtils_PyRun_StringFlagsMC            = 0x12356C14; //U
uintptr_t FunctionsAddress::PythonUtils_PyGILState_ReleaseMC           = 0x12355390; //U


/*Actor::normalTick - 游戏主循环 hook*/
uintptr_t FunctionsAddress::Minecraft_update_Hook = 0x74C74F8;

uintptr_t FunctionsAddress::Actor_getClientInstance = 0x74C6238;
uintptr_t FunctionsAddress::Actor_getDimension = 0xD8E5854;

/*Level::_render - 世界渲染完成回调*/
uintptr_t FunctionsAddress::Level_Render_Hook = 0x7B6FC08;

/*ClientInstance::getCamera - 投影矩阵访问*/
uintptr_t FunctionsAddress::ClientInstance_getCamera = 0x74636F0;

/*newReceivePacket - 网络包接收*/
uintptr_t FunctionsAddress::newReceivePacket_Hook = 0xA78E8B0;
