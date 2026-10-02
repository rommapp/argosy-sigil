// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "save_name.h"
#include <errno.h>
#include <jni.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    /* SigilResult(titleId, rawSerial, saveId, platformSlug, source, usage, experimental, features, switchContentType, titleVersion) */
    g_result_ctor = find_method(env, g_result_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIZIIJ)V");
}

static void load_exception_class(JNIEnv *env) {
    if (g_exception_class) return;
    g_exception_class = global_class(env, "com/nendo/sigil/SigilException");
    if (!g_exception_class) return;
    /* SigilException(code, message, overflow, overflowBlocks) */
    g_exception_ctor = find_method(env, g_exception_class, "<init>", "(ILjava/lang/String;Ljava/lang/String;I)V");
}

/* Throws SigilException for `code`, naming the save that didn't fit and the
 * blocks it lacked when `overflow` is given. */
static void throw_sigil_overflow(JNIEnv *env, int code, const char *overflow, uint32_t blocks) {
    if ((*env)->ExceptionCheck(env)) return;
    load_exception_class(env);
    if (g_exception_class && g_exception_ctor) {
        char escaped[3 * SIGIL_CARD_NAME_MAX + 1];
        sigil_save_name_escape(overflow ? overflow : "", escaped, sizeof(escaped));
        jstring jmessage = (*env)->NewStringUTF(env, sigil_strerror(code));
        jstring joverflow = (*env)->NewStringUTF(env, escaped);
        jobject ex = (*env)->NewObject(env, g_exception_class, g_exception_ctor, (jint)code, jmessage, joverflow,
                                       (jint)blocks);
        if (ex) {
            (*env)->Throw(env, (jthrowable)ex);
            (*env)->DeleteLocalRef(env, jmessage);
            (*env)->DeleteLocalRef(env, joverflow);
            return;
        }
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, jmessage);
        (*env)->DeleteLocalRef(env, joverflow);
    }
    jclass fallback = find_class(env, "java/lang/IllegalStateException");
    if (fallback) (*env)->ThrowNew(env, fallback, sigil_strerror(code));
}

static void throw_sigil(JNIEnv *env, int code) { throw_sigil_overflow(env, code, NULL, 0); }

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

static bool unit_classes_ready(void) {
    return g_member_class && g_member_ctor && g_unit_class && g_unit_ctor
        && g_array_list_class && g_array_list_ctor && g_array_list_add;
}

