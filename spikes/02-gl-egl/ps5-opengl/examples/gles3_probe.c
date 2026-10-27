/* Minimal OpenGL ES 3 consumer of toolchain/ps5-opengl-gles: EGL display,
 * window surface (the TV), ES 3 context, a few cleared frames, and the PS5
 * texture-descriptor export of an eye-buffer-shaped texture. Logs to stdout
 * (the native title's log). Not run on hardware yet. */
#include <stdint.h>
#include <stdio.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include "ps5_egl_export.h"

int
main(void)
{
   EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major = 0, minor = 0, count = 0;
   if (!eglInitialize(display, &major, &minor))
      return 1;
   printf("[gles3-probe] EGL %d.%d apis=\"%s\"\n", major, minor,
          eglQueryString(display, EGL_CLIENT_APIS));

   const EGLint config_attributes[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
   EGLConfig config;
   if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count ||
       !eglBindAPI(EGL_OPENGL_ES_API))
      return 2;
   EGLSurface surface = eglCreateWindowSurface(display, config, 0, NULL);
   const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
   if (!surface || !context || !eglMakeCurrent(display, surface, surface, context))
      return 3;
   printf("[gles3-probe] GL_VERSION=\"%s\" GLSL=\"%s\" renderer=\"%s\"\n",
          glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION),
          glGetString(GL_RENDERER));

   /* Eye-buffer-shaped render target: single-level RGBA8 2D = SW_64KB_R_X. */
   GLuint texture, framebuffer;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2048, 1024);
   glGenFramebuffers(1, &framebuffer);
   glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
   glClearColor(0.0f, 0.5f, 1.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);

   uint32_t tsharp[8], width = 0, height = 0, pitch = 0, swizzle = 0;
   uint64_t address = 0;
   if (eglExportTextureDescriptorPS5(display, context, texture, tsharp, &address,
                                     &width, &height, &pitch, &swizzle))
      printf("[gles3-probe] export va=0x%llx %ux%u pitch=%u sw_mode=%u T#=%08x %08x %08x %08x "
             "%08x %08x %08x %08x\n", (unsigned long long)address, width, height, pitch,
             swizzle, tsharp[0], tsharp[1], tsharp[2], tsharp[3], tsharp[4], tsharp[5],
             tsharp[6], tsharp[7]);
   else
      printf("[gles3-probe] export failed 0x%x\n", eglGetError());

   for (int frame = 0; frame < 120; ++frame) {
      glClearColor((float)(frame % 60) / 60.0f, 0.2f, 0.4f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      if (!eglSwapBuffers(display, surface))
         return 4;
   }
   printf("[gles3-probe] done\n");
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return 0;
}
