#include <jni.h>
#include <android/log.h>
#include <string.h>
#include "zygisk.hpp"

#define LOG_TAG "WAStatusHD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {
constexpr const char *kTargetPackage = "com.whatsapp";

class WAStatusHDModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
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

        if (!target_) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_) return;

        LOGI("Injected into com.whatsapp; target baseline=2.26.34; LSPosed=not-required");
        LOGI("MediaQuality bootstrap active (alpha); unsupported hooks remain fail-open");
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
};
} // namespace

REGISTER_ZYGISK_MODULE(WAStatusHDModule)
