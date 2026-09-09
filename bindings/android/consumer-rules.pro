# Every class in the binding is looked up by name from sigil_jni.c, and its
# constructors by exact signature. Shrinking or renaming any of them aborts
# the process at the first FindClass. Keep the whole package.
-keep class com.nendo.sigil.** { *; }
