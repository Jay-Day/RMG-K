#ifndef RMGK_VULKAN_OVERLAY_H
#define RMGK_VULKAN_OVERLAY_H

/*
 * How the app draws its on-screen display over a graphics plugin that renders
 * with Vulkan. The plugin owns the device and the swapchain, so it calls these
 * each frame and the app records its draws into the plugin's own render pass.
 *
 * The app hands the hooks to the plugin through the plugin's
 * RmgkSetVulkanOverlay export. Handles are passed as plain pointers so neither
 * side needs the other's Vulkan headers.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RmgkVulkanOverlayFrame
{
    void* instance;       /* VkInstance */
    void* physicalDevice; /* VkPhysicalDevice */
    void* device;         /* VkDevice */
    void* queue;          /* VkQueue the app may submit texture uploads to */
    uint32_t queueFamily;
    uint32_t imageCount;  /* swapchain images */
    uint32_t format;      /* VkFormat of the swapchain */
    uint32_t width;       /* swapchain size in pixels */
    uint32_t height;
    /* vkGetInstanceProcAddr */
    void* (*getInstanceProcAddr)(void* instance, const char* name);
    /* Held by the app around anything it submits to the queue. */
    void (*queueLock)(void* userData);
    void (*queueUnlock)(void* userData);
    void* userData;
} RmgkVulkanOverlayFrame;

typedef struct RmgkVulkanOverlayHooks
{
    /* Before the swapchain render pass, with no command buffer of the plugin's
       open. Returns nonzero when there is something to draw this frame. */
    int (*prepare)(const RmgkVulkanOverlayFrame* frame);
    /* Inside the swapchain render pass (one color attachment, first subpass),
       after the plugin's own draws. Only after prepare returned nonzero. Leaves
       the command buffer's bound state changed. */
    void (*draw)(void* commandBuffer /* VkCommandBuffer */);
    /* Before the device goes away, with the device idle. */
    void (*shutdown)(void);
} RmgkVulkanOverlayHooks;

#ifdef __cplusplus
}
#endif

#endif /* RMGK_VULKAN_OVERLAY_H */
