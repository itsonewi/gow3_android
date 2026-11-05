// spartanport iOS: the FidelityFX Vulkan "portable" upscaler is not built here.
//
// vk_temporal_upscaler.cpp drives FSR 3 through third_party/fsr-vulkan, a
// submodule the iOS build neither checks out nor links: its provider loader is
// a desktop driver component and MoltenVK exposes none. The renderer still
// needs the API's types to compile, so this stand-in declares them with the
// fields the upscaler uses and answers every call with "unavailable".
//
// The upscaler treats a failed context creation as "FSR 3 off" (it returns
// false from EnsureResources, exactly as on a device that cannot run FSR), so
// the game's TAA and the port's non-FSR paths keep working.

#pragma once

#if defined(SPARTAN_IOS)

#include <vulkan/vulkan.h>

struct FfxVkPortableUpscaleContext;

enum FfxVkPortableResourceState : unsigned int {
    FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ = 0,
    FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS = 1,
};

static constexpr unsigned int FFX_VK_PORTABLE_OK = 0;
static constexpr unsigned int FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT = 1u << 0;
static constexpr unsigned int FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE = 1u << 1;

struct FfxVkPortableImage {
    unsigned int structSize;
    VkImage image;
    VkFormat format;
    unsigned int extent[2];
    unsigned int mipCount;
    unsigned int arrayLayers;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    FfxVkPortableResourceState state;
};

struct FfxVkPortableDeviceInfo {
    unsigned int structSize;
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
    VkQueue queue;
    unsigned int queueFamilyIndex;
    VkBool32 shaderFloat16Enabled;
    VkBool32 subgroupSizeControlEnabled;
    VkBool32 synchronization2Enabled;
    VkBool32 shaderStorageImageWriteWithoutFormatEnabled;
};

struct FfxVkPortableUpscaleCreateInfo {
    unsigned int structSize;
    unsigned int flags;
    unsigned int maxRenderSize[2];
    unsigned int maxOutputSize[2];
};

struct FfxVkPortableUpscaleDispatchInfo {
    unsigned int structSize;
    VkCommandBuffer commandBuffer;
    FfxVkPortableImage color;
    FfxVkPortableImage depth;
    FfxVkPortableImage motionVectors;
    FfxVkPortableImage output;
    FfxVkPortableImage exposure;
    FfxVkPortableImage reactiveMask;
    FfxVkPortableImage transparencyAndCompositionMask;
    float jitterOffset[2];
    float motionVectorScale[2];
    unsigned int renderSize[2];
    unsigned int outputSize[2];
    float frameTimeMilliseconds;
    float preExposure;
    float cameraNear;
    float cameraFar;
    float cameraVerticalFovRadians;
    float viewSpaceToMeters;
    float sharpness;
    VkBool32 enableSharpening;
    VkBool32 reset;
    unsigned int frameId;
};

struct FfxVkPortableMemoryUsage {
    unsigned int structSize;
    unsigned long long totalUsageInBytes;
};

static inline const char* ffxVkPortableValidationIssueName(unsigned long long) {
    return "ios: no FFX provider";
}

static inline unsigned long long ffxVkPortableValidateUpscaleCreateInfo(
    const FfxVkPortableUpscaleCreateInfo*) {
    return 0;
}

static inline unsigned long long ffxVkPortableValidateUpscaleDispatchInfo(
    const FfxVkPortableUpscaleCreateInfo*, const FfxVkPortableUpscaleDispatchInfo*) {
    return 0;
}

static inline unsigned int ffxVkPortableUpscaleContextCreate(
    const FfxVkPortableDeviceInfo*, const FfxVkPortableUpscaleCreateInfo*,
    FfxVkPortableUpscaleContext**) {
    // Not FFX_VK_PORTABLE_OK: the upscaler reports FSR 3 as unavailable.
    return 1;
}

static inline void ffxVkPortableUpscaleContextDestroy(FfxVkPortableUpscaleContext*) {}

static inline unsigned int ffxVkPortableUpscaleContextRecordDispatch(
    FfxVkPortableUpscaleContext*, const FfxVkPortableUpscaleDispatchInfo*) {
    return 1;
}

static inline void ffxVkPortableUpscaleContextGetMemoryUsage(FfxVkPortableUpscaleContext*,
                                                             FfxVkPortableMemoryUsage* usage) {
    if (usage) {
        usage->totalUsageInBytes = 0;
    }
}

#endif // SPARTAN_IOS
