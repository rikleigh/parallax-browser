/* Host check of the patched (GLES) ps5-opengl EGL facade on top of the real
 * ps5 Gallium screen (src/gallium/ps5/ps5_screen.c: caps + format table) and
 * the real Mesa 26.2 state tracker. No GPU work is submitted: this answers
 * which OpenGL / OpenGL ES versions Mesa derives from the driver caps and
 * exercises the EGL API/config/context logic. Built by hostcheck/run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES3/gl32.h>

#include "ps5_egl_export.h"

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x40
#endif

/* psbc (the PS5 shader compiler) is not built for the host; Mesa only stores
 * the options pointer at screen creation. */
static _Alignas(64) unsigned char nir_options[8][8192];
const void *psbc_get_nir_options(int stage) { return nir_options[stage & 7]; }
void psbc_free_output(void *output) { (void)output; }
void psbc_free_tessellation_output(void *output) { (void)output; }
void hostcheck_es_diag(int compressed_fallback);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: "); } \
   else printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* PS5VR's eye-buffer T# (ref/PS5VR/app/engine/media/src/eng_agc_writer.c,
 * build_tsharp_2d_internal + eng_agc_build_tsharp_render_target), restated
 * for comparison only. */
static void
ps5vr_tsharp_render_target(uint32_t out[8], uint64_t va, uint32_t w, uint32_t h, int fp16)
{
   memset(out, 0, 32);
   out[0] = (uint32_t)(va >> 8);
   out[1] = (uint32_t)(va >> 40) | ((fp16 ? 71u : 56u) << 20) | (((w - 1u) & 3u) << 30);
   out[2] = (((w - 1u) >> 2) & 0xfffu) | ((h - 1u) << 14);
   out[3] = 4u | (5u << 3) | (6u << 6) | (7u << 9) | (9u << 28) | (27u << 20);
   out[5] = 7u << 20;
}

static void
export_case(EGLDisplay d, EGLContext ctx, const char *label, GLenum target,
            GLsizei levels, GLenum format, GLsizei w, GLsizei h, int expect_ok,
            unsigned expect_sw)
{
   GLuint tex;
   uint32_t t[8] = {0}, ew = 0, eh = 0, pitch = 0, sw = 0;
   uint64_t va = 0;
   glGenTextures(1, &tex);
   glBindTexture(target, tex);
   glTexStorage2D(target, levels, format, w, h);
   EGLBoolean ok = eglExportTextureDescriptorPS5(d, ctx, tex, t, &va, &ew, &eh, &pitch, &sw);
   EGLint err = eglGetError();
   if (!expect_ok) {
      CHECK(!ok && err == EGL_BAD_MATCH, "export %s refused (0x%x)", label, err);
   } else {
      CHECK(ok && sw == expect_sw && va && !(va & 255) && ew == (uint32_t)w && eh == (uint32_t)h,
            "export %s: va=0x%llx (64K-aligned=%d) %ux%u pitch=%u sw_mode=%u",
            label, (unsigned long long)va, !(va & 0xffff), ew, eh, pitch, sw);
      printf("      T# %08x %08x %08x %08x %08x %08x %08x %08x\n",
             t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
      if (sw == EGL_PS5_SW_MODE_64KB_R_X && levels == 1) {
         uint32_t v[8];
         ps5vr_tsharp_render_target(v, va, ew, eh, format == GL_RGBA16F);
         printf("      PS5VR %08x %08x %08x %08x %08x %08x %08x %08x  differing words:",
                v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
         for (int i = 0; i < 8; i++)
            if (v[i] != t[i]) printf(" w%d(xor %08x)", i, v[i] ^ t[i]);
         printf("\n");
      }
   }
   glDeleteTextures(1, &tex);
}

static EGLContext
make(EGLDisplay d, EGLConfig c, const EGLint *attr, EGLint *err)
{
   EGLContext ctx = eglCreateContext(d, c, EGL_NO_CONTEXT, attr);
   *err = eglGetError();
   return ctx;
}

static void
report_current(const char *label)
{
   const char *version = (const char *)glGetString(GL_VERSION);
   const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
   GLint major = 0, minor = 0, n = 0;
   glGetIntegerv(GL_MAJOR_VERSION, &major);
   glGetIntegerv(GL_MINOR_VERSION, &minor);
   glGetIntegerv(GL_NUM_EXTENSIONS, &n);
   printf("      %s: GL_VERSION=\"%s\" GLSL=\"%s\" (%d.%d, %d extensions)\n",
          label, version ? version : "(null)", glsl ? glsl : "(null)",
          major, minor, n);
}

static int
has_ext(const char *name)
{
   GLint n = 0;
   glGetIntegerv(GL_NUM_EXTENSIONS, &n);
   for (GLint i = 0; i < n; i++)
      if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), name))
         return 1;
   return 0;
}

