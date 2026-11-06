/*
 * Copyright (C) 2026 ps5webkit contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.
 */

// "View in VR": the playing <video> in the PS VR2 headset, on the render
// thread (PS5Renderer.h). The headset path is Spike 3b's and Spike 9's
// (spikes/common/psvr2): Hmd2 reprojection at 120 Hz with half-rate
// rendering (60 frames/s from us), the tracker's eye orientations for the
// predicted display time, three GL eye buffers (two eyes side by side)
// exported zero-copy as T#s. Each frame samples the newest converted video
// frame (the same RGBA texture the TV path draws, hardware- or
// software-decoded) along each eye pixel's view ray: equirectangular 360 or
// 180, mono or side-by-side/top-bottom stereo, equidistant fisheye, or a
// flat virtual screen. Video sits at infinity, so only orientation matters.
//
// The loop: page buffers are still taken (the UI main thread waits for
// them), video frames are consumed at the headset's frame times (older ones
// dropped and their decoder slots released, the shown one released once the
// GPU is done, exactly as on the TV), and the TV gets a notice, redrawn only
// when it changes (a TV swap is cheap when the previous flip is long done).
//
// /data/ps5webkit/vr-sim: no Hmd2 at all; eye buffers are rendered on a 60 Hz
// timer with a scripted head direction (setVRSimLook) and can be read back
// (the capture request): log-only tests of everything but the headset.

#include "config.h"
#include "PS5Renderer.h"

#include "PS5Sce.h"
#include "PS5Text.h"
#include "PS5VideoServer.h"
#include <algorithm>
#include <cmath>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <fcntl.h>
#include <psvr2/psvr2.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// spikes/common/psvr2 is linked into the title only; the payloads link these
// objects without it.
extern "C" {
void psvr2_set_logger(psvr2_log_fn) __attribute__((weak));
int psvr2_open(int user_id) __attribute__((weak));
void psvr2_eye_tangents(int eye, float overscan, float tan[4]) __attribute__((weak));
int psvr2_start(const uint32_t tsharp[][8], int count, uint32_t eye_w, uint32_t eye_h) __attribute__((weak));
int psvr2_begin_frame(psvr2_frame_t*) __attribute__((weak));
int psvr2_submit(int index, const psvr2_frame_t*, const float tan[2][4]) __attribute__((weak));
void psvr2_close(void) __attribute__((weak));
int psvr2_worn(void) __attribute__((weak));
void psvr2_require_headset(int on) __attribute__((weak));
int psvr2_device_status(uint32_t* status, int* worn, int* lens_mm) __attribute__((weak));
}

void ps5RendererUploadPanel(unsigned& texture, const PS5TextImage&);

#define VRLOG(...) g_message("[vr] " __VA_ARGS__)

static constexpr int kEyeSize = 2048;     // Spike 9: 1536 looked the same, 2048 x4 removes aliasing
static constexpr int kSamples = 4;        // rotated-grid supersampling per eye pixel
static constexpr float kOverscan = 1.15f; // reprojection margin (Spike 3b/9)
static constexpr int64_t kFrameUS = 16667; // 60 frames/s rendered, 120 Hz out
static constexpr float kFlatHalfWidth = 0.70f; // flat screen: tan(35 deg), a 70 degree wide screen
static constexpr float kStillRadians = 0.07f;  // entry recentre: the head within 4 degrees...
static constexpr unsigned kStillFrames = 30;   // ...for 0.5 s
static constexpr int64_t kEntryRecenterUS = 5000000; // given up after 5 s
static constexpr int64_t kStalePoseUS = 2000000; // the stale first poses: live after leaving them or 2 s
static constexpr float kJumpRadians = 0.26f;       // tracking jump: the yaw moving over 15 degrees...
static constexpr float kJumpRadiansPerSecond = 26; // ...at over 1500 degrees/s (no head turns that fast)
static constexpr int64_t kNoTrackingUS = 15000000; // no tracked pose at all this long after entering: back to the TV
static constexpr int64_t kStatusPollUS = 1000000;  // Hmd2's device status (connected, worn) while in VR
static constexpr unsigned kDisconnectedPolls = 2;  // status not 0 this many polls in a row: back to the TV

struct PS5VRState {
    bool sim { false };
    bool simUntracked { false }; // /data/ps5webkit/vr-sim-untracked: simulated frames without a pose
    // the headset's device status, polled (kStatusPollUS)
    int64_t statusPollUS { 0 };
    uint32_t devStatus { 0 };
    int devWorn { -1 };
    unsigned disconnectedPolls { 0 };
    int eye { kEyeSize };
    GLuint eyeTexture[PSVR2_MAX_BUFFERS] { };
    GLuint eyeFBO[PSVR2_MAX_BUFFERS] { };
    float tan[2][4] { };
    int projection { 0 };
    float fisheyeHalf { static_cast<float>(M_PI / 2) };
    bool recenter { false };
    float yaw0 { 0 };
    // entry recentre
    bool entryRecenter { true };
    bool haveFirstPose { false }, poseLive { false };
    float firstYaw { 0 };
    int64_t liveUS { 0 };
    float stillYaw { 0 };
    unsigned stillFrames { 0 };
    // tracking jumps (the front follows them)
    bool havePrevYaw { false };
    float prevYaw { 0 };
    int64_t prevYawUS { 0 };
    unsigned yawStepLogs { 0 };
    int sourceStream { -1 };
    int sourceWidth { 0 }, sourceHeight { 0 };
    PS5Renderer::VREnded ended { nullptr };
    void* userData { nullptr };
    // sim
    int64_t simNextUS { 0 };
    float simYaw { 0 }, simPitch { 0 };
    // text
    GLuint hintTexture { 0 };
    int hintWidth { 0 }, hintHeight { 0 };
    bool hintIsPanel { false }; // the application's panel (setVRPanel), not text
    int64_t hintUntilUS { 0 };
    GLuint tvTexture { 0 };
    int tvWidth { 0 }, tvHeight { 0 };
    bool tvDirty { true };
    // stats
    int64_t startUS { 0 }, windowUS { 0 };
    uint64_t frames { 0 }, beginErrors { 0 }, submitErrors { 0 }, tracked { 0 };
    unsigned consecutiveErrors { 0 };
    unsigned windowFrames { 0 }, windowTracked { 0 }, windowVideo { 0 };
    double windowRenderMS { 0 }, windowRenderMaxMS { 0 }, windowWaitMS { 0 };
    unsigned tvSwaps { 0 };
    double tvSwapMaxMS { 0 };
};

