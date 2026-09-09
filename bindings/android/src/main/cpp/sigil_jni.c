// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <jni.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jclass g_result_class = NULL;
static jmethodID g_result_ctor = NULL;
static jclass g_exception_class = NULL;
static jmethodID g_exception_ctor = NULL;

static void load_result_class(JNIEnv *env) {
    if (g_result_class) return;
    jclass cls = (*env)->FindClass(env, "com/nendo/sigil/SigilResult");
    if (!cls) return;
    g_result_class = (jclass)(*env)->NewGlobalRef(env, cls);
    /* SigilResult(titleId, rawSerial, saveId, platformSlug, source, usage, experimental, features, switchContentType, titleVersion) */
    g_result_ctor = (*env)->GetMethodID(env, g_result_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIZIIJ)V");
}

static void load_exception_class(JNIEnv *env) {
    if (g_exception_class) return;
    jclass cls = (*env)->FindClass(env, "com/nendo/sigil/SigilException");
    if (!cls) return;
    g_exception_class = (jclass)(*env)->NewGlobalRef(env, cls);
    /* SigilException(code, message) */
    g_exception_ctor = (*env)->GetMethodID(env, g_exception_class, "<init>", "(ILjava/lang/String;)V");
}

static void throw_sigil(JNIEnv *env, int code) {
    load_exception_class(env);
    if (!g_exception_class || !g_exception_ctor) return;
    jstring jmessage = (*env)->NewStringUTF(env, sigil_strerror(code));
    jobject ex = (*env)->NewObject(env, g_exception_class, g_exception_ctor, (jint)code, jmessage);
    if (ex) (*env)->Throw(env, (jthrowable)ex);
    (*env)->DeleteLocalRef(env, jmessage);
}

static jclass g_member_class = NULL;
static jmethodID g_member_ctor = NULL;
static jclass g_unit_class = NULL;
static jmethodID g_unit_ctor = NULL;
static jclass g_array_list_class = NULL;
static jmethodID g_array_list_ctor = NULL;
static jmethodID g_array_list_add = NULL;

static void load_unit_classes(JNIEnv *env) {
    if (g_unit_class) return;
    jclass member = (*env)->FindClass(env, "com/nendo/sigil/SigilSaveMember");
    jclass unit   = (*env)->FindClass(env, "com/nendo/sigil/SigilSaveUnit");
    jclass list   = (*env)->FindClass(env, "java/util/ArrayList");
    if (!member || !unit || !list) return;
    g_member_class = (jclass)(*env)->NewGlobalRef(env, member);
    g_unit_class   = (jclass)(*env)->NewGlobalRef(env, unit);
    g_array_list_class = (jclass)(*env)->NewGlobalRef(env, list);
    /* SigilSaveMember(path, entry, roleCode, present) */
    g_member_ctor = (*env)->GetMethodID(env, g_member_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;IZ)V");
    /* SigilSaveUnit(key, shapeCode, members, expected, unkeyed, artifact, contentHash, identityHash) */
    g_unit_ctor = (*env)->GetMethodID(env, g_unit_class, "<init>",
        "(Ljava/lang/String;ILjava/util/List;Ljava/util/List;Ljava/util/List;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    g_array_list_ctor = (*env)->GetMethodID(env, g_array_list_class, "<init>", "()V");
    g_array_list_add  = (*env)->GetMethodID(env, g_array_list_class, "add", "(Ljava/lang/Object;)Z");
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
    if (g_unit_class) {
        (*env)->DeleteGlobalRef(env, g_member_class);
        (*env)->DeleteGlobalRef(env, g_unit_class);
        (*env)->DeleteGlobalRef(env, g_array_list_class);
        g_member_class = NULL;
        g_unit_class = NULL;
        g_array_list_class = NULL;
        g_member_ctor = NULL;
        g_unit_ctor = NULL;
        g_array_list_ctor = NULL;
        g_array_list_add = NULL;
    }
}

JNIEXPORT jstring JNICALL
Java_com_nendo_sigil_Sigil_nativeVersion(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return (*env)->NewStringUTF(env, sigil_version());
}

JNIEXPORT jstring JNICALL
Java_com_nendo_sigil_Sigil_nativePlatformSlug(JNIEnv *env, jclass clazz, jstring jslug) {
    (void)clazz;
    const char *slug = jslug ? (*env)->GetStringUTFChars(env, jslug, NULL) : NULL;
    const char *canonical = sigil_platform_to_slug(sigil_platform_from_slug(slug));
    if (slug) (*env)->ReleaseStringUTFChars(env, jslug, slug);
    return (*env)->NewStringUTF(env, canonical);
}

JNIEXPORT jstring JNICALL
Java_com_nendo_sigil_Sigil_nativeContentStem(JNIEnv *env, jclass clazz, jstring jcontent) {
    (void)clazz;
    char stem[SIGIL_SAVE_ENTRY_MAX];
    const char *content = jcontent ? (*env)->GetStringUTFChars(env, jcontent, NULL) : NULL;
    sigil_content_stem(content, stem, sizeof(stem));
    if (content) (*env)->ReleaseStringUTFChars(env, jcontent, content);
    return (*env)->NewStringUTF(env, stem);
}

JNIEXPORT jbyteArray JNICALL
Java_com_nendo_sigil_Sigil_nativeLoadHeaderKey(JNIEnv *env, jclass clazz, jstring jpath) {
    (void)clazz;
    if (!jpath) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    uint8_t key[32];
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    int rc = sigil_load_header_key_from_prod_keys(path, key);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
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

    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) { throw_sigil(env, SIGIL_ERR_OOM); return NULL; }

    const char *slug = jplatform_slug ? (*env)->GetStringUTFChars(env, jplatform_slug, NULL) : NULL;
    const char *prod_keys = jprod_keys_path ? (*env)->GetStringUTFChars(env, jprod_keys_path, NULL) : NULL;
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
    r.struct_version = SIGIL_RESULT_V3;
    int rc = sigil_extract_from_path(path, sigil_platform_from_slug(slug), &opts, &r);

    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (slug)      (*env)->ReleaseStringUTFChars(env, jplatform_slug, slug);
    if (prod_keys) (*env)->ReleaseStringUTFChars(env, jprod_keys_path, prod_keys);
    if (keys_text) (*env)->ReleaseByteArrayElements(env, jprod_keys_text, keys_text, JNI_ABORT);

    if (rc != SIGIL_OK) { throw_sigil(env, rc); return NULL; }

    load_result_class(env);
    if (!g_result_class || !g_result_ctor) return NULL;

    jstring jtitle   = (*env)->NewStringUTF(env, r.title_id);
    jstring jraw     = (*env)->NewStringUTF(env, r.raw_serial);
    jstring jsave_id = (*env)->NewStringUTF(env, r.save_id);
    jstring jslug    = (*env)->NewStringUTF(env, sigil_platform_to_slug(r.platform));

    return (*env)->NewObject(env, g_result_class, g_result_ctor,
                             jtitle, jraw, jsave_id, jslug,
                             (jint)r.source, (jint)r.usage,
                             r.experimental ? JNI_TRUE : JNI_FALSE,
                             (jint)r.features,
                             (jint)r.switch_content_type,
                             (jlong)r.title_version);
}

