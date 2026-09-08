#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "zygisk.hpp"
#include "lsplant.hpp"
#include "dobby.h"
#include "elf_util.h"
#include "hooker_dex.h"

#define LOG_TAG "WAStatusHD"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
constexpr const char *kTargetPackage = "com.whatsapp";
constexpr const char *kExactTargetVersion = "2.26.34.82";

JavaVM *g_vm = nullptr;
std::unique_ptr<wastatushd::ElfImg> g_art;
std::mutex g_lsplant_mutex;
bool g_lsplant_initialized = false;

void clearException(JNIEnv *env, const char *where) {
    if (!env || !env->ExceptionCheck()) return;
    env->ExceptionClear();
    LOGW("JNI exception cleared at %s", where);
}

bool initLsplant(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(g_lsplant_mutex);
    if (g_lsplant_initialized) return true;

    g_art = std::make_unique<wastatushd::ElfImg>("libart.so");
    if (!g_art || !g_art->isValid()) {
        LOGE("v0.4 FAIL: libart symbol image unavailable");
        g_art.reset();
        return false;
    }

    lsplant::InitInfo info{
        .inline_hooker = [](void *target, void *replacement) -> void * {
            void *backup = nullptr;
            const int rc = DobbyHook(target, replacement, &backup);
            if (rc != 0) {
                LOGE("DobbyHook failed target=%p rc=%d", target, rc);
                return nullptr;
            }
            return backup;
        },
        .inline_unhooker = [](void *target) -> bool {
            return DobbyDestroy(target) == 0;
        },
        .art_symbol_resolver = [](std::string_view symbol) -> void * {
            return g_art ? g_art->getSymbAddress<void *>(symbol) : nullptr;
        },
        .art_symbol_prefix_resolver = [](std::string_view prefix) -> void * {
            return g_art ? g_art->getSymbPrefixFirstAddress<void *>(prefix) : nullptr;
        },
        .generated_class_name = "WAHD_",
        .generated_source_name = "WAStatusHD",
        .generated_field_name = "hooker",
        .generated_method_name = "{target}",
    };

    g_lsplant_initialized = lsplant::Init(env, info);
    if (!g_lsplant_initialized) {
        clearException(env, "lsplant::Init");
        LOGE("v0.4 FAIL: LSPlant initialization failed");
        g_art.reset();
        return false;
    }

    LOGI("v0.4 LSPlant INIT OK; Dobby=%s", DobbyGetVersion());
    return true;
}

jobject nativeHook(JNIEnv *env, jclass, jobject target, jobject hooker, jobject callback) {
    if (!g_lsplant_initialized || !target || !hooker || !callback) return nullptr;
    jobject backup = lsplant::Hook(env, target, hooker, callback);
    if (!backup) {
        clearException(env, "lsplant::Hook");
        LOGE("v0.4 nativeHook FAIL target=%p", target);
    } else {
        LOGI("v0.4 nativeHook OK target=%p", target);
    }
    return backup;
}

jboolean nativeDeoptimize(JNIEnv *env, jclass, jobject target) {
    if (!g_lsplant_initialized || !target) return JNI_FALSE;
    const bool ok = lsplant::Deoptimize(env, target);
    if (!ok) clearException(env, "lsplant::Deoptimize");
    return ok ? JNI_TRUE : JNI_FALSE;
}

void nativeLog(JNIEnv *env, jclass, jstring message) {
    if (!message) return;
    const char *chars = env->GetStringUTFChars(message, nullptr);
    if (chars) {
        LOGI("%s", chars);
        env->ReleaseStringUTFChars(message, chars);
    }
}

static JNINativeMethod kEntryNatives[] = {
    {const_cast<char *>("nativeHook"),
     const_cast<char *>("(Ljava/lang/reflect/Method;Ljava/lang/Object;Ljava/lang/reflect/Method;)Ljava/lang/reflect/Method;"),
     reinterpret_cast<void *>(nativeHook)},
    {const_cast<char *>("nativeDeoptimize"),
     const_cast<char *>("(Ljava/lang/reflect/Method;)Z"),
     reinterpret_cast<void *>(nativeDeoptimize)},
    {const_cast<char *>("nativeLog"),
     const_cast<char *>("(Ljava/lang/String;)V"),
     reinterpret_cast<void *>(nativeLog)},
};

