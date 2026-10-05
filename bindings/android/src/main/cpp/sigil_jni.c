// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "save_name.h"
#include "utf16.h"
#include <jni.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* `s` as standard UTF-8 in memory the caller frees with jni_free, or NULL for
 * a null string or when memory runs out. */
static char *jni_utf8(JNIEnv *env, jstring s) {
    if (!s) return NULL;
    jsize len = (*env)->GetStringLength(env, s);
    const jchar *units = (*env)->GetStringChars(env, s, NULL);
    if (!units) return NULL;
    char *out = (char *)malloc(sigil_utf8_capacity((size_t)len));
    if (out) sigil_utf16_to_utf8((const uint16_t *)units, (size_t)len, out);
    (*env)->ReleaseStringChars(env, s, units);
    return out;
}

static void jni_free(const char *utf8) {
    free((void *)utf8);
}

/* A Java string for standard UTF-8 `utf8`; bytes that aren't UTF-8 become
 * U+FFFD. NULL with an exception pending when memory runs out. */
static jstring jni_string(JNIEnv *env, const char *utf8) {
    uint16_t *units = (uint16_t *)malloc((sigil_utf16_capacity(utf8) + 1) * sizeof(uint16_t));
    if (!units) return NULL;
    size_t n = sigil_utf8_to_utf16(utf8, units);
    jstring s = (*env)->NewString(env, (const jchar *)units, (jsize)n);
    free(units);
    return s;
}

static jclass g_result_class = NULL;
static jmethodID g_result_ctor = NULL;
static jclass g_exception_class = NULL;
static jmethodID g_exception_ctor = NULL;

/* A class R8 renamed or dropped leaves ClassNotFoundException pending, and the
 * next JNI call on top of it aborts the process; clear it and report NULL. */
static jclass find_class(JNIEnv *env, const char *name) {
    jclass cls = (*env)->FindClass(env, name);
    if (!cls) (*env)->ExceptionClear(env);
    return cls;
}

static jmethodID find_method(JNIEnv *env, jclass cls, const char *name, const char *sig) {
    jmethodID id = (*env)->GetMethodID(env, cls, name, sig);
    if (!id) (*env)->ExceptionClear(env);
    return id;
}

static jclass global_class(JNIEnv *env, const char *name) {
    jclass cls = find_class(env, name);
    if (!cls) return NULL;
    jclass global = (jclass)(*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);
    return global;
}