/* ---- save units ------------------------------------------------------------ */

typedef struct {
    const char *root;
} open_ctx;

static sigil_io *open_member(void *ctx, const char *relative_path) {
    open_ctx *o = (open_ctx *)ctx;
    if (!o->root) return NULL;
    char path[SIGIL_SAVE_PATH_MAX * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", o->root, relative_path);
    if (n <= 0 || (size_t)n >= sizeof(path)) return NULL;
    return sigil_io_open_file(path);
}

static jobject member_list(JNIEnv *env, const sigil_save_member *members, size_t count) {
    jobject list = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    if (!list) return NULL;
    for (size_t i = 0; i < count; i++) {
        jstring jpath  = (*env)->NewStringUTF(env, members[i].path);
        jstring jentry = (*env)->NewStringUTF(env, members[i].entry);
        jobject m = (*env)->NewObject(env, g_member_class, g_member_ctor,
                                      jpath, jentry, (jint)members[i].role,
                                      members[i].present ? JNI_TRUE : JNI_FALSE);
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
        jstring s = (*env)->NewStringUTF(env, items[i]);
        (*env)->CallBooleanMethod(env, list, g_array_list_add, s);
        (*env)->DeleteLocalRef(env, s);
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
        out[i] = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
        if (s) (*env)->DeleteLocalRef(env, s);
    }
    *out_count = count;
    return out;
}

static void release_strings(JNIEnv *env, jobjectArray arr, const char **strings, jsize count) {
    if (!strings) return;
    for (jsize i = 0; i < count; i++) {
        if (!strings[i]) continue;
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, arr, i);
        if (s) {
            (*env)->ReleaseStringUTFChars(env, s, strings[i]);
            (*env)->DeleteLocalRef(env, s);
        }
    }
    free((void *)strings);
}

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeLocateSaves(JNIEnv *env, jclass clazz,
                                              jstring jlayout, jstring jplatform,
                                              jstring jcontent, jstring jtitle_id,
                                              jstring jsave_id, jint features,
                                              jobjectArray jopt_keys, jobjectArray jopt_values,
                                              jobjectArray jlisting) {
    (void)clazz;
    if (!jlayout || !jcontent) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }

    const char *layout   = (*env)->GetStringUTFChars(env, jlayout, NULL);
    const char *platform = jplatform ? (*env)->GetStringUTFChars(env, jplatform, NULL) : NULL;
    const char *content  = (*env)->GetStringUTFChars(env, jcontent, NULL);
    const char *title_id = jtitle_id ? (*env)->GetStringUTFChars(env, jtitle_id, NULL) : NULL;
    const char *save_id  = jsave_id ? (*env)->GetStringUTFChars(env, jsave_id, NULL) : NULL;

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
    result.struct_version = SIGIL_RESULT_V3;
    result.features = (uint32_t)features;
    if (title_id) strncpy(result.title_id, title_id, sizeof(result.title_id) - 1);
    if (save_id)  strncpy(result.save_id, save_id, sizeof(result.save_id) - 1);

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

    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req, &unit);

    jobject out = NULL;
    if (rc == SIGIL_OK && unit) {
        load_unit_classes(env);
        if (g_unit_class && g_unit_ctor && g_member_ctor && g_array_list_ctor) {
            jstring jkey      = (*env)->NewStringUTF(env, unit->key);
            jstring jartifact = (*env)->NewStringUTF(env, unit->artifact);
            jstring jhash     = (*env)->NewStringUTF(env, unit->content_hash);
            jstring jidentity = (*env)->NewStringUTF(env, unit->identity_hash);
            jobject members   = member_list(env, unit->members, unit->member_count);
            jobject expected  = member_list(env, unit->expected, unit->expected_count);
            jobject unkeyed   = string_list(env, unit->unkeyed, unit->unkeyed_count);
            if (members && expected && unkeyed) {
                out = (*env)->NewObject(env, g_unit_class, g_unit_ctor,
                                        jkey, (jint)unit->shape, members, expected, unkeyed,
                                        jartifact, jhash, jidentity);
            }
        }
    }
    sigil_save_unit_free(unit);

    free(options);
    release_strings(env, jopt_keys, keys, key_count);
    release_strings(env, jopt_values, values, value_count);
    release_strings(env, jlisting, listing, listing_count);
    (*env)->ReleaseStringUTFChars(env, jlayout, layout);
    if (platform) (*env)->ReleaseStringUTFChars(env, jplatform, platform);
    (*env)->ReleaseStringUTFChars(env, jcontent, content);
    if (title_id) (*env)->ReleaseStringUTFChars(env, jtitle_id, title_id);
    if (save_id)  (*env)->ReleaseStringUTFChars(env, jsave_id, save_id);

    if (rc != SIGIL_OK) throw_sigil(env, rc);
    else if (!out && !(*env)->ExceptionCheck(env)) throw_sigil(env, SIGIL_ERR_OOM);
    return out;
}