bool loadAndStartPayload(JNIEnv *env, jobject app, jobject appLoader) {
    if (!initLsplant(env)) return false;

    jclass byteBufferClass = env->FindClass("java/nio/ByteBuffer");
    jclass inMemoryClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
    if (!byteBufferClass || !inMemoryClass || !classLoaderClass) {
        clearException(env, "payload classes");
        LOGE("v0.4 FAIL: payload classloader classes unavailable");
        return false;
    }

    jobject dexBuffer = env->NewDirectByteBuffer(classes_dex, static_cast<jlong>(classes_dex_len));
    jmethodID inMemoryCtor = env->GetMethodID(
        inMemoryClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jmethodID loadClass = env->GetMethodID(
        classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!dexBuffer || !inMemoryCtor || !loadClass) {
        clearException(env, "payload methods");
        LOGE("v0.4 FAIL: payload loader methods unavailable");
        return false;
    }

    jobject payloadLoader = env->NewObject(inMemoryClass, inMemoryCtor, dexBuffer, appLoader);
    if (!payloadLoader || env->ExceptionCheck()) {
        clearException(env, "InMemoryDexClassLoader");
        LOGE("v0.4 FAIL: InMemoryDexClassLoader creation failed");
        return false;
    }

    jstring entryName = env->NewStringUTF("wa.hd.Entry");
    jobject entryClassObject = env->CallObjectMethod(payloadLoader, loadClass, entryName);
    if (!entryClassObject || env->ExceptionCheck()) {
        clearException(env, "load wa.hd.Entry");
        LOGE("v0.4 FAIL: Entry class unavailable");
        return false;
    }
    jclass entryClass = reinterpret_cast<jclass>(entryClassObject);

    if (env->RegisterNatives(entryClass, kEntryNatives,
                             sizeof(kEntryNatives) / sizeof(kEntryNatives[0])) != JNI_OK) {
        clearException(env, "RegisterNatives Entry");
        LOGE("v0.4 FAIL: Entry RegisterNatives failed");
        return false;
    }

    jmethodID start = env->GetStaticMethodID(
        entryClass, "start", "(Landroid/app/Application;)Z");
    if (!start) {
        clearException(env, "Entry.start");
        LOGE("v0.4 FAIL: Entry.start unavailable");
        return false;
    }

    jboolean active = env->CallStaticBooleanMethod(entryClass, start, app);
    if (env->ExceptionCheck()) {
        clearException(env, "Entry.start invoke");
        LOGE("v0.4 FAIL: Entry.start threw");
        return false;
    }

    if (active == JNI_TRUE) {
        LOGI("v0.4 ACTUAL HOOK ACTIVE: central WhatsApp quality properties intercepted");
        return true;
    }

    LOGE("v0.4 FAIL: payload reported hooks inactive");
    return false;
}

void *runtimeInit(void *) {
    if (!g_vm) return nullptr;

    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGE("v0.4 FAIL: AttachCurrentThread");
        return nullptr;
    }

    jclass activityThread = env->FindClass("android/app/ActivityThread");
    jmethodID currentApplication = activityThread ? env->GetStaticMethodID(
        activityThread, "currentApplication", "()Landroid/app/Application;") : nullptr;
    if (!activityThread || !currentApplication) {
        clearException(env, "ActivityThread.currentApplication");
        g_vm->DetachCurrentThread();
        return nullptr;
    }

    jobject app = nullptr;
    for (int i = 0; i < 80 && !app; ++i) {
        app = env->CallStaticObjectMethod(activityThread, currentApplication);
        if (env->ExceptionCheck()) {
            clearException(env, "currentApplication");
            app = nullptr;
        }
        if (!app) usleep(250000);
    }
    if (!app) {
        LOGE("v0.4 FAIL: Application unavailable");
        env->DeleteLocalRef(activityThread);
        g_vm->DetachCurrentThread();
        return nullptr;
    }

    jclass appClass = env->GetObjectClass(app);
    jmethodID getPackageName = env->GetMethodID(appClass, "getPackageName", "()Ljava/lang/String;");
    jmethodID getPackageManager = env->GetMethodID(
        appClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jmethodID getClassLoader = env->GetMethodID(
        appClass, "getClassLoader", "()Ljava/lang/ClassLoader;");

    jstring pkgName = getPackageName ? static_cast<jstring>(env->CallObjectMethod(app, getPackageName)) : nullptr;
    const char *pkgChars = pkgName ? env->GetStringUTFChars(pkgName, nullptr) : nullptr;
    jobject appLoader = getClassLoader ? env->CallObjectMethod(app, getClassLoader) : nullptr;

    jobject pm = getPackageManager ? env->CallObjectMethod(app, getPackageManager) : nullptr;
    jclass pmClass = pm ? env->GetObjectClass(pm) : nullptr;
    jmethodID getPackageInfo = pmClass ? env->GetMethodID(
        pmClass, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;") : nullptr;
    jobject pkgInfo = (pm && getPackageInfo && pkgName)
        ? env->CallObjectMethod(pm, getPackageInfo, pkgName, 0) : nullptr;
    if (env->ExceptionCheck()) {
        clearException(env, "getPackageInfo");
        pkgInfo = nullptr;
    }

    bool exact = false;
    std::string versionText = "?";
    if (pkgInfo) {
        jclass piClass = env->GetObjectClass(pkgInfo);
        jfieldID versionField = env->GetFieldID(piClass, "versionName", "Ljava/lang/String;");
        jstring version = versionField
            ? static_cast<jstring>(env->GetObjectField(pkgInfo, versionField)) : nullptr;
        const char *v = version ? env->GetStringUTFChars(version, nullptr) : nullptr;
        if (v) {
            versionText = v;
            exact = strcmp(v, kExactTargetVersion) == 0;
            env->ReleaseStringUTFChars(version, v);
        }
        if (version) env->DeleteLocalRef(version);
        env->DeleteLocalRef(piClass);
    }

    LOGI("v0.4 runtime package=%s version=%s exactTarget=%s loader=%s",
         pkgChars ? pkgChars : "?", versionText.c_str(), exact ? "yes" : "no",
         appLoader ? "ready" : "missing");

    if (exact && appLoader) {
        const bool active = loadAndStartPayload(env, app, appLoader);
        if (!active) {
            LOGE("v0.4 STOP CONDITION: actual hook path failed; WhatsApp left fail-open");
        }
    } else {
        LOGW("v0.4 SKIP: unsupported runtime; no method mutation attempted");
    }

    if (pkgChars) env->ReleaseStringUTFChars(pkgName, pkgChars);
    if (pkgName) env->DeleteLocalRef(pkgName);
    if (pkgInfo) env->DeleteLocalRef(pkgInfo);
    if (pmClass) env->DeleteLocalRef(pmClass);
    if (pm) env->DeleteLocalRef(pm);
    if (appLoader) env->DeleteLocalRef(appLoader);
    env->DeleteLocalRef(appClass);
    env->DeleteLocalRef(app);
    env->DeleteLocalRef(activityThread);
    g_vm->DetachCurrentThread();
    return nullptr;
}

class WAStatusHDModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        if (env_) env_->GetJavaVM(&g_vm);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        target_ = false;
        if (!args || !args->nice_name) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        const char *process = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (process) {
            target_ = strcmp(process, kTargetPackage) == 0;
            env_->ReleaseStringUTFChars(args->nice_name, process);
        }
        if (!target_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_) return;
        LOGI("Injected v0.4 into com.whatsapp; exact target=2.26.34.82; standalone LSPlant");
        pthread_t thread;
        if (pthread_create(&thread, nullptr, runtimeInit, nullptr) == 0) {
            pthread_detach(thread);
        } else {
            LOGE("v0.4 FAIL: runtime thread creation");
        }
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
};
} // namespace

REGISTER_ZYGISK_MODULE(WAStatusHDModule)
