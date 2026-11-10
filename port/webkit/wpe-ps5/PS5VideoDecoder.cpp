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
#include <string>
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
    uint64_t formatChanges { 0 };
    size_t frameBytes { 0 }, frameAlign { 0 };
    bool needKeyframe { true };
    std::vector<uint8_t> paramSets; // last SPS/PPS(/VPS), Annex-B
    std::vector<uint8_t> sequenceSets; // the SPS (and VPS) among them
    std::map<uint32_t, int64_t> outstanding; // key -> PTS of AUs without a picture yet
    pthread_t thread { };
    bool threadStarted { false };

    // Recovery from a decoder that fails every call (decoder thread).
    bool hwStuck { false };
    int stuckRC { 0 };
    unsigned errorRun { 0 }; // decode calls in a row that failed
    uint64_t ausSinceReset { 0 }, picturesSinceReset { 0 };
    double lastCallMS { 0 }, lastSlotWaitMS { 0 };
    // Test knobs (/data/ps5webkit/vdec-inject, read at DOPEN).
    bool injectNoSequenceReset { false }, injectNoRecovery { false };
    uint32_t injectCorruptKey { 0 }, injectTruncateKey { 0 };

    // Stats (decoder thread).
    uint64_t aus { 0 }, pictures { 0 }, hidden { 0 }, errors { 0 }, skipped { 0 }, noPTS { 0 }, longWaits { 0 };
    uint64_t recoveries { 0 }, flagged { 0 };
    uint64_t lostPictures { 0 }; // guarded by lock: held pictures older than one presented (never released)
    int64_t windowStartUS { 0 };
    uint64_t windowAUs { 0 }, windowPictures { 0 };
    double windowCallMS { 0 }, windowCallMaxMS { 0 }, windowSlotWaitMS { 0 };
    unsigned windowCalls { 0 };
    int firstRC { 0 };

    // What this decoder counts against the decoder budget (openHardware).
    bool budgeted { false };
    double budgetDirectMB { 0 }, budgetFlexibleMB { 0 };

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

// SPS (H.264), VPS/SPS (HEVC): the sets that describe the sequence (size,
// profile, reference frames); a PPS may change without a new sequence.
static bool isSequenceSet(unsigned codec, uint8_t header)
{
    if (codec == PS5WK_VCODEC_H264)
        return (header & 0x1f) == 7;
    unsigned type = (header >> 1) & 0x3f;
    return type == 32 || type == 33;
}

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
static std::vector<uint8_t> paramSetsOf(unsigned codec, const uint8_t* p, size_t n, bool sequenceOnly = false)
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
        if (end > i && (sequenceOnly ? isSequenceSet(codec, p[i]) : isParamSet(codec, p[i]))) {
            static const uint8_t startCode[4] = { 0, 0, 0, 1 };
            std::vector<uint8_t> unit(startCode, startCode + 4);
            unit.insert(unit.end(), p + i, p + end);
            // The sequence sets once each (codec_data's copy in front of an
            // identical in-band one is the same sequence).
            if (!sequenceOnly || std::search(out.begin(), out.end(), unit.begin(), unit.end()) == out.end())
                out.insert(out.end(), unit.begin(), unit.end());
        }
        i = next;
    }
    return out;
}

// The NAL unit types of an Annex-B access unit, for the log ("7 8 5" or
// "33 34 19 ..."), at most `max` of them.
static std::string nalTypesOf(unsigned codec, const uint8_t* p, size_t n, unsigned max = 12)
{
    std::string out;
    unsigned count = 0;
    for (size_t k = 0; k + 3 < n; k++) {
        if (p[k] || p[k + 1] || p[k + 2] != 1)
            continue;
        uint8_t header = p[k + 3];
        unsigned type = codec == PS5WK_VCODEC_H264 ? header & 0x1f : (header >> 1) & 0x3f;
        if (count++ == max) {
            out += " ...";
            break;
        }
        char b[8];
        snprintf(b, sizeof b, "%s%u", out.empty() ? "" : " ", type);
        out += b;
        k += 3;
    }
    return out.empty() ? std::string("none") : out;
}

// H.264 SPS -> one line for the log: profile, level, size, reference frames
// and the DPB the level allows at that size (Videodec2 sizes its DPB from
// the level it is opened with: a stream asking for more references than
// its level allows is not conforming).
namespace {
struct BitReader {
    std::vector<uint8_t> rbsp;
    size_t bit { 0 };
    bool overrun { false };
    BitReader(const uint8_t* p, size_t n)
    {
        unsigned zeros = 0;
        for (size_t i = 0; i < n; i++) {
            if (zeros >= 2 && p[i] == 3) {
                zeros = 0; // emulation prevention byte
                continue;
            }
            rbsp.push_back(p[i]);
            zeros = p[i] ? 0 : zeros + 1;
        }
    }
    unsigned u(unsigned bits)
    {
        unsigned v = 0;
        for (unsigned i = 0; i < bits; i++) {
            if (bit >= rbsp.size() * 8) {
                overrun = true;
                return v;
            }
            v = v << 1 | ((rbsp[bit >> 3] >> (7 - (bit & 7))) & 1);
            bit++;
        }
        return v;
    }
    unsigned ue()
    {
        unsigned zeros = 0;
        while (!u(1) && !overrun && zeros < 32)
            zeros++;
        return zeros ? (1u << zeros) - 1 + u(zeros) : 0;
    }
    int se()
    {
        unsigned v = ue();
        return v & 1 ? static_cast<int>((v + 1) / 2) : -static_cast<int>(v / 2);
    }
};
}

static unsigned h264MaxDpbMbs(unsigned level)
{
    switch (level) {
    case 9: case 10: return 396;
    case 11: return 900;
    case 12: case 13: case 20: return 2376;
    case 21: return 4752;
    case 22: case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40: case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    case 51: case 52: return 184320;
    default: return level >= 60 ? 696320 : 0;
    }
}