static void load_result_class(JNIEnv *env) {
    if (g_result_class) return;
    g_result_class = global_class(env, "com/nendo/sigil/SigilResult");
    if (!g_result_class) return;
    /* SigilResult(titleId, rawSerial, saveId, platformSlug, source, usage, experimental, features, switchContentType,
     *             titleVersion, n64Header, n64Md5, n64Md5N64) */
    g_result_ctor = find_method(env, g_result_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIZIIJLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
}

static jclass g_list_class = NULL;
static jmethodID g_list_ctor = NULL;
static jmethodID g_list_add = NULL;
static jclass g_profile_class = NULL;
static jmethodID g_profile_ctor = NULL;

static void load_exception_class(JNIEnv *env) {
    if (g_exception_class) return;
    g_exception_class = global_class(env, "com/nendo/sigil/SigilException");
    if (!g_exception_class) return;
    /* SigilException(code, message, problem, blocksShort, profiles) */
    g_exception_ctor = find_method(env, g_exception_class, "<init>",
        "(ILjava/lang/String;Ljava/lang/String;ILjava/util/List;)V");
}

static bool profile_classes_ready(void) {
    return g_list_class && g_list_ctor && g_list_add && g_profile_class && g_profile_ctor;
}

static void load_profile_classes(JNIEnv *env) {
    if (profile_classes_ready()) return;
    if (!g_list_class) g_list_class = global_class(env, "java/util/ArrayList");
    if (!g_profile_class) g_profile_class = global_class(env, "com/nendo/sigil/SigilProfile");
    if (!g_list_class || !g_profile_class) return;
    g_list_ctor = find_method(env, g_list_class, "<init>", "()V");
    g_list_add = find_method(env, g_list_class, "add", "(Ljava/lang/Object;)Z");
    /* SigilProfile(id, name) */
    g_profile_ctor = find_method(env, g_profile_class, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V");
}

/* An ArrayList of SigilProfile, or NULL with an exception pending or the classes missing. */
static jobject profile_list(JNIEnv *env, const sigil_save_profile *profiles, size_t count) {
    load_profile_classes(env);
    if (!profile_classes_ready()) return NULL;
    jobject list = (*env)->NewObject(env, g_list_class, g_list_ctor);
    for (size_t i = 0; list && i < count; i++) {
        jstring jid = jni_string(env,profiles[i].id);
        jstring jname = jni_string(env,profiles[i].name);
        jobject p = (*env)->NewObject(env, g_profile_class, g_profile_ctor, jid, jname);
        if (p) (*env)->CallBooleanMethod(env, list, g_list_add, p);
        (*env)->DeleteLocalRef(env, jid);
        (*env)->DeleteLocalRef(env, jname);
        if (!p || (*env)->ExceptionCheck(env)) return NULL;
        (*env)->DeleteLocalRef(env, p);
    }
    return list;
}

/* Throws SigilException for `code`, naming what is at fault when `problem`
 * is given, for a save that didn't fit the blocks it lacked, and the profiles
 * the emulator lists. */
static void throw_sigil_problem(JNIEnv *env, int code, const char *problem, uint32_t blocks_short,
                                const sigil_save_profile *profiles, size_t profile_count) {
    if ((*env)->ExceptionCheck(env)) return;
    load_exception_class(env);
    jobject jprofiles = g_exception_class && g_exception_ctor ? profile_list(env, profiles, profile_count) : NULL;
    if (jprofiles) {
        char escaped[3 * SIGIL_SAVE_PATH_MAX + 1] = "";
        sigil_save_lines_escape(problem ? problem : "", escaped, sizeof(escaped));
        jstring jmessage = jni_string(env,sigil_strerror(code));
        jstring jproblem = jni_string(env,escaped);
        jobject ex = (*env)->NewObject(env, g_exception_class, g_exception_ctor, (jint)code, jmessage, jproblem,
                                       (jint)blocks_short, jprofiles);
        (*env)->DeleteLocalRef(env, jmessage);
        (*env)->DeleteLocalRef(env, jproblem);
        (*env)->DeleteLocalRef(env, jprofiles);
        if (ex) {
            (*env)->Throw(env, (jthrowable)ex);
            return;
        }
    }
    (*env)->ExceptionClear(env);
    jclass fallback = find_class(env, "java/lang/IllegalStateException");
    if (fallback) (*env)->ThrowNew(env, fallback, sigil_strerror(code));
}

static void throw_sigil(JNIEnv *env, int code) { throw_sigil_problem(env, code, NULL, 0, NULL, 0); }

static void throw_binding_broken(JNIEnv *env, const char *what) {
    if ((*env)->ExceptionCheck(env)) return;
    char message[160];
    snprintf(message, sizeof(message), "sigil binding: %s is missing from the apk; check the keep rules", what);
    jclass cls = find_class(env, "java/lang/IllegalStateException");
    if (cls) (*env)->ThrowNew(env, cls, message);
}

static jclass g_member_class = NULL;
static jmethodID g_member_ctor = NULL;
static jclass g_unit_class = NULL;
static jmethodID g_unit_ctor = NULL;
static jclass g_array_list_class = NULL;
static jmethodID g_array_list_ctor = NULL;
static jmethodID g_array_list_add = NULL;
static jclass g_card_entry_class = NULL;
static jmethodID g_card_entry_ctor = NULL;
static jclass g_card_listing_class = NULL;
static jmethodID g_card_listing_ctor = NULL;
static jclass g_sync_result_class = NULL;
static jmethodID g_sync_result_ctor = NULL;
static jclass g_companion_result_class = NULL;
static jmethodID g_companion_result_ctor = NULL;
static jclass g_alternate_class = NULL;
static jmethodID g_alternate_ctor = NULL;

/* SigilSaveAlternate and the ArrayList that carries it, for units and sync results alike. */
static bool alternate_class_ready(void) {
    return g_alternate_class && g_alternate_ctor && g_array_list_class && g_array_list_ctor && g_array_list_add;
}

static void load_alternate_class(JNIEnv *env) {
    if (alternate_class_ready()) return;
    if (!g_alternate_class) g_alternate_class = global_class(env, "com/nendo/sigil/SigilSaveAlternate");
    if (!g_array_list_class) g_array_list_class = global_class(env, "java/util/ArrayList");
    if (!g_alternate_class || !g_array_list_class) return;
    /* SigilSaveAlternate(path, shared, optionKeys, optionValues) */
    g_alternate_ctor = find_method(env, g_alternate_class, "<init>",
        "(Ljava/lang/String;ZLjava/util/List;Ljava/util/List;)V");
    if (!g_array_list_ctor) g_array_list_ctor = find_method(env, g_array_list_class, "<init>", "()V");
    if (!g_array_list_add) g_array_list_add = find_method(env, g_array_list_class, "add", "(Ljava/lang/Object;)Z");
}

static bool unit_classes_ready(void) {
    return g_member_class && g_member_ctor && g_unit_class && g_unit_ctor && alternate_class_ready();
}

static void load_unit_classes(JNIEnv *env) {
    if (unit_classes_ready()) return;
    load_alternate_class(env);
    if (!g_member_class) g_member_class = global_class(env, "com/nendo/sigil/SigilSaveMember");
    if (!g_unit_class) g_unit_class = global_class(env, "com/nendo/sigil/SigilSaveUnit");
    if (!g_member_class || !g_unit_class) return;
    /* SigilSaveMember(path, entry, roleCode, present, areaCode) */
    g_member_ctor = find_method(env, g_member_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;IZI)V");
    /* SigilSaveUnit(key, shapeCode, members, expected, unkeyed, artifact, contentHash, identityHash, alternates) */
    g_unit_ctor = find_method(env, g_unit_class, "<init>",
        "(Ljava/lang/String;ILjava/util/List;Ljava/util/List;Ljava/util/List;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/List;)V");
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) return;
    if (g_result_class) {
        (*env)->DeleteGlobalRef(env, g_result_class);
        g_result_class = NULL;
        g_result_ctor = NULL;
    }
    if (g_exception_class) {
        (*env)->DeleteGlobalRef(env, g_exception_class);
        g_exception_class = NULL;
        g_exception_ctor = NULL;
    }
    if (g_list_class) (*env)->DeleteGlobalRef(env, g_list_class);
    if (g_profile_class) (*env)->DeleteGlobalRef(env, g_profile_class);
    g_list_class = NULL;
    g_profile_class = NULL;
    g_list_ctor = NULL;
    g_list_add = NULL;
    g_profile_ctor = NULL;
    if (g_member_class) (*env)->DeleteGlobalRef(env, g_member_class);
    if (g_unit_class) (*env)->DeleteGlobalRef(env, g_unit_class);
    if (g_array_list_class) (*env)->DeleteGlobalRef(env, g_array_list_class);
    g_member_class = NULL;
    g_unit_class = NULL;
    g_array_list_class = NULL;
    g_member_ctor = NULL;
    g_unit_ctor = NULL;
    g_array_list_ctor = NULL;
    g_array_list_add = NULL;
    if (g_card_entry_class) (*env)->DeleteGlobalRef(env, g_card_entry_class);
    if (g_card_listing_class) (*env)->DeleteGlobalRef(env, g_card_listing_class);
    g_card_entry_class = NULL;
    g_card_listing_class = NULL;
    g_card_entry_ctor = NULL;
    g_card_listing_ctor = NULL;
    if (g_sync_result_class) (*env)->DeleteGlobalRef(env, g_sync_result_class);
    g_sync_result_class = NULL;
    g_sync_result_ctor = NULL;
    if (g_companion_result_class) (*env)->DeleteGlobalRef(env, g_companion_result_class);
    g_companion_result_class = NULL;
    g_companion_result_ctor = NULL;
    if (g_alternate_class) (*env)->DeleteGlobalRef(env, g_alternate_class);
    g_alternate_class = NULL;
    g_alternate_ctor = NULL;
}

JNIEXPORT jstring JNICALL
Java_com_nendo_sigil_Sigil_nativeVersion(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return jni_string(env,sigil_version());
}

JNIEXPORT jstring JNICALL
Java_com_nendo_sigil_Sigil_nativePlatformSlug(JNIEnv *env, jclass clazz, jstring jslug) {
    (void)clazz;
    const char *slug = jslug ? jni_utf8(env, jslug) : NULL;
    const char *canonical = sigil_platform_to_slug(sigil_platform_from_slug(slug));
    if (slug) jni_free(slug);
    return jni_string(env,canonical);
}

JNIEXPORT jstring JNICALL
Java_com_nendo_sigil_Sigil_nativeContentStem(JNIEnv *env, jclass clazz, jstring jcontent) {
    (void)clazz;
    char stem[SIGIL_SAVE_ENTRY_MAX];
    const char *content = jcontent ? jni_utf8(env, jcontent) : NULL;
    sigil_content_stem(content, stem, sizeof(stem));
    if (content) jni_free(content);
    return jni_string(env,stem);
}

JNIEXPORT jbyteArray JNICALL
Java_com_nendo_sigil_Sigil_nativeLoadHeaderKey(JNIEnv *env, jclass clazz, jstring jpath) {
    (void)clazz;
    if (!jpath) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    uint8_t key[32];
    const char *path = jni_utf8(env, jpath);
    int rc = sigil_load_header_key_from_prod_keys(path, key);
    jni_free(path);
    if (rc != SIGIL_OK) { throw_sigil(env, rc); return NULL; }
    jbyteArray out = (*env)->NewByteArray(env, 32);
    if (out) (*env)->SetByteArrayRegion(env, out, 0, 32, (const jbyte *)key);
    return out;
}

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeExtract(JNIEnv *env, jclass clazz,
                                          jstring jpath,
                                          jstring jplatform_slug,
                                          jstring jprod_keys_path,
                                          jbyteArray jprod_keys_text,
                                          jbyteArray jheader_key,
                                          jint flags) {
    (void)clazz;
    if (!jpath) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }

    const char *path = jni_utf8(env, jpath);
    if (!path) { throw_sigil(env, SIGIL_ERR_OOM); return NULL; }

    const char *slug = jplatform_slug ? jni_utf8(env, jplatform_slug) : NULL;
    const char *prod_keys = jprod_keys_path ? jni_utf8(env, jprod_keys_path) : NULL;
    jbyte *keys_text = jprod_keys_text ? (*env)->GetByteArrayElements(env, jprod_keys_text, NULL) : NULL;
    jsize keys_text_len = jprod_keys_text ? (*env)->GetArrayLength(env, jprod_keys_text) : 0;
    uint8_t header_key[32];
    bool has_header_key = jheader_key && (*env)->GetArrayLength(env, jheader_key) == 32;
    if (has_header_key) (*env)->GetByteArrayRegion(env, jheader_key, 0, 32, (jbyte *)header_key);

    sigil_support sup = {
        .struct_version = SIGIL_SUPPORT_V1,
        .switch_header_key = has_header_key ? header_key : NULL,
        .switch_prod_keys_path = prod_keys,
        .switch_prod_keys_text = (const char *)keys_text,
        .switch_prod_keys_text_len = (size_t)keys_text_len,
    };
    bool need_support = has_header_key || prod_keys || keys_text;
    sigil_options opts = {
        .struct_version = SIGIL_OPTIONS_V1,
        .support = need_support ? &sup : NULL,
        .flags = (uint32_t)flags,
    };

    sigil_result r;
    memset(&r, 0, sizeof(r));
    r.struct_version = SIGIL_RESULT_V4;
    int rc = sigil_extract_from_path(path, sigil_platform_from_slug(slug), &opts, &r);

    jni_free(path);
    if (slug)      jni_free(slug);
    if (prod_keys) jni_free(prod_keys);
    if (keys_text) (*env)->ReleaseByteArrayElements(env, jprod_keys_text, keys_text, JNI_ABORT);

    if (rc != SIGIL_OK) { throw_sigil(env, rc); return NULL; }

    load_result_class(env);
    if (!g_result_class || !g_result_ctor) { throw_binding_broken(env, "SigilResult"); return NULL; }

    jstring jtitle   = jni_string(env,r.title_id);
    jstring jraw     = jni_string(env,r.raw_serial);
    jstring jsave_id = jni_string(env,r.save_id);
    jstring jslug    = jni_string(env,sigil_platform_to_slug(r.platform));
    jstring jheader  = jni_string(env,r.n64_header);
    jstring jmd5     = jni_string(env,r.n64_md5);
    jstring jmd5_n64 = jni_string(env,r.n64_md5_n64);

    return (*env)->NewObject(env, g_result_class, g_result_ctor,
                             jtitle, jraw, jsave_id, jslug,
                             (jint)r.source, (jint)r.usage,
                             r.experimental ? JNI_TRUE : JNI_FALSE,
                             (jint)r.features,
                             (jint)r.switch_content_type,
                             (jlong)r.title_version,
                             jheader, jmd5, jmd5_n64);
}

