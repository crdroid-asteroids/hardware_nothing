/*
 * Nothing Camera drives the glyph (fill light, countdown, camera toy) through LightsManager lights
 * 105, 106 and 120. Stock Nothing OS maps those to the lights HAL's ILightsExtension, which the qti
 * lights service attaches to its ILights binder. AOSP never does, so the camera finds no such light.
 *
 * Loaded first by vendor.qti.hardware.lights.service: when it registers ILights/default, register a
 * wrapper instead that lists those lights too (type CAMERA, not a system light) and sends their state to setLightExtensionState().
 */

#define LOG_TAG "GlyphLightsShim"

#include <aidl/android/hardware/light/BnLights.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <log/log.h>

#include <cstdio>
#include <cstring>
#include <memory>

using aidl::android::hardware::light::BnLights;
using aidl::android::hardware::light::HwLight;
using aidl::android::hardware::light::HwLightState;
using aidl::android::hardware::light::ILights;
using aidl::android::hardware::light::LightType;

namespace {

constexpr const char* kLightsInstance = "android.hardware.light.ILights/default";
constexpr int32_t kGlyphLights[] = {105, 106, 120};
// vendor.hardware.light.ILightsExtension.setLightExtensionState(int id, long state, int brightness)
constexpr transaction_code_t kSetLightExtensionState = FIRST_CALL_TRANSACTION + 0;

bool isGlyphLight(int32_t id) {
    for (int32_t glyph : kGlyphLights)
        if (id == glyph) return true;
    return false;
}

int32_t glyphBrightness() {
    static int32_t brightness = [] {
        int value = 4095;
        if (FILE* f = fopen("/sys/class/leds/aw20036_led/max_brightness", "r")) {
            if (fscanf(f, "%d", &value) != 1 || value <= 0) value = 4095;
            fclose(f);
        }
        return value;
    }();
    return brightness;
}

class GlyphLights : public BnLights {
  public:
    GlyphLights(std::shared_ptr<ILights> hal, ndk::SpAIBinder ext)
        : mHal(std::move(hal)), mExt(std::move(ext)) {}

    ndk::ScopedAStatus setLightState(int32_t id, const HwLightState& state) override {
        if (!isGlyphLight(id)) return mHal->setLightState(id, state);
        if (!mExt.get()) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

        AParcel* in = nullptr;
        binder_status_t ret = AIBinder_prepareTransaction(mExt.get(), &in);
        if (ret == STATUS_OK) ret = AParcel_writeInt32(in, id);
        if (ret == STATUS_OK) ret = AParcel_writeInt64(in, static_cast<uint32_t>(state.color));
        if (ret == STATUS_OK) ret = AParcel_writeInt32(in, glyphBrightness());
        AParcel* out = nullptr;
        if (ret == STATUS_OK) ret = AIBinder_transact(mExt.get(), kSetLightExtensionState, &in, &out, 0);
        else AParcel_delete(in);
        if (out) AParcel_delete(out);

        ALOGI("light %d state %d -> extension (%d)", id, state.color, ret);
        return ret == STATUS_OK ? ndk::ScopedAStatus::ok() : ndk::ScopedAStatus::fromStatus(ret);
    }

    ndk::ScopedAStatus getLights(std::vector<HwLight>* lights) override {
        ndk::ScopedAStatus status = mHal->getLights(lights);
        if (!status.isOk() || !mExt.get()) return status;
        for (int32_t id : kGlyphLights) {
            HwLight light;
            light.id = id;
            light.ordinal = 0;
            light.type = LightType::CAMERA;
            lights->push_back(light);
        }
        return status;
    }

  private:
    std::shared_ptr<ILights> mHal;
    ndk::SpAIBinder mExt;
};

std::shared_ptr<GlyphLights> gWrapper;

}  // namespace

extern "C" binder_exception_t AServiceManager_addService(AIBinder* binder, const char* instance) {
    static auto real = reinterpret_cast<binder_exception_t (*)(AIBinder*, const char*)>(
            dlsym(RTLD_NEXT, "AServiceManager_addService"));
    if (!real) return EX_ILLEGAL_STATE;
    if (!binder || !instance || strcmp(instance, kLightsInstance) != 0) return real(binder, instance);

    AIBinder_incStrong(binder);  // SpAIBinder adopts this reference
    std::shared_ptr<ILights> hal = ILights::fromBinder(ndk::SpAIBinder(binder));
    AIBinder* ext = nullptr;
    AIBinder_getExtension(binder, &ext);
    if (!hal || !ext) {
        ALOGW("no ILights or extension (%p), registering the HAL as is", ext);
        if (ext) AIBinder_decStrong(ext);
        return real(binder, instance);
    }

    gWrapper = ndk::SharedRefBase::make<GlyphLights>(hal, ndk::SpAIBinder(ext));
    ndk::SpAIBinder wrapper = gWrapper->asBinder();
    AIBinder_setExtension(wrapper.get(), ext);
    ALOGI("exposing glyph lights 105, 106, 120 through the lights extension");
    return real(wrapper.get(), instance);
}
