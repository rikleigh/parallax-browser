/* egl_lookup.c - what ANGLE's EGL backend dlsym()s on other platforms.
 *
 * ps5-opengl is a static archive, and its eglGetProcAddress only knows the
 * GL entry points (_mesa_glapi_get_proc_address), not egl*. ANGLE
 * (patches/0001) asks this table first and falls through to
 * eglGetProcAddress for GL.
 */
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <string.h>

#define E(fn) {#fn, (void *)(fn)}
static const struct { const char *name; void *fn; } k_egl[] = {
    E(eglBindAPI), E(eglBindTexImage), E(eglChooseConfig), E(eglCopyBuffers),
    E(eglCreateContext), E(eglCreatePbufferFromClientBuffer), E(eglCreatePbufferSurface),
    E(eglCreatePixmapSurface), E(eglCreateWindowSurface), E(eglDestroyContext),
    E(eglDestroySurface), E(eglGetConfigAttrib), E(eglGetConfigs), E(eglGetCurrentContext),
    E(eglGetCurrentDisplay), E(eglGetCurrentSurface), E(eglGetDisplay), E(eglGetError),
    E(eglGetProcAddress), E(eglInitialize), E(eglMakeCurrent), E(eglQueryAPI),
    E(eglQueryContext), E(eglQueryString), E(eglQuerySurface), E(eglReleaseTexImage),
    E(eglReleaseThread), E(eglSurfaceAttrib), E(eglSwapBuffers), E(eglSwapInterval),
    E(eglTerminate), E(eglWaitClient), E(eglWaitGL), E(eglWaitNative),
};

void *angle_ps5_egl_get_proc_address(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof k_egl / sizeof *k_egl; i++)
        if (strcmp(k_egl[i].name, name) == 0)
            return k_egl[i].fn;
    return (void *)eglGetProcAddress(name);
}
