/* swapchain.c - swapchains for libopenxr_psvr2: GL texture images owned by
 * the backend, FIFO acquire/wait/release. */
#include "xr_internal.h"

#include <stdlib.h>
#include <string.h>

/* GL enums (no GL headers needed in the core). */
#define RT_GL_RGBA8 0x8058
#define RT_GL_SRGB8_ALPHA8 0x8C43
#define RT_GL_DEPTH_COMPONENT24 0x81A6
#define RT_GL_DEPTH24_STENCIL8 0x88F0

/* Runtime preference order. WebKit's selectColorFormat() takes GL_RGBA8
 * whenever it is listed (it writes display-referred values, like a canvas),
 * which also matches the UNORM T# Hmd2 samples, so RGBA8 comes first. */
static const int64_t kFormats[] = {RT_GL_RGBA8, RT_GL_SRGB8_ALPHA8, RT_GL_DEPTH24_STENCIL8,
                                   RT_GL_DEPTH_COMPONENT24};
#define N_FORMATS (sizeof kFormats / sizeof kFormats[0])
#define RT_SWAPCHAIN_IMAGES 3 /* = PSVR2_MAX_BUFFERS; WebKit only needs >= 2 */
#define RT_MAX_DIM 16384      /* GFX10 T# limit */

XRRT_CORE XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session, uint32_t formatCapacityInput,
                                                          uint32_t *formatCountOutput, int64_t *formats)
{
    RT_GUARD();
    if (!rt_get_session(session))
        return XR_ERROR_HANDLE_INVALID;
    int copy;
    XrResult r = rt_two_call(formatCapacityInput, formatCountOutput, (uint32_t)N_FORMATS, formats, &copy);
    if (copy)
        memcpy(formats, kFormats, sizeof kFormats);
    return r;
}

XRRT_CORE XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *createInfo,
                                                XrSwapchain *swapchain)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !swapchain || createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    int known = 0;
    for (size_t i = 0; i < N_FORMATS; i++)
        known |= kFormats[i] == createInfo->format;
    if (!known)
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if (createInfo->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)
        return XR_ERROR_FEATURE_UNSUPPORTED;
    if (createInfo->width == 0 || createInfo->height == 0 || createInfo->mipCount == 0 ||
        createInfo->arraySize == 0 || createInfo->faceCount == 0 || createInfo->sampleCount == 0)
        return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->width > RT_MAX_DIM || createInfo->height > RT_MAX_DIM)
        return XR_ERROR_LIMIT_REACHED;
    /* Only plain 2D, single-sampled, single-layer images (what WebKit's
     * projection layer creates). Cube faces and arrays would need the
     * composition-layer extensions WebKit only uses when advertised. */
    if (createInfo->sampleCount != 1 || createInfo->faceCount != 1 || createInfo->arraySize != 1)
        return XR_ERROR_FEATURE_UNSUPPORTED;
    rt_swapchain *sc = calloc(1, sizeof *sc);
    if (!sc)
        return XR_ERROR_OUT_OF_MEMORY;
    sc->sess = s;
    sc->info = *createInfo;
    sc->info.next = NULL;
    sc->n_images = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1 : RT_SWAPCHAIN_IMAGES;
    sc->last_released = -1;
    XrResult r = s->inst->be->swapchain_create(&sc->info, sc->n_images, sc->images, &sc->be_priv);
    if (XR_FAILED(r)) {
        free(sc);
        return r;
    }
    sc->handle = (XrSwapchain)(uintptr_t)rt_handle_new(OBJ_SWAPCHAIN, sc);
    sc->next = s->swapchains;
    s->swapchains = sc;
    *swapchain = sc->handle;
    RT_INFO("swapchain %ux%u format 0x%llx, %u images", createInfo->width, createInfo->height,
            (unsigned long long)createInfo->format, sc->n_images);
    return XR_SUCCESS;
}

void rt_swapchain_destroy_locked(rt_swapchain *sc)
{
    rt_session *s = sc->sess;
    for (rt_swapchain **pp = &s->swapchains; *pp; pp = &(*pp)->next) {
        if (*pp == sc) {
            *pp = sc->next;
            break;
        }
    }
    s->inst->be->swapchain_destroy(sc->be_priv, sc->n_images, sc->images);
    rt_handle_free(RT_H(sc->handle));
    free(sc);
}

XRRT_CORE XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain)
{
    RT_GUARD();
    rt_swapchain *sc = rt_handle_get(RT_H(swapchain), OBJ_SWAPCHAIN);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    rt_swapchain_destroy_locked(sc);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput,
                                                         uint32_t *imageCountOutput,
                                                         XrSwapchainImageBaseHeader *images)
{
    RT_GUARD();
    rt_swapchain *sc = rt_handle_get(RT_H(swapchain), OBJ_SWAPCHAIN);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    int copy;
    XrResult r = rt_two_call(imageCapacityInput, imageCountOutput, sc->n_images, images, &copy);
    if (!copy)
        return r;
    XrSwapchainImageOpenGLESKHR *gl = (XrSwapchainImageOpenGLESKHR *)images;
    for (uint32_t i = 0; i < sc->n_images; i++) {
        if (gl[i].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR)
            return XR_ERROR_VALIDATION_FAILURE;
        gl[i].image = (uint32_t)sc->images[i];
    }
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                      const XrSwapchainImageAcquireInfo *acquireInfo,
                                                      uint32_t *index)
{
    RT_GUARD();
    rt_swapchain *sc = rt_handle_get(RT_H(swapchain), OBJ_SWAPCHAIN);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    if ((acquireInfo && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) || !index)
        return XR_ERROR_VALIDATION_FAILURE;
    if (sc->n_acquired == sc->n_images)
        return XR_ERROR_CALL_ORDER_INVALID;
    if ((sc->info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) && sc->last_released >= 0)
        return XR_ERROR_CALL_ORDER_INVALID;
    const uint32_t i = sc->next_acquire;
    sc->next_acquire = (sc->next_acquire + 1) % sc->n_images;
    sc->acquired[sc->n_acquired++] = i;
    *index = i;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *waitInfo)
{
    RT_GUARD();
    rt_swapchain *sc = rt_handle_get(RT_H(swapchain), OBJ_SWAPCHAIN);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    if (!waitInfo || waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    /* One outstanding wait at a time, on the oldest acquired image. */
    if (sc->n_waited >= sc->n_acquired || sc->n_waited > 0)
        return XR_ERROR_CALL_ORDER_INVALID;
    /* The backend reads an image only inside xrEndFrame (blit/copy on this
     * thread), so a released image is free again by the time it can come
     * round: nothing to wait for. */
    sc->n_waited = 1;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                      const XrSwapchainImageReleaseInfo *releaseInfo)
{
    RT_GUARD();
    rt_swapchain *sc = rt_handle_get(RT_H(swapchain), OBJ_SWAPCHAIN);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    if (releaseInfo && releaseInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (sc->n_waited == 0)
        return XR_ERROR_CALL_ORDER_INVALID;
    sc->last_released = sc->acquired[0];
    memmove(sc->acquired, sc->acquired + 1, (sc->n_acquired - 1) * sizeof sc->acquired[0]);
    sc->n_acquired--;
    sc->n_waited = 0;
    return XR_SUCCESS;
}