static void load_unit_classes(JNIEnv *env) {
    if (unit_classes_ready()) return;
    if (!g_member_class) g_member_class = global_class(env, "com/nendo/sigil/SigilSaveMember");
    if (!g_unit_class) g_unit_class = global_class(env, "com/nendo/sigil/SigilSaveUnit");
    if (!g_array_list_class) g_array_list_class = global_class(env, "java/util/ArrayList");
    if (!g_member_class || !g_unit_class || !g_array_list_class) return;
    /* SigilSaveMember(path, entry, roleCode, present) */
    g_member_ctor = find_method(env, g_member_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;IZ)V");
    /* SigilSaveUnit(key, shapeCode, members, expected, unkeyed, artifact, contentHash, identityHash) */
    g_unit_ctor = find_method(env, g_unit_class, "<init>",
        "(Ljava/lang/String;ILjava/util/List;Ljava/util/List;Ljava/util/List;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    g_array_list_ctor = find_method(env, g_array_list_class, "<init>", "()V");
    g_array_list_add  = find_method(env, g_array_list_class, "add", "(Ljava/lang/Object;)Z");
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
    if (!g_result_class || !g_result_ctor) { throw_binding_broken(env, "SigilResult"); return NULL; }

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
    bool binding_broken = false;
    if (rc == SIGIL_OK && unit) {
        load_unit_classes(env);
        binding_broken = !unit_classes_ready();
        if (!binding_broken) {
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
    else if (binding_broken) throw_binding_broken(env, "SigilSaveMember or SigilSaveUnit");
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
    /* SigilCardListing(formatCode, totalBlocks, freeBlocks, freeSlots, corruptCount, entries) */
    g_card_listing_ctor = find_method(env, g_card_listing_class, "<init>",
        "(IIIIILjava/util/List;)V");
    if (!g_array_list_ctor) g_array_list_ctor = find_method(env, g_array_list_class, "<init>", "()V");
    if (!g_array_list_add) g_array_list_add = find_method(env, g_array_list_class, "add", "(Ljava/lang/Object;)Z");
}

static jstring save_name_string(JNIEnv *env, const char *raw) {
    char escaped[3 * SIGIL_CARD_NAME_MAX + 1];
    sigil_save_name_escape(raw, escaped, sizeof(escaped));
    return (*env)->NewStringUTF(env, escaped);
}

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeListCard(JNIEnv *env, jclass clazz, jstring jpath) {
    (void)clazz;
    if (!jpath) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    sigil_io *io = path ? sigil_io_open_file(path) : NULL;
    if (path) (*env)->ReleaseStringUTFChars(env, jpath, path);
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
    jobject entries = (*env)->NewObject(env, g_array_list_class, g_array_list_ctor);
    if (entries) {
        for (size_t i = 0; i < listing->entry_count; i++) {
            const sigil_card_entry *e = &listing->entries[i];
            jstring jname  = save_name_string(env, e->name);
            jstring jowner = save_name_string(env, e->owner_id);
            jobject je = (*env)->NewObject(env, g_card_entry_class, g_card_entry_ctor,
                                           jname, jowner, (jint)e->blocks, (jint)e->first_block);
            if (je) (*env)->CallBooleanMethod(env, entries, g_array_list_add, je);
            (*env)->DeleteLocalRef(env, jname);
            (*env)->DeleteLocalRef(env, jowner);
            if (je) (*env)->DeleteLocalRef(env, je);
        }
        out = (*env)->NewObject(env, g_card_listing_class, g_card_listing_ctor,
                                (jint)listing->format, (jint)listing->total_blocks,
                                (jint)listing->free_blocks, (jint)listing->free_slots,
                                (jint)listing->corrupt_count, entries);
    }
    sigil_card_listing_free(listing);
    if (!out && !(*env)->ExceptionCheck(env)) throw_sigil(env, SIGIL_ERR_OOM);
    return out;
}

/* ---- sync ------------------------------------------------------------------ */

static void load_sync_result_class(JNIEnv *env) {
    if (g_sync_result_class && g_sync_result_ctor && g_companion_result_ctor && g_array_list_add) return;
    if (!g_sync_result_class) g_sync_result_class = global_class(env, "com/nendo/sigil/SigilSyncResult");
    if (!g_companion_result_class)
        g_companion_result_class = global_class(env, "com/nendo/sigil/SigilCompanionResult");
    if (!g_array_list_class) g_array_list_class = global_class(env, "java/util/ArrayList");
    if (!g_sync_result_class || !g_companion_result_class || !g_array_list_class) return;
    /* SigilSyncResult(artifact, shapeCode, data, contentHash, identityHash, changed, conflict, state,
     *                 holding, unowned, restoreAgain, companions) */
    g_sync_result_ctor = find_method(env, g_sync_result_class, "<init>",
        "(Ljava/lang/String;I[BLjava/lang/String;Ljava/lang/String;ZZ[B[BLjava/util/List;ZLjava/util/List;)V");
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

static int make_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        int rc = mkdir(path, 0755);
        *p = '/';
        if (rc != 0 && errno != EEXIST) return -1;
    }
    return 0;
}

static int write_member(void *ctx, const char *relative_path, const uint8_t *data, size_t len) {
    open_ctx *o = (open_ctx *)ctx;
    char path[SIGIL_SAVE_PATH_MAX * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", o->root, relative_path);
    if (n <= 0 || (size_t)n >= sizeof(path) || make_parents(path) != 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t wrote = fwrite(data, 1, len, f);
    int closed = fclose(f);
    return wrote == len && closed == 0 ? 0 : -1;
}

static int remove_member(void *ctx, const char *relative_path) {
    open_ctx *o = (open_ctx *)ctx;
    char path[SIGIL_SAVE_PATH_MAX * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", o->root, relative_path);
    if (n <= 0 || (size_t)n >= sizeof(path)) return -1;
    return unlink(path) == 0 ? 0 : -1;
}

static jbyteArray byte_array(JNIEnv *env, const uint8_t *data, size_t len) {
    jbyteArray out = (*env)->NewByteArray(env, (jsize)len);
    if (out && len) (*env)->SetByteArrayRegion(env, out, 0, (jsize)len, (const jbyte *)data);
    return out;
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
        release_strings(env, b->ids_array, b->ids, b->id_count);
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
        jstring jhash     = (*env)->NewStringUTF(env, c->content_hash);
        jstring jidentity = (*env)->NewStringUTF(env, c->identity_hash);
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
                                       jstring jsave_id, jint features,
                                       jobjectArray jopt_keys, jobjectArray jopt_values,
                                       jobjectArray jlisting, jobjectArray jgame_ids,
                                       jbyteArray jstate, jboolean unmanaged, jboolean overwrite_local,
                                       jobjectArray jclaimed, jobjectArray jcompanion_ids,
                                       jobjectArray jcompanion_units) {
    (void)clazz;
    if (!jroot || !jlayout || !jcontent) { throw_sigil(env, SIGIL_ERR_INVALID_ARG); return NULL; }

    const char *root     = (*env)->GetStringUTFChars(env, jroot, NULL);
    const char *layout   = (*env)->GetStringUTFChars(env, jlayout, NULL);
    const char *platform = jplatform ? (*env)->GetStringUTFChars(env, jplatform, NULL) : NULL;
    const char *content  = (*env)->GetStringUTFChars(env, jcontent, NULL);
    const char *title_id = jtitle_id ? (*env)->GetStringUTFChars(env, jtitle_id, NULL) : NULL;
    const char *save_id  = jsave_id ? (*env)->GetStringUTFChars(env, jsave_id, NULL) : NULL;

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
    result.struct_version = SIGIL_RESULT_V3;
    result.features = (uint32_t)features;
    result.platform = sigil_platform_from_slug(platform);
    if (title_id) strncpy(result.title_id, title_id, sizeof(result.title_id) - 1);
    if (save_id)  strncpy(result.save_id, save_id, sizeof(result.save_id) - 1);

    open_ctx octx = { root };
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
    req.save.open_ctx = &octx;
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
    req.write = write_member;
    req.remove = remove_member;
    req.write_ctx = &octx;

    sigil_sync_result *r = NULL;
    int rc = !claims_valid || !companions_valid ? SIGIL_ERR_INVALID_ARG
           : unit ? sigil_restore(&req, (const uint8_t *)unit, (size_t)unit_len, &r) : sigil_collect(&req, &r);

    jobject out = NULL;
    bool binding_broken = false;
    if (rc == SIGIL_OK && r) {
        load_sync_result_class(env);
        binding_broken = !g_sync_result_class || !g_sync_result_ctor || !g_companion_result_class
                      || !g_companion_result_ctor || !g_array_list_ctor || !g_array_list_add;
        if (!binding_broken) {
            jstring jartifact = (*env)->NewStringUTF(env, r->artifact);
            jstring jhash     = (*env)->NewStringUTF(env, r->content_hash);
            jstring jidentity = (*env)->NewStringUTF(env, r->identity_hash);
            jbyteArray jdata  = r->data ? byte_array(env, r->data, r->len) : NULL;
            jbyteArray jnew   = byte_array(env, r->state, r->state_len);
            jbyteArray jheld  = r->holding ? byte_array(env, r->holding, r->holding_len) : NULL;
            jobject junowned  = name_list(env, r->unowned, r->unowned_count);
            jobject jcompanions = junowned ? companion_list(env, r->companions, r->companion_count) : NULL;
            if (jcompanions) {
                out = (*env)->NewObject(env, g_sync_result_class, g_sync_result_ctor,
                                        jartifact, (jint)r->shape, jdata, jhash, jidentity,
                                        r->changed ? JNI_TRUE : JNI_FALSE, r->conflict ? JNI_TRUE : JNI_FALSE, jnew,
                                        jheld, junowned, r->restore_again ? JNI_TRUE : JNI_FALSE, jcompanions);
            }
        }
    }
    char overflow[SIGIL_CARD_NAME_MAX] = "";
    uint32_t overflow_blocks = 0;
    if (rc == SIGIL_ERR_NO_SPACE && r) {
        memcpy(overflow, r->overflow, sizeof(overflow));
        overflow[sizeof(overflow) - 1] = '\0';
        overflow_blocks = r->overflow_blocks;
    }
    sigil_sync_result_free(r);
    release_companions(env, borrowed, borrowed_count);
    free(companions);

    free(options);
    if (unit)  (*env)->ReleaseByteArrayElements(env, junit, unit, JNI_ABORT);
    if (state) (*env)->ReleaseByteArrayElements(env, jstate, state, JNI_ABORT);
    release_strings(env, jopt_keys, keys, key_count);
    release_strings(env, jopt_values, values, value_count);
    release_strings(env, jlisting, listing, listing_count);
    release_strings(env, jgame_ids, ids, id_count);
    release_strings(env, jclaimed, claimed, claimed_count);
    free(claim_bytes);
    free(claim_names);
    (*env)->ReleaseStringUTFChars(env, jroot, root);
    (*env)->ReleaseStringUTFChars(env, jlayout, layout);
    if (platform) (*env)->ReleaseStringUTFChars(env, jplatform, platform);
    (*env)->ReleaseStringUTFChars(env, jcontent, content);
    if (title_id) (*env)->ReleaseStringUTFChars(env, jtitle_id, title_id);
    if (save_id)  (*env)->ReleaseStringUTFChars(env, jsave_id, save_id);

    if (rc != SIGIL_OK) throw_sigil_overflow(env, rc, overflow, overflow_blocks);
    else if (binding_broken) throw_binding_broken(env, "SigilSyncResult");
    else if (!out && !(*env)->ExceptionCheck(env)) throw_sigil(env, SIGIL_ERR_OOM);
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