/* ---- save units ------------------------------------------------------------ */

/* The caller's SigilFileAccess and the save root its calls are relative to.
 * Every file sigil opens, writes or removes goes through it. */
typedef struct {
    JNIEnv *env;
    jobject access;
    jstring root;
} access_ctx;

static jclass g_access_class = NULL;
static jmethodID g_access_read = NULL;
static jmethodID g_access_write = NULL;
static jmethodID g_access_remove = NULL;

static bool load_access_class(JNIEnv *env) {
    if (g_access_read && g_access_write && g_access_remove) return true;
    if (!g_access_class) g_access_class = global_class(env, "com/nendo/sigil/SigilFileAccess");
    if (!g_access_class) return false;
    g_access_read = find_method(env, g_access_class, "read", "(Ljava/lang/String;Ljava/lang/String;)[B");
    g_access_write = find_method(env, g_access_class, "write", "(Ljava/lang/String;Ljava/lang/String;[B)Z");
    g_access_remove = find_method(env, g_access_class, "remove", "(Ljava/lang/String;Ljava/lang/String;)Z");
    return g_access_read && g_access_write && g_access_remove;
}

/* A failed or throwing call is an I/O error to sigil; the Java exception is
 * cleared so the native call can finish and raise SIGIL_ERR_IO itself. */
static bool access_threw(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return false;
    (*env)->ExceptionClear(env);
    return true;
}

typedef struct {
    uint8_t *data;
    size_t len;
} bytes_io;

static int bytes_read(void *ctx, uint64_t off, void *buf, size_t len) {
    bytes_io *b = (bytes_io *)ctx;
    if (off >= b->len) return 0;
    size_t n = len < b->len - (size_t)off ? len : b->len - (size_t)off;
    memcpy(buf, b->data + off, n);
    return (int)n;
}

static int64_t bytes_size(void *ctx) { return (int64_t)((bytes_io *)ctx)->len; }

static void bytes_close(void *ctx) {
    bytes_io *b = (bytes_io *)ctx;
    free(b->data);
    free(b);
}

static sigil_io *open_member(void *ctx, const char *relative_path) {
    access_ctx *a = (access_ctx *)ctx;
    JNIEnv *env = a->env;
    jstring jpath = jni_string(env, relative_path);
    jbyteArray data = jpath ? (jbyteArray)(*env)->CallObjectMethod(env, a->access, g_access_read, a->root, jpath) : NULL;
    if (jpath) (*env)->DeleteLocalRef(env, jpath);
    if (access_threw(env) || !data) return NULL;
    jsize len = (*env)->GetArrayLength(env, data);
    bytes_io *b = (bytes_io *)calloc(1, sizeof(*b));
    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    if (b) b->data = (uint8_t *)malloc(len ? (size_t)len : 1);
    if (!b || !io || !b->data) {
        if (b) free(b->data);
        free(b);
        free(io);
        (*env)->DeleteLocalRef(env, data);
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)b->data);
    (*env)->DeleteLocalRef(env, data);
    b->len = (size_t)len;
    io->read = bytes_read;
    io->size = bytes_size;
    io->close = bytes_close;
    io->ctx = b;
    return io;
}

static jobject member_list(JNIEnv *env, const sigil_save_member *members, size_t count) {
    jobject list = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    if (!list) return NULL;
    for (size_t i = 0; i < count; i++) {
        jstring jpath  = jni_string(env,members[i].path);
        jstring jentry = jni_string(env,members[i].entry);
        jobject m = (*env)->NewObject(env, g_member_class, g_member_ctor,
                                      jpath, jentry, (jint)members[i].role,
                                      members[i].present ? JNI_TRUE : JNI_FALSE, (jint)members[i].area);
        if (m) (*env)->CallBooleanMethod(env, list, g_array_list_add, m);
        (*env)->DeleteLocalRef(env, jpath);
        (*env)->DeleteLocalRef(env, jentry);
        if (m) (*env)->DeleteLocalRef(env, m);
    }
    return list;
}

