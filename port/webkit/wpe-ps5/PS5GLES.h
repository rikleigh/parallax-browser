/*
 * PS5GLES.h - what differs when the title renders on ANGLE (OpenGL ES 3.1 on
 * Vulkan, over RADV) instead of ps5-opengl's desktop GL 4.6 core context.
 *
 * GPU=radv builds (port/webkit/build.sh; HANDOVER-radv.md Phase 2) define
 * PS5WK_GPU_RADV. Everything here is a no-op otherwise, so the release
 * build's GL is unchanged.
 */
#pragma once

#include <epoxy/gl.h>
#include <string>
#include <string.h>

#if PS5WK_GPU_RADV
// The title's shaders are written as "#version 430 core" (std430 SSBOs,
// unpackUnorm4x8, gl_VertexID): all of it is in ES 3.1, which also wants
// default precisions.
static inline std::string ps5ShaderSource(const char* source)
{
    static const char desktop[] = "#version 430 core";
    if (strncmp(source, desktop, sizeof(desktop) - 1))
        return source;
    return std::string("#version 310 es\nprecision highp float;\nprecision highp int;\n") + (source + sizeof(desktop) - 1);
}
// ES takes BGRA pixels only into BGRA textures (EXT_texture_format_BGRA8888:
// the internal format is GL_BGRA_EXT too).
static constexpr GLenum kPS5BGRAInternalFormat = GL_BGRA_EXT;
#else
static inline std::string ps5ShaderSource(const char* source) { return source; }
static constexpr GLenum kPS5BGRAInternalFormat = GL_RGBA8;
#endif

// glShaderSource with ps5ShaderSource's text.
static inline void ps5ShaderSourceGL(GLuint shader, const char* source)
{
    std::string text = ps5ShaderSource(source);
    const char* p = text.c_str();
    glShaderSource(shader, 1, &p, nullptr);
}
