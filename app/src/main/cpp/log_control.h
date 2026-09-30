#ifndef LOG_CONTROL_H
#define LOG_CONTROL_H

#include <android/log.h>

// ==================== 日志开关 ====================
// 开启日志：注释掉下面这行
// 关闭日志：取消注释下面这行（发布时用这个）

 #define DISABLE_LOG

// ==================== 不要修改以下内容 ====================
#ifdef DISABLE_LOG
    #define LOGI(...) ((void)0)
    #define LOGE(...) ((void)0)
#else
    #define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
    #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

#endif // LOG_CONTROL_H