static std::string describeH264SPS(const uint8_t* nal, size_t n)
{
    // nal: the SPS NAL unit after its start code (header byte first).
    if (n < 4)
        return "short SPS";
    BitReader b(nal + 1, n - 1);
    unsigned profile = b.u(8), constraints = b.u(8), level = b.u(8), id = b.ue();
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86
        || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        chroma = b.ue();
        if (chroma == 3)
            b.u(1);
        b.ue();
        b.ue();
        b.u(1);
        if (b.u(1)) {
            for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); i++) {
                if (!b.u(1))
                    continue;
                int last = 8, next = 8;
                for (unsigned j = 0; j < (i < 6 ? 16u : 64u); j++) {
                    if (next)
                        next = (last + b.se() + 256) % 256;
                    last = next ? next : last;
                }
            }
        }
    }
    unsigned frameNumBits = b.ue() + 4, pocType = b.ue();
    if (!pocType)
        b.ue();
    else if (pocType == 1) {
        b.u(1);
        b.se();
        b.se();
        unsigned cycle = b.ue();
        for (unsigned i = 0; i < cycle && i < 256; i++)
            b.se();
    }
    unsigned refs = b.ue(), gaps = b.u(1);
    unsigned widthMbs = b.ue() + 1, heightMapUnits = b.ue() + 1, frameMbsOnly = b.u(1);
    if (!frameMbsOnly)
        b.u(1);
    b.u(1);
    if (b.u(1)) {
        b.ue();
        b.ue();
        b.ue();
        b.ue();
    }
    int reorder = -1, maxDecFrames = -1;
    if (b.u(1)) { // VUI
        if (b.u(1) && b.u(8) == 255)
            b.u(32);
        if (b.u(1))
            b.u(1);
        if (b.u(1)) {
            b.u(4);
            if (b.u(1))
                b.u(24);
        }
        if (b.u(1)) {
            b.ue();
            b.ue();
        }
        if (b.u(1)) {
            b.u(32);
            b.u(32);
            b.u(1);
        }
        auto hrd = [&] {
            unsigned count = b.ue() + 1;
            b.u(8);
            for (unsigned i = 0; i < count && i < 32; i++) {
                b.ue();
                b.ue();
                b.u(1);
            }
            b.u(20);
        };
        bool nal = b.u(1);
        if (nal)
            hrd();
        bool vcl = b.u(1);
        if (vcl)
            hrd();
        if (nal || vcl)
            b.u(1);
        b.u(1);
        if (b.u(1)) {
            b.u(1);
            b.ue();
            b.ue();
            b.ue();
            b.ue();
            reorder = static_cast<int>(b.ue());
            maxDecFrames = static_cast<int>(b.ue());
        }
    }
    unsigned heightMbs = heightMapUnits * (frameMbsOnly ? 1 : 2);
    unsigned dpbMbs = h264MaxDpbMbs(level == 11 && (constraints & 0x10) && profile != 100 ? 9 : level);
    unsigned dpbFrames = dpbMbs && widthMbs * heightMbs ? std::min(16u, dpbMbs / (widthMbs * heightMbs)) : 0;
    char line[320];
    snprintf(line, sizeof line,
        "SPS %u: profile %u (constraints 0x%02x) level %u.%u, %ux%u MBs (%ux%u)%s, %u reference frames%s, frame_num %u bits, POC type %u; "
        "VUI reorder %d, max_dec_frame_buffering %d; the level's DPB holds %u frames at this size%s%s",
        id, profile, constraints, level / 10, level % 10, widthMbs, heightMbs, widthMbs * 16, heightMbs * 16, frameMbsOnly ? "" : " interlaced", refs,
        gaps ? " (gaps allowed)" : "", frameNumBits, pocType, reorder, maxDecFrames, dpbFrames,
        dpbFrames && (refs > dpbFrames || maxDecFrames > static_cast<int>(dpbFrames)) ? " (MORE THAN THE LEVEL ALLOWS)" : "",
        b.overrun ? " (truncated)" : "");
    return line;
}

