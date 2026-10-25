/*
 * The AOSP audio HIDL service builds audio_port_config arrays without speaker_layout_channel_mask
 * (SkipSpeakerLayoutChannelMaskField.h, 216 bytes each), the CAF primary HAL is built with it
 * (220 bytes). Element 0 reads fine, every later one is shifted by 4 bytes, so any patch with
 * two sinks (ringtone on speaker + USB/BT headset, some call routes) gets a garbage device and
 * fails. This module is loaded as the primary HAL (ro.hardware.audio.primary), opens the real
 * one and hands it arrays in its own layout.
 */

#define LOG_TAG "AudioPortShim"

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <system/audio.h>

#define REAL_HAL "/vendor/lib64/hw/audio.primary.volcano.so"

/* Layout the HIDL side sends: same struct minus the trailing speaker_layout_channel_mask */
#define PEER_SIZE (sizeof(struct audio_port_config) - sizeof(audio_channel_mask_t))

_Static_assert(offsetof(struct audio_port_config, ext) + sizeof(((struct audio_port_config *)0)->ext) ==
               sizeof(struct audio_port_config), "ext is not the last member");
_Static_assert(offsetof(struct audio_port_config_device_ext, speaker_layout_channel_mask) +
               sizeof(audio_channel_mask_t) == sizeof(struct audio_port_config_device_ext),
               "speaker_layout_channel_mask is not the last device field");

static struct hw_module_t *g_real;
static int (*g_orig_create_patch)(struct audio_hw_device *, unsigned int, const struct audio_port_config *,
                                  unsigned int, const struct audio_port_config *, audio_patch_handle_t *);
static int (*g_orig_set_port_config)(struct audio_hw_device *, const struct audio_port_config *);

static struct audio_port_config *to_hal_layout(unsigned int n, const struct audio_port_config *peer) {
    struct audio_port_config *configs = calloc(n ? n : 1, sizeof(*configs));
    if (!configs) return NULL;
    for (unsigned int i = 0; i < n; i++)
        memcpy(&configs[i], (const char *)peer + i * PEER_SIZE, PEER_SIZE);
    return configs;
}

static int shim_create_audio_patch(struct audio_hw_device *dev, unsigned int num_sources,
                                   const struct audio_port_config *sources, unsigned int num_sinks,
                                   const struct audio_port_config *sinks, audio_patch_handle_t *handle) {
    struct audio_port_config *src = to_hal_layout(num_sources, sources);
    struct audio_port_config *snk = to_hal_layout(num_sinks, sinks);
    int ret;
    if (src && snk) {
        ret = g_orig_create_patch(dev, num_sources, src, num_sinks, snk, handle);
    } else {
        ALOGE("out of memory, passing the patch through");
        ret = g_orig_create_patch(dev, num_sources, sources, num_sinks, sinks, handle);
    }
    free(src);
    free(snk);
    return ret;
}

static int shim_set_audio_port_config(struct audio_hw_device *dev, const struct audio_port_config *config) {
    struct audio_port_config hal;
    memset(&hal, 0, sizeof(hal));
    memcpy(&hal, config, PEER_SIZE);
    return g_orig_set_port_config(dev, &hal);
}

static int wrapper_open(const struct hw_module_t *module __unused, const char *name, struct hw_device_t **device) {
    if (!g_real) {
        void *handle = dlopen(REAL_HAL, RTLD_NOW);
        if (handle) g_real = dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR);
        if (!g_real) {
            ALOGE("cannot load %s: %s", REAL_HAL, dlerror());
            return -EINVAL;
        }
    }

    int ret = g_real->methods->open(g_real, name, device);
    if (ret || !device || !*device || strcmp(name, AUDIO_HARDWARE_INTERFACE)) return ret;

    struct audio_hw_device *dev = (struct audio_hw_device *)*device;
    if (dev->create_audio_patch && dev->create_audio_patch != shim_create_audio_patch) {
        g_orig_create_patch = dev->create_audio_patch;
        dev->create_audio_patch = shim_create_audio_patch;
    }
    if (dev->set_audio_port_config && dev->set_audio_port_config != shim_set_audio_port_config) {
        g_orig_set_port_config = dev->set_audio_port_config;
        dev->set_audio_port_config = shim_set_audio_port_config;
    }
    ALOGI("converting audio_port_config %zu -> %zu bytes for %s", PEER_SIZE, sizeof(struct audio_port_config),
          g_real->name ? g_real->name : REAL_HAL);
    return ret;
}

static struct hw_module_methods_t wrapper_methods = {
    .open = wrapper_open,
};

__attribute__((visibility("default")))
struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Asteroids primary audio HAL",
        .author = "Nothing",
        .methods = &wrapper_methods,
    },
};
