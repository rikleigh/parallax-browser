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

#include "config.h"
#include "PS5VideoDecoder.h"

#include <algorithm>
#include <deque>
#include <errno.h>
#include <glib.h>
#include <map>
#include <memory>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define DLOG(...) g_message("[vdec] " __VA_ARGS__)

static const ps5wk_vdec_backend* backend;

extern "C" void ps5wk_video_set_decoder_backend(const ps5wk_vdec_backend* b)
{
    backend = b;
    if (b)
        DLOG("backend %s: codecs 0x%x, up to %dx%d%s%s", b->name, b->codecs, b->max_width, b->max_height,
            b->report ? "; " : "", b->report ? b->report() : "");
}

namespace PS5VideoDecoder {

// Bit 62 of the pts given to the hardware: a VP9 frame that is decoded but
// never shown (alt-ref); its picture is dropped.
static constexpr uint64_t hiddenFlag = 1ull << 62;

static bool mallocAllocate(size_t bytes, unsigned* glBuffer, uint8_t** map)
{
    *glBuffer = 0;
    *map = static_cast<uint8_t*>(aligned_alloc(65536, (bytes + 65535) & ~static_cast<size_t>(65535)));
    return *map;
}
static void mallocFree(unsigned, uint8_t* map, size_t) { free(map); }
static BufferProvider provider { mallocAllocate, mallocFree };

void setBufferProvider(const BufferProvider& p) { provider = p; }

unsigned codecs() { return backend ? backend->codecs : 0; }
bool available() { return backend && backend->codecs; }
void maxSize(int& width, int& height)
{
    width = backend ? backend->max_width : 0;
    height = backend ? backend->max_height : 0;
}

static int64_t nowUS() { return g_get_monotonic_time(); }

struct Slot {
    unsigned glBuffer { 0 };
    uint8_t* map { nullptr };  // the allocation
    size_t bytes { 0 };
    uint8_t* frame { nullptr }; // aligned start handed to the decoder
    enum State { Free, Decoding, Held } state { Free };
    uint32_t token { 0 };
    uint32_t epoch { 0 };
    bool pinned { false };
    // the picture
    uint32_t offset { 0 }, pitch { 0 }, width { 0 }, height { 0 };
};

struct AccessUnit {
    std::vector<uint8_t> data;
    uint32_t key { 0 }, epoch { 0 }, flags { 0 };
    int64_t ptsNS { -1 };
};

struct Decoder {
    uint32_t id { 0 };
    int fd { -1 };
    int pid { 0 };
    int streamID { 0 };
    GMutex lock;
    GCond cond;
    GMutex sendLock;

    // Guarded by lock.
    std::deque<AccessUnit> queue;
    uint32_t epoch { 0 };
    bool resetPending { false };
    bool drainPending { false };
    uint32_t drainEpoch { 0 };
    bool closing { false };
    std::vector<Slot> slots;
    uint32_t nextToken { 1 };

    // Decoder thread (and DOPEN, before it runs).
    void* hw { nullptr };
    unsigned codec { 0 }, profile { 0 }, bitDepth { 8 };
    int width { 0 }, height { 0 };
    size_t frameBytes { 0 }, frameAlign { 0 };
    bool needKeyframe { true };
    std::vector<uint8_t> paramSets; // last SPS/PPS(/VPS), Annex-B
    std::map<uint32_t, int64_t> outstanding; // key -> PTS of AUs without a picture yet
    pthread_t thread { };
    bool threadStarted { false };

    // Stats (decoder thread).
    uint64_t aus { 0 }, pictures { 0 }, hidden { 0 }, errors { 0 }, skipped { 0 }, noPTS { 0 }, reclaimed { 0 };
    int64_t windowStartUS { 0 };
    uint64_t windowAUs { 0 }, windowPictures { 0 };
    double windowCallMS { 0 }, windowCallMaxMS { 0 }, windowSlotWaitMS { 0 };
    unsigned windowCalls { 0 };
    int firstRC { 0 };