static jobject string_list(JNIEnv *env, char (*items)[SIGIL_SAVE_PATH_MAX], size_t count) {
    jobject list = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    if (!list) return NULL;
    for (size_t i = 0; i < count; i++) {
        jstring s = jni_string(env,items[i]);
        (*env)->CallBooleanMethod(env, list, g_array_list_add, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return list;
}

/* An ArrayList of SigilSaveAlternate, or NULL with an exception pending. */
static jobject alternate_list(JNIEnv *env, const sigil_save_alternate *alternates, size_t count) {
    jobject list = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    for (size_t i = 0; list && i < count; i++) {
        const sigil_save_alternate *a = &alternates[i];
        jstring jpath = jni_string(env,a->path);
        jobject keys = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
        jobject values = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
        for (size_t o = 0; keys && values && o < a->option_count; o++) {
            jstring k = jni_string(env,a->options[o].key);
            jstring v = jni_string(env,a->options[o].value);
            (*env)->CallBooleanMethod(env, keys, g_array_list_add, k);
            (*env)->CallBooleanMethod(env, values, g_array_list_add, v);
            (*env)->DeleteLocalRef(env, k);
            (*env)->DeleteLocalRef(env, v);
        }
        jobject alt = jpath && keys && values && !(*env)->ExceptionCheck(env)
                          ? (*env)->NewObject(env, g_alternate_class, g_alternate_ctor, jpath,
                                              a->shared ? JNI_TRUE : JNI_FALSE, keys, values)
                          : NULL;
        if (alt) (*env)->CallBooleanMethod(env, list, g_array_list_add, alt);
        (*env)->DeleteLocalRef(env, jpath);
        (*env)->DeleteLocalRef(env, keys);
        (*env)->DeleteLocalRef(env, values);
        if (!alt || (*env)->ExceptionCheck(env)) return NULL;
        (*env)->DeleteLocalRef(env, alt);
    }
    return list;
}

/* NULL-terminated copy of a Java String[]; free with release_strings. */
static const char **borrow_strings(JNIEnv *env, jobjectArray arr, jsize *out_count) {
    jsize count = arr ? (*env)->GetArrayLength(env, arr) : 0;
    const char **out = (const char **)calloc((size_t)count + 1, sizeof(char *));
    if (!out) { *out_count = 0; return NULL; }
    for (jsize i = 0; i < count; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, arr, i);
        out[i] = jni_utf8(env, s);
        if (s) (*env)->DeleteLocalRef(env, s);
    }
    *out_count = count;
    return out;
}

static void release_strings(const char **strings, jsize count) {
    if (!strings) return;
    for (jsize i = 0; i < count; i++) jni_free(strings[i]);
    free((void *)strings);
}

/* The stored N64 fields, SigilResult.n64Fields: header, md5, md5_n64. */
static void copy_n64_fields(JNIEnv *env, jobjectArray jn64, sigil_result *result) {
    jsize count = 0;
    const char **n64 = borrow_strings(env, jn64, &count);
    char *fields[] = { result->n64_header, result->n64_md5, result->n64_md5_n64 };
    size_t caps[] = { sizeof(result->n64_header), sizeof(result->n64_md5), sizeof(result->n64_md5_n64) };
    for (jsize i = 0; n64 && i < count && i < 3; i++) {
        if (n64[i]) snprintf(fields[i], caps[i], "%s", n64[i]);
    }
    release_strings(n64, count);
}

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeLocateSaves(JNIEnv *env, jclass clazz,
                                              jstring jlayout, jstring jplatform,
                                              jstring jcontent, jstring jtitle_id, jstring jraw_serial,
                                              jstring jsave_id, jint features, jobjectArray jn64,
                                              jobjectArray jopt_keys, jobjectArray jopt_values,
                                              jobjectArray jlisting, jstring jroot, jstring jprofile,
                                              jobject jaccess) {
    (void)clazz;
    if (!jlayout || !jcontent || !jaccess) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    if (!load_access_class(env)) { throw_binding_broken(env, "SigilFileAccess"); return NULL; }

    const char *layout   = jni_utf8(env, jlayout);
    const char *platform = jplatform ? jni_utf8(env, jplatform) : NULL;
    const char *content  = jni_utf8(env, jcontent);
    const char *title_id = jtitle_id ? jni_utf8(env, jtitle_id) : NULL;
    const char *raw_serial = jraw_serial ? jni_utf8(env, jraw_serial) : NULL;
    const char *save_id  = jsave_id ? jni_utf8(env, jsave_id) : NULL;
    const char *root     = jroot ? jni_utf8(env, jroot) : NULL;
    const char *profile  = jprofile ? jni_utf8(env, jprofile) : NULL;

    jsize key_count = 0, value_count = 0, listing_count = 0;
    const char **keys    = borrow_strings(env, jopt_keys, &key_count);
    const char **values  = borrow_strings(env, jopt_values, &value_count);
    const char **listing = borrow_strings(env, jlisting, &listing_count);

    sigil_save_option *options = NULL;
    size_t option_count = 0;
    if (keys && values && key_count == value_count && key_count > 0) {
        options = (sigil_save_option *)calloc((size_t)key_count, sizeof(*options));
        if (options) {
            for (jsize i = 0; i < key_count; i++) {
                options[i].key = keys[i];
                options[i].value = values[i];
            }
            option_count = (size_t)key_count;
        }
    }

    sigil_result result;
    memset(&result, 0, sizeof(result));
    result.struct_version = SIGIL_RESULT_V4;
    result.features = (uint32_t)features;
    if (title_id) strncpy(result.title_id, title_id, sizeof(result.title_id) - 1);
    if (raw_serial) strncpy(result.raw_serial, raw_serial, sizeof(result.raw_serial) - 1);
    if (save_id)  strncpy(result.save_id, save_id, sizeof(result.save_id) - 1);
    copy_n64_fields(env, jn64, &result);

    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = layout;
    req.platform = platform;
    req.content_path = content;
    req.result = &result;
    req.features = (uint32_t)features;
    req.options = options;
    req.option_count = option_count;
    req.listing = listing;
    req.listing_count = (size_t)listing_count;
    req.root_path = root;
    req.profile = profile;
    access_ctx actx = { env, jaccess, jroot };
    if (root && sigil_save_layout_top(layout)) {
        req.open = open_member;
        req.open_ctx = &actx;
    }

    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req, &unit);

    jobject out = NULL;
    bool binding_broken = false;
    if (rc == SIGIL_OK && unit) {
        load_unit_classes(env);
        binding_broken = !unit_classes_ready();
        if (!binding_broken) {
            jstring jkey      = jni_string(env,unit->key);
            jstring jartifact = jni_string(env,unit->artifact);
            jstring jhash     = jni_string(env,unit->content_hash);
            jstring jidentity = jni_string(env,unit->identity_hash);
            jobject members   = member_list(env, unit->members, unit->member_count);
            jobject expected  = member_list(env, unit->expected, unit->expected_count);
            jobject unkeyed   = string_list(env, unit->unkeyed, unit->unkeyed_count);
            jobject alternates = alternate_list(env, unit->alternates, unit->alternate_count);
            if (members && expected && unkeyed && alternates) {
                out = (*env)->NewObject(env, g_unit_class, g_unit_ctor,
                                        jkey, (jint)unit->shape, members, expected, unkeyed,
                                        jartifact, jhash, jidentity, alternates);
            }
        }
    }
    sigil_save_unit_free(unit);

    free(options);
    release_strings(keys, key_count);
    release_strings(values, value_count);
    release_strings(listing, listing_count);
    jni_free(layout);
    if (platform) jni_free(platform);
    jni_free(content);
    if (title_id) jni_free(title_id);
    if (raw_serial) jni_free(raw_serial);
    if (save_id)  jni_free(save_id);
    if (root)     jni_free(root);
    if (profile)  jni_free(profile);

    if (rc != SIGIL_OK) throw_sigil(env, rc);
    else if (binding_broken) throw_binding_broken(env, "SigilSaveMember or SigilSaveUnit");
    else if (!out && !(*env)->ExceptionCheck(env)) throw_sigil(env, SIGIL_ERR_OOM);
    return out;
}

