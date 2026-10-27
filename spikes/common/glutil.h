/* glutil.h - small GL helpers shared by the GL spikes (2, 3b).
 * Desktop GL 4.6 core through ps5-opengl, which exports every core entry
 * point as a symbol, so glcorearb.h prototypes link directly. */
#pragma once

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static inline GLuint glu_shader(GLenum type, const char *src, char *err, size_t err_size)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLsizei n = 0;
        glGetShaderInfoLog(s, (GLsizei)err_size, &n, err);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* Returns the linked program, or 0 with the reason in err. */
static inline GLuint glu_program(const char *vs, const char *fs, char *err, size_t err_size)
{
    err[0] = 0;
    GLuint v = glu_shader(GL_VERTEX_SHADER, vs, err, err_size);
    GLuint f = v ? glu_shader(GL_FRAGMENT_SHADER, fs, err, err_size) : 0;
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLsizei n = 0;
        glGetProgramInfoLog(p, (GLsizei)err_size, &n, err);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* Column-major 4x4 matrices. */
typedef struct { float m[16]; } mat4;

static inline mat4 mat4_mul(mat4 a, mat4 b)
{
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

static inline mat4 mat4_translate(float x, float y, float z)
{
    mat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}};
    return r;
}

static inline mat4 mat4_scale(float s)
{
    mat4 r = {{s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1}};
    return r;
}

/* Rotation from a unit quaternion (x, y, z, w). */
static inline mat4 mat4_quat(const float q[4])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    mat4 r = {{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
               2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
               2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
               0, 0, 0, 1}};
    return r;
}

/* Rigid inverse: transpose the rotation, negate the rotated translation. */
static inline mat4 mat4_view_from_pose(const float pos[3], const float quat[4])
{
    mat4 r = mat4_quat(quat), v;
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++)
            v.m[c * 4 + row] = r.m[row * 4 + c];
    v.m[3] = v.m[7] = v.m[11] = 0;
    for (int row = 0; row < 3; row++)
        v.m[12 + row] = -(v.m[row] * pos[0] + v.m[4 + row] * pos[1] + v.m[8 + row] * pos[2]);
    v.m[15] = 1;
    return v;
}

/* Asymmetric frustum from tangents (left, right, up, down; all positive),
 * the way OpenXR's XrFovf / Hmd2's FOV are given. flip_y renders upside
 * down, so a top-down readback or a top-left-origin consumer gets rows in
 * the order it expects. */
static inline mat4 mat4_frustum_tan(const float tan[4], float n, float f, int flip_y)
{
    const float l = -tan[0], r = tan[1], u = tan[2], d = -tan[3];
    mat4 m;
    memset(&m, 0, sizeof m);
    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (u - d) * (flip_y ? -1.0f : 1.0f);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (u + d) / (u - d) * (flip_y ? -1.0f : 1.0f);
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -2 * f * n / (f - n);
    return m;
}