    Decoder()
    {
        g_mutex_init(&lock);
        g_cond_init(&cond);
        g_mutex_init(&sendLock);
    }
    ~Decoder()
    {
        g_mutex_clear(&lock);
        g_cond_clear(&cond);
        g_mutex_clear(&sendLock);
    }
};

static GMutex decodersLock;
static std::map<uint32_t, std::shared_ptr<Decoder>> decoders; // guarded by decodersLock
static uint32_t nextDecoderID;

static std::shared_ptr<Decoder> find(uint32_t id)
{
    g_mutex_lock(&decodersLock);
    auto it = decoders.find(id);
    std::shared_ptr<Decoder> d = it == decoders.end() ? nullptr : it->second;
    g_mutex_unlock(&decodersLock);
    return d;
}

static void send(Decoder& d, const ps5wk_video_msg& msg)
{
    g_mutex_lock(&d.sendLock);
    const char* p = reinterpret_cast<const char*>(&msg);
    size_t sent = 0;
    while (d.fd >= 0 && sent < sizeof msg) {
        ssize_t n = ::send(d.fd, p + sent, sizeof msg - sent, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        sent += static_cast<size_t>(n);
    }
    g_mutex_unlock(&d.sendLock);
}

static void sendPicture(Decoder& d, uint32_t key, uint32_t epoch, uint32_t status, const Slot* slot)
{
    ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_PICTURE;
    msg.ps5wk_key = key;
    msg.ps5wk_epoch = epoch;
    msg.status = status;
    msg.ps5wk_decoder = d.id;
    if (slot) {
        msg.ps5wk_token = slot->token;
        msg.width = slot->width;
        msg.height = slot->height;
        msg.ps5wk_pitch = slot->pitch;
        msg.ps5wk_bit_depth = d.bitDepth;
    }
    // Decoder thread only (outstanding is its own).
    auto it = d.outstanding.find(key);
    if (it != d.outstanding.end()) {
        msg.pts_ns = it->second;
        d.outstanding.erase(it);
        // Display order: access units with an earlier PTS won't come out any more.
        if (slot) {
            for (auto o = d.outstanding.begin(); o != d.outstanding.end();)
                o = o->second < msg.pts_ns ? d.outstanding.erase(o) : std::next(o);
        }
    }
    send(d, msg);
}

// Receiver thread: no picture for this access unit (not accepted).
static void sendRefused(Decoder& d, uint32_t key, uint32_t epoch)
{
    ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_PICTURE;
    msg.ps5wk_key = key;
    msg.ps5wk_epoch = epoch;
    msg.status = 4;
    msg.ps5wk_decoder = d.id;
    send(d, msg);
}

// ---------------------------------------------------------------- bitstream

static bool isParamSet(unsigned codec, uint8_t header)
{
    if (codec == PS5WK_VCODEC_H264) {
        unsigned type = header & 0x1f;
        return type == 7 || type == 8;
    }
    unsigned type = (header >> 1) & 0x3f;
    return type >= 32 && type <= 34;
}

// The parameter-set NAL units of an Annex-B access unit, with 4-byte start codes.
static std::vector<uint8_t> paramSetsOf(unsigned codec, const uint8_t* p, size_t n)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    auto nextStart = [&](size_t from, size_t& codeLength) -> size_t {
        for (size_t k = from; k + 3 <= n; k++) {
            if (!p[k] && !p[k + 1] && p[k + 2] == 1) {
                codeLength = (k && !p[k - 1]) ? 4 : 3;
                return k + 3;
            }
        }
        codeLength = 0;
        return n;
    };
    size_t codeLength;
    i = nextStart(0, codeLength);
    while (i < n) {
        size_t nextLength;
        size_t next = nextStart(i, nextLength);
        size_t end = next < n ? next - nextLength : n;
        if (end > i && isParamSet(codec, p[i])) {
            static const uint8_t startCode[4] = { 0, 0, 0, 1 };
            out.insert(out.end(), startCode, startCode + 4);
            out.insert(out.end(), p + i, p + end);
        }
        i = next;
    }
    return out;
}

// VP9 superframe (annex B of the VP9 spec) -> frames.
static void splitSuperframe(const uint8_t* p, size_t n, std::vector<std::pair<const uint8_t*, size_t>>& out)
{
    out.clear();
    if (n) {
        uint8_t marker = p[n - 1];
        if ((marker & 0xe0) == 0xc0) {
            int frames = (marker & 7) + 1, mag = ((marker >> 3) & 3) + 1;
            size_t index = 2 + static_cast<size_t>(mag) * frames;
            if (n >= index && p[n - index] == marker) {
                const uint8_t* q = p + n - index + 1;
                size_t offset = 0;
                for (int f = 0; f < frames; f++) {
                    size_t size = 0;
                    for (int b = 0; b < mag; b++)
                        size |= static_cast<size_t>(*q++) << (8 * b);
                    if (offset + size > n - index)
                        break;
                    if (size)
                        out.emplace_back(p + offset, size);
                    offset += size;
                }
                if (!out.empty())
                    return;
            }
        }
    }
    out.emplace_back(p, n);
}

// From a VP9 frame's uncompressed header: shown? (show_existing_frame counts as shown)
static bool vp9Shown(const uint8_t* p, size_t n)
{
    if (n < 1 || (p[0] >> 6) != 2)
        return true;
    int profile = ((p[0] >> 5) & 1) | (((p[0] >> 4) & 1) << 1);
    int bit = profile == 3 ? 2 : 3;
    if ((p[0] >> bit) & 1)
        return true;
    bit -= 2;
    return bit >= 0 ? (p[0] >> bit) & 1 : n > 1 ? (p[1] >> (8 + bit)) & 1 : 1;
}

// ---------------------------------------------------------------- decoding

// A free slot, waiting for one (the consumer releases them); -1 when the
// decoder is closing or resetting.
static int waitFreeSlot(Decoder& d)
{
    int64_t start = nowUS();
    g_mutex_lock(&d.lock);
    int found = -1;
    unsigned waits = 0;
    int64_t lastReclaim = start;
    for (;;) {
        if (d.closing || d.resetPending)
            break;
        // Safety net: a picture handed out but dropped on its way to the
        // screen without anybody telling us would hold its slot forever.
        // After 1.5 s without a free slot, take back the oldest one nobody
        // is drawing (worst case: a frame skipped after a long pause).
        int64_t now = g_get_monotonic_time();
        // The older half of the pictures nobody is drawing goes back at
        // once (one at a time only crawled at ~1 picture per second).
        if (now - start > 1500000 && now - lastReclaim > 500000) {
            std::vector<Slot*> held;
            for (auto& s : d.slots) {
                if (s.state == Slot::Held && !s.pinned)
                    held.push_back(&s);
            }
            std::sort(held.begin(), held.end(), [](Slot* a, Slot* b) { return a->token < b->token; });
            size_t count = std::max<size_t>(1, held.size() / 2);
            for (size_t i = 0; i < count && i < held.size(); i++)
                held[i]->state = Slot::Free;
            if (!held.empty()) {
                d.reclaimed += std::min(count, held.size());
                DLOG("decoder %u: no free output slot for %.1f s: took back %zu pictures from %u (%llu so far)", d.id, (now - start) / 1e6,
                    std::min(count, held.size()), held[0]->token, (unsigned long long)d.reclaimed);
            }
            lastReclaim = now;
        }
        for (size_t i = 0; i < d.slots.size(); i++) {
            if (d.slots[i].state == Slot::Free) {
                found = static_cast<int>(i);
                break;
            }
        }
        if (found >= 0) {
            d.slots[found].state = Slot::Decoding;
            break;
        }
        g_cond_wait_until(&d.cond, &d.lock, g_get_monotonic_time() + 500000);
        if (++waits == 20) {
            unsigned held = 0, pinned = 0;
            for (auto& s : d.slots) {
                held += s.state == Slot::Held;
                pinned += s.pinned;
            }
            DLOG("decoder %u: no free output slot for 10 s (%u held, %u pinned)", d.id, held, pinned);
        }
    }
    g_mutex_unlock(&d.lock);
    d.windowSlotWaitMS += (nowUS() - start) / 1000.0;
    return found;
}

static void freeSlot(Decoder& d, int index)
{
    g_mutex_lock(&d.lock);
    d.slots[index].state = Slot::Free;
    d.slots[index].pinned = false;
    g_cond_broadcast(&d.cond);
    g_mutex_unlock(&d.lock);
}

// A picture out of the decoder (into slot `passed`): to the proxy, or dropped.
static void handlePicture(Decoder& d, const ps5wk_vdec_picture& pic, int passed, uint32_t epoch)
{
    if (!pic.valid) {
        freeSlot(d, passed);
        return;
    }
    int index = passed;
    auto inside = [&](const Slot& s) {
        auto* b = static_cast<uint8_t*>(pic.buffer);
        return b >= s.frame && b < s.map + s.bytes;
    };
    if (pic.buffer && !inside(d.slots[passed])) {
        index = -1;
        for (size_t i = 0; i < d.slots.size(); i++)
            if (inside(d.slots[i]))
                index = static_cast<int>(i);
        if (index < 0 || d.slots[index].state == Slot::Held) {
            static unsigned reported;
            if (reported++ < 4)
                DLOG("decoder %u: picture at %p is not in the slot passed (%p); dropped", d.id, pic.buffer, d.slots[passed].frame);
            freeSlot(d, passed);
            return;
        }
        freeSlot(d, passed);
        g_mutex_lock(&d.lock);
        d.slots[index].state = Slot::Decoding;
        g_mutex_unlock(&d.lock);
    }

    uint64_t pts = pic.pts;
    if (!pic.has_pts) {
        // Display order: the smallest PTS among the access units sent.
        d.noPTS++;
        int64_t best = INT64_MAX;
        uint32_t bestKey = 0;
        for (auto& [key, ptsNS] : d.outstanding) {
            if (ptsNS < best) {
                best = ptsNS;
                bestKey = key;
            }
        }
        pts = bestKey;
    }
    if (pts & hiddenFlag) {
        d.hidden++;
        freeSlot(d, index);
        return;
    }
    uint32_t key = static_cast<uint32_t>(pts);
    Slot copy;
    g_mutex_lock(&d.lock);
    Slot& s = d.slots[index];
    s.state = Slot::Held;
    s.token = d.nextToken++;
    s.epoch = epoch;
    s.pinned = false;
    s.offset = static_cast<uint32_t>(pic.buffer ? static_cast<uint8_t*>(pic.buffer) - s.map : s.frame - s.map);
    s.pitch = pic.pitch_bytes;
    s.width = pic.width;
    s.height = pic.height;
    copy = s;
    bool stale = epoch != d.epoch;
    if (stale)
        s.state = Slot::Free;
    g_cond_broadcast(&d.cond);
    g_mutex_unlock(&d.lock);
    if (stale)
        return;
    d.pictures++;
    d.windowPictures++;
    sendPicture(d, key, epoch, 0, &copy);
}

static void logStats(Decoder& d)
{
    int64_t now = nowUS();
    if (!d.windowStartUS)
        d.windowStartUS = now;
    double seconds = (now - d.windowStartUS) / 1e6;
    if (seconds < 5)
        return;
    if (d.windowAUs || d.windowPictures) {
        unsigned held = 0, pinned = 0;
        size_t queued;
        g_mutex_lock(&d.lock);
        for (auto& s : d.slots) {
            held += s.state == Slot::Held;
            pinned += s.pinned;
        }
        queued = d.queue.size();
        g_mutex_unlock(&d.lock);
        DLOG("decoder %u (pid %d): %.1f AUs/s in, %.1f pictures/s out; decode call %.2f ms avg %.2f max, waited %.0f ms for slots; "
             "%u/%zu slots held (%u pinned), %zu AUs queued; so far %llu pictures, %llu hidden, %llu errors, %llu skipped, %llu without pts",
            d.id, d.pid, d.windowAUs / seconds, d.windowPictures / seconds, d.windowCalls ? d.windowCallMS / d.windowCalls : 0.0,
            d.windowCallMaxMS, d.windowSlotWaitMS, held, d.slots.size(), pinned, queued, (unsigned long long)d.pictures,
            (unsigned long long)d.hidden, (unsigned long long)d.errors, (unsigned long long)d.skipped, (unsigned long long)d.noPTS);
    }
    d.windowStartUS = now;
    d.windowAUs = d.windowPictures = 0;
    d.windowCallMS = d.windowCallMaxMS = d.windowSlotWaitMS = 0;
    d.windowCalls = 0;
}

static bool decodeUnit(Decoder& d, const uint8_t* data, size_t size, uint64_t pts, int64_t ptsNS, uint32_t epoch)
{
    int slot = waitFreeSlot(d);
    if (slot < 0)
        return false;
    ps5wk_vdec_picture pic;
    memset(&pic, 0, sizeof pic);
    int64_t start = nowUS();
    int rc = backend->decode(d.hw, data, size, pts, ptsNS, d.slots[slot].frame, &pic);
    double ms = (nowUS() - start) / 1000.0;
    d.windowCallMS += ms;
    d.windowCallMaxMS = std::max(d.windowCallMaxMS, ms);
    d.windowCalls++;
    if (rc) {
        if (!d.errors++)
            d.firstRC = rc;
        if (d.errors <= 5 || !(d.errors % 100))
            DLOG("decoder %u: decode rc 0x%08x (%llu errors; AU %zu bytes, key %u)", d.id, static_cast<unsigned>(rc),
                (unsigned long long)d.errors, size, static_cast<unsigned>(pts));
    }
    handlePicture(d, pic, slot, epoch);
    return !rc;
}

static void processAU(Decoder& d, AccessUnit& au)
{
    d.aus++;
    d.windowAUs++;
    if (au.epoch != d.epoch)
        return;
    const bool keyframe = au.flags & PS5WK_VAU_KEYFRAME;
    if (d.codec == PS5WK_VCODEC_VP9) {
        if (d.needKeyframe && !keyframe) {
            d.skipped++;
            sendPicture(d, au.key, au.epoch, 2, nullptr);
            return;
        }
        d.needKeyframe = false;
        d.outstanding[au.key] = au.ptsNS;
        std::vector<std::pair<const uint8_t*, size_t>> frames;
        splitSuperframe(au.data.data(), au.data.size(), frames);
        bool shown = false, ok = true;
        for (auto& [p, n] : frames) {
            bool show = vp9Shown(p, n);
            shown |= show;
            ok &= decodeUnit(d, p, n, au.key | (show ? 0 : hiddenFlag), au.ptsNS, au.epoch);
        }
        if (!shown || !ok)
            sendPicture(d, au.key, au.epoch, shown ? 3 : 1, nullptr);
        return;
    }

    // H.264 / HEVC (Annex-B): start at a keyframe with its parameter sets.
    if (d.needKeyframe && !keyframe) {
        d.skipped++;
        sendPicture(d, au.key, au.epoch, 2, nullptr);
        return;
    }
    auto sets = paramSetsOf(d.codec, au.data.data(), au.data.size());
    if (!sets.empty())
        d.paramSets = std::move(sets);
    else if (d.needKeyframe && !d.paramSets.empty())
        au.data.insert(au.data.begin(), d.paramSets.begin(), d.paramSets.end());
    d.needKeyframe = false;
    d.outstanding[au.key] = au.ptsNS;
    if (!decodeUnit(d, au.data.data(), au.data.size(), au.key, au.ptsNS, au.epoch))
        sendPicture(d, au.key, au.epoch, 3, nullptr);
}

static void drain(Decoder& d, uint32_t epoch)
{
    for (int i = 0; i < 64; i++) {
        int slot = waitFreeSlot(d);
        if (slot < 0)
            return;
        ps5wk_vdec_picture pic;
        memset(&pic, 0, sizeof pic);
        int rc = backend->flush(d.hw, d.slots[slot].frame, &pic);
        bool valid = pic.valid;
        handlePicture(d, pic, slot, epoch);
        if (rc || !valid)
            break;
    }
    // Whatever was sent and never came out.
    std::vector<uint32_t> lost;
    for (auto& [key, ptsNS] : d.outstanding)
        lost.push_back(key);
    for (auto key : lost)
        sendPicture(d, key, epoch, 3, nullptr);
    d.outstanding.clear();
    backend->reset(d.hw);
    d.needKeyframe = true;
}

static void* decoderThread(void* data)
{
    auto d = *static_cast<std::shared_ptr<Decoder>*>(data);
    delete static_cast<std::shared_ptr<Decoder>*>(data);
    for (;;) {
        g_mutex_lock(&d->lock);
        while (!d->closing && !d->resetPending && d->queue.empty() && !d->drainPending)
            g_cond_wait_until(&d->cond, &d->lock, g_get_monotonic_time() + 1000000);
        if (d->closing) {
            g_mutex_unlock(&d->lock);
            break;
        }
        if (d->resetPending) {
            d->resetPending = false;
            g_mutex_unlock(&d->lock);
            backend->reset(d->hw);
            d->needKeyframe = true;
            d->outstanding.clear();
            continue;
        }
        if (!d->queue.empty()) {
            AccessUnit au = std::move(d->queue.front());
            d->queue.pop_front();
            g_cond_broadcast(&d->cond);
            g_mutex_unlock(&d->lock);
            processAU(*d, au);
            logStats(*d);
            continue;
        }
        d->drainPending = false;
        uint32_t epoch = d->drainEpoch;
        g_mutex_unlock(&d->lock);
        drain(*d, epoch);
        ps5wk_video_msg msg;
        memset(&msg, 0, sizeof msg);
        msg.type = PS5WK_VMSG_DRAINED;
        msg.ps5wk_epoch = epoch;
        msg.ps5wk_decoder = d->id;
        send(*d, msg);
    }
    return nullptr;
}

// ---------------------------------------------------------------- open/close

static void freeSlots(Decoder& d)
{
    for (auto& s : d.slots) {
        if (s.map)
            provider.free(s.glBuffer, s.map, s.bytes);
    }
    d.slots.clear();
}

static void closeHardware(Decoder& d)
{
    if (d.threadStarted) {
        g_mutex_lock(&d.lock);
        d.closing = true;
        g_cond_broadcast(&d.cond);
        g_mutex_unlock(&d.lock);
        pthread_join(d.thread, nullptr);
        d.threadStarted = false;
    }
    g_mutex_lock(&decodersLock);
    decoders.erase(d.id);
    g_mutex_unlock(&decodersLock);
    if (d.hw)
        backend->close(d.hw);
    d.hw = nullptr;
    // Buffers go back on the renderer's thread, after the frame in flight.
    freeSlots(d);
}

static unsigned openDecoders()
{
    g_mutex_lock(&decodersLock);
    unsigned n = decoders.size();
    g_mutex_unlock(&decodersLock);
    return n;
}

// DOPEN: returns the status for DOPEN_ACK.
static uint32_t openHardware(const std::shared_ptr<Decoder>& d, const ps5wk_video_msg& msg)
{
    unsigned codec = msg.role;
    unsigned profile = msg.ps5wk_profile, depth = msg.ps5wk_bit_depth ? msg.ps5wk_bit_depth : 8;
    int width = static_cast<int>(msg.width), height = static_cast<int>(msg.height);
    if (!backend || !(backend->codecs & codec) || (depth > 8 && !(backend->codecs & PS5WK_VCODEC_10BIT)))
        return 1;
    if (width > backend->max_width || height > backend->max_height)
        return 1;
    // Room for adaptive streams: VP9 is cheap at 4K (13 MB), H.264 up to
    // 1080p costs 24 MB; HEVC is sized as asked.
    int openWidth = width, openHeight = height;
    if (codec == PS5WK_VCODEC_VP9 && openWidth * openHeight < 3840 * 2160) {
        openWidth = std::max(openWidth, 3840);
        openHeight = std::max(openHeight, 2160);
    } else if (codec == PS5WK_VCODEC_H264 && openWidth * openHeight < 1920 * 1088) {
        openWidth = std::max(openWidth, 1920);
        openHeight = std::max(openHeight, 1088);
    }

    // Same decoder again (caps renegotiated, e.g. a resolution switch): keep
    // it if it covers the new size.
    if (d->hw) {
        if (d->codec == codec && d->profile == profile && d->bitDepth == depth && width <= d->width && height <= d->height) {
            DLOG("decoder %u: %dx%d fits the decoder (%dx%d); kept", d->id, width, height, d->width, d->height);
            return 0;
        }
        DLOG("decoder %u: reopening for %dx%d (was %dx%d)", d->id, width, height, d->width, d->height);
        closeHardware(*d);
    }

    char error[200] = "";
    size_t frameBytes = 0, frameAlign = 0;
    void* hw = nullptr;
    int64_t start = nowUS();
    // Another decoder may still be closing (switching videos): wait for it
    // rather than fall back to software.
    for (int attempt = 0; attempt < 30; attempt++) {
        hw = backend->open(codec, profile, depth, openWidth, openHeight, &frameBytes, &frameAlign, error, sizeof error);
        if (hw || !openDecoders())
            break;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }
    if (!hw) {
        DLOG("decoder for pid %d: codec 0x%x profile %u %u-bit %dx%d: open failed (%s)", d->pid, codec, profile, depth, openWidth,
            openHeight, error);
        return 2;
    }
    double openMS = (nowUS() - start) / 1000.0;
    frameAlign = std::max<size_t>(frameAlign, 256);
    unsigned count = frameBytes <= (16u << 20) ? 12 : frameBytes <= (50u << 20) ? 9 : 8;
    std::vector<Slot> slots;
    for (unsigned i = 0; i < count; i++) {
        Slot s;
        s.bytes = frameBytes + frameAlign;
        if (!provider.allocate(s.bytes, &s.glBuffer, &s.map)) {
            DLOG("decoder for pid %d: output buffer %u of %.1f MB failed", d->pid, i, s.bytes / 1048576.0);
            break;
        }
        uintptr_t a = reinterpret_cast<uintptr_t>(s.map);
        s.frame = reinterpret_cast<uint8_t*>((a + frameAlign - 1) & ~(static_cast<uintptr_t>(frameAlign) - 1));
        slots.push_back(s);
    }
    if (slots.size() < 4) {
        for (auto& s : slots)
            provider.free(s.glBuffer, s.map, s.bytes);
        backend->close(hw);
        return 3;
    }
    d->hw = hw;
    d->codec = codec;
    d->profile = profile;
    d->bitDepth = depth;
    d->width = openWidth;
    d->height = openHeight;
    d->frameBytes = frameBytes;
    d->frameAlign = frameAlign;
    d->needKeyframe = true;
    d->paramSets.clear();
    d->outstanding.clear();
    g_mutex_lock(&d->lock);
    d->slots = std::move(slots);
    d->closing = false;
    d->resetPending = false;
    d->queue.clear();
    g_mutex_unlock(&d->lock);
    g_mutex_lock(&decodersLock);
    d->id = ++nextDecoderID;
    decoders[d->id] = d;
    g_mutex_unlock(&decodersLock);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 512 * 1024);
    auto* arg = new std::shared_ptr<Decoder>(d);
    if (pthread_create(&d->thread, &attributes, decoderThread, arg)) {
        delete arg;
        closeHardware(*d);
        return 4;
    }
    d->threadStarted = true;
    const char* names[] = { "?", "H.264", "HEVC", "?", "VP9" };
    DLOG("decoder %u for pid %d: %s profile %u, %u-bit, %dx%d (stream %dx%d), up in %.0f ms; %zu output buffers of %.1f MB",
        d->id, d->pid, names[codec < 5 ? codec : 0], profile, depth, openWidth, openHeight, width, height, openMS, d->slots.size(),
        (frameBytes + frameAlign) / 1048576.0);
    return 0;
}

