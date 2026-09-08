#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "zygisk.hpp"

#define LOG_TAG "WAStatusHD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {
constexpr const char *kTargetPackage = "com.whatsapp";
constexpr const char *kTargetVersionPrefix = "2.26.34";

JavaVM *g_vm = nullptr;

void clearException(JNIEnv *env, const char *where) {
    if (!env->ExceptionCheck()) return;
    env->ExceptionClear();
    LOGW("JNI exception cleared at %s", where);
}

void *runtimeProbe(void *) {
    if (!g_vm) return nullptr;

    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("runtime probe: AttachCurrentThread failed");
        return nullptr;
    }

    // Zygisk postAppSpecialize runs before WhatsApp Application is guaranteed
    // to exist. Retry for ~15 seconds without blocking WhatsApp's main thread.
    jobject app = nullptr;
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread) {
        clearException(env, "FindClass(ActivityThread)");
        g_vm->DetachCurrentThread();
        return nullptr;
    }

    jmethodID currentApplication = env->GetStaticMethodID(
        activityThread, "currentApplication", "()Landroid/app/Application;");
    if (!currentApplication) {
        clearException(env, "ActivityThread.currentApplication");
        env->DeleteLocalRef(activityThread);
        g_vm->DetachCurrentThread();
        return nullptr;
    }

    for (int i = 0; i < 60 && !app; ++i) {
        app = env->CallStaticObjectMethod(activityThread, currentApplication);
        if (env->ExceptionCheck()) {
            clearException(env, "Call currentApplication");
            app = nullptr;
        }
        if (!app) usleep(250000);
    }

    if (!app) {
        LOGW("runtime probe: Application unavailable after retry window");
        env->DeleteLocalRef(activityThread);
        g_vm->DetachCurrentThread();
        return nullptr;
    }

    jclass appClass = env->GetObjectClass(app);
    jmethodID getPackageName = env->GetMethodID(appClass, "getPackageName", "()Ljava/lang/String;");
    jmethodID getPackageManager = env->GetMethodID(
        appClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jmethodID getClassLoader = env->GetMethodID(appClass, "getClassLoader", "()Ljava/lang/ClassLoader;");

    jstring pkgName = static_cast<jstring>(env->CallObjectMethod(app, getPackageName));
    const char *pkgChars = pkgName ? env->GetStringUTFChars(pkgName, nullptr) : nullptr;

    jobject loader = env->CallObjectMethod(app, getClassLoader);
    if (loader) {
        jclass objectClass = env->FindClass("java/lang/Object");
        jmethodID getClass = env->GetMethodID(objectClass, "getClass", "()Ljava/lang/Class;");
        jobject loaderClassObj = env->CallObjectMethod(loader, getClass);
        jclass classClass = env->FindClass("java/lang/Class");
        jmethodID getName = env->GetMethodID(classClass, "getName", "()Ljava/lang/String;");
        jstring loaderName = static_cast<jstring>(env->CallObjectMethod(loaderClassObj, getName));
        const char *loaderChars = loaderName ? env->GetStringUTFChars(loaderName, nullptr) : nullptr;
        LOGI("runtime probe: package=%s classLoader=%s",
             pkgChars ? pkgChars : "?", loaderChars ? loaderChars : "?");
        if (loaderChars) env->ReleaseStringUTFChars(loaderName, loaderChars);
        if (loaderName) env->DeleteLocalRef(loaderName);
        if (loaderClassObj) env->DeleteLocalRef(loaderClassObj);
        env->DeleteLocalRef(classClass);
        env->DeleteLocalRef(objectClass);
    }

    jobject pm = env->CallObjectMethod(app, getPackageManager);
    jclass pmClass = pm ? env->GetObjectClass(pm) : nullptr;
    jmethodID getPackageInfo = pmClass ? env->GetMethodID(
        pmClass,
        "getPackageInfo",
        "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;") : nullptr;

    jobject pkgInfo = (pm && getPackageInfo && pkgName)
        ? env->CallObjectMethod(pm, getPackageInfo, pkgName, 0)
        : nullptr;

    if (env->ExceptionCheck()) {
        clearException(env, "PackageManager.getPackageInfo");
        pkgInfo = nullptr;
    }

    if (pkgInfo) {
        jclass pkgInfoClass = env->GetObjectClass(pkgInfo);
        jfieldID versionNameField = env->GetFieldID(
            pkgInfoClass, "versionName", "Ljava/lang/String;");
        jstring versionName = versionNameField
            ? static_cast<jstring>(env->GetObjectField(pkgInfo, versionNameField))
            : nullptr;
        const char *versionChars = versionName
            ? env->GetStringUTFChars(versionName, nullptr)
            : nullptr;

        const bool expected = versionChars &&
            strncmp(versionChars, kTargetVersionPrefix, strlen(kTargetVersionPrefix)) == 0;
        LOGI("runtime probe: versionName=%s targetMatch=%s",
             versionChars ? versionChars : "?", expected ? "yes" : "no");

        if (versionChars) env->ReleaseStringUTFChars(versionName, versionChars);
        if (versionName) env->DeleteLocalRef(versionName);
        env->DeleteLocalRef(pkgInfoClass);
        env->DeleteLocalRef(pkgInfo);
    } else {
        LOGW("runtime probe: failed to resolve WhatsApp versionName");
    }

    LOGI("v0.2 probe ready: ART hook engine integration is gated by exact runtime mapping");

    if (pkgChars) env->ReleaseStringUTFChars(pkgName, pkgChars);
    if (pkgName) env->DeleteLocalRef(pkgName);
    if (pmClass) env->DeleteLocalRef(pmClass);
    if (pm) env->DeleteLocalRef(pm);
    if (loader) env->DeleteLocalRef(loader);
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

        LOGI("Injected into com.whatsapp; requested target=2.26.34; LSPosed=not-required");

        pthread_t thread;
        if (pthread_create(&thread, nullptr, runtimeProbe, nullptr) == 0) {
            pthread_detach(thread);
        } else {
            LOGW("failed to start runtime probe thread");
        }
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
};
} // namespace

REGISTER_ZYGISK_MODULE(WAStatusHDModule)