int
main(void)
{
   EGLint major, minor, err, n = 0, value = 0;
   hostcheck_es_diag(1);
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   CHECK(eglInitialize(d, &major, &minor), "eglInitialize -> EGL %d.%d", major, minor);
   printf("      EGL_CLIENT_APIS=\"%s\"\n      EGL_EXTENSIONS=\"%s\"\n",
          eglQueryString(d, EGL_CLIENT_APIS), eglQueryString(d, EGL_EXTENSIONS));
   CHECK(!strcmp(eglQueryString(d, EGL_CLIENT_APIS), "OpenGL OpenGL_ES"),
         "EGL_CLIENT_APIS lists OpenGL_ES");

   EGLConfig configs[4];
   eglGetConfigs(d, configs, 4, &n);
   for (EGLint i = 0; i < n; i++) {
      EGLint id, rt, conf, st;
      eglGetConfigAttrib(d, configs[i], EGL_CONFIG_ID, &id);
      eglGetConfigAttrib(d, configs[i], EGL_RENDERABLE_TYPE, &rt);
      eglGetConfigAttrib(d, configs[i], EGL_CONFORMANT, &conf);
      eglGetConfigAttrib(d, configs[i], EGL_SURFACE_TYPE, &st);
      CHECK((rt & (EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT)) ==
               (EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT),
            "config %d RENDERABLE_TYPE=0x%x CONFORMANT=0x%x SURFACE_TYPE=0x%x",
            id, rt, conf, st);
   }
   /* ANGLE-style config choice (DisplayEGL): ES3 renderable pbuffer. */
   const EGLint choose[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                             EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                             EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
                             EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 24,
                             EGL_BIND_TO_TEXTURE_RGBA, EGL_DONT_CARE, EGL_NONE };
   EGLConfig cfg = NULL;
   CHECK(eglChooseConfig(d, choose, &cfg, 1, &n) && n == 1,
         "eglChooseConfig(ES3 pbuffer, COLOR_BUFFER_TYPE, BIND_TO_TEXTURE_RGBA) -> %d", n);

   CHECK(eglBindAPI(EGL_OPENGL_ES_API) && eglQueryAPI() == EGL_OPENGL_ES_API,
         "eglBindAPI(EGL_OPENGL_ES_API)");

   const EGLint es1[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
   CHECK(!make(d, cfg, es1, &err) && err == EGL_BAD_MATCH, "ES 1.x refused (0x%x)", err);
   CHECK(!make(d, cfg, NULL, &err) && err == EGL_BAD_MATCH,
         "default (=ES 1) refused (0x%x)", err);
   const EGLint esprof[] = { EGL_CONTEXT_CLIENT_VERSION, 3,
                             EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE };
   CHECK(!make(d, cfg, esprof, &err) && err == EGL_BAD_ATTRIBUTE,
         "ES + profile mask -> EGL_BAD_ATTRIBUTE (0x%x)", err);
   const EGLint es21[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 2,
                           EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE };
   CHECK(!make(d, cfg, es21, &err) && err == EGL_BAD_MATCH, "ES 2.1 refused (0x%x)", err);
   const EGLint es40[] = { EGL_CONTEXT_CLIENT_VERSION, 4, EGL_NONE };
   CHECK(!make(d, cfg, es40, &err) && err == EGL_BAD_MATCH, "ES 4.0 refused (0x%x)", err);

   static const struct { int major, minor; } wanted[] = {
      { 2, 0 }, { 3, 0 }, { 3, 1 }, { 3, 2 } };
   int best = 0;
   for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
      const EGLint attr[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, wanted[i].major,
                              EGL_CONTEXT_MINOR_VERSION_KHR, wanted[i].minor, EGL_NONE };
      EGLContext ctx = make(d, cfg, attr, &err);
      printf("%s ES %d.%d context: %p (0x%x)\n", ctx ? "ok:  " : "info:",
             wanted[i].major, wanted[i].minor, (void *)ctx, err);
      if (!ctx)
         continue;
      best = wanted[i].major * 10 + wanted[i].minor;
      eglQueryContext(d, ctx, EGL_CONTEXT_CLIENT_TYPE, &value);
      CHECK(value == EGL_OPENGL_ES_API, "EGL_CONTEXT_CLIENT_TYPE = EGL_OPENGL_ES_API");
      CHECK(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx),
            "surfaceless eglMakeCurrent");
      char label[32];
      snprintf(label, sizeof(label), "ES %d.%d request", wanted[i].major, wanted[i].minor);
      report_current(label);
      if (wanted[i].major == 3 && wanted[i].minor == 0) {
         static const char *es32_prereqs[] = {
            "GL_EXT_color_buffer_float", "GL_KHR_blend_equation_advanced",
            "GL_KHR_robustness", "GL_KHR_texture_compression_astc_ldr",
            "GL_OES_copy_image", "GL_OES_draw_buffers_indexed",
            "GL_OES_draw_elements_base_vertex", "GL_OES_geometry_shader",
            "GL_OES_primitive_bounding_box", "GL_OES_sample_variables",
            "GL_OES_tessellation_shader", "GL_OES_texture_buffer",
            "GL_OES_texture_cube_map_array", "GL_OES_texture_stencil8",
            "GL_EXT_color_buffer_half_float", "GL_OES_EGL_image",
            "GL_EXT_texture_format_BGRA8888", "GL_OES_texture_float_linear",
            "GL_EXT_texture_filter_anisotropic", "GL_ANGLE_instanced_arrays",
         };
         for (unsigned e = 0; e < sizeof(es32_prereqs) / sizeof(es32_prereqs[0]); e++)
            printf("      %-40s %s\n", es32_prereqs[e], has_ext(es32_prereqs[e]) ? "yes" : "NO");
      }
      eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(d, ctx);
   }
   printf("RESULT: highest OpenGL ES context version created = %d.%d\n", best / 10, best % 10);

   /* ES context with a pbuffer, as ANGLE/WebKit would use for offscreen. */
   const EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
   EGLSurface s = eglCreatePbufferSurface(d, cfg, pb);
   const EGLint es3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext ctx = make(d, cfg, es3, &err);
   CHECK(s && ctx && eglMakeCurrent(d, s, s, ctx), "ES3 context current on 64x64 pbuffer");
   eglQueryContext(d, ctx, EGL_CONTEXT_CLIENT_VERSION, &value);
   printf("      EGL_CONTEXT_CLIENT_VERSION=%d\n", value);
   CHECK(strstr(eglQueryString(d, EGL_EXTENSIONS), "EGL_PS5_texture_descriptor_export") &&
         eglGetProcAddress("eglExportTextureDescriptorPS5") ==
            (__eglMustCastToProperFunctionPointerType)eglExportTextureDescriptorPS5,
         "EGL_PS5_texture_descriptor_export advertised + eglGetProcAddress");
   export_case(d, ctx, "RGBA8 2048x1024 1 level", GL_TEXTURE_2D, 1, GL_RGBA8, 2048, 1024, 1,
               EGL_PS5_SW_MODE_64KB_R_X);
   export_case(d, ctx, "SRGB8_A8 1000x600 1 level", GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, 1000, 600, 1,
               EGL_PS5_SW_MODE_64KB_R_X);
   export_case(d, ctx, "RGBA16F 512x512 1 level", GL_TEXTURE_2D, 1, GL_RGBA16F, 512, 512, 1,
               EGL_PS5_SW_MODE_64KB_R_X);
   /* Renderable textures outside the native layout (mip chains, other
    * formats) are linear plus a tiled render-staging copy: not exportable. */
   export_case(d, ctx, "RGBA8 256x256 3 levels (linear + staging)", GL_TEXTURE_2D, 3, GL_RGBA8,
               256, 256, 0, 0);
   export_case(d, ctx, "RGB10_A2 256x256 (linear + staging)", GL_TEXTURE_2D, 1, GL_RGB10_A2,
               256, 256, 0, 0);
   export_case(d, ctx, "DEPTH32F 256x256", GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 256, 256, 0, 0);
   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(d, ctx);
   eglDestroySurface(d, s);

   /* OpenGL still works next to it. */
   CHECK(eglBindAPI(EGL_OPENGL_API), "eglBindAPI(EGL_OPENGL_API)");
   const EGLint core[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                           EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                           EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE };
   ctx = make(d, cfg, core, &err);
   CHECK(ctx && eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx),
         "GL 4.6 core context (0x%x)", err);
   if (ctx) {
      report_current("GL 4.6 core");
      eglQueryContext(d, ctx, EGL_CONTEXT_CLIENT_TYPE, &value);
      CHECK(value == EGL_OPENGL_API, "EGL_CONTEXT_CLIENT_TYPE = EGL_OPENGL_API");
      eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(d, ctx);
   }
   CHECK(eglTerminate(d), "eglTerminate");
   printf("%d failure(s)\n", failures);
   return failures != 0;
}