// The SPSs of an Annex-B buffer, described (H.264 only).
static void logSequence(const char* prefix, uint32_t id, unsigned codec, const std::vector<uint8_t>& sets)
{
    if (codec != PS5WK_VCODEC_H264)
        return;
    const uint8_t* p = sets.data();
    size_t n = sets.size();
    for (size_t k = 0; k + 3 < n; k++) {
        if (p[k] || p[k + 1] || p[k + 2] != 1)
            continue;
        size_t start = k + 3, end = start;
        while (end + 2 < n && !(p[end] == 0 && p[end + 1] == 0 && (p[end + 2] == 1 || (p[end + 2] == 0 && end + 3 < n && p[end + 3] == 1))))
            end++;
        if (end + 2 >= n)
            end = n;
        if ((p[start] & 0x1f) == 7)
            DLOG("decoder %u: %s%s", id, prefix, describeH264SPS(p + start, end - start).c_str());
        k = end - 1;
    }
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

// A free slot, waiting for one; -1 when the decoder is closing or resetting.
//
// A picture handed to the WebProcess (PICTURE) is the WebProcess's until it
// gives it back: ps5vdecproxy releases pictures it won't output (PRELEASE),
// every placeholder buffer that is freed before ps5videosink got it to the
// title releases its picture (PRELEASE, from wherever GStreamer frees it),
// and pictures the sink did hand over come back from the consumer once
// presented or skipped. A flush (DFLUSH) or the connection closing takes
// back everything. So no free slot just means that the pictures decoded
// ahead are waiting: a paused, prerolled or hidden (suspended) video holds
// up to the vqueue and sink's buffers plus the pictures it hasn't read yet,
// and the decoder waits until the video plays, flushes or closes. (Until
// 2026-11-01 a 1.5 s timeout took back the older half of the held pictures
// here. They were still the WebProcess's: when the video played, its frames
// named pictures that had been overwritten, the title skipped them, and the
// decoder had thrown away whole runs of pictures, 6 every 1.5-2 s while a
// feed's paused and hidden players waited.)
static int waitFreeSlot(Decoder& d)
{
    int64_t start = nowUS();
    g_mutex_lock(&d.lock);
    int found = -1;
    bool reported = false;
    for (;;) {
        if (d.closing || d.resetPending)
            break;
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
        if (!reported && nowUS() - start > 10000000) {
            reported = true;
            unsigned held = 0, pinned = 0;
            for (auto& s : d.slots) {
                held += s.state == Slot::Held;
                pinned += s.pinned;
            }
            d.longWaits++;
            DLOG("decoder %u: every output slot held for 10 s (%u held, %u being drawn; %zu AUs queued): the video is paused or hidden, "
                 "decoding goes on when it plays, flushes or closes",
                d.id, held, pinned, d.queue.size());
        }
        g_cond_wait_until(&d.cond, &d.lock, g_get_monotonic_time() + 1000000);
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
        uint64_t lost;
        g_mutex_lock(&d.lock);
        for (auto& s : d.slots) {
            held += s.state == Slot::Held;
            pinned += s.pinned;
        }
        queued = d.queue.size();
        lost = d.lostPictures;
        g_mutex_unlock(&d.lock);
        DLOG("decoder %u (pid %d): %.1f AUs/s in, %.1f pictures/s out; decode call %.2f ms avg %.2f max, waited %.0f ms for slots; "
             "%u/%zu slots held (%u pinned), %zu AUs queued; so far %llu pictures, %llu hidden, %llu errors, %llu skipped, %llu without pts, "
             "%llu resets after errors, %llu flagged, %llu lost without a release",
            d.id, d.pid, d.windowAUs / seconds, d.windowPictures / seconds, d.windowCalls ? d.windowCallMS / d.windowCalls : 0.0,
            d.windowCallMaxMS, d.windowSlotWaitMS, held, d.slots.size(), pinned, queued, (unsigned long long)d.pictures,
            (unsigned long long)d.hidden, (unsigned long long)d.errors, (unsigned long long)d.skipped, (unsigned long long)d.noPTS,
            (unsigned long long)d.recoveries, (unsigned long long)d.flagged, (unsigned long long)lost);
    }
    d.windowStartUS = now;
    d.windowAUs = d.windowPictures = 0;
    d.windowCallMS = d.windowCallMaxMS = d.windowSlotWaitMS = 0;
    d.windowCalls = 0;
}

// Videodec2's error states (libSceVideodec2 13.60, read from the
// disassembly): sceVideodec2Decode keeps three flags in the decoder object
// (+0x48, +0x4c, +0x50). Once a decode sets one, every later Decode returns
// its code at once, without decoding, until sceVideodec2Reset clears them
// (Reset zeroes +0x44..+0x53; a new decoder starts clear):
//   0x811d0300  VdecCore 0x80c0000f, its decode status 0x18 (hardware
//               result 9): a new sequence without a reset (BBC's in-band
//               resolution switch, 2026-10-28)
//   0x811d0304  VdecCore 0x80c00013, its decode status 0x15 or 0x1a
//               (hardware result 3 or 11): the 4K60 H.264 HLS freeze of
//               2026-10-30, every AU failed in 0.08 ms for minutes
//   0x811d0100  Decode after a Flush without a Reset
// Other decode errors (0x811d0301-0303: an IRAP refused, ...) are per AU.
static bool stickyError(int rc)
{
    unsigned u = static_cast<unsigned>(rc);
    return u == 0x811d0300u || u == 0x811d0304u || u == 0x811d0100u;
}

// Decode calls in a row that may fail with a per-AU error before the decoder
// is reset anyway (and decoding resumes at the next keyframe).
static constexpr unsigned maxErrorRun = 8;

static bool decodeUnit(Decoder& d, const uint8_t* data, size_t size, uint64_t pts, int64_t ptsNS, uint32_t epoch, bool keyframe)
{
    int64_t waitStart = nowUS();
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
    d.ausSinceReset++;
    if (rc) {
        if (!d.errors++)
            d.firstRC = rc;
        if (!d.errorRun++) {
            // The first failure after decoding went well: what led to it.
            unsigned held = 0;
            g_mutex_lock(&d.lock);
            for (auto& s : d.slots)
                held += s.state == Slot::Held;
            g_mutex_unlock(&d.lock);
            DLOG("decoder %u: decode rc 0x%08x%s at key %u (%s, %zu bytes, NAL types %s) after %llu AUs and %llu pictures since the last reset; "
                 "this call %.2f ms, waited %.1f ms for a slot (previous call %.2f ms, waited %.1f ms), %u/%zu slots held, %llu pictures flagged so far",
                d.id, static_cast<unsigned>(rc), stickyError(rc) ? " (sticky until a reset)" : "", static_cast<unsigned>(pts),
                keyframe ? "keyframe" : "not a keyframe", size, nalTypesOf(d.codec, data, size).c_str(), (unsigned long long)d.ausSinceReset,
                (unsigned long long)d.picturesSinceReset, ms, (start - waitStart) / 1000.0, d.lastCallMS, d.lastSlotWaitMS, held, d.slots.size(),
                (unsigned long long)d.flagged);
        } else if (d.errors <= 5 || !(d.errors % 100))
            DLOG("decoder %u: decode rc 0x%08x (%llu errors; AU %zu bytes, key %u)", d.id, static_cast<unsigned>(rc),
                (unsigned long long)d.errors, size, static_cast<unsigned>(pts));
        if (!d.injectNoRecovery && (stickyError(rc) || d.errorRun >= maxErrorRun)) {
            d.hwStuck = true;
            d.stuckRC = rc;
        }
    } else
        d.errorRun = 0;
    d.lastCallMS = ms;
    d.lastSlotWaitMS = (start - waitStart) / 1000.0;
    if (pic.valid) {
        d.picturesSinceReset++;
        if (pic.error && !d.flagged++)
            DLOG("decoder %u: first picture flagged with an error by the decoder (key %u)", d.id, static_cast<unsigned>(pic.pts));
    }
    handlePicture(d, pic, slot, epoch);
    return !rc;
}

// The decoder fails every call (a sticky error, or maxErrorRun failures in
// a row): what is in flight is lost; a fresh decoder (vdec2_renew: delete,
// create, reset) takes the stream again at the next keyframe, with the
// cached parameter sets in front if it carries none. Without this the
// picture froze for good while the audio went on (2026-10-30), until a
// variant switch happened to drain and reset the decoder.
static void recover(Decoder& d, uint32_t epoch)
{
    d.hwStuck = false;
    d.recoveries++;
    std::vector<uint32_t> lost;
    for (auto& [key, ptsNS] : d.outstanding)
        lost.push_back(key);
    for (auto key : lost)
        sendPicture(d, key, epoch, 3, nullptr);
    d.outstanding.clear();
    int64_t start = nowUS();
    int rc = backend->reset(d.hw);
    if (d.recoveries <= 10 || !(d.recoveries % 50))
        DLOG("decoder %u: reset after rc 0x%08x (%u failed calls in a row, %llu AUs since the last reset): %zu pictures in flight dropped, "
             "reset rc 0x%08x in %.1f ms; decoding resumes at a keyframe (%llu recoveries so far)",
            d.id, static_cast<unsigned>(d.stuckRC), d.errorRun, (unsigned long long)d.ausSinceReset, lost.size(), static_cast<unsigned>(rc),
            (nowUS() - start) / 1000.0, (unsigned long long)d.recoveries);
    d.errorRun = 0;
    d.ausSinceReset = d.picturesSinceReset = 0;
    d.needKeyframe = true;
}

static void drain(Decoder&, uint32_t epoch);

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
            ok &= decodeUnit(d, p, n, au.key | (show ? 0 : hiddenFlag), au.ptsNS, au.epoch, keyframe);
            if (d.hwStuck)
                break;
        }
        if (!shown || !ok)
            sendPicture(d, au.key, au.epoch, shown ? 3 : 1, nullptr);
        if (d.hwStuck)
            recover(d, au.epoch);
        return;
    }

    // H.264 / HEVC (Annex-B): start at a keyframe with its parameter sets.
    if (d.codec == PS5WK_VCODEC_H264) {
        // Diagnostics: an AU holding more than one picture's start (two
        // access unit delimiters, or IDR and non-IDR slices together), or a
        // non-keyframe carrying an SPS.
        unsigned auds = 0, idr = 0, nonIdr = 0, sps = 0;
        const uint8_t* p = au.data.data();
        size_t n = au.data.size();
        for (size_t k = 0; k + 3 < n; k++) {
            if (p[k] || p[k + 1] || p[k + 2] != 1)
                continue;
            unsigned type = p[k + 3] & 0x1f;
            auds += type == 9;
            idr += type == 5;
            nonIdr += type == 1;
            sps += type == 7;
            k += 3;
        }
        if (auds > 1 || (idr && nonIdr) || (sps && !keyframe) || (idr && !keyframe)) {
            static unsigned reported;
            if (reported++ < 20)
                DLOG("decoder %u: odd AU %u (%s, %zu bytes): NAL types %s", d.id, au.key, keyframe ? "keyframe" : "not a keyframe", n,
                    nalTypesOf(d.codec, p, n, 24).c_str());
        }
    }
    if (d.needKeyframe && !keyframe) {
        d.skipped++;
        sendPicture(d, au.key, au.epoch, 2, nullptr);
        return;
    }
    auto sets = paramSetsOf(d.codec, au.data.data(), au.data.size());
    if (!sets.empty()) {
        // A new sequence (another SPS: a resolution switch inside the
        // stream, as adaptive players make them) needs the hardware drained
        // and reset first: Videodec2 failed every decode call after an
        // in-band H.264 switch from 396x704 to 540x960 without it
        // (0x811d0300, BBC News, 2026-10-28), whether or not the caps
        // changed. Repeated identical SPSs don't count.
        auto sequence = keyframe ? paramSetsOf(d.codec, sets.data(), sets.size(), true) : std::vector<uint8_t>();
        if (!sequence.empty()) {
            if (sequence != d.sequenceSets)
                logSequence(d.sequenceSets.empty() ? "" : "new ", d.id, d.codec, sequence);
            if (!d.sequenceSets.empty() && sequence != d.sequenceSets && !d.needKeyframe && !d.injectNoSequenceReset) {
                d.formatChanges++;
                DLOG("decoder %u: new sequence parameters at AU %u: drained and reset (%llu so far)", d.id, au.key,
                    (unsigned long long)d.formatChanges);
                drain(d, au.epoch);
            }
            d.sequenceSets = std::move(sequence);
        }
        d.paramSets = sets;
    }
    bool prepended = false;
    if (sets.empty() && d.needKeyframe && !d.paramSets.empty()) {
        au.data.insert(au.data.begin(), d.paramSets.begin(), d.paramSets.end());
        prepended = true;
    }
    d.needKeyframe = false;
    d.outstanding[au.key] = au.ptsNS;
    if (au.key && (au.key == d.injectCorruptKey || au.key == d.injectTruncateKey) && au.data.size() > 64) {
        // Test knob: damage this AU's slice data (the last NAL unit's second
        // half), or cut it in half, as a lost or truncated segment would.
        size_t size = au.data.size();
        if (au.key == d.injectTruncateKey)
            au.data.resize(size / 2);
        else
            for (size_t i = size / 2; i < size; i++)
                au.data[i] = static_cast<uint8_t>(i * 2654435761u >> 13) | 1;
        DLOG("decoder %u: AU %u %s (/data/ps5webkit/vdec-inject)", d.id, au.key, au.key == d.injectTruncateKey ? "cut in half" : "damaged");
    }
    bool ok = decodeUnit(d, au.data.data(), au.data.size(), au.key, au.ptsNS, au.epoch, keyframe);
    if (!ok && d.hwStuck && keyframe) {
        // The keyframe itself met the stuck decoder (a sticky error answers
        // at once, without decoding): a fresh decoder takes it again, once,
        // so no GOP is lost (BBC's switch without the drain, 0x811d0300,
        // decoded at the retry).
        d.outstanding.erase(au.key);
        recover(d, au.epoch);
        if (sets.empty() && !prepended && !d.paramSets.empty())
            au.data.insert(au.data.begin(), d.paramSets.begin(), d.paramSets.end());
        d.needKeyframe = false;
        d.outstanding[au.key] = au.ptsNS;
        ok = decodeUnit(d, au.data.data(), au.data.size(), au.key, au.ptsNS, au.epoch, keyframe);
        if (d.recoveries <= 10 || !(d.recoveries % 50))
            DLOG("decoder %u: keyframe %u again on the fresh decoder: %s", d.id, au.key, ok ? "decoded" : "failed too");
    }
    if (!ok)
        sendPicture(d, au.key, au.epoch, 3, nullptr);
    if (d.hwStuck)
        recover(d, au.epoch);
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
    d.hwStuck = false;
    d.errorRun = 0;
    d.ausSinceReset = d.picturesSinceReset = 0;
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
            d->hwStuck = false;
            d->errorRun = 0;
            d->ausSinceReset = d->picturesSinceReset = 0;
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

