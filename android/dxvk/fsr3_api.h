#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

/* C ABI keeps the optional Android 35 SDK and its static C++ runtime isolated. */
typedef struct FpFsr3Image {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} FpFsr3Image;

typedef struct FpFsr3Frame {
    FpFsr3Image color, depth, motion, reactive, output;
    FpFsr3Image flow, sceneChange, dilatedDepth, dilatedMotion, previousDepth;
    float jitterX, jitterY, milliseconds, nearPlane, farPlane, verticalFov;
    uint32_t reset;
} FpFsr3Frame;

typedef struct FpFsr3Api {
    uint32_t version, bytes;
    void *(*create)(VkDevice, VkPhysicalDevice, PFN_vkGetDeviceProcAddr,
                    uint32_t, uint32_t, uint32_t, uint32_t, int invertedDepth, uint32_t *error);
    void (*destroy)(void *);
    uint32_t (*flow)(void *, VkCommandBuffer, const FpFsr3Frame *);
    uint32_t (*upscale)(void *, VkCommandBuffer, const FpFsr3Frame *);
    void (*jitter)(uint32_t index, uint32_t width, uint32_t outputWidth, float *x, float *y);
} FpFsr3Api;

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default"))) const FpFsr3Api *frostpunk_fsr3_api(void);
#ifdef __cplusplus
}
#endif