static bool readFully(int fd, void* buffer, size_t size)
{
    auto* p = static_cast<uint8_t*>(buffer);
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(fd, p + got, size - got, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

void serve(int fd, int pid, int streamID)
{
    auto d = std::make_shared<Decoder>();
    d->fd = fd;
    d->pid = pid;
    d->streamID = streamID;
    ps5wk_video_msg msg;
    uint64_t auBytes = 0, refused = 0;
    while (readFully(fd, &msg, sizeof msg)) {
        switch (msg.type) {
        case PS5WK_VMSG_DOPEN: {
            uint32_t status = openHardware(d, msg);
            if (!status) {
                g_mutex_lock(&d->lock);
                d->epoch = msg.ps5wk_epoch; // the proxy's current epoch
                g_mutex_unlock(&d->lock);
            }
            ps5wk_video_msg ack;
            memset(&ack, 0, sizeof ack);
            ack.type = PS5WK_VMSG_DOPEN_ACK;
            ack.status = status;
            ack.ps5wk_decoder = status ? 0 : d->id;
            send(*d, ack);
            break;
        }
        case PS5WK_VMSG_AU: {
            AccessUnit au;
            if (msg.payload_bytes > (64u << 20))
                goto done;
            au.data.resize(msg.payload_bytes);
            if (!readFully(fd, au.data.data(), au.data.size()))
                goto done;
            auBytes += au.data.size();
            au.key = msg.ps5wk_key;
            au.epoch = msg.ps5wk_epoch;
            au.flags = msg.ps5wk_flags;
            au.ptsNS = msg.pts_ns;
            g_mutex_lock(&d->lock);
            bool accept = d->hw && au.epoch == d->epoch;
            if (accept) {
                // Bounded: the proxy waits for pictures, so this stays short.
                while (d->queue.size() >= 64 && !d->closing)
                    g_cond_wait_until(&d->cond, &d->lock, g_get_monotonic_time() + 100000);
                d->queue.push_back(std::move(au));
                g_cond_broadcast(&d->cond);
            }
            g_mutex_unlock(&d->lock);
            if (!accept) {
                if (!refused++)
                    DLOG("decoder %u: AU refused (epoch %u, decoder at %u%s)", d->id, msg.ps5wk_epoch, d->epoch, d->hw ? "" : ", no decoder");
                sendRefused(*d, msg.ps5wk_key, msg.ps5wk_epoch);
            }
            break;
        }
        case PS5WK_VMSG_DFLUSH: {
            unsigned freed = 0, pinned = 0;
            g_mutex_lock(&d->lock);
            d->epoch = msg.ps5wk_epoch;
            d->queue.clear();
            d->resetPending = true;
            d->drainPending = false;
            for (auto& s : d->slots) {
                pinned += s.pinned;
                if (s.state == Slot::Held && !s.pinned) {
                    s.state = Slot::Free;
                    freed++;
                }
            }
            g_cond_broadcast(&d->cond);
            g_mutex_unlock(&d->lock);
            DLOG("decoder %u: flush (epoch %u): %u pictures dropped, %u on screen", d->id, msg.ps5wk_epoch, freed, pinned);
            break;
        }
        case PS5WK_VMSG_DDRAIN:
            g_mutex_lock(&d->lock);
            d->drainPending = true;
            d->drainEpoch = msg.ps5wk_epoch;
            g_cond_broadcast(&d->cond);
            g_mutex_unlock(&d->lock);
            break;
        case PS5WK_VMSG_PRELEASE:
            release(d->id, msg.ps5wk_token, false);
            break;
        default:
            break;
        }
    }
done:
    if (d->id || d->aus) { // a connection that never opened a decoder (no hardware) says nothing
        DLOG("decoder %u from pid %d closed: %llu AUs (%.1f MB), %llu pictures, %llu hidden, %llu errors (first rc 0x%08x), %llu skipped before a keyframe, %llu refused",
            d->id, pid, (unsigned long long)d->aus, auBytes / 1048576.0, (unsigned long long)d->pictures, (unsigned long long)d->hidden,
            (unsigned long long)d->errors, static_cast<unsigned>(d->firstRC), (unsigned long long)d->skipped, (unsigned long long)refused);
    }
    closeHardware(*d);
    d->fd = -1;
}

// ---------------------------------------------------------------- consumer side

bool pin(uint32_t decoder, uint32_t token, PS5VideoPicture& picture)
{
    auto d = find(decoder);
    if (!d)
        return false;
    bool found = false;
    g_mutex_lock(&d->lock);
    for (auto& s : d->slots) {
        if (s.state == Slot::Held && s.token == token) {
            s.pinned = true;
            picture.glBuffer = s.glBuffer;
            picture.map = s.map;
            picture.bufferBytes = s.bytes;
            picture.offset = s.offset;
            picture.pitch = s.pitch;
            picture.codedHeight = s.height;
            picture.bitDepth = d->bitDepth;
            found = true;
            break;
        }
    }
    g_mutex_unlock(&d->lock);
    return found;
}

void release(uint32_t decoder, uint32_t token, bool wasPinned)
{
    auto d = find(decoder);
    if (!d)
        return;
    g_mutex_lock(&d->lock);
    for (auto& s : d->slots) {
        if (s.state != Slot::Held)
            continue;
        // This picture, and older ones nobody showed (dropped by the sink's
        // QoS before they reached us).
        if (s.token == token || (s.token < token && !s.pinned)) {
            if (s.token == token && s.pinned && !wasPinned)
                continue; // being drawn: released after the swap
            s.state = Slot::Free;
            s.pinned = false;
        }
    }
    g_cond_broadcast(&d->cond);
    g_mutex_unlock(&d->lock);
}

} // namespace PS5VideoDecoder

// ---------------------------------------------------------------- fake backend (ps5wk-shot)

namespace {
struct FakeDecoder {
    std::multimap<int64_t, uint64_t> held; // PTS -> pts given; display order out
};

void* fakeOpen(unsigned, unsigned, unsigned, int, int, size_t* frameBytes, size_t* frameAlign, char*, size_t)
{
    *frameBytes = 64 * 1024;
    *frameAlign = 4096;
    return new FakeDecoder;
}

void fakeOut(FakeDecoder* f, void* frame, ps5wk_vdec_picture* pic)
{
    auto it = f->held.begin();
    pic->valid = 1;
    pic->width = 64;
    pic->height = 64;
    pic->pitch_bytes = 64;
    pic->buffer = frame;
    pic->has_pts = 1;
    pic->pts = it->second;
    f->held.erase(it);
}

int fakeDecode(void* h, const void*, size_t, uint64_t pts, int64_t ptsNS, void* frame, ps5wk_vdec_picture* pic)
{
    auto* f = static_cast<FakeDecoder*>(h);
    // PS5WK_FAKE_VDEC_DELAY_MS=<ms>[:<s>]: a slow decoder (frames late at
    // the sink: QoS), for the first <s> seconds only if given (a burst of
    // load, as on YouTube's busy pages).
    static const char* knob = g_getenv("PS5WK_FAKE_VDEC_DELAY_MS");
    static const int delayMS = knob ? atoi(knob) : 0;
    static const int burstS = knob && strchr(knob, ':') ? atoi(strchr(knob, ':') + 1) : 0;
    static const int64_t startUS = g_get_monotonic_time();
    if (delayMS > 0 && (!burstS || g_get_monotonic_time() - startUS < burstS * 1000000LL)) {
        struct timespec ts = { delayMS / 1000, (delayMS % 1000) * 1000000L };
        nanosleep(&ts, nullptr);
    }
    f->held.emplace(ptsNS, pts);
    if (f->held.size() > 4) // a reorder window, as the hardware's pipeline
        fakeOut(f, frame, pic);
    return 0;
}

int fakeFlush(void* h, void* frame, ps5wk_vdec_picture* pic)
{
    auto* f = static_cast<FakeDecoder*>(h);
    if (!f->held.empty())
        fakeOut(f, frame, pic);
    return 0;
}

int fakeReset(void* h)
{
    static_cast<FakeDecoder*>(h)->held.clear();
    return 0;
}

void fakeClose(void* h) { delete static_cast<FakeDecoder*>(h); }

const ps5wk_vdec_backend fakeBackend = {
    "fake (pictures without content, display order by PTS)",
    PS5WK_VCODEC_H264 | PS5WK_VCODEC_HEVC | PS5WK_VCODEC_VP9 | PS5WK_VCODEC_10BIT,
    8192, 4352, fakeOpen, fakeDecode, fakeFlush, fakeReset, fakeClose, nullptr,
};
} // namespace

extern "C" const ps5wk_vdec_backend* ps5wk_video_fake_decoder_backend(void)
{
    return &fakeBackend;
}