JNIEXPORT jobjectArray JNICALL
Java_com_nendo_sigil_Sigil_nativeHashSaves(JNIEnv *env, jclass clazz,
                                            jstring jroot, jstring jkey, jint shape,
                                            jobjectArray jpaths, jobjectArray jentries,
                                            jintArray jroles, jobject jaccess) {
    (void)clazz;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!jroot || !jkey || !jpaths || !jentries || !jroles || !jaccess || !string_class) {
        throw_sigil(env, SIGIL_ERR_INVALID_ARG);
        return NULL;
    }
    if (!load_access_class(env)) { throw_binding_broken(env, "SigilFileAccess"); return NULL; }

    jsize path_count = 0, entry_count = 0;
    const char **paths   = borrow_strings(env, jpaths, &path_count);
    const char **entries = borrow_strings(env, jentries, &entry_count);
    jsize role_count = (*env)->GetArrayLength(env, jroles);
    jint *roles = (*env)->GetIntArrayElements(env, jroles, NULL);
    const char *root = jni_utf8(env, jroot);
    const char *key  = jni_utf8(env, jkey);

    int rc = SIGIL_ERR_INVALID_ARG;
    sigil_save_unit unit;
    memset(&unit, 0, sizeof(unit));
    if (paths && entries && roles && path_count == entry_count && path_count == role_count) {
        unit.struct_version = SIGIL_SAVE_UNIT_V1;
        unit.shape = (int)shape;
        strncpy(unit.key, key, SIGIL_SAVE_ENTRY_MAX - 1);
        unit.members = (sigil_save_member *)calloc((size_t)path_count, sizeof(sigil_save_member));
        if (!unit.members) {
            rc = SIGIL_ERR_OOM;
        } else {
            for (jsize i = 0; i < path_count; i++) {
                strncpy(unit.members[i].path, paths[i] ? paths[i] : "", SIGIL_SAVE_PATH_MAX - 1);
                strncpy(unit.members[i].entry, entries[i] ? entries[i] : "", SIGIL_SAVE_ENTRY_MAX - 1);
                unit.members[i].role = (int)roles[i];
                unit.members[i].present = 1;
            }
            unit.member_count = (size_t)path_count;
            access_ctx actx = { env, jaccess, jroot };
            rc = sigil_save_hash(&unit, open_member, &actx);
        }
    }

    jobjectArray out = NULL;
    if (rc == SIGIL_OK) {
        out = (*env)->NewObjectArray(env, 2, string_class, NULL);
        jstring jhash     = jni_string(env,unit.content_hash);
        jstring jidentity = jni_string(env,unit.identity_hash);
        (*env)->SetObjectArrayElement(env, out, 0, jhash);
        (*env)->SetObjectArrayElement(env, out, 1, jidentity);
        (*env)->DeleteLocalRef(env, jhash);
        (*env)->DeleteLocalRef(env, jidentity);
    }

    free(unit.members);
    release_strings(paths, path_count);
    release_strings(entries, entry_count);
    if (roles) (*env)->ReleaseIntArrayElements(env, jroles, roles, JNI_ABORT);
    jni_free(root);
    jni_free(key);

    if (rc != SIGIL_OK) throw_sigil(env, rc);
    return out;
}

/* ---- memory cards ---------------------------------------------------------- */

static bool card_classes_ready(void) {
    return g_card_entry_class && g_card_entry_ctor && g_card_listing_class && g_card_listing_ctor
        && g_array_list_class && g_array_list_ctor && g_array_list_add;
}

static void load_card_classes(JNIEnv *env) {
    if (card_classes_ready()) return;
    if (!g_card_entry_class) g_card_entry_class = global_class(env, "com/nendo/sigil/SigilCardEntry");
    if (!g_card_listing_class) g_card_listing_class = global_class(env, "com/nendo/sigil/SigilCardListing");
    if (!g_array_list_class) g_array_list_class = global_class(env, "java/util/ArrayList");
    if (!g_card_entry_class || !g_card_listing_class || !g_array_list_class) return;
    /* SigilCardEntry(name, ownerId, blocks, firstBlock) */
    g_card_entry_ctor = find_method(env, g_card_entry_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;II)V");
    /* SigilCardListing(formatCode, totalBlocks, freeBlocks, freeSlots, corruptCount, entries, corruptEntries) */
    g_card_listing_ctor = find_method(env, g_card_listing_class, "<init>",
        "(IIIIILjava/util/List;Ljava/util/List;)V");
    if (!g_array_list_ctor) g_array_list_ctor = find_method(env, g_array_list_class, "<init>", "()V");
    if (!g_array_list_add) g_array_list_add = find_method(env, g_array_list_class, "add", "(Ljava/lang/Object;)Z");
}

static jstring save_name_string(JNIEnv *env, const char *raw) {
    char escaped[3 * SIGIL_CARD_NAME_MAX + 1];
    sigil_save_name_escape(raw, escaped, sizeof(escaped));
    return jni_string(env,escaped);
}

/* An ArrayList of SigilCardEntry for `count` entries, or NULL with an exception pending. */
static jobject card_entry_list(JNIEnv *env, const sigil_card_entry *entries, size_t count) {
    jobject list = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    for (size_t i = 0; list && i < count; i++) {
        const sigil_card_entry *e = &entries[i];
        jstring jname  = save_name_string(env, e->name);
        jstring jowner = save_name_string(env, e->owner_id);
        jobject je = (*env)->NewObject(env, g_card_entry_class, g_card_entry_ctor,
                                       jname, jowner, (jint)e->blocks, (jint)e->first_block);
        if (je) (*env)->CallBooleanMethod(env, list, g_array_list_add, je);
        (*env)->DeleteLocalRef(env, jname);
        (*env)->DeleteLocalRef(env, jowner);
        if (je) (*env)->DeleteLocalRef(env, je);
    }
    return list;
}

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeListCard(JNIEnv *env, jclass clazz, jstring jroot, jstring jname,
                                          jobject jaccess) {
    (void)clazz;
    if (!jroot || !jname || !jaccess) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    if (!load_access_class(env)) { throw_binding_broken(env, "SigilFileAccess"); return NULL; }
    const char *name = jni_utf8(env, jname);
    access_ctx actx = { env, jaccess, jroot };
    sigil_io *io = name ? open_member(&actx, name) : NULL;
    if (name) jni_free(name);
    if (!io) { throw_sigil(env, SIGIL_ERR_IO); return NULL; }

    sigil_card_listing *listing = NULL;
    int rc = sigil_card_list(io, &listing);
    sigil_io_close(io);
    if (rc != SIGIL_OK) { throw_sigil(env, rc); return NULL; }

    load_card_classes(env);
    if (!card_classes_ready()) {
        sigil_card_listing_free(listing);
        throw_binding_broken(env, "SigilCardEntry or SigilCardListing");
        return NULL;
    }

    jobject out = NULL;
    jobject entries = card_entry_list(env, listing->entries, listing->entry_count);
    jobject corrupt = entries ? card_entry_list(env, listing->corrupt_entries, listing->corrupt_entry_count) : NULL;
    if (entries && corrupt) {
        out = (*env)->NewObject(env, g_card_listing_class, g_card_listing_ctor,
                                (jint)listing->format, (jint)listing->total_blocks,
                                (jint)listing->free_blocks, (jint)listing->free_slots,
                                (jint)listing->corrupt_count, entries, corrupt);
    }
    sigil_card_listing_free(listing);
    if (!out && !(*env)->ExceptionCheck(env)) throw_sigil(env, SIGIL_ERR_OOM);
    return out;
}

