/* Spike 12 P0b/P0c, GL side: what the browser's compositor does, reduced.
 * A GL 4.6 core context on the window surface (as PS5Renderer::initializeGL),
 * presenting every vblank; and the video path's SSBO -> texture pass
 * (PS5Renderer.cpp, kConvertFragmentShader) fed by a persistent buffer that
 * the Vulkan side imports by host pointer. */
#define SPIKE_NAME "12-webgpu-p0b"
#include "common/spike.h"
#include "shared.h"

#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>

struct gl_shared g_gl;

static const char *kFullscreenVS =
    "#version 430 core\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
/* The frame as Vulkan's vkCmdCopyImageToBuffer leaves it: RGBA8, row 0 first. */
static const char *kBridgeFS =
    "#version 430 core\n"
    "layout(std430, binding = 0) readonly buffer Frame { uint d[]; };\n"
    "uniform ivec3 geom; // width, height, offset in uints\n"
    "out vec4 colour;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    colour = unpackUnorm4x8(d[geom.z + p.y * geom.x + p.x]);\n"
    "}\n";
static const char *kBlitFS =
    "#version 430 core\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 colour;\n"
    "void main() { colour = texture(tex, vec2(uv.x, 1.0 - uv.y)); }\n";

static GLuint compile(GLenum type, const char *src, char *err, size_t n)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, (GLsizei)n, NULL, err);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs, const char *fs, char *err, size_t n)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs, err, n), f = v ? compile(GL_FRAGMENT_SHADER, fs, err, n) : 0;
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, (GLsizei)n, NULL, err);
        return 0;
    }
    return p;
}

void *gl_thread_main(void *arg)
{
    (void)arg;
    struct gl_shared *g = &g_gl;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API)) {
        snprintf(g->info, sizeof g->info, "EGL init 0x%04x", eglGetError());
        g->state = -1;
        return NULL;
    }
    const EGLint cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    const EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE };
    EGLConfig cfg;
    EGLint count = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &count) || count != 1) {
        snprintf(g->info, sizeof g->info, "eglChooseConfig 0x%04x", eglGetError());
        g->state = -1;
        return NULL;
    }
    EGLSurface surface = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
    EGLContext ctx = surface != EGL_NO_SURFACE ? eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs) : EGL_NO_CONTEXT;
    if (surface == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surface, surface, ctx)) {
        snprintf(g->info, sizeof g->info, "surface %p context %p error 0x%04x", surface, ctx, eglGetError());
        g->state = -1;
        return NULL;
    }
    EGLint sw = 0, sh = 0;
    eglQuerySurface(dpy, surface, EGL_WIDTH, &sw);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &sh);
    eglSwapInterval(dpy, 1);

    char err[512] = "";
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint bridge_prog = link_program(kFullscreenVS, kBridgeFS, err, sizeof err);
    GLuint blit_prog = bridge_prog ? link_program(kFullscreenVS, kBlitFS, err, sizeof err) : 0;
    if (!bridge_prog || !blit_prog) {
        snprintf(g->info, sizeof g->info, "GL program: %.200s", err);
        g->state = -1;
        return NULL;
    }
    snprintf(g->info, sizeof g->info, "%s, surface %dx%d", (const char *)glGetString(GL_VERSION), sw, sh);
    g->state = 1;

    GLuint buffer = 0, tex = 0, fbo = 0;
    int tex_valid = 0;
    while (!g->stop) {
        if (g->want_bridge && !g->bridge_state) {
            /* 4 MiB of slack: the Vulkan side imports from an aligned address inside. */
            const size_t bytes = (BRIDGE_BYTES + (4u << 20) + 0xffff) & ~(size_t)0xffff;
            const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            glBufferStorage(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, NULL, flags);
            g->map = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, flags);
            g->map_bytes = bytes;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, BRIDGE_W, BRIDGE_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            g->bridge_state = g->map && status == GL_FRAMEBUFFER_COMPLETE ? 1 : -1;
        }
        const int posted = g->posted;
        if (g->bridge_state == 1 && posted != g->drawn) {
            const double t0 = spike_now_ms();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, BRIDGE_W, BRIDGE_H);
            glDisable(GL_BLEND);
            glUseProgram(bridge_prog);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
            glUniform3i(glGetUniformLocation(bridge_prog, "geom"), BRIDGE_W, BRIDGE_H, (GLint)g->offset_words);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            if (g->verify) {
                glFinish();
                glReadPixels(BRIDGE_W / 2, BRIDGE_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g->centre);
                glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g->corner);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            g->pass_ms = spike_now_ms() - t0;
            if (!g->verify) {
                g->pass_ms_sum += g->pass_ms;
                g->pass_count++;
            }
            tex_valid = 1;
            g->drawn = posted;
        }
        const unsigned long n = g->frames;
        glViewport(0, 0, sw, sh);
        glClearColor(0.1f, 0.25f + 0.2f * sinf((float)n * 0.05f), 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (tex_valid) {
            /* The Vulkan frame, inset so the animated clear shows GL is alive. */
            glViewport(sw / 8, sh / 8, sw * 3 / 4, sh * 3 / 4);
            glUseProgram(blit_prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1i(glGetUniformLocation(blit_prog, "tex"), 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (glGetError() != GL_NO_ERROR)
            g->gl_errors++;
        if (eglSwapBuffers(dpy, surface))
            g->frames++;
        else
            g->swap_failures++;
    }
    return NULL;
}
