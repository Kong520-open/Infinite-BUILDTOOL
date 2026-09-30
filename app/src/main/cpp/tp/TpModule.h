#ifndef MINECRAFT_TPMODULE_H
#define MINECRAFT_TPMODULE_H

#include <string>
#include <jni.h>

bool RegisterTpModuleNatives(JNIEnv* env) noexcept;

// C++ 内部接口
class TpModule {
public:
    static JavaVM* javaVM;
    static std::string worldId;
    static void setJavaVM(JavaVM* vm);
    static void updateWorldId(const std::string& id);
};

#endif // MINECRAFT_TPMODULE_H