JNIEXPORT jobjectArray JNICALL
Java_com_nendo_sigil_Sigil_nativeHashSaves(JNIEnv *env, jclass clazz,
                                            jstring jroot, jstring jkey, jint shape,
                                            jobjectArray jpaths, jobjectArray jentries,
                                            jintArray jroles) {
    (void)clazz;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!jroot || !jkey || !jpaths || !jentries || !jroles || !string_class) {
        throw_sigil(env, SIGIL_ERR_INVALID_ARG);
        return NULL;
    }

    jsize path_count = 0, entry_count = 0;
    const char **paths   = borrow_strings(env, jpaths, &path_count);
    const char **entries = borrow_strings(env, jentries, &entry_count);
    jsize role_count = (*env)->GetArrayLength(env, jroles);
    jint *roles = (*env)->GetIntArrayElements(env, jroles, NULL);
    const char *root = (*env)->GetStringUTFChars(env, jroot, NULL);
    const char *key  = (*env)->GetStringUTFChars(env, jkey, NULL);

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
            open_ctx octx = { root };
            rc = sigil_save_hash(&unit, open_member, &octx);
        }
    }

    jobjectArray out = NULL;
    if (rc == SIGIL_OK) {
        out = (*env)->NewObjectArray(env, 2, string_class, NULL);
        jstring jhash     = (*env)->NewStringUTF(env, unit.content_hash);
        jstring jidentity = (*env)->NewStringUTF(env, unit.identity_hash);
        (*env)->SetObjectArrayElement(env, out, 0, jhash);
        (*env)->SetObjectArrayElement(env, out, 1, jidentity);
        (*env)->DeleteLocalRef(env, jhash);
        (*env)->DeleteLocalRef(env, jidentity);
    }

    free(unit.members);
    release_strings(env, jpaths, paths, path_count);
    release_strings(env, jentries, entries, entry_count);
    if (roles) (*env)->ReleaseIntArrayElements(env, jroles, roles, JNI_ABORT);
    (*env)->ReleaseStringUTFChars(env, jroot, root);
    (*env)->ReleaseStringUTFChars(env, jkey, key);

    if (rc != SIGIL_OK) throw_sigil(env, rc);
    return out;
}

JNIEXPORT jobjectArray JNICALL
Java_com_nendo_sigil_Sigil_nativeLayoutSubdirs(JNIEnv *env, jclass clazz, jstring jlayout) {
    (void)clazz;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!jlayout || !string_class) return (*env)->NewObjectArray(env, 0, string_class, NULL);

    const char *layout = (*env)->GetStringUTFChars(env, jlayout, NULL);
    const char *subdirs[16];
    size_t count = sigil_save_layout_subdirs(layout, subdirs, 16);
    (*env)->ReleaseStringUTFChars(env, jlayout, layout);

    jobjectArray out = (*env)->NewObjectArray(env, (jsize)count, string_class, NULL);
    for (size_t i = 0; i < count; i++) {
        jstring s = (*env)->NewStringUTF(env, subdirs[i]);
        (*env)->SetObjectArrayElement(env, out, (jsize)i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return out;
}