/* ---- sync ------------------------------------------------------------------ */

static void load_sync_result_class(JNIEnv *env) {
    if (g_sync_result_class && g_sync_result_ctor && g_companion_result_ctor && alternate_class_ready()) return;
    load_alternate_class(env);
    if (!g_sync_result_class) g_sync_result_class = global_class(env, "com/nendo/sigil/SigilSyncResult");
    if (!g_companion_result_class)
        g_companion_result_class = global_class(env, "com/nendo/sigil/SigilCompanionResult");
    if (!g_array_list_class) g_array_list_class = global_class(env, "java/util/ArrayList");
    if (!g_sync_result_class || !g_companion_result_class || !g_array_list_class) return;
    /* SigilSyncResult(artifact, shapeCode, data, contentHash, identityHash, changed, state,
     *                 holding, unowned, restoreAgain, companions, profiles, profile, alternates, hardcoreMarker,
     *                 unownedChanged) */
    g_sync_result_ctor = find_method(env, g_sync_result_class, "<init>",
        "(Ljava/lang/String;I[BLjava/lang/String;Ljava/lang/String;Z[B[BLjava/util/List;ZLjava/util/List;Ljava/util/List;Ljava/lang/String;Ljava/util/List;ZI)V");
    /* SigilCompanionResult(data, contentHash, identityHash, changed) */
    g_companion_result_ctor = find_method(env, g_companion_result_class, "<init>",
        "([BLjava/lang/String;Ljava/lang/String;Z)V");
    if (!g_array_list_ctor) g_array_list_ctor = find_method(env, g_array_list_class, "<init>", "()V");
    if (!g_array_list_add) g_array_list_add = find_method(env, g_array_list_class, "add", "(Ljava/lang/Object;)Z");
}

