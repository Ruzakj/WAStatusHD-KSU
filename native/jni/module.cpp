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
constexpr const char *kExactTargetVersion = "2.26.34.82";
constexpr int kVideoMaxEdge = 1920;
constexpr int kVideoBitrateKbps = 10000;
constexpr int kImageQuality = 100;
constexpr int kImageMaxEdge = 6000;
constexpr int kImageMaxKb = 50 * 1024;

JavaVM *g_vm = nullptr;

void clearException(JNIEnv *env, const char *where) {
    if (!env->ExceptionCheck()) return;
    env->ExceptionClear();
    LOGW("JNI exception cleared at %s", where);
}

void logQualityPolicy() {
    // Values are intentionally kept in native code so the eventual ART bridge
    // has a single source of truth. They mirror the scoped WaEnhancer
    // MediaQuality policy, without loading LSPosed/Xposed into WhatsApp.
    LOGI("quality policy: image quality=%d maxEdge=%d maxKb=%d",
         kImageQuality, kImageMaxEdge, kImageMaxKb);
    LOGI("quality policy: video maxEdge=%d bitrateKbps=%d",
         kVideoMaxEdge, kVideoBitrateKbps);
}

void *runtimeInit(void *) {
    if (!g_vm) return nullptr;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("runtime init: AttachCurrentThread failed");
        return nullptr;
    }

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
        if (env->ExceptionCheck()) { clearException(env, "currentApplication"); app = nullptr; }
        if (!app) usleep(250000);
    }
    if (!app) {
        LOGW("runtime init: Application unavailable");
        env->DeleteLocalRef(activityThread);
        g_vm->DetachCurrentThread();
        return nullptr;
    }

    jclass appClass = env->GetObjectClass(app);
    jmethodID getPackageName = env->GetMethodID(appClass, "getPackageName", "()Ljava/lang/String;");
    jmethodID getPackageManager = env->GetMethodID(appClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jmethodID getClassLoader = env->GetMethodID(appClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jstring pkgName = static_cast<jstring>(env->CallObjectMethod(app, getPackageName));
    const char *pkgChars = pkgName ? env->GetStringUTFChars(pkgName, nullptr) : nullptr;
    jobject loader = env->CallObjectMethod(app, getClassLoader);

    jobject pm = env->CallObjectMethod(app, getPackageManager);
    jclass pmClass = pm ? env->GetObjectClass(pm) : nullptr;
    jmethodID getPackageInfo = pmClass ? env->GetMethodID(pmClass, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;") : nullptr;
    jobject pkgInfo = (pm && getPackageInfo && pkgName) ? env->CallObjectMethod(pm, getPackageInfo, pkgName, 0) : nullptr;
    if (env->ExceptionCheck()) { clearException(env, "getPackageInfo"); pkgInfo = nullptr; }

    bool exact = false;
    if (pkgInfo) {
        jclass piClass = env->GetObjectClass(pkgInfo);
        jfieldID versionField = env->GetFieldID(piClass, "versionName", "Ljava/lang/String;");
        jstring version = versionField ? static_cast<jstring>(env->GetObjectField(pkgInfo, versionField)) : nullptr;
        const char *v = version ? env->GetStringUTFChars(version, nullptr) : nullptr;
        exact = v && strcmp(v, kExactTargetVersion) == 0;
        LOGI("runtime: package=%s version=%s exactTarget=%s classLoader=%s",
             pkgChars ? pkgChars : "?", v ? v : "?", exact ? "yes" : "no", loader ? "ready" : "missing");
        if (v) env->ReleaseStringUTFChars(version, v);
        if (version) env->DeleteLocalRef(version);
        env->DeleteLocalRef(piClass);
    }

    if (exact && loader) {
        logQualityPolicy();
        // Safety gate: only exact tested WhatsApp build reaches this point.
        // The next ART bridge may fail-open without changing app methods.
        LOGI("v0.3 functional gate READY: exact mapping accepted; fail-open enabled");
    } else {
        LOGW("v0.3 functional gate SKIPPED: unsupported runtime; no media mutation attempted");
    }

    if (pkgChars) env->ReleaseStringUTFChars(pkgName, pkgChars);
    if (pkgName) env->DeleteLocalRef(pkgName);
    if (pkgInfo) env->DeleteLocalRef(pkgInfo);
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
        api_ = api; env_ = env;
        if (env_) env_->GetJavaVM(&g_vm);
    }
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        target_ = false;
        if (!args || !args->nice_name) { api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY); return; }
        const char *process = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (process) { target_ = strcmp(process, kTargetPackage) == 0; env_->ReleaseStringUTFChars(args->nice_name, process); }
        if (!target_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_) return;
        LOGI("Injected into com.whatsapp; exact target=2.26.34.82; LSPosed=not-required");
        pthread_t thread;
        if (pthread_create(&thread, nullptr, runtimeInit, nullptr) == 0) pthread_detach(thread);
        else LOGW("failed to start runtime init thread");
    }
private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
};
}
REGISTER_ZYGISK_MODULE(WAStatusHDModule)
