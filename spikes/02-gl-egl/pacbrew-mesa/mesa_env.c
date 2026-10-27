/* mesa_env.c - linked next to spikes/02-gl-egl/src/gl_egl.c (unchanged) in
 * gl_egl_mesa.elf. elfldr passes a payload no environment, so Mesa's
 * diagnostics are switched on here, in a constructor that runs before main()
 * and therefore before eglInitialize() reads them. In a payload stderr goes
 * back over the elfldr socket, so the logs land in tools/send.sh's log.
 * Existing values win (overwrite = 0). Build with -DPS5WK_MESA_QUIET to drop
 * the extra logging.
 */
#include <stdlib.h>

__attribute__((constructor)) static void ps5wk_mesa_env(void)
{
#ifndef PS5WK_MESA_QUIET
    setenv("EGL_LOG_LEVEL", "debug", 0);  /* EGL platform/driver selection */
    setenv("MESA_LOG_LEVEL", "info", 0);
#endif
    /* RADV's own default (patch 0028) for a process that cannot write /app0. */
    setenv("MESA_SHADER_CACHE_DIR", "/data/radv-shader-cache", 0);
}