static jobject name_list(JNIEnv *env, char (*items)[SIGIL_CARD_NAME_MAX], size_t count) {
    jobject list = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    if (!list) return NULL;
    for (size_t i = 0; i < count; i++) {
        jstring s = save_name_string(env, items[i]);
        (*env)->CallBooleanMethod(env, list, g_array_list_add, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return list;
}

static jbyteArray byte_array(JNIEnv *env, const uint8_t *data, size_t len) {
    jbyteArray out = (*env)->NewByteArray(env, (jsize)len);
    if (out && len) (*env)->SetByteArrayRegion(env, out, 0, (jsize)len, (const jbyte *)data);
    return out;
}

static int write_member(void *ctx, const char *relative_path, const uint8_t *data, size_t len) {
    access_ctx *a = (access_ctx *)ctx;
    JNIEnv *env = a->env;
    jstring jpath = jni_string(env, relative_path);
    jbyteArray jdata = jpath ? byte_array(env, data, len) : NULL;
    jboolean ok = jdata ? (*env)->CallBooleanMethod(env, a->access, g_access_write, a->root, jpath, jdata) : JNI_FALSE;
    if (jpath) (*env)->DeleteLocalRef(env, jpath);
    if (jdata) (*env)->DeleteLocalRef(env, jdata);
    return !access_threw(env) && ok ? 0 : -1;
}

static int remove_member(void *ctx, const char *relative_path) {
    access_ctx *a = (access_ctx *)ctx;
    JNIEnv *env = a->env;
    jstring jpath = jni_string(env, relative_path);
    jboolean ok = jpath ? (*env)->CallBooleanMethod(env, a->access, g_access_remove, a->root, jpath) : JNI_FALSE;
    if (jpath) (*env)->DeleteLocalRef(env, jpath);
    return !access_threw(env) && ok ? 0 : -1;
}

typedef struct {
    jobjectArray ids_array;
    const char **ids;
    jsize        id_count;
    jbyteArray   unit_array;
    jbyte       *unit;
} borrowed_companion;

/* Borrows each companion's ids and unit; NULL when the arrays disagree or memory runs out. */
static sigil_sync_companion *borrow_companions(JNIEnv *env, jobjectArray jids, jobjectArray junits,
                                               borrowed_companion **out_borrowed, jsize *out_count) {
    *out_borrowed = NULL;
    *out_count = 0;
    jsize count = jids ? (*env)->GetArrayLength(env, jids) : 0;
    jsize unit_count = junits ? (*env)->GetArrayLength(env, junits) : 0;
    if (count != unit_count) return NULL;
    sigil_sync_companion *companions = calloc((size_t)count + 1, sizeof(*companions));
    borrowed_companion *borrowed = calloc((size_t)count + 1, sizeof(*borrowed));
    if (!companions || !borrowed) { free(companions); free(borrowed); return NULL; }
    *out_borrowed = borrowed;
    for (jsize i = 0; i < count; i++) {
        borrowed_companion *b = &borrowed[i];
        b->ids_array = (jobjectArray)(*env)->GetObjectArrayElement(env, jids, i);
        b->ids = borrow_strings(env, b->ids_array, &b->id_count);
        b->unit_array = (jbyteArray)(*env)->GetObjectArrayElement(env, junits, i);
        b->unit = b->unit_array ? (*env)->GetByteArrayElements(env, b->unit_array, NULL) : NULL;
        *out_count = i + 1;
        if (!b->ids || (b->unit_array && !b->unit)) { free(companions); return NULL; }
        companions[i].game_ids = b->ids;
        companions[i].game_id_count = (size_t)b->id_count;
        companions[i].unit = (const uint8_t *)b->unit;
        companions[i].unit_len = b->unit ? (size_t)(*env)->GetArrayLength(env, b->unit_array) : 0;
    }
    return companions;
}

static void release_companions(JNIEnv *env, borrowed_companion *borrowed, jsize count) {
    if (!borrowed) return;
    for (jsize i = 0; i < count; i++) {
        borrowed_companion *b = &borrowed[i];
        release_strings(b->ids, b->id_count);
        if (b->unit) (*env)->ReleaseByteArrayElements(env, b->unit_array, b->unit, JNI_ABORT);
        if (b->ids_array) (*env)->DeleteLocalRef(env, b->ids_array);
        if (b->unit_array) (*env)->DeleteLocalRef(env, b->unit_array);
    }
    free(borrowed);
}

static jobject companion_list(JNIEnv *env, const sigil_sync_companion_result *items, size_t count) {
    jobject list = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    if (!list) return NULL;
    for (size_t i = 0; i < count; i++) {
        const sigil_sync_companion_result *c = &items[i];
        jbyteArray jdata  = c->data ? byte_array(env, c->data, c->len) : NULL;
        jstring jhash     = jni_string(env,c->content_hash);
        jstring jidentity = jni_string(env,c->identity_hash);
        jobject item = (*env)->NewObject(env, g_companion_result_class, g_companion_result_ctor,
                                         jdata, jhash, jidentity, c->changed ? JNI_TRUE : JNI_FALSE);
        if (item) (*env)->CallBooleanMethod(env, list, g_array_list_add, item);
        if (jdata) (*env)->DeleteLocalRef(env, jdata);
        (*env)->DeleteLocalRef(env, jhash);
        (*env)->DeleteLocalRef(env, jidentity);
        if (!item || (*env)->ExceptionCheck(env)) return NULL;
        (*env)->DeleteLocalRef(env, item);
    }
    return list;
}

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeSync(JNIEnv *env, jclass clazz,
                                       jbyteArray junit, jstring jroot, jstring jlayout,
                                       jstring jplatform, jstring jcontent, jstring jtitle_id,
                                       jstring jraw_serial, jstring jsave_id, jint features, jobjectArray jn64,
                                       jobjectArray jopt_keys, jobjectArray jopt_values,
                                       jobjectArray jlisting, jobjectArray jgame_ids,
                                       jbyteArray jstate, jboolean unmanaged, jboolean overwrite_local,
                                       jobjectArray jclaimed, jobjectArray jcompanion_ids,
                                       jobjectArray jcompanion_units, jboolean repair, jstring jprofile,
                                       jobject jaccess) {
    (void)clazz;
    if (!jroot || !jlayout || !jcontent || !jaccess) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    if (!load_access_class(env)) { throw_binding_broken(env, "SigilFileAccess"); return NULL; }

    const char *root     = jni_utf8(env, jroot);
    const char *profile  = jprofile ? jni_utf8(env, jprofile) : NULL;
    const char *layout   = jni_utf8(env, jlayout);
    const char *platform = jplatform ? jni_utf8(env, jplatform) : NULL;
    const char *content  = jni_utf8(env, jcontent);
    const char *title_id = jtitle_id ? jni_utf8(env, jtitle_id) : NULL;
    const char *raw_serial = jraw_serial ? jni_utf8(env, jraw_serial) : NULL;
    const char *save_id  = jsave_id ? jni_utf8(env, jsave_id) : NULL;

    jsize key_count = 0, value_count = 0, listing_count = 0, id_count = 0, claimed_count = 0;
    const char **keys    = borrow_strings(env, jopt_keys, &key_count);
    const char **values  = borrow_strings(env, jopt_values, &value_count);
    const char **listing = borrow_strings(env, jlisting, &listing_count);
    const char **ids     = borrow_strings(env, jgame_ids, &id_count);
    const char **claimed = borrow_strings(env, jclaimed, &claimed_count);
    jbyte *state = jstate ? (*env)->GetByteArrayElements(env, jstate, NULL) : NULL;
    jsize state_len = jstate ? (*env)->GetArrayLength(env, jstate) : 0;
    jbyte *unit = junit ? (*env)->GetByteArrayElements(env, junit, NULL) : NULL;
    jsize unit_len = junit ? (*env)->GetArrayLength(env, junit) : 0;

    sigil_save_option *options = NULL;
    size_t option_count = 0;
    if (keys && values && key_count == value_count && key_count > 0) {
        options = (sigil_save_option *)calloc((size_t)key_count, sizeof(*options));
        if (options) {
            for (jsize i = 0; i < key_count; i++) {
                options[i].key = keys[i];
                options[i].value = values[i];
            }
            option_count = (size_t)key_count;
        }
    }

    sigil_result result;
    memset(&result, 0, sizeof(result));
    result.struct_version = SIGIL_RESULT_V4;
    result.features = (uint32_t)features;
    result.platform = sigil_platform_from_slug(platform);
    if (title_id) strncpy(result.title_id, title_id, sizeof(result.title_id) - 1);
    if (raw_serial) strncpy(result.raw_serial, raw_serial, sizeof(result.raw_serial) - 1);
    if (save_id)  strncpy(result.save_id, save_id, sizeof(result.save_id) - 1);
    copy_n64_fields(env, jn64, &result);

    access_ctx actx = { env, jaccess, jroot };
    sigil_sync_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SYNC_REQUEST_V1;
    req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.save.layout = layout;
    req.save.platform = platform;
    req.save.content_path = content;
    req.save.result = &result;
    req.save.features = (uint32_t)features;
    req.save.options = options;
    req.save.option_count = option_count;
    req.save.listing = listing;
    req.save.listing_count = (size_t)listing_count;
    req.save.open = open_member;
    req.save.open_ctx = &actx;
    req.save.root_path = root;
    req.save.profile = profile;
    req.game_ids = ids;
    req.game_id_count = (size_t)id_count;
    char (*claim_bytes)[SIGIL_CARD_NAME_MAX] = calloc((size_t)claimed_count + 1, SIGIL_CARD_NAME_MAX);
    const char **claim_names = (const char **)calloc((size_t)claimed_count + 1, sizeof(char *));
    bool claims_valid = claim_bytes && claim_names;
    for (jsize i = 0; i < claimed_count && claims_valid; i++) {
        claims_valid = claimed[i] && sigil_save_name_unescape(claimed[i], claim_bytes[i], SIGIL_CARD_NAME_MAX);
        claim_names[i] = claim_bytes[i];
    }
    req.claimed = claim_names;
    req.claimed_count = (size_t)claimed_count;
    borrowed_companion *borrowed = NULL;
    jsize borrowed_count = 0;
    sigil_sync_companion *companions = borrow_companions(env, jcompanion_ids, jcompanion_units,
                                                         &borrowed, &borrowed_count);
    bool companions_valid = companions != NULL;
    req.companions = companions;
    req.companion_count = companions ? (size_t)borrowed_count : 0;
    req.mode = unmanaged ? SIGIL_SYNC_UNMANAGED : SIGIL_SYNC_MANAGED;
    req.state = (const uint8_t *)state;
    req.state_len = (size_t)state_len;
    req.overwrite_local = overwrite_local ? 1 : 0;
    req.repair = repair ? 1 : 0;
    req.write = write_member;
    req.remove = remove_member;
    req.write_ctx = &actx;

    sigil_sync_result *r = NULL;
    int rc = !claims_valid || !companions_valid ? SIGIL_ERR_INVALID_ARG
           : unit ? sigil_restore(&req, (const uint8_t *)unit, (size_t)unit_len, &r) : sigil_collect(&req, &r);

    jobject out = NULL;
    bool binding_broken = false;
    if (rc == SIGIL_OK && r) {
        load_sync_result_class(env);
        binding_broken = !g_sync_result_class || !g_sync_result_ctor || !g_companion_result_class
                      || !g_companion_result_ctor || !alternate_class_ready();
        if (!binding_broken) {
            jstring jartifact = jni_string(env,r->artifact);
            jstring jhash     = jni_string(env,r->content_hash);
            jstring jidentity = jni_string(env,r->identity_hash);
            jbyteArray jdata  = r->data ? byte_array(env, r->data, r->len) : NULL;
            jbyteArray jnew   = byte_array(env, r->state, r->state_len);
            jbyteArray jheld  = r->holding ? byte_array(env, r->holding, r->holding_len) : NULL;
            jobject junowned  = name_list(env, r->unowned, r->unowned_count);
            jobject jcompanions = junowned ? companion_list(env, r->companions, r->companion_count) : NULL;
            jobject jprofiles = jcompanions ? profile_list(env, r->profiles, r->profile_count) : NULL;
            jstring jprofile_id = jprofiles ? jni_string(env,r->profile) : NULL;
            binding_broken = jcompanions && !jprofiles && !(*env)->ExceptionCheck(env);
            jobject jalternates = jprofile_id ? alternate_list(env, r->alternates, r->alternate_count) : NULL;
            if (jalternates) {
                out = (*env)->NewObject(env, g_sync_result_class, g_sync_result_ctor,
                                        jartifact, (jint)r->shape, jdata, jhash, jidentity,
                                        r->changed ? JNI_TRUE : JNI_FALSE, jnew,
                                        jheld, junowned, r->restore_again ? JNI_TRUE : JNI_FALSE, jcompanions,
                                        jprofiles, jprofile_id, jalternates,
                                        r->hardcore_marker ? JNI_TRUE : JNI_FALSE, (jint)r->unowned_changed);
            }
        }
    }
    char problem[SIGIL_SAVE_PATH_MAX] = "";
    uint32_t blocks_short = 0;
    sigil_save_profile *profiles = NULL;
    size_t profile_count = 0;
    if (rc != SIGIL_OK && r) {
        memcpy(problem, r->problem, sizeof(problem));
        problem[sizeof(problem) - 1] = '\0';
        blocks_short = r->blocks_short;
        profiles = r->profiles;
        profile_count = r->profile_count;
        r->profiles = NULL;
    }
    sigil_sync_result_free(r);
    release_companions(env, borrowed, borrowed_count);
    free(companions);

    free(options);
    if (unit)  (*env)->ReleaseByteArrayElements(env, junit, unit, JNI_ABORT);
    if (state) (*env)->ReleaseByteArrayElements(env, jstate, state, JNI_ABORT);
    release_strings(keys, key_count);
    release_strings(values, value_count);
    release_strings(listing, listing_count);
    release_strings(ids, id_count);
    release_strings(claimed, claimed_count);
    free(claim_bytes);
    free(claim_names);
    jni_free(root);
    if (profile) jni_free(profile);
    jni_free(layout);
    if (platform) jni_free(platform);
    jni_free(content);
    if (title_id) jni_free(title_id);
    if (raw_serial) jni_free(raw_serial);
    if (save_id)  jni_free(save_id);

    if (rc != SIGIL_OK) throw_sigil_problem(env, rc, problem, blocks_short, profiles, profile_count);
    free(profiles);
    if (rc != SIGIL_OK) return NULL;
    if (binding_broken) throw_binding_broken(env, "SigilSyncResult or SigilProfile");
    else if (!out && !(*env)->ExceptionCheck(env)) throw_sigil(env, SIGIL_ERR_OOM);
    return out;
}

JNIEXPORT jobjectArray JNICALL
Java_com_nendo_sigil_Sigil_nativeSaveBase(JNIEnv *env, jclass clazz, jstring jlayout, jstring jpath) {
    (void)clazz;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!jlayout || !jpath || !string_class) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    const char *layout = jni_utf8(env, jlayout);
    const char *path = jni_utf8(env, jpath);
    char base[4096], profile[SIGIL_PROFILE_ID_MAX];
    int rc = sigil_save_base(layout, path, base, sizeof(base), profile, sizeof(profile));
    jni_free(layout);
    jni_free(path);
    if (rc != SIGIL_OK) { throw_sigil(env, rc); return NULL; }
    jobjectArray out = (*env)->NewObjectArray(env, 2, string_class, NULL);
    jstring jbase = jni_string(env,base);
    jstring jprofile = jni_string(env,profile);
    if (out) {
        (*env)->SetObjectArrayElement(env, out, 0, jbase);
        (*env)->SetObjectArrayElement(env, out, 1, jprofile);
    }
    (*env)->DeleteLocalRef(env, jbase);
    (*env)->DeleteLocalRef(env, jprofile);
    return out;
}

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeListProfiles(JNIEnv *env, jclass clazz, jstring jlayout, jstring jroot,
                                               jobjectArray jlisting, jobject jaccess) {
    (void)clazz;
    if (!jlayout || !jroot || !jaccess) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    if (!load_access_class(env)) { throw_binding_broken(env, "SigilFileAccess"); return NULL; }
    const char *layout = jni_utf8(env, jlayout);
    const char *root = jni_utf8(env, jroot);
    jsize listing_count = 0;
    const char **listing = borrow_strings(env, jlisting, &listing_count);

    access_ctx actx = { env, jaccess, jroot };
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = layout;
    req.listing = listing;
    req.listing_count = (size_t)listing_count;
    req.root_path = root;
    req.open = open_member;
    req.open_ctx = &actx;
    sigil_save_profile *profiles = NULL;
    size_t count = 0;
    int rc = listing ? sigil_save_profiles(&req, &profiles, &count) : SIGIL_ERR_OOM;

    release_strings(listing, listing_count);
    jni_free(layout);
    jni_free(root);
    jobject out = rc == SIGIL_OK ? profile_list(env, profiles, count) : NULL;
    sigil_save_profiles_free(profiles);
    if (rc != SIGIL_OK) throw_sigil(env, rc);
    else if (!out && !(*env)->ExceptionCheck(env)) throw_binding_broken(env, "SigilProfile");
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_nendo_sigil_Sigil_nativeLayoutTop(JNIEnv *env, jclass clazz, jstring jlayout) {
    (void)clazz;
    if (!jlayout) return NULL;
    const char *layout = jni_utf8(env, jlayout);
    const char *top = sigil_save_layout_top(layout);
    jni_free(layout);
    return top ? jni_string(env,top) : NULL;
}

JNIEXPORT jobjectArray JNICALL
Java_com_nendo_sigil_Sigil_nativeLayoutSubdirs(JNIEnv *env, jclass clazz, jstring jlayout) {
    (void)clazz;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!jlayout || !string_class) return (*env)->NewObjectArray(env, 0, string_class, NULL);

    const char *layout = jni_utf8(env, jlayout);
    const char *subdirs[16];
    size_t count = sigil_save_layout_subdirs(layout, subdirs, 16);
    jni_free(layout);

    jobjectArray out = (*env)->NewObjectArray(env, (jsize)count, string_class, NULL);
    for (size_t i = 0; i < count; i++) {
        jstring s = jni_string(env,subdirs[i]);
        (*env)->SetObjectArrayElement(env, out, (jsize)i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return out;
}
