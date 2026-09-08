// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jclass g_result_class = NULL;
static jmethodID g_result_ctor = NULL;

static void load_result_class(JNIEnv *env) {
    if (g_result_class) return;
    jclass cls = (*env)->FindClass(env, "com/nendo/sigil/SigilResult");
    if (!cls) return;
    g_result_class = (jclass)(*env)->NewGlobalRef(env, cls);
    /* SigilResult(titleId, rawSerial, saveId, platformSlug, source, usage, experimental, features) */
    g_result_ctor = (*env)->GetMethodID(env, g_result_class, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIZI)V");
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

JNIEXPORT jobject JNICALL
Java_com_nendo_sigil_Sigil_nativeExtract(JNIEnv *env, jclass clazz,
                                          jstring jpath,
                                          jstring jplatform_slug,
                                          jstring jprod_keys_path) {
    (void)clazz;
    if (!jpath) return NULL;

    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return NULL;

    const char *slug = NULL;
    if (jplatform_slug) slug = (*env)->GetStringUTFChars(env, jplatform_slug, NULL);

    const char *prod_keys = NULL;
    if (jprod_keys_path) prod_keys = (*env)->GetStringUTFChars(env, jprod_keys_path, NULL);

    sigil_platform hint = sigil_platform_from_slug(slug);

    sigil_support sup = {
        .struct_version = SIGIL_SUPPORT_V1,
        .switch_prod_keys_path = prod_keys,
    };
    sigil_options opts = {
        .struct_version = SIGIL_OPTIONS_V1,
        .support = prod_keys ? &sup : NULL,
        .flags = SIGIL_FLAG_FILENAME_FALLBACK,
    };

    sigil_result r;
    int rc = sigil_extract_from_path(path, hint, &opts, &r);

    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (slug)      (*env)->ReleaseStringUTFChars(env, jplatform_slug, slug);
    if (prod_keys) (*env)->ReleaseStringUTFChars(env, jprod_keys_path, prod_keys);

    if (rc != SIGIL_OK) return NULL;

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
                             (jint)r.features);
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

/* Copies a Java String[] into a NULL-terminated C array the caller frees
 * with release_strings. */
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
Java_com_nendo_sigil_Sigil_nativeResolveSaveUnit(JNIEnv *env, jclass clazz,
                                                  jstring jlayout, jstring jplatform,
                                                  jstring jcontent, jstring jtitle_id,
                                                  jstring jsave_id, jint features,
                                                  jobjectArray jopt_keys, jobjectArray jopt_values,
                                                  jobjectArray jlisting, jstring jroot) {
    (void)clazz;
    if (!jlayout || !jcontent) return NULL;

    const char *layout   = (*env)->GetStringUTFChars(env, jlayout, NULL);
    const char *platform = jplatform ? (*env)->GetStringUTFChars(env, jplatform, NULL) : NULL;
    const char *content  = (*env)->GetStringUTFChars(env, jcontent, NULL);
    const char *title_id = jtitle_id ? (*env)->GetStringUTFChars(env, jtitle_id, NULL) : NULL;
    const char *save_id  = jsave_id ? (*env)->GetStringUTFChars(env, jsave_id, NULL) : NULL;
    const char *root     = jroot ? (*env)->GetStringUTFChars(env, jroot, NULL) : NULL;

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

    open_ctx octx = { root };
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = layout;
    req.platform = platform;
    req.content_name = content;
    req.result = &result;
    req.features = (uint32_t)features;
    req.options = options;
    req.option_count = option_count;
    req.listing = listing;
    req.listing_count = (size_t)listing_count;
    req.open = root ? open_member : NULL;
    req.open_ctx = &octx;

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
    if (root)     (*env)->ReleaseStringUTFChars(env, jroot, root);
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
