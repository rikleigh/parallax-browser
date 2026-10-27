/* xr_math.h - pose and quaternion helpers for libopenxr_psvr2 (Y up, -Z
 * forward, right-handed, quaternions x y z w: OpenXR's conventions). */
#pragma once

#include <math.h>
#include <openxr/openxr.h>

static inline XrQuaternionf q_identity(void)
{
    XrQuaternionf q = {0, 0, 0, 1};
    return q;
}

static inline XrQuaternionf q_mul(XrQuaternionf a, XrQuaternionf b)
{
    XrQuaternionf r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}

static inline XrQuaternionf q_conj(XrQuaternionf q)
{
    XrQuaternionf r = {-q.x, -q.y, -q.z, q.w};
    return r;
}

static inline float q_norm(XrQuaternionf q)
{
    return sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

static inline XrQuaternionf q_normalize(XrQuaternionf q)
{
    const float n = q_norm(q);
    if (n < 1e-6f)
        return q_identity();
    XrQuaternionf r = {q.x / n, q.y / n, q.z / n, q.w / n};
    return r;
}

static inline XrQuaternionf q_axis_angle(float ax, float ay, float az, float angle)
{
    const float s = sinf(angle * 0.5f);
    XrQuaternionf q = {ax * s, ay * s, az * s, cosf(angle * 0.5f)};
    return q;
}

/* v' = q v q* */
static inline XrVector3f q_rotate(XrQuaternionf q, XrVector3f v)
{
    const float tx = 2 * (q.y * v.z - q.z * v.y);
    const float ty = 2 * (q.z * v.x - q.x * v.z);
    const float tz = 2 * (q.x * v.y - q.y * v.x);
    XrVector3f r = {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
                    v.z + q.w * tz + (q.x * ty - q.y * tx)};
    return r;
}

/* Rotation about +Y that matches q's heading (forward vector projected on
 * the floor plane). */
static inline XrQuaternionf q_yaw_only(XrQuaternionf q)
{
    const XrVector3f fwd = {0, 0, -1};
    const XrVector3f f = q_rotate(q, fwd);
    if (fabsf(f.x) < 1e-6f && fabsf(f.z) < 1e-6f)
        return q_identity();
    const float yaw = atan2f(-f.x, -f.z);
    return q_axis_angle(0, 1, 0, yaw);
}

/* Angle between two orientations, radians. */
static inline float q_angle(XrQuaternionf a, XrQuaternionf b)
{
    const XrQuaternionf d = q_mul(q_conj(a), b);
    float w = fabsf(d.w);
    if (w > 1.0f)
        w = 1.0f;
    return 2.0f * acosf(w);
}

static inline XrPosef pose_identity(void)
{
    XrPosef p = {{0, 0, 0, 1}, {0, 0, 0}};
    return p;
}

/* a * b: b expressed in a's frame, mapped to a's parent frame. */
static inline XrPosef pose_mul(XrPosef a, XrPosef b)
{
    XrPosef r;
    r.orientation = q_normalize(q_mul(a.orientation, b.orientation));
    const XrVector3f t = q_rotate(a.orientation, b.position);
    r.position.x = a.position.x + t.x;
    r.position.y = a.position.y + t.y;
    r.position.z = a.position.z + t.z;
    return r;
}

static inline XrPosef pose_inv(XrPosef a)
{
    XrPosef r;
    r.orientation = q_conj(a.orientation);
    const XrVector3f t = q_rotate(r.orientation, a.position);
    r.position.x = -t.x;
    r.position.y = -t.y;
    r.position.z = -t.z;
    return r;
}

/* `space` relative to `base` (both in the same parent frame). */
static inline XrPosef pose_relative(XrPosef base, XrPosef space)
{
    return pose_mul(pose_inv(base), space);
}

static inline float v3_dist(XrVector3f a, XrVector3f b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