// The shaders (GL 4.3 core, as the rest of the renderer).
static const char* kVRVertexShader = R"(#version 430 core
out vec2 uv;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p; // uv.y = 0 is memory row 0: the top of the eye image (what Hmd2 expects)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// proj: 0 flat, 1 360, 2 360 SBS, 3 360 TB, 4 180, 5 180 SBS, 6 180 TB,
// 7 fisheye SBS (equidistant). Stereo: left eye = left / top half.
static const char* kVRFragmentShader = R"(#version 430 core
in vec2 uv;
uniform sampler2D video;  // RGBA, row 0 = the picture's top row
uniform sampler2D hint;   // premultiplied, row 0 = top
uniform int haveVideo;
uniform vec4 tan_lrud;    // this eye's tangents: left, right, up, down
uniform mat3 rot;         // eye orientation, recentred
uniform int proj;
uniform int eye;
uniform vec2 px;          // one eye pixel in uv
uniform int samples;
uniform float fisheyeHalf;
uniform vec2 flatHalf;
uniform vec4 hintRect;    // tangent space x0 y0 x1 y1 (y up); x0 >= x1: no hint
out vec4 o;
const float PI = 3.14159265;
const vec4 black = vec4(0.0, 0.0, 0.0, 1.0);
const vec4 room = vec4(0.035, 0.037, 0.045, 1.0);
vec4 ray(vec2 p)
{
    vec3 d = rot * normalize(vec3(mix(-tan_lrud.x, tan_lrud.y, p.x), mix(tan_lrud.z, -tan_lrud.w, p.y), -1.0));
    vec2 tc;
    if (proj == 0) {
        if (d.z > -1e-4)
            return room;
        vec2 q = (d.xy / -d.z) / flatHalf;
        if (abs(q.x) > 1.0 || abs(q.y) > 1.0)
            return room;
        tc = vec2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    } else if (proj == 7) {
        float th = acos(clamp(-d.z, -1.0, 1.0));
        if (th > fisheyeHalf)
            return black;
        float l = length(d.xy);
        vec2 dir = l > 1e-6 ? d.xy / l : vec2(0.0);
        float r = 0.5 * th / fisheyeHalf;
        tc = vec2(0.5 + r * dir.x, 0.5 - r * dir.y);
    } else {
        float lon = atan(d.x, -d.z), lat = asin(clamp(d.y, -1.0, 1.0));
        if (proj <= 3)
            tc = vec2(lon / (2.0 * PI) + 0.5, 0.5 - lat / PI);
        else {
            if (abs(lon) > 0.5 * PI)
                return black;
            tc = vec2(lon / PI + 0.5, 0.5 - lat / PI);
        }
    }
    if (proj == 2 || proj == 5 || proj == 7)
        tc.x = tc.x * 0.5 + 0.5 * float(eye);
    else if (proj == 3 || proj == 6)
        tc.y = tc.y * 0.5 + 0.5 * float(eye);
    return haveVideo != 0 ? vec4(textureLod(video, tc, 0.0).rgb, 1.0) : black;
}
void main()
{
    if (samples < 4)
        o = ray(uv);
    else {
        // rotated grid: the video has more pixels per degree than the eye
        // buffer, so one tap per pixel shimmers (Spike 9)
        o = 0.25 * (ray(uv + px * vec2(-0.125, -0.375)) + ray(uv + px * vec2(0.375, -0.125))
            + ray(uv + px * vec2(0.125, 0.375)) + ray(uv + px * vec2(-0.375, 0.125)));
    }
    if (hintRect.x < hintRect.z) {
        // Head-locked hint, at the same tangents in both eyes (no disparity).
        vec2 t = vec2(mix(-tan_lrud.x, tan_lrud.y, uv.x), mix(tan_lrud.z, -tan_lrud.w, uv.y));
        vec2 h = vec2((t.x - hintRect.x) / (hintRect.z - hintRect.x), (hintRect.w - t.y) / (hintRect.w - hintRect.y));
        if (h.x >= 0.0 && h.x <= 1.0 && h.y >= 0.0 && h.y <= 1.0) {
            vec4 c = texture(hint, h);
            o = vec4(c.rgb + o.rgb * (1.0 - c.a), 1.0);
        }
    }
}
)";

static GLuint s_program;
static GLint s_uVideo, s_uHint, s_uHaveVideo, s_uTan, s_uRot, s_uProj, s_uEye, s_uPx, s_uSamples, s_uFisheyeHalf, s_uFlatHalf, s_uHintRect;

static GLuint compileShader(GLenum type, const char* source, char* log, size_t logSize)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(shader, static_cast<GLsizei>(logSize), nullptr, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool PS5Renderer::vrCreatePrograms()
{
    if (s_program)
        return true;
    char log[512] = { };
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVRVertexShader, log, sizeof(log));
    GLuint fs = vs ? compileShader(GL_FRAGMENT_SHADER, kVRFragmentShader, log, sizeof(log)) : 0;
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        VRLOG("eye shader failed: %s", log);
        return false;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        VRLOG("eye shader link failed: %s", log);
        glDeleteProgram(program);
        return false;
    }
    s_program = program;
    s_uVideo = glGetUniformLocation(program, "video");
    s_uHint = glGetUniformLocation(program, "hint");
    s_uHaveVideo = glGetUniformLocation(program, "haveVideo");
    s_uTan = glGetUniformLocation(program, "tan_lrud");
    s_uRot = glGetUniformLocation(program, "rot");
    s_uProj = glGetUniformLocation(program, "proj");
    s_uEye = glGetUniformLocation(program, "eye");
    s_uPx = glGetUniformLocation(program, "px");
    s_uSamples = glGetUniformLocation(program, "samples");
    s_uFisheyeHalf = glGetUniformLocation(program, "fisheyeHalf");
    s_uFlatHalf = glGetUniformLocation(program, "flatHalf");
    s_uHintRect = glGetUniformLocation(program, "hintRect");
    return true;
}

static int64_t nowUS()
{
    return g_get_monotonic_time();
}

static bool fileExists(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static const char* projectionName(int projection)
{
    static const char* const names[] = { "flat", "360", "360 SBS", "360 TB", "180", "180 SBS", "180 TB", "fisheye SBS" };
    return projection >= 0 && projection < PS5Renderer::VRProjectionCount ? names[projection] : "?";
}

// ---------------------------------------------------------------- quaternions
// Column-major 3x3 from a unit quaternion (x, y, z, w), as spikes/common/glutil.h.
static void quatToMat3(const float q[4], float m[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r[9] = {
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
        2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
        2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y),
    };
    memcpy(m, r, sizeof(r));
}

static void mat3Mul(const float a[9], const float b[9], float out[9])
{
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) {
            float s = 0;
            for (int k = 0; k < 3; k++)
                s += a[k * 3 + row] * b[c * 3 + k];
            out[c * 3 + row] = s;
        }
}

static void quatMul(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// The head's yaw (about +Y; positive = turned left), as Spike 9 recentres.
static float yawOf(const float q[4])
{
    return atan2f(2 * (q[3] * q[1] + q[0] * q[2]), 1 - 2 * (q[1] * q[1] + q[0] * q[0]));
}

// ---------------------------------------------------------------- main thread API

struct VRDoneCall {
    PS5Renderer::VRDone done;
    PS5Renderer::VREnded ended;
    void* userData;
    bool ok;
    std::string message;
};

static void postToMain(VRDoneCall* call)
{
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto* call = static_cast<VRDoneCall*>(data);
        if (call->done)
            call->done(call->ok, call->message.c_str(), call->userData);
        else if (call->ended)
            call->ended(call->message.c_str(), call->userData);
        return G_SOURCE_REMOVE;
    }, call, [](gpointer data) { delete static_cast<VRDoneCall*>(data); });
}

void PS5Renderer::startVR(int projection, float fisheyeDegrees, VRDone done, VREnded ended, void* userData)
{
    if (!m_initialized) {
        if (done)
            done(false, "No display.", userData);
        return;
    }
    g_mutex_lock(&m_lock);
    m_vrRequest.start = true;
    m_vrRequest.stop = false;
    m_vrRequest.projection = projection;
    m_vrRequest.fisheyeDegrees = fisheyeDegrees;
    m_vrRequest.done = done;
    m_vrRequest.ended = ended;
    m_vrRequest.userData = userData;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::stopVR(bool wait)
{
    if (!m_initialized)
        return;
    g_mutex_lock(&m_lock);
    m_vrRequest.stop = true;
    m_vrRequest.stopWhy.clear();
    g_cond_broadcast(&m_cond);
    if (wait) {
        int64_t deadline = nowUS() + 3000000;
        while ((m_vrActiveShared || m_vrRequest.stop || m_vrRequest.start) && nowUS() < deadline)
            g_cond_wait_until(&m_cond, &m_lock, deadline);
    }
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::endVR(const char* why)
{
    if (!m_initialized)
        return;
    g_mutex_lock(&m_lock);
    if (m_vrActiveShared) {
        m_vrRequest.stop = true;
        m_vrRequest.stopWhy = why && *why ? why : "back to the TV";
        g_cond_broadcast(&m_cond);
    }
    g_mutex_unlock(&m_lock);
}

bool PS5Renderer::vrActive()
{
    g_mutex_lock(&m_lock);
    bool active = m_vrActiveShared;
    g_mutex_unlock(&m_lock);
    return active;
}

bool PS5Renderer::vrTracking()
{
    g_mutex_lock(&m_lock);
    bool tracking = m_vrActiveShared && nowUS() - m_vrTrackedUS < 1000000;
    g_mutex_unlock(&m_lock);
    return tracking;
}

void PS5Renderer::setVRProjection(int projection, float fisheyeDegrees)
{
    g_mutex_lock(&m_lock);
    m_vrRequest.setProjection = true;
    m_vrRequest.projection = projection;
    m_vrRequest.fisheyeDegrees = fisheyeDegrees;
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::recenterVR()
{
    g_mutex_lock(&m_lock);
    m_vrRequest.recenter = true;
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::setVRText(const char* hint, double seconds, const char* tvText)
{
    g_mutex_lock(&m_lock);
    m_vrRequest.textChanged = true;
    m_vrRequest.panel = false;
    m_vrRequest.hint = hint ? hint : "";
    m_vrRequest.hintSeconds = seconds;
    if (tvText) {
        m_vrRequest.tvTextChanged = true;
        m_vrRequest.tvPanel = false;
        m_vrRequest.tvText = tvText;
    }
    g_mutex_unlock(&m_lock);
}

// 0xAARRGGBB words (bytes B G R A) to RGBA bytes, both premultiplied.
static void canvasToRGBA(const uint32_t* px, int width, int height, std::vector<uint8_t>& out)
{
    out.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; i++) {
        const uint32_t p = px[i];
        out[i * 4] = (p >> 16) & 0xff;
        out[i * 4 + 1] = (p >> 8) & 0xff;
        out[i * 4 + 2] = p & 0xff;
        out[i * 4 + 3] = p >> 24;
    }
}

void PS5Renderer::setVRPanel(const uint32_t* panel, int width, int height, double seconds, const uint32_t* tv, int tvWidth, int tvHeight)
{
    std::vector<uint8_t> panelRGBA, tvRGBA;
    const bool havePanel = panel && width > 0 && height > 0 && seconds > 0;
    if (havePanel)
        canvasToRGBA(panel, width, height, panelRGBA);
    if (tv && tvWidth > 0 && tvHeight > 0)
        canvasToRGBA(tv, tvWidth, tvHeight, tvRGBA);
    g_mutex_lock(&m_lock);
    m_vrRequest.textChanged = true;
    m_vrRequest.panel = true;
    m_vrRequest.panelRGBA.swap(panelRGBA);
    m_vrRequest.panelWidth = havePanel ? width : 0;
    m_vrRequest.panelHeight = havePanel ? height : 0;
    m_vrRequest.hintSeconds = havePanel ? seconds : 0;
    if (!tvRGBA.empty()) {
        m_vrRequest.tvTextChanged = true;
        m_vrRequest.tvPanel = true;
        m_vrRequest.tvRGBA.swap(tvRGBA);
        m_vrRequest.tvPanelWidth = tvWidth;
        m_vrRequest.tvPanelHeight = tvHeight;
    }
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::setVRSimLook(float yawDegrees, float pitchDegrees)
{
    g_mutex_lock(&m_lock);
    m_vrRequest.simLook = true;
    m_vrRequest.simYaw = yawDegrees;
    m_vrRequest.simPitch = pitchDegrees;
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::requestVideoThumbnails(unsigned count, ThumbnailsDone done, void* userData)
{
    if (!m_initialized) {
        if (done)
            done(nullptr, 0, 0, 0, userData);
        return;
    }
    g_mutex_lock(&m_lock);
    m_thumbnailRequest = { true, std::clamp(count, 1u, 8u), done, userData };
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

// ---------------------------------------------------------------- render thread

void PS5Renderer::vrHandleRequests()
{
    g_mutex_lock(&m_lock);
    VRRequest request = m_vrRequest;
    m_vrRequest.start = m_vrRequest.stop = m_vrRequest.setProjection = m_vrRequest.recenter = false;
    m_vrRequest.textChanged = m_vrRequest.tvTextChanged = m_vrRequest.simLook = false;
    m_vrRequest.stopWhy.clear();
    g_mutex_unlock(&m_lock);

    if (request.stop && m_vr)
        vrStop(request.stopWhy.empty() ? nullptr : request.stopWhy.c_str()); // endVR(): ended(why) follows
    if (request.start && request.stop) {
        // Stopped before it started (another page, wind-down).
        request.start = false;
        postToMain(new VRDoneCall { request.done, nullptr, request.userData, false, "" });
    }
    if (request.start) {
        char message[256] = { };
        bool ok = m_vr || vrStart(request.projection, request.fisheyeDegrees, message, sizeof(message));
        if (m_vr) {
            m_vr->ended = request.ended;
            m_vr->userData = request.userData;
        }
        g_mutex_lock(&m_lock);
        m_vrActiveShared = m_vr;
        g_cond_broadcast(&m_cond);
        g_mutex_unlock(&m_lock);
        postToMain(new VRDoneCall { request.done, nullptr, request.userData, ok, message });
    } else if (request.stop) {
        g_mutex_lock(&m_lock);
        g_cond_broadcast(&m_cond); // stopVR(wait)
        g_mutex_unlock(&m_lock);
    }
    if (!m_vr)
        return;
    if (request.setProjection) {
        m_vr->projection = std::clamp(request.projection, 0, static_cast<int>(VRProjectionCount) - 1);
        m_vr->fisheyeHalf = std::clamp(request.fisheyeDegrees, 120.0f, 240.0f) * static_cast<float>(M_PI) / 360.0f;
        VRLOG("projection %s%s", projectionName(m_vr->projection), m_vr->projection == VRFisheyeSBS ? " (fisheye)" : "");
    }
    if (request.recenter)
        m_vr->recenter = true;
    if (request.simLook) {
        m_vr->simYaw = request.simYaw;
        m_vr->simPitch = request.simPitch;
    }
    if (request.textChanged || request.tvTextChanged)
        vrUpdateText(nowUS());
}

// The hint and TV notice textures from the texts (or the panels) the main
// thread set.
void PS5Renderer::vrUpdateText(int64_t now)
{
    PS5TextImage image, tvImage;
    g_mutex_lock(&m_lock);
    std::string hint = m_vrRequest.hint, tvText = m_vrRequest.tvText;
    double seconds = m_vrRequest.hintSeconds;
    const bool panel = m_vrRequest.panel, tvPanel = m_vrRequest.tvPanel;
    if (panel) {
        image.rgba = m_vrRequest.panelRGBA;
        image.width = m_vrRequest.panelWidth;
        image.height = m_vrRequest.panelHeight;
    }
    if (tvPanel) {
        tvImage.rgba = m_vrRequest.tvRGBA;
        tvImage.width = m_vrRequest.tvPanelWidth;
        tvImage.height = m_vrRequest.tvPanelHeight;
    }
    g_mutex_unlock(&m_lock);
    if (seconds > 0 && (panel ? image.width > 0 : !hint.empty() && ps5RenderTextPanel(hint.c_str(), 64, image))) {
        ps5RendererUploadPanel(m_vr->hintTexture, image);
        // Shown at about half size in the eye buffer: mipmaps against shimmer.
        glBindTexture(GL_TEXTURE_2D, m_vr->hintTexture);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        m_vr->hintWidth = image.width;
        m_vr->hintHeight = image.height;
        m_vr->hintIsPanel = panel;
        m_vr->hintUntilUS = now + static_cast<int64_t>(seconds * 1e6);
    } else
        m_vr->hintUntilUS = 0;
    if (tvPanel ? tvImage.width > 0 : !tvText.empty() && ps5RenderTextPanel(tvText.c_str(), 52, tvImage)) {
        ps5RendererUploadPanel(m_vr->tvTexture, tvImage);
        m_vr->tvWidth = tvImage.width;
        m_vr->tvHeight = tvImage.height;
    }
    m_vr->tvDirty = true;
}

static void vrLog(const char* line)
{
    g_message("[vr] %s", line);
}

bool PS5Renderer::vrStart(int projection, float fisheyeDegrees, char* message, size_t messageSize)
{
    int64_t start = nowUS();
    if (!m_convertProgram || !vrCreatePrograms()) {
        snprintf(message, messageSize, "View in VR is not available (no video shader).");
        return false;
    }
    if (!m_videoSourceCount) {
        snprintf(message, messageSize, "No video is playing on this page.");
        return false;
    }
    auto* vr = new PS5VRState;
    vr->sim = fileExists("/data/ps5webkit/vr-sim");
    vr->simUntracked = vr->sim && fileExists("/data/ps5webkit/vr-sim-untracked");
    vr->projection = std::clamp(projection, 0, static_cast<int>(VRProjectionCount) - 1);
    vr->fisheyeHalf = std::clamp(fisheyeDegrees, 120.0f, 240.0f) * static_cast<float>(M_PI) / 360.0f;

    if (!vr->sim) {
        if (!psvr2_open || !psvr2_start) {
            snprintf(message, messageSize, "View in VR needs the browser title (no headset support in this build).");
            delete vr;
            return false;
        }
        psvr2_set_logger(vrLog);
        // No headset, no VR mode: Hmd2, the tracker, the reprojection and VR
        // mode all open without one (device status 2), and the title then
        // sat in VR mode with the pad gone (2026-11-02, a touchpad pressed by
        // accident with the PS VR2 unplugged).
        if (psvr2_require_headset)
            psvr2_require_headset(1);
        int32_t user = -1;
        if (sceUserServiceGetInitialUser)
            sceUserServiceGetInitialUser(&user);
        int rc = psvr2_open(user);
        if (rc != 0) {
            if (rc == PSVR2_NO_HEADSET) {
                uint32_t status = 0;
                int worn = 0, lens = 0;
                if (psvr2_device_status)
                    psvr2_device_status(&status, &worn, &lens);
                VRLOG("PS VR2 not connected (device status %u, worn %d, lens %d mm): staying on the TV", status, worn, lens);
                snprintf(message, messageSize, "PS VR2 not connected. Connect the headset, turn it on and press the touchpad again.");
            } else
                snprintf(message, messageSize, "The PS VR2 headset could not be opened. Connect it, turn it on and try again.");
            delete vr;
            return false;
        }
    }

    // Three side-by-side eye buffers, exported as T#s (ps5-opengl lays
    // immutable RGBA8 textures out as SW_64KB_R_X, what Hmd2 takes).
    typedef EGLBoolean (*ExportFunction)(EGLDisplay, EGLContext, GLuint, uint32_t[8], uint64_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
    auto exportDescriptor = reinterpret_cast<ExportFunction>(eglGetProcAddress("eglExportTextureDescriptorPS5"));
    uint32_t tsharp[PSVR2_MAX_BUFFERS][8] = { };
    bool ok = exportDescriptor;
    glGenTextures(PSVR2_MAX_BUFFERS, vr->eyeTexture);
    glGenFramebuffers(PSVR2_MAX_BUFFERS, vr->eyeFBO);
    for (int b = 0; ok && b < PSVR2_MAX_BUFFERS; b++) {
        glBindTexture(GL_TEXTURE_2D, vr->eyeTexture[b]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2 * vr->eye, vr->eye);
        glBindFramebuffer(GL_FRAMEBUFFER, vr->eyeFBO[b]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vr->eyeTexture[b], 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        uint64_t address = 0;
        uint32_t width = 0, height = 0, pitch = 0, swizzle = 0;
        if (ok)
            ok = exportDescriptor(m_eglDisplay, m_eglContext, vr->eyeTexture[b], tsharp[b], &address, &width, &height, &pitch, &swizzle);
        if (ok && !b)
            VRLOG("eye buffers %dx%d, T# export: address 0x%llx %ux%u pitch %u sw_mode %u", 2 * vr->eye, vr->eye,
                static_cast<unsigned long long>(address), width, height, pitch, swizzle);
        if (ok) {
            // Start black, so nothing stale is ever shown.
            glViewport(0, 0, 2 * vr->eye, vr->eye);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    if (!ok) {
        VRLOG("eye buffer setup failed (export %s, GL 0x%x, EGL 0x%04x)", exportDescriptor ? "found" : "missing", glGetError(), eglGetError());
        snprintf(message, messageSize, "The headset could not be set up (eye buffers).");
        if (!vr->sim)
            psvr2_close();
        glDeleteFramebuffers(PSVR2_MAX_BUFFERS, vr->eyeFBO);
        glDeleteTextures(PSVR2_MAX_BUFFERS, vr->eyeTexture);
        delete vr;
        return false;
    }

    if (!vr->sim) {
        if (psvr2_start(tsharp, PSVR2_MAX_BUFFERS, vr->eye, vr->eye) != 0) {
            snprintf(message, messageSize, "The PS VR2 headset could not start VR mode. Is it connected and turned on?");
            psvr2_close();
            glDeleteFramebuffers(PSVR2_MAX_BUFFERS, vr->eyeFBO);
            glDeleteTextures(PSVR2_MAX_BUFFERS, vr->eyeTexture);
            delete vr;
            return false;
        }
        psvr2_eye_tangents(0, kOverscan, vr->tan[0]);
        psvr2_eye_tangents(1, kOverscan, vr->tan[1]);
    } else {
        // The PS VR2's own field of view (Spike 9's log): out 1.8418, in
        // 0.9472, up/down 1.3290.
        const float out = 1.8418f * kOverscan, in = 0.9472f * kOverscan, ud = 1.3290f * kOverscan;
        const float left[4] = { out, in, ud, ud }, right[4] = { in, out, ud, ud };
        memcpy(vr->tan[0], left, sizeof(left));
        memcpy(vr->tan[1], right, sizeof(right));
        vr->simNextUS = nowUS();
    }

    vr->startUS = vr->windowUS = nowUS();
    m_vr = vr;
    vrUpdateText(vr->startUS);
    VRLOG("headset %s in %.0f ms: %s, eye buffers %dx%d x%d, %d samples per pixel; %u video stream(s)", vr->sim ? "SIMULATED" : "up",
        (nowUS() - start) / 1000.0, projectionName(vr->projection), 2 * vr->eye, vr->eye, PSVR2_MAX_BUFFERS, kSamples, m_videoSourceCount);
    return true;
}

void PS5Renderer::vrStop(const char* why)
{
    PS5VRState* vr = m_vr;
    if (!vr)
        return;
    int64_t start = nowUS();
    if (!vr->sim)
        psvr2_close(); // leaves VR mode first, so the compositor stops reading the eye buffers
    glFinish();
    glDeleteFramebuffers(PSVR2_MAX_BUFFERS, vr->eyeFBO);
    glDeleteTextures(PSVR2_MAX_BUFFERS, vr->eyeTexture);
    if (vr->hintTexture)
        glDeleteTextures(1, &vr->hintTexture);
    if (vr->tvTexture)
        glDeleteTextures(1, &vr->tvTexture);
    double seconds = (nowUS() - vr->startUS) / 1e6;
    VRLOG("headset %s after %.1f s%s%s: %llu frames (%.1f/s), %llu tracked, %llu begin errors, %llu submit errors, %u TV redraws (swap max %.1f ms); released in %.0f ms",
        vr->sim ? "simulation ended" : "released", seconds, why ? " - " : "", why ? why : "", static_cast<unsigned long long>(vr->frames),
        seconds > 0 ? vr->frames / seconds : 0.0, static_cast<unsigned long long>(vr->tracked), static_cast<unsigned long long>(vr->beginErrors),
        static_cast<unsigned long long>(vr->submitErrors), vr->tvSwaps, vr->tvSwapMaxMS, (nowUS() - start) / 1000.0);
    PS5Renderer::VREnded ended = vr->ended;
    void* userData = vr->userData;
    delete vr;
    m_vr = nullptr;
    g_mutex_lock(&m_lock);
    m_vrActiveShared = false;
    m_frameRequested = true; // the page back on the TV
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
    m_lastSwapEnd = 0;
    m_vsyncGrid = 0;
    if (why && ended)
        postToMain(new VRDoneCall { nullptr, ended, userData, false, why });
}

// The TV while the video plays in the headset: the notice on black.
void PS5Renderer::vrDrawTV(int64_t now)
{
    PS5VRState* vr = m_vr;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (vr->tvTexture) {
        double scale = std::min(1.0, std::min(m_width * 0.9 / vr->tvWidth, m_height * 0.8 / vr->tvHeight));
        drawPanel(vr->tvTexture, vr->tvWidth, vr->tvHeight, m_width / 2.0, m_height / 2.0, scale);
    }
    int64_t swapStart = nowUS();
    eglSwapBuffers(m_eglDisplay, m_eglSurface);
    double ms = (nowUS() - swapStart) / 1000.0;
    vr->tvSwaps++;
    vr->tvSwapMaxMS = std::max(vr->tvSwapMaxMS, ms);
    vr->tvDirty = false;
    (void)now;
}

void PS5Renderer::vrRenderEyes(unsigned buffer, const void* framePointer)
{
    PS5VRState* vr = m_vr;
    const auto& frame = *static_cast<const psvr2_frame_t*>(framePointer);
    const VideoSource* source = nullptr;
    for (unsigned i = 0; i < m_videoSourceCount; i++) {
        if (m_videoSources[i].stream == vr->sourceStream)
            source = &m_videoSources[i];
    }
    if (!source) {
        // The newest stream that showed a frame (the playing video).
        for (unsigned i = 0; i < m_videoSourceCount; i++) {
            if (!source || m_videoSources[i].lastFrameUS > source->lastFrameUS)
                source = &m_videoSources[i];
        }
        if (source) {
            VRLOG("showing video stream %d (%dx%d)", source->stream, source->width, source->height);
            vr->sourceStream = source->stream;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, vr->eyeFBO[buffer]);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(s_program);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, vr->hintTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source ? source->texture : 0);
    glUniform1i(s_uVideo, 0);
    glUniform1i(s_uHint, 1);
    glUniform1i(s_uHaveVideo, source ? 1 : 0);
    glUniform1i(s_uProj, vr->projection);
    glUniform1i(s_uSamples, kSamples);
    glUniform2f(s_uPx, 1.0f / vr->eye, 1.0f / vr->eye);
    glUniform1f(s_uFisheyeHalf, vr->fisheyeHalf);
    float aspect = source && source->width > 0 ? static_cast<float>(source->height) / source->width : 9.0f / 16;
    glUniform2f(s_uFlatHalf, kFlatHalfWidth, kFlatHalfWidth * aspect);
    if (vr->hintTexture && vr->hintUntilUS && nowUS() < vr->hintUntilUS) {
        // Text about 2.5 degrees tall (the panel's 40 px lines), at most 60
        // degrees wide, centred about 22 degrees below the view's centre.
        // The application's panel: 1240 px 0.95 tangent wide (about 50
        // degrees), its 30 px hint text about 1.3 degrees tall.
        const float w = vr->hintIsPanel ? std::min(vr->hintWidth * (0.95f / 1240), 1.15f) : std::clamp(vr->hintWidth * 0.0016f, 0.5f, 1.15f);
        const float h = w * vr->hintHeight / std::max(1, vr->hintWidth), cy = vr->hintIsPanel ? -0.42f : -0.40f;
        glUniform4f(s_uHintRect, -w / 2, cy - h / 2, w / 2, cy + h / 2);
    } else
        glUniform4f(s_uHintRect, 0, 0, 0, 0);

    // The video's front starts at the tracking space's forward (-z), where
    // the system centres its own VR views, and moves to where the head
    // faces once tracking is live and the head keeps still (within 4
    // degrees for 0.5 s): a viewer facing elsewhere started at the edge of
    // a 180 video. The tracker's first poses after it registers are a stale
    // default (yaw about -91 degrees here, jittering by a degree or so for
    // over 0.6 s; PS5VR saw "always 90.5" in its convention), which put
    // LEFT 90 in front in the first headset test, so poses only count once
    // the yaw has left the first pose's by more than the still radius, or
    // after 2 s (a viewer who happens to face that way). Given up after 5 s;
    // Square cancels it. Square recentres on the live pose (yaw only,
    // so the horizon stays level).
    // The tracking space itself can turn under the head after entry: on
    // 2026-11-04, re-entering VR recentred at entry on a live, still pose
    // (yaw -107 degrees), and half a second later the same head read about
    // 0 (a turn of 100 degrees no head makes that fast), leaving the video's
    // front off to the side. Once the entry recentre is over, a jump like
    // that moves the front with it, so the video stays where the head left it.
    if (frame.tracked && !vr->sim) {
        const float y = yawOf(frame.head.quat);
        const int64_t t = nowUS();
        if (vr->havePrevYaw) {
            const float step = remainderf(y - vr->prevYaw, static_cast<float>(2 * M_PI));
            const float seconds = std::max<int64_t>(t - vr->prevYawUS, kFrameUS / 2) / 1e6f;
            const bool jump = fabsf(step) > kJumpRadians && fabsf(step) / seconds > kJumpRadiansPerSecond;
            if (fabsf(step) > 0.09f && vr->yawStepLogs < 12 && t - vr->startUS < 10000000) {
                vr->yawStepLogs++;
                VRLOG("head yaw stepped %.1f degrees in %.0f ms at %.2f s (%.1f -> %.1f)%s", step * 180 / M_PI, seconds * 1000,
                    (t - vr->startUS) / 1e6, vr->prevYaw * 180 / M_PI, y * 180 / M_PI,
                    jump && !vr->entryRecenter ? ": tracking jump, the front moves with it" : "");
            }
            if (jump && !vr->entryRecenter)
                vr->yaw0 = remainderf(vr->yaw0 + step, static_cast<float>(2 * M_PI));
        }
        vr->havePrevYaw = true;
        vr->prevYaw = y;
        vr->prevYawUS = t;
    }
    if (frame.tracked && vr->recenter) {
        vr->yaw0 = yawOf(frame.head.quat);
        vr->recenter = false;
        vr->entryRecenter = false;
        VRLOG("recentred (head yaw %.1f degrees, positive = left)", vr->yaw0 * 180 / M_PI);
    }
    if (vr->entryRecenter && !vr->sim && frame.tracked) {
        const float y = yawOf(frame.head.quat);
        if (!vr->haveFirstPose) {
            vr->firstYaw = y;
            vr->haveFirstPose = true;
        } else if (!vr->poseLive && (fabsf(remainderf(y - vr->firstYaw, static_cast<float>(2 * M_PI))) > kStillRadians
            || nowUS() - vr->startUS > kStalePoseUS)) {
            vr->poseLive = true;
            vr->liveUS = nowUS();
        }
        if (vr->poseLive) {
            if (!vr->stillFrames || fabsf(remainderf(y - vr->stillYaw, static_cast<float>(2 * M_PI))) > kStillRadians) {
                vr->stillYaw = y;
                vr->stillFrames = 1;
            } else if (++vr->stillFrames >= kStillFrames) {
                vr->yaw0 = y;
                vr->entryRecenter = false;
                VRLOG("recentred at entry after %.2f s (head yaw %.1f degrees, positive = left; poses live after %.2f s, first yaw %.1f)",
                    (nowUS() - vr->startUS) / 1e6, y * 180 / M_PI, (vr->liveUS - vr->startUS) / 1e6, vr->firstYaw * 180 / M_PI);
            }
        }
    }
    if (vr->entryRecenter && nowUS() - vr->startUS > kEntryRecenterUS) {
        vr->entryRecenter = false;
        VRLOG("not recentred at entry: %s within %.0f s", vr->poseLive ? "the head did not keep still" : "no live head pose",
            kEntryRecenterUS / 1e6);
    }
    if (frame.tracked && !vr->sim && (vr->frames == 2 || vr->frames == 20 || vr->frames == 40 || vr->frames == 60 || vr->frames == 90 || vr->frames == 300))
        VRLOG("head yaw %.1f degrees at headset frame %llu (positive = left; the front is at yaw %.1f)", yawOf(frame.head.quat) * 180 / M_PI,
            static_cast<unsigned long long>(vr->frames), vr->yaw0 * 180 / M_PI);
    const float yawQuat[4] = { 0, sinf(-vr->yaw0 / 2), 0, cosf(-vr->yaw0 / 2) };
    float yaw[9];
    quatToMat3(yawQuat, yaw);
    for (int eye = 0; eye < 2; eye++) {
        const float* q = frame.eye[eye].quat;
        const bool haveEye = q[3] != 0 || q[1] != 0 || q[0] != 0 || q[2] != 0;
        float er[9], r[9];
        quatToMat3(haveEye ? q : frame.head.quat, er);
        mat3Mul(yaw, er, r);
        glViewport(eye * vr->eye, 0, vr->eye, vr->eye);
        glUniform4fv(s_uTan, 1, vr->tan[eye]);
        glUniformMatrix3fv(s_uRot, 1, GL_FALSE, r);
        glUniform1i(s_uEye, eye);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Debug readback of an eye buffer (both eyes, a quarter size), as a PPM.
void PS5Renderer::vrCapture(unsigned buffer, char* path)
{
    PS5VRState* vr = m_vr;
    const int w = 2 * vr->eye, h = vr->eye, f = 4, ow = w / f, oh = h / f;
    auto* rgba = static_cast<uint8_t*>(g_try_malloc(static_cast<size_t>(w) * h * 4));
    auto* rgb = static_cast<uint8_t*>(g_try_malloc(static_cast<size_t>(ow) * oh * 3));
    bool ok = rgba && rgb;
    if (ok) {
        glFinish();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, vr->eyeFBO[buffer]);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        // Memory row 0 is the top of the eye image already.
        for (int y = 0; y < oh; y++) {
            for (int x = 0; x < ow; x++) {
                for (int c = 0; c < 3; c++) {
                    unsigned sum = 0;
                    for (int dy = 0; dy < f; dy++)
                        for (int dx = 0; dx < f; dx++)
                            sum += rgba[(static_cast<size_t>(y * f + dy) * w + x * f + dx) * 4 + c];
                    rgb[(static_cast<size_t>(y) * ow + x) * 3 + c] = static_cast<uint8_t>(sum / (f * f));
                }
            }
        }
        FILE* file = fopen(path, "wb");
        ok = file;
        if (file) {
            fprintf(file, "P6\n%d %d\n255\n", ow, oh);
            ok = fwrite(rgb, 1, static_cast<size_t>(ow) * oh * 3, file) == static_cast<size_t>(ow) * oh * 3;
            ok = !fclose(file) && ok;
        }
    }
    VRLOG("SHOT %s (eye buffers %dx%d -> %dx%d) %s; look %.0f/%.0f, %s", path, w, h, ow, oh, ok ? "written" : "FAILED", vr->simYaw, vr->simPitch,
        projectionName(vr->projection));
    g_free(rgba);
    g_free(rgb);
    g_free(path);
}

void PS5Renderer::vrFrame()
{
    PS5VRState* vr = m_vr;

    // What the other threads queued (never waits: the headset paces us).
    g_mutex_lock(&m_lock);
    m_job = m_pageJob;
    m_pageJob.pending = false;
    if (m_job.pending)
        g_cond_broadcast(&m_cond);
    m_frameRequested = false;
    char* capturePath = m_capturePath;
    m_capturePath = nullptr;
    g_mutex_unlock(&m_lock);
    serviceBufferRequests();
    bool commitGeometry = m_job.pending;
    if (m_job.pending)
        copyPage();
    auto finishPageJob = [&] {
        if (m_job.pending) {
            g_free(m_job.damage);
            if (m_job.done)
                m_job.done(m_job.userData);
            m_job = { };
        }
    };

    // The frame slot and the pose to render it with.
    int64_t waitStart = nowUS();
    psvr2_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    int64_t displayUS;
    if (vr->sim) {
        int64_t now = nowUS();
        if (vr->simNextUS > now) {
            int64_t wait = vr->simNextUS - now;
            struct timespec ts = { static_cast<time_t>(wait / 1000000), static_cast<long>(wait % 1000000) * 1000 };
            nanosleep(&ts, nullptr);
        } else if (now - vr->simNextUS > 100000)
            vr->simNextUS = now;
        vr->simNextUS += kFrameUS;
        frame.frame = vr->frames + 1;
        frame.tracked = !vr->simUntracked;
        // Head direction: yaw about +Y (positive = left), then pitch.
        const float yaw = -vr->simYaw * static_cast<float>(M_PI) / 180, pitch = vr->simPitch * static_cast<float>(M_PI) / 180;
        const float qy[4] = { 0, sinf(yaw / 2), 0, cosf(yaw / 2) }, qp[4] = { sinf(pitch / 2), 0, 0, cosf(pitch / 2) };
        quatMul(qy, qp, frame.head.quat);
        memcpy(frame.eye[0].quat, frame.head.quat, sizeof(frame.head.quat));
        memcpy(frame.eye[1].quat, frame.head.quat, sizeof(frame.head.quat));
        displayUS = vr->simNextUS;
    } else {
        if (psvr2_begin_frame(&frame) != 0) {
            vr->beginErrors++;
            finishPageJob();
            if (++vr->consecutiveErrors >= 180) { // 3 s without a frame slot
                vrStop("the headset stopped taking frames");
                return;
            }
            g_usleep(8000);
            return;
        }
        vr->consecutiveErrors = 0;
        // Hmd2 predicts the display time on the process clock; video frames
        // are due on CLOCK_MONOTONIC.
        int64_t process = sceKernelGetProcessTime ? static_cast<int64_t>(sceKernelGetProcessTime()) : 0;
        int64_t ahead = process && frame.display_time ? static_cast<int64_t>(frame.display_time) - process : kFrameUS;
        displayUS = nowUS() + std::clamp<int64_t>(ahead, 0, 100000);
    }
    double waitMS = (nowUS() - waitStart) / 1000.0;
    int64_t renderStart = nowUS();
    vr->tracked += frame.tracked != 0;
    vr->windowTracked += frame.tracked != 0;
    if (frame.tracked) {
        g_mutex_lock(&m_lock);
        m_vrTrackedUS = renderStart;
        g_mutex_unlock(&m_lock);
    }

    // Video: the newest frame due by the display time (older ones dropped,
    // slots released), converted into its stream's texture.
    m_videoDrawCount = 0;
    m_videoSourceCount = 0;
    unsigned convertedBefore = 0;
    PS5VideoServer::consume(displayUS, kFrameUS / 2, commitGeometry,
        [this, &convertedBefore](PS5VideoStream& stream, const PS5VideoFrame* videoFrame, const uint8_t* data) {
            if (videoFrame)
                convertedBefore++;
            handleVideo(stream, videoFrame, data);
        });
    vr->windowVideo += convertedBefore;
    serviceThumbnails(nowUS());
    if (vr->sourceStream >= 0) {
        bool present = false;
        for (unsigned i = 0; i < m_videoSourceCount; i++)
            present |= m_videoSources[i].stream == vr->sourceStream;
        if (!present)
            vr->sourceStream = -1; // closed: the next one is picked
    }

    const unsigned buffer = static_cast<unsigned>(frame.frame % PSVR2_MAX_BUFFERS);
    vrRenderEyes(buffer, &frame);
    if (capturePath)
        vrCapture(buffer, capturePath);
    glFinish(); // the compositor must not sample a half-drawn buffer; the video's GL buffer is free again
    double renderMS = (nowUS() - renderStart) / 1000.0;
    if (!vr->sim && psvr2_submit(static_cast<int>(buffer), &frame, vr->tan) != 0)
        vr->submitErrors++;
    PS5VideoServer::presented();
    finishPageJob();

    int64_t now = nowUS();
    if (vr->hintUntilUS && now >= vr->hintUntilUS)
        vr->hintUntilUS = 0;
    if (vr->tvDirty)
        vrDrawTV(now);

    vr->frames++;
    vr->windowFrames++;
    vr->windowRenderMS += renderMS;
    vr->windowRenderMaxMS = std::max(vr->windowRenderMaxMS, renderMS);
    vr->windowWaitMS += waitMS;
    double seconds = (now - vr->windowUS) / 1e6;
    if (seconds >= 5) {
        VRLOG("[present] %u headset frames (%.1f/s), render %.2f ms avg %.1f max, waited %.1f ms avg; %u tracked; %u video frames shown (%.1f/s); "
              "%s, stream %d %dx%d%s; begin errors %llu, submit errors %llu",
            vr->windowFrames, vr->windowFrames / seconds, vr->windowRenderMS / std::max(1u, vr->windowFrames), vr->windowRenderMaxMS,
            vr->windowWaitMS / std::max(1u, vr->windowFrames), vr->windowTracked, vr->windowVideo, vr->windowVideo / seconds,
            projectionName(vr->projection), vr->sourceStream, vr->sourceWidth, vr->sourceHeight, vr->sim ? " (simulated)" : "",
            static_cast<unsigned long long>(vr->beginErrors), static_cast<unsigned long long>(vr->submitErrors));
        vr->windowUS = now;
        vr->windowFrames = vr->windowTracked = vr->windowVideo = 0;
        vr->windowRenderMS = vr->windowRenderMaxMS = vr->windowWaitMS = 0;
    }
    for (unsigned i = 0; i < m_videoSourceCount; i++) {
        if (m_videoSources[i].stream == vr->sourceStream) {
            vr->sourceWidth = m_videoSources[i].width;
            vr->sourceHeight = m_videoSources[i].height;
        }
    }

    // Back to the TV by itself, so nothing can keep the title in VR mode
    // with no way out: the headset unplugged (Hmd2's device status), or no
    // tracked pose at all since entering (a headset that never shows up).
    if (!vr->sim && psvr2_device_status && now - vr->statusPollUS >= kStatusPollUS) {
        vr->statusPollUS = now;
        uint32_t status = 0;
        int worn = 0, lens = 0;
        if (!psvr2_device_status(&status, &worn, &lens)) {
            if (status != vr->devStatus || worn != vr->devWorn)
                VRLOG("headset device status %u, %s (lens %d mm) after %.1f s", status, worn ? "worn" : "not worn", lens, (now - vr->startUS) / 1e6);
            vr->devStatus = status;
            vr->devWorn = worn;
            vr->disconnectedPolls = status ? vr->disconnectedPolls + 1 : 0;
            if (vr->disconnectedPolls >= kDisconnectedPolls) {
                vrStop("The PS VR2 headset is not connected any more. Back on the TV.");
                return;
            }
        }
    }
    if ((!vr->sim || vr->simUntracked) && !vr->tracked && now - vr->startUS > kNoTrackingUS) {
        char why[192];
        snprintf(why, sizeof(why), "No head tracking from the PS VR2 in %.0f s. Back on the TV: put the headset on, then press the touchpad.",
            kNoTrackingUS / 1e6);
        vrStop(why);
        return;
    }
}

// ---------------------------------------------------------------- video thumbnails (layout guess)

// Each output pixel is one cell of the video: the mean luma of 8 x 8
// bilinear taps spread over it (row 0 = the picture's top, as the video
// texture's row 0).
static const char* kThumbnailFragmentShader = R"(#version 430 core
uniform sampler2D video;
uniform vec2 cell; // one thumbnail cell in the video's uv
out vec4 color;
void main()
{
    vec2 base = floor(gl_FragCoord.xy) * cell;
    float sum = 0.0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++)
            sum += dot(texture(video, base + (vec2(float(i), float(j)) + 0.5) * cell / 8.0).rgb, vec3(0.2126, 0.7152, 0.0722));
    color = vec4(sum / 64.0, 0.0, 0.0, 1.0);
}
)";

static GLuint s_thumbProgram, s_thumbTexture, s_thumbFBO;
static GLint s_uThumbVideo, s_uThumbCell;
static bool s_thumbFailed;

struct ThumbnailsCall {
    PS5Renderer::ThumbnailsDone done;
    void* userData;
    std::vector<uint8_t> luma;
    unsigned frames;
    int width, height;
};

static void postThumbnails(ThumbnailsCall* call)
{
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto* call = static_cast<ThumbnailsCall*>(data);
        if (call->done)
            call->done(call->frames ? call->luma.data() : nullptr, call->frames, call->width, call->height, call->userData);
        return G_SOURCE_REMOVE;
    }, call, [](gpointer data) { delete static_cast<ThumbnailsCall*>(data); });
}

// One thumbnail of the video texture into out (kThumbnailSize^2 bytes).
bool PS5Renderer::takeThumbnail(unsigned texture, uint8_t* out)
{
    constexpr int n = kThumbnailSize;
    if (!s_thumbProgram && !s_thumbFailed) {
        char log[512] = { };
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVRVertexShader, log, sizeof(log));
        GLuint fs = vs ? compileShader(GL_FRAGMENT_SHADER, kThumbnailFragmentShader, log, sizeof(log)) : 0;
        GLuint program = 0;
        GLint ok = 0;
        if (vs && fs) {
            program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok)
                glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        }
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        if (ok) {
            glGenTextures(1, &s_thumbTexture);
            glBindTexture(GL_TEXTURE_2D, s_thumbTexture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, n, n);
            glGenFramebuffers(1, &s_thumbFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, s_thumbFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_thumbTexture, 0);
            ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (!ok)
                snprintf(log, sizeof(log), "framebuffer incomplete");
        }
        if (!ok) {
            VRLOG("thumbnail shader failed: %s", log);
            if (program)
                glDeleteProgram(program);
            s_thumbFailed = true;
            return false;
        }
        s_thumbProgram = program;
        s_uThumbVideo = glGetUniformLocation(program, "video");
        s_uThumbCell = glGetUniformLocation(program, "cell");
    }
    if (!s_thumbProgram)
        return false;
    glBindFramebuffer(GL_FRAMEBUFFER, s_thumbFBO);
    glViewport(0, 0, n, n);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(s_thumbProgram);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(s_uThumbVideo, 0);
    glUniform2f(s_uThumbCell, 1.0f / n, 1.0f / n);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    static uint8_t rgba[n * n * 4];
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, rgba); // waits for the draw: a 64 KB read
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int i = 0; i < n * n; i++)
        out[i] = rgba[i * 4];
    return true;
}

// After each consume pass (TV or headset): the pending request's next
// thumbnail when the playing stream has a new frame, the answer when enough
// were taken or the time is up.
void PS5Renderer::serviceThumbnails(int64_t now)
{
    Thumbnails& t = m_thumbnails;
    g_mutex_lock(&m_lock);
    ThumbnailRequest request = m_thumbnailRequest;
    m_thumbnailRequest.pending = false;
    g_mutex_unlock(&m_lock);
    if (request.pending) {
        if (t.active)
            postThumbnails(new ThumbnailsCall { t.done, t.userData, { }, 0, t.width, t.height }); // replaced
        t = { };
        t.active = true;
        t.count = request.count;
        t.done = request.done;
        t.userData = request.userData;
        t.deadlineUS = now + 1000000;
    }
    if (!t.active)
        return;
    // The stream the headset shows, else the newest (the playing video).
    const VideoSource* source = nullptr;
    for (unsigned i = 0; i < m_videoSourceCount; i++) {
        if (m_vr && m_videoSources[i].stream == m_vr->sourceStream)
            source = &m_videoSources[i];
    }
    for (unsigned i = 0; !source && i < m_videoSourceCount; i++) {
        if (!source || m_videoSources[i].lastFrameUS > source->lastFrameUS)
            source = &m_videoSources[i];
    }
    if (source && now >= t.nextUS && (!t.taken || source->lastFrameUS != t.lastFrameUS)) {
        constexpr size_t bytes = kThumbnailSize * kThumbnailSize;
        t.luma.resize((t.taken + 1) * bytes);
        int64_t start = nowUS();
        if (takeThumbnail(source->texture, t.luma.data() + t.taken * bytes)) {
            t.taken++;
            t.width = source->width;
            t.height = source->height;
            t.lastFrameUS = source->lastFrameUS;
            t.nextUS = now + 200000;
            t.stallUS = now + 400000; // no newer frame by then (paused): answer
            VRLOG("thumbnail %u/%u of video stream %d (%dx%d) in %.2f ms", t.taken, t.count, source->stream, source->width, source->height,
                (nowUS() - start) / 1000.0);
        } else
            t.deadlineUS = now; // no shader: answer with what there is
    }
    bool stalled = t.taken && now >= t.stallUS && (!source || source->lastFrameUS == t.lastFrameUS);
    if (t.taken >= t.count || now >= t.deadlineUS || stalled) {
        auto* call = new ThumbnailsCall { t.done, t.userData, std::move(t.luma), t.taken, t.width, t.height };
        t = { };
        postThumbnails(call);
    }
}