// ---------------------------------------------------------------- budget
//
// Decoder admission (COMPOSITING-PLAN.md, M6). A feed that starts 60 muted
// autoplay videos at once opened 170 decoders: 5.1-7.7 GB of direct memory
// at the load (VP9 decoders are opened at 3840x2160 for adaptive streams:
// 535 MB each plus 12 x 12 MB output buffers), flexible memory down to 5 MB,
// then MapNamedFlexibleMemory failures. WebKit suspends the off-screen ones
// (closing their decoders) only once it knows they are off screen. Now:
// - At most `decoders` hardware decoders at once, their memory (decoder +
//   output buffers, as measured at open) within `direct` MB, and opening
//   one must leave `reserve` MB of flexible memory free.
// - A DOPEN over budget waits up to `wait` ms for decoders to close (the
//   off-screen videos WebKit is suspending), then is refused: the proxy
//   falls back to software decoding for that pipeline run only (decodebin3
//   drops its decoder at PAUSED -> READY, so a suspended video that comes
//   back asks again).
// - VP9 decoders are sized to the stream (at least 1920x1088); 4K headroom
//   only for streams above 1080p and while it fits in half the budget. A
//   stream that later grows past its decoder reopens it (the proxy drains
//   first).
// /data/ps5webkit/vdec-budget (read at each DOPEN) overrides, e.g.
// "decoders=8 direct=2048 reserve=64 wait=500 vp9headroom=1 h264small=1".

extern "C" double ps5wk_title_direct_mb(void) __attribute__((weak)); // titlemem.c (the title only)
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t*) __attribute__((weak));

struct BudgetLimits {
    unsigned decoders { 8 };
    double directMB { 2048 };
    double reserveMB { 64 };
    int waitMS { 500 };
    int vp9Headroom { 1 }; // 0: never 4K headroom, 1: streams above 1080p, 2: every VP9 stream
    int h264Small { 1 }; // 1: H.264 streams up to 640x480 get a decoder of their own size; 0: 1920x1088
};

struct Cost {
    double directMB { 0 }, flexibleMB { 0 };
};

static GMutex budgetLock;
// Guarded by budgetLock: what the open decoders (and opens in progress) count.
static unsigned budgetDecoders;
static double budgetDirectMB, budgetPendingFlexibleMB;
static unsigned budgetPeakDecoders;
static double budgetPeakDirectMB;
static uint64_t budgetRefused, budgetWaited, budgetSmallVP9;
static BudgetLimits budgetLastLimits; // as read at the last DOPEN (the overlay)
static std::map<uint64_t, Cost> measuredCosts; // (codec, depth, size) -> cost measured at open
// Opens one at a time, so the memory measured around one is its own.
static GMutex openLock;

static BudgetLimits readBudgetLimits()
{
    BudgetLimits limits;
    FILE* f = fopen("/data/ps5webkit/vdec-budget", "r");
    if (!f)
        return limits;
    char word[64];
    while (fscanf(f, "%63s", word) == 1) {
        unsigned u;
        double v;
        int i;
        if (sscanf(word, "decoders=%u", &u) == 1)
            limits.decoders = u;
        else if (sscanf(word, "direct=%lf", &v) == 1)
            limits.directMB = v;
        else if (sscanf(word, "reserve=%lf", &v) == 1)
            limits.reserveMB = v;
        else if (sscanf(word, "wait=%d", &i) == 1)
            limits.waitMS = std::max(0, i);
        else if (sscanf(word, "vp9headroom=%d", &i) == 1)
            limits.vp9Headroom = i;
        else if (sscanf(word, "h264small=%d", &i) == 1)
            limits.h264Small = i;
    }
    fclose(f);
    return limits;
}

static double flexibleFreeMB()
{
    size_t size = 0;
    if (!sceKernelAvailableFlexibleMemorySize || sceKernelAvailableFlexibleMemorySize(&size))
        return 1e9; // unknown: no flexible limit
    return size / 1048576.0;
}

static double directInUseMB()
{
    return ps5wk_title_direct_mb ? ps5wk_title_direct_mb() : -1;
}

static uint64_t costKey(unsigned codec, unsigned depth, int width, int height)
{
    return static_cast<uint64_t>(codec) << 56 | static_cast<uint64_t>(depth) << 48 | static_cast<uint64_t>(width & 0xffffff) << 24 | (height & 0xffffff);
}

// Measured at an earlier open of the same codec and size, else scaled from
// what Videodec2 took in the logged runs (README, Step 2b): H.264 1920x1088
// 70 MB + 12 x 3.2 MB direct, 24 MB flexible; VP9 3840x2160 535 + 12 x 12 MB,
// 13 MB; HEVC 3840x2160 337 + 12 x 12 MB (10-bit 512 + 9 x 24), 77 MB.
static Cost estimateCost(unsigned codec, unsigned depth, int width, int height)
{
    g_mutex_lock(&budgetLock);
    auto it = measuredCosts.find(costKey(codec, depth, width, height));
    bool known = it != measuredCosts.end();
    Cost cost = known ? it->second : Cost { };
    g_mutex_unlock(&budgetLock);
    if (known)
        return cost;
    double area = static_cast<double>(width) * height;
    double refArea = 3840.0 * 2160;
    // HEVC above 4K: the title opens it at pipeline depth 2 (titlevdec.c),
    // which takes far less than the 4K figures scaled by area: 7680x3840
    // took 139 MB flexible and 941 MB + 9 x 42 MB direct, not 274 and 1707
    // (which refused every 8K clip: the title has ~270 MB flexible free).
    // A line through the two depth-2 opens in the logs, 4320x2160 (68 MB,
    // 471 + 9 x 13.6 MB) and 7680x3840. 10-bit direct memory wasn't
    // measured at depth 2: it stays scaled from 4K.
    if (codec == PS5WK_VCODEC_HEVC && area > 3840.0 * 2176) {
        double megapixels = area / 1e6;
        cost.flexibleMB = 35 + 3.55 * megapixels;
        cost.directMB = depth > 8 ? 730 * area / refArea : 256 + 36.1 * megapixels;
        return cost;
    }
    if (codec == PS5WK_VCODEC_H264) {
        refArea = 1920.0 * 1088;
        cost = { 110, 24 };
    } else if (codec == PS5WK_VCODEC_VP9)
        cost = { 680, 13 };
    else
        cost = depth > 8 ? Cost { 730, 77 } : Cost { 480, 77 };
    double scale = area / refArea;
    cost.directMB *= std::max(0.25, scale);
    cost.flexibleMB *= std::max(0.35, scale);
    return cost;
}

// Reserves `cost` for `d`, waiting up to limits.waitMS for room. False:
// over budget (why says how).
static bool admit(Decoder& d, const BudgetLimits& limits, const Cost& cost, char* why, size_t whySize)
{
    int64_t start = nowUS(), deadline = start + static_cast<int64_t>(limits.waitMS) * 1000;
    bool waited = false;
    for (;;) {
        double flexible = flexibleFreeMB();
        g_mutex_lock(&budgetLock);
        budgetLastLimits = limits;
        bool countOK = budgetDecoders < limits.decoders;
        bool directOK = budgetDirectMB + cost.directMB <= limits.directMB;
        bool flexibleOK = flexible - budgetPendingFlexibleMB - cost.flexibleMB >= limits.reserveMB;
        if (countOK && directOK && flexibleOK) {
            budgetDecoders++;
            budgetDirectMB += cost.directMB;
            budgetPendingFlexibleMB += cost.flexibleMB;
            budgetPeakDecoders = std::max(budgetPeakDecoders, budgetDecoders);
            budgetPeakDirectMB = std::max(budgetPeakDirectMB, budgetDirectMB);
            budgetWaited += waited;
            g_mutex_unlock(&budgetLock);
            d.budgeted = true;
            d.budgetDirectMB = cost.directMB;
            d.budgetFlexibleMB = cost.flexibleMB;
            if (waited)
                DLOG("decoder for pid %d: waited %.0f ms for the decoder budget", d.pid, (nowUS() - start) / 1000.0);
            return true;
        }
        if (nowUS() >= deadline) {
            budgetRefused++;
            snprintf(why, whySize, "%s%s%s (%u/%u decoders, %.0f + %.0f of %.0f MB direct, flexible %.0f MB free, %.0f needed + %.0f kept)",
                countOK ? "" : "decoders", !directOK ? (countOK ? "direct memory" : ", direct memory") : "",
                !flexibleOK ? (countOK && directOK ? "flexible memory" : ", flexible memory") : "", budgetDecoders, limits.decoders,
                budgetDirectMB, cost.directMB, limits.directMB, flexible - budgetPendingFlexibleMB, cost.flexibleMB, limits.reserveMB);
            g_mutex_unlock(&budgetLock);
            return false;
        }
        g_mutex_unlock(&budgetLock);
        waited = true;
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }
}

// The open finished (measured: the decoder's real cost) or failed.
static void settleBudget(Decoder& d, const Cost* measured)
{
    if (!d.budgeted)
        return;
    g_mutex_lock(&budgetLock);
    budgetPendingFlexibleMB -= d.budgetFlexibleMB;
    if (!measured) {
        budgetDecoders--;
        budgetDirectMB -= d.budgetDirectMB;
        d.budgeted = false;
    } else {
        budgetDirectMB += measured->directMB - d.budgetDirectMB;
        budgetPeakDirectMB = std::max(budgetPeakDirectMB, budgetDirectMB);
        d.budgetDirectMB = measured->directMB;
        d.budgetFlexibleMB = measured->flexibleMB;
    }
    g_mutex_unlock(&budgetLock);
}

void budget(Budget& out)
{
    g_mutex_lock(&budgetLock);
    out = { budgetDecoders, budgetLastLimits.decoders, budgetDirectMB, budgetLastLimits.directMB, budgetRefused };
    g_mutex_unlock(&budgetLock);
}

static void releaseBudget(Decoder& d)
{
    if (!d.budgeted)
        return;
    g_mutex_lock(&budgetLock);
    budgetDecoders--;
    budgetDirectMB = std::max(0.0, budgetDirectMB - d.budgetDirectMB);
    g_mutex_unlock(&budgetLock);
    d.budgeted = false;
}

static std::string budgetSummary()
{
    char line[200];
    g_mutex_lock(&budgetLock);
    snprintf(line, sizeof line, "budget %u decoders, %.0f MB (peak %u, %.0f MB; %llu refused, %llu waited, %llu VP9 sized to the stream)", budgetDecoders,
        budgetDirectMB, budgetPeakDecoders, budgetPeakDirectMB, (unsigned long long)budgetRefused, (unsigned long long)budgetWaited,
        (unsigned long long)budgetSmallVP9);
    g_mutex_unlock(&budgetLock);
    return line;
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
    releaseBudget(d);
}

static unsigned openDecoders()
{
    g_mutex_lock(&decodersLock);
    unsigned n = decoders.size();
    g_mutex_unlock(&decodersLock);
    return n;
}

// Test knobs, /data/ps5webkit/vdec-inject (words, read at each DOPEN):
//   noseqreset   don't drain and reset at a new in-band SPS (reproduces
//                Videodec2's sticky 0x811d0300 with switch-h264.mp4)
//   norecover    don't reset a decoder that fails every call (the old way)
//   corrupt=<key> / truncate=<key>   damage / cut that AU (H.264, HEVC)
static void readInjectKnobs(Decoder& d)
{
    d.injectNoSequenceReset = d.injectNoRecovery = false;
    d.injectCorruptKey = d.injectTruncateKey = 0;
    FILE* f = fopen("/data/ps5webkit/vdec-inject", "r");
    if (!f)
        return;
    char word[64];
    while (fscanf(f, "%63s", word) == 1) {
        unsigned key = 0;
        if (!strcmp(word, "noseqreset"))
            d.injectNoSequenceReset = true;
        else if (!strcmp(word, "norecover"))
            d.injectNoRecovery = true;
        else if (sscanf(word, "corrupt=%u", &key) == 1)
            d.injectCorruptKey = key;
        else if (sscanf(word, "truncate=%u", &key) == 1)
            d.injectTruncateKey = key;
    }
    fclose(f);
    DLOG("decoder for pid %d: /data/ps5webkit/vdec-inject:%s%s corrupt key %u, truncate key %u", d.pid, d.injectNoSequenceReset ? " noseqreset" : "",
        d.injectNoRecovery ? " norecover" : "", d.injectCorruptKey, d.injectTruncateKey);
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
    // Test knob: /data/ps5webkit/vdec-refuse holding "<width>x<height>"
    // refuses that size, as Videodec2 does some (the WebProcess must then
    // decode it in software).
    if (FILE* knob = fopen("/data/ps5webkit/vdec-refuse", "r")) {
        int refuseWidth = 0, refuseHeight = 0;
        bool match = fscanf(knob, "%dx%d", &refuseWidth, &refuseHeight) == 2 && refuseWidth == width && refuseHeight == height;
        fclose(knob);
        if (match) {
            DLOG("decoder for pid %d: %dx%d refused (/data/ps5webkit/vdec-refuse)", d->pid, width, height);
            return 2;
        }
    }
    // Same decoder again (caps renegotiated, e.g. a resolution switch): keep
    // it if it covers the new size. (The proxy drained it first, which
    // resets the hardware; a new SPS in band resets it too, see processAU.)
    if (d->hw) {
        if (d->codec == codec && d->profile == profile && d->bitDepth == depth && width <= d->width && height <= d->height) {
            DLOG("decoder %u: %dx%d fits the decoder (%dx%d); kept", d->id, width, height, d->width, d->height);
            return 0;
        }
        DLOG("decoder %u: reopening for %dx%d (was %dx%d)", d->id, width, height, d->width, d->height);
        closeHardware(*d);
    }

    // Room for adaptive streams. VP9 is cheap at 4K in flexible memory (13
    // MB) but not in direct memory (535 MB + 12 x 12 MB output buffers, for
    // a 480x854 feed clip too), so a VP9 stream up to 1080p gets a
    // 1920x1088 decoder (a later switch above it reopens the decoder; the
    // proxy drains first), and one above 1080p 3840x2160 while that fits in
    // half the budget (vp9headroom=2: 4K for every VP9 stream, the old way).
    // H.264 up to 1080p costs 24 MB flexible and ~145 MB direct, as much for
    // a 364x208 hover preview: a page of them filled the budget with eight
    // previews (1.17 GB direct, the title down to 75 MB of flexible memory)
    // and sent every later one to software decoding in the WebProcess, which
    // then ran out of threads. H.264 up to 640x480 gets a decoder of its own
    // size (macroblock-rounded); a stream that grows past it reopens it, as
    // VP9 does, and if Videodec2 refuses the small size it is opened at
    // 1920x1088 as before. HEVC is sized as asked.
    BudgetLimits limits = readBudgetLimits();
    int openWidth = width, openHeight = height;
    if (codec == PS5WK_VCODEC_VP9 && openWidth * openHeight < 3840 * 2160) {
        Cost headroom = estimateCost(codec, depth, std::max(openWidth, 3840), std::max(openHeight, 2160));
        g_mutex_lock(&budgetLock);
        bool big = limits.vp9Headroom >= 2 || openWidth * openHeight > 1920 * 1088;
        bool roomy = big && limits.vp9Headroom && budgetDirectMB + headroom.directMB <= limits.directMB / 2;
        if (!roomy)
            budgetSmallVP9++;
        g_mutex_unlock(&budgetLock);
        if (roomy) {
            openWidth = std::max(openWidth, 3840);
            openHeight = std::max(openHeight, 2160);
        } else if (openWidth * openHeight <= 1920 * 1088) {
            openWidth = std::max(openWidth, 1920);
            openHeight = std::max(openHeight, 1088);
        }
    } else if (codec == PS5WK_VCODEC_H264 && openWidth * openHeight < 1920 * 1088) {
        if (limits.h264Small && openWidth * openHeight <= 640 * 480) {
            openWidth = (openWidth + 15) & ~15;
            openHeight = (openHeight + 15) & ~15;
        } else {
            openWidth = std::max(openWidth, 1920);
            openHeight = std::max(openHeight, 1088);
        }
    }

    const char* names[] = { "?", "H.264", "HEVC", "?", "VP9" };
    const char* codecName = names[codec < 5 ? codec : 0];
    char why[240] = "";
    if (!admit(*d, limits, estimateCost(codec, depth, openWidth, openHeight), why, sizeof why)) {
        DLOG("decoder for pid %d: %s %dx%d (stream %dx%d) refused, over the decoder budget after %d ms: %s; software decoding for this stream",
            d->pid, codecName, openWidth, openHeight, width, height, limits.waitMS, why);
        return 5;
    }

    char error[200] = "";
    size_t frameBytes = 0, frameAlign = 0;
    void* hw = nullptr;
    int64_t start = nowUS();
    g_mutex_lock(&openLock);
    double flexibleBefore = flexibleFreeMB(), directBefore = directInUseMB();
    // Another decoder may still be closing (switching videos): wait for it
    // rather than fall back to software.
    for (int attempt = 0; attempt < 30; attempt++) {
        hw = backend->open(codec, profile, depth, openWidth, openHeight, &frameBytes, &frameAlign, error, sizeof error);
        if (!hw && codec == PS5WK_VCODEC_H264 && openWidth * openHeight < 1920 * 1088) {
            DLOG("decoder for pid %d: H.264 at %dx%d refused (%s); opening at 1920x1088", d->pid, openWidth, openHeight, error);
            openWidth = std::max(openWidth, 1920);
            openHeight = std::max(openHeight, 1088);
            hw = backend->open(codec, profile, depth, openWidth, openHeight, &frameBytes, &frameAlign, error, sizeof error);
        }
        if (hw || !openDecoders())
            break;
        g_mutex_unlock(&openLock);
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
        g_mutex_lock(&openLock);
        flexibleBefore = flexibleFreeMB();
        directBefore = directInUseMB();
    }
    if (!hw) {
        g_mutex_unlock(&openLock);
        settleBudget(*d, nullptr);
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
        g_mutex_unlock(&openLock);
        for (auto& s : slots)
            provider.free(s.glBuffer, s.map, s.bytes);
        backend->close(hw);
        settleBudget(*d, nullptr);
        return 3;
    }
    // What this decoder really costs (decoder + output buffers), for the
    // budget and the next estimate of this codec and size.
    Cost measured { directBefore >= 0 ? directInUseMB() - directBefore : 0, flexibleBefore - flexibleFreeMB() };
    g_mutex_unlock(&openLock);
    size_t slotBytes = 0;
    for (auto& s : slots)
        slotBytes += s.bytes;
    // No measurement (payload), or another decoder's buffers were freed
    // meanwhile (the renderer frees them at the top of a frame): the
    // estimate, and never less than the output buffers.
    bool reliable = directBefore >= 0 && measured.directMB > slotBytes / 1048576.0 && measured.flexibleMB >= 0;
    if (!reliable)
        measured.directMB = std::max(estimateCost(codec, depth, openWidth, openHeight).directMB, slotBytes / 1048576.0);
    measured.flexibleMB = std::max(0.0, measured.flexibleMB);
    if (reliable) {
        g_mutex_lock(&budgetLock);
        measuredCosts[costKey(codec, depth, openWidth, openHeight)] = measured;
        g_mutex_unlock(&budgetLock);
    }
    settleBudget(*d, &measured);
    d->hw = hw;
    d->codec = codec;
    d->profile = profile;
    d->bitDepth = depth;
    d->width = openWidth;
    d->height = openHeight;
    d->frameBytes = frameBytes;
    d->frameAlign = frameAlign;
    d->needKeyframe = true;
    d->hwStuck = false;
    d->errorRun = 0;
    d->ausSinceReset = d->picturesSinceReset = 0;
    readInjectKnobs(*d);
    d->paramSets.clear();
    d->sequenceSets.clear();
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
    DLOG("decoder %u for pid %d: %s profile %u, %u-bit, %dx%d (stream %dx%d), up in %.0f ms; %zu output buffers of %.1f MB; costs %.0f MB direct, %.1f MB flexible; %s",
        d->id, d->pid, codecName, profile, depth, openWidth, openHeight, width, height, openMS, d->slots.size(),
        (frameBytes + frameAlign) / 1048576.0, d->budgetDirectMB, d->budgetFlexibleMB, budgetSummary().c_str());
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
    uint64_t auBytes = 0, refused = 0, preleases = 0;
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
            // Bounded by the proxy (at most 32 frames in flight). Never wait
            // here: the PRELEASEs that free output slots come on this socket.
            bool accept = d->hw && au.epoch == d->epoch && d->queue.size() < 256;
            if (accept) {
                d->queue.push_back(std::move(au));
                g_cond_broadcast(&d->cond);
            }
            g_mutex_unlock(&d->lock);
            if (!accept) {
                if (!refused++)
                    DLOG("decoder %u: AU refused (epoch %u, decoder at %u%s)", d->id, msg.ps5wk_epoch, d->epoch, !d->hw ? ", no decoder" : au.epoch == d->epoch ? ", 256 AUs queued" : "");
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
            preleases++;
            release(d->id, msg.ps5wk_token, false);
            break;
        default:
            break;
        }
    }
done:
    if (d->id || d->aus) { // a connection that never opened a decoder (no hardware) says nothing
        g_mutex_lock(&d->lock);
        uint64_t lost = d->lostPictures;
        g_mutex_unlock(&d->lock);
        DLOG("decoder %u from pid %d closed: %llu AUs (%.1f MB), %llu pictures, %llu hidden, %llu errors (first rc 0x%08x), %llu skipped before a keyframe, %llu refused, %llu format changes, %llu resets after errors; "
             "%llu PRELEASEs, %llu waits of 10 s for a slot, %llu pictures lost without a release",
            d->id, pid, (unsigned long long)d->aus, auBytes / 1048576.0, (unsigned long long)d->pictures, (unsigned long long)d->hidden,
            (unsigned long long)d->errors, static_cast<unsigned>(d->firstRC), (unsigned long long)d->skipped, (unsigned long long)refused,
            (unsigned long long)d->formatChanges, (unsigned long long)d->recoveries, (unsigned long long)preleases,
            (unsigned long long)d->longWaits, (unsigned long long)lost);
    }
    uint32_t closedID = d->id;
    bool hadDecoder = d->budgeted;
    closeHardware(*d);
    if (hadDecoder)
        DLOG("decoder %u released: %s", closedID, budgetSummary().c_str());
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
        if (s.token == token) {
            if (s.pinned && !wasPinned)
                continue; // being drawn: released after the swap
            s.state = Slot::Free;
            s.pinned = false;
        } else if (wasPinned && s.token < token && !s.pinned) {
            // Presented: tokens follow display order and the sink renders in
            // order, so an older picture still held here was lost on the way
            // without a release (a safety net: every path releases now).
            s.state = Slot::Free;
            d->lostPictures++;
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
    unsigned calls { 0 };
    bool stuck { false }; // as Videodec2's sticky errors: every call fails until a reset
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
    // PS5WK_FAKE_VDEC_STICKY=<n>: call n fails and so does every call after
    // it until a reset, with Videodec2's 0x811d0304 (the recovery path).
    static const char* sticky = g_getenv("PS5WK_FAKE_VDEC_STICKY");
    if (sticky && ++f->calls == static_cast<unsigned>(atoi(sticky)))
        f->stuck = true;
    if (f->stuck)
        return static_cast<int>(0x811d0304u);
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
    static_cast<FakeDecoder*>(h)->stuck = false;
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
