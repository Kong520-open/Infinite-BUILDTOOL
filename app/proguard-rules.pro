# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# Debug builds use R8 shrinking and optimization. Keep every surviving binary
# class name unchanged so Xposed class discovery, Android component names and
# any external reflection continue to use their source names. This does not
# disable shrinking or method/field optimization/obfuscation.
-keepnames class **

# xposed_init is an asset, so R8 cannot see this entry point through normal
# bytecode reachability analysis. Native code also looks NativeCore up by its
# source class descriptor and dynamically registers its JNI methods.
-keep,allowoptimization class com.kong.buildtool.BuildToolsHookInit { *; }
-keep,allowoptimization class com.vdl.kong520.NativeCore { *; }

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile
