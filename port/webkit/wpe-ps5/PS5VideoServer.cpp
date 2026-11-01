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
#include "PS5VideoServer.h"

#include <algorithm>
#include <errno.h>
#include <memory>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define VLOG(...) g_message("[video] " __VA_ARGS__)

namespace PS5VideoServer {

static GMutex streamsLock;
static std::vector<std::shared_ptr<PS5VideoStream>> streams; // guarded by streamsLock
static void (*wakeFunction)();
static char socketPath[108];
static int nextStreamID;

static constexpr int64_t geometryCommitDelayUS = 100000;
static constexpr int64_t activeAfterFrameUS = 500000;

static void wake()
{
    if (wakeFunction)
        wakeFunction();
}

static int64_t nowUS()
{
    return g_get_monotonic_time();
}

// Reads one whole message; an fd passed with it (SCM_RIGHTS) goes to *passedFD.
static bool readMessage(int fd, ps5wk_video_msg& msg, int* passedFD)
{
    *passedFD = -1;
    auto* p = reinterpret_cast<char*>(&msg);
    size_t got = 0;
    while (got < sizeof msg) {
        struct iovec iov = { p + got, sizeof msg - got };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        mh.msg_control = control;
        mh.msg_controllen = sizeof control;
        ssize_t n = recvmsg(fd, &mh, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        for (struct cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                int received;
                memcpy(&received, CMSG_DATA(cm), sizeof(int));
                if (*passedFD >= 0)
                    close(*passedFD);
                *passedFD = received;
            }
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

// Pinned REMOTE pictures waiting for presented().
static GMutex pinnedLock;
static std::vector<std::pair<uint32_t, uint32_t>> pinnedPictures; // decoder, token

static void sendRelease(PS5VideoStream& stream, uint32_t slot, uint32_t generation)
{
    ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_RELEASE;
    msg.slot = slot;
    msg.generation = generation;
    msg.displayed = stream.displayed;
    msg.dropped = stream.dropped;
    if (stream.fd >= 0)
        send(stream.fd, &msg, sizeof msg, MSG_DONTWAIT);
}

// A frame leaves the queue without being drawn (dropped, flushed, closed).
static void discardFrame(PS5VideoStream& stream, const PS5VideoFrame& frame)
{
    if (frame.remote) {
        PS5VideoDecoder::release(frame.decoder, frame.token, false);
        sendRelease(stream, frame.token, 0xffffffffu); // counts only: no shared-memory slot
    } else
        sendRelease(stream, frame.slot, frame.generation);
}

static void* receiverThread(void* data)
{
    auto stream = *static_cast<std::shared_ptr<PS5VideoStream>*>(data);
    delete static_cast<std::shared_ptr<PS5VideoStream>*>(data);
    int fd = stream->fd;
    ps5wk_video_msg msg;
    int passed = -1;
    bool hello = false;
    while (readMessage(fd, msg, &passed)) {
        if (!hello) {
            if (msg.type != PS5WK_VMSG_HELLO || msg.magic != PS5WK_VIDEO_MAGIC || msg.version != PS5WK_VIDEO_VERSION) {
                VLOG("stream %d: bad hello (type %u magic 0x%x version %u)", stream->id, msg.type, msg.magic, msg.version);
                break;
            }
            hello = true;
            stream->pid = msg.pid;
            // Version 2: what the title can decode (ps5vdecproxy asks;
            // sinks ignore it).
            ps5wk_video_msg ack;
            memset(&ack, 0, sizeof ack);
            ack.type = PS5WK_VMSG_HELLO_ACK;
            ack.magic = PS5WK_VIDEO_MAGIC;
            ack.version = PS5WK_VIDEO_VERSION;
            ack.role = PS5VideoDecoder::codecs();
            int maxWidth, maxHeight;
            PS5VideoDecoder::maxSize(maxWidth, maxHeight);
            ack.width = static_cast<uint32_t>(maxWidth);
            ack.height = static_cast<uint32_t>(maxHeight);
            send(fd, &ack, sizeof ack, 0);
            if (msg.role == PS5WK_VROLE_QUERY) {
                g_mutex_lock(&streamsLock);
                streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
                g_mutex_unlock(&streamsLock);
                close(fd);
                stream->fd = -1;
                return nullptr;
            }
            if (msg.role == PS5WK_VROLE_DECODER) {
                // Not a video on screen: the decoder service takes the connection.
                g_mutex_lock(&streamsLock);
                streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
                g_mutex_unlock(&streamsLock);
                VLOG("stream %d from pid %d is a decoder connection", stream->id, msg.pid);
                PS5VideoDecoder::serve(fd, msg.pid, stream->id);
                close(fd);
                stream->fd = -1;
                return nullptr;
            }
            stream->key = static_cast<uint64_t>(msg.pts_ns);
            VLOG("stream %d from pid %d connected%s", stream->id, msg.pid, stream->key ? " (with a stream key)" : "");
            continue;
        }
        switch (msg.type) {
        case PS5WK_VMSG_SHM: {
            void* map = MAP_FAILED;
            size_t bytes = static_cast<size_t>(msg.slots) * msg.slot_bytes;
            if (passed >= 0 && msg.slots && msg.slots <= PS5WK_VIDEO_MAX_SLOTS && bytes)
                map = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, passed, 0);
            if (passed >= 0)
                close(passed);
            passed = -1;
            g_mutex_lock(&stream->lock);
            if (stream->shm)
                munmap(stream->shm, stream->shmBytes);
            stream->shm = map == MAP_FAILED ? nullptr : static_cast<uint8_t*>(map);
            stream->shmBytes = stream->shm ? bytes : 0;
            stream->slots = stream->shm ? msg.slots : 0;
            stream->slotBytes = msg.slot_bytes;
            stream->generation = msg.generation;
            stream->queue.clear(); // older generation: gone
            g_mutex_unlock(&stream->lock);
            VLOG("stream %d: %u slots of %.1f MB (generation %u)%s", stream->id, msg.slots, msg.slot_bytes / 1048576.0,
                msg.generation, map == MAP_FAILED ? ": MAP FAILED" : "");
            break;
        }
        case PS5WK_VMSG_FRAME: {
            PS5VideoFrame frame;
            frame.slot = msg.slot;
            frame.generation = msg.generation;
            frame.format = msg.format;
            frame.width = msg.width;
            frame.height = msg.height;
            frame.colorimetry = msg.colorimetry;
            memcpy(frame.stride, msg.stride, sizeof frame.stride);
            memcpy(frame.offset, msg.offset, sizeof frame.offset);
            frame.targetUS = msg.target_us;
            frame.ptsNS = msg.pts_ns;
            if (frame.format == PS5WK_VFMT_REMOTE) {
                frame.remote = true;
                frame.decoder = msg.ps5wk_decoder;
                frame.token = msg.ps5wk_token;
            }
            bool ok;
            g_mutex_lock(&stream->lock);
            ok = frame.remote || (stream->shm && frame.generation == stream->generation && frame.slot < stream->slots);
            if (ok)
                stream->queue.push_back(frame);
            g_mutex_unlock(&stream->lock);
            if (!ok)
                discardFrame(*stream, frame);
            else
                wake();
            break;
        }
        case PS5WK_VMSG_RDROP:
            PS5VideoDecoder::release(msg.ps5wk_decoder, msg.ps5wk_token, false);
            break;
        case PS5WK_VMSG_GEOMETRY:
            g_mutex_lock(&stream->lock);
            stream->pending.visible = msg.visible;
            stream->pending.x = msg.rect[0];
            stream->pending.y = msg.rect[1];
            stream->pending.width = msg.rect[2];
            stream->pending.height = msg.rect[3];
            if (!stream->hasPending)
                stream->pendingSinceUS = nowUS();
            stream->hasPending = true;
            g_mutex_unlock(&stream->lock);
            wake();
            break;
        case PS5WK_VMSG_FLUSH: {
            std::deque<PS5VideoFrame> dropped;
            g_mutex_lock(&stream->lock);
            dropped.swap(stream->queue);
            g_mutex_unlock(&stream->lock);
            for (auto& frame : dropped)
                discardFrame(*stream, frame);
            break;
        }
        default:
            break;
        }
        if (passed >= 0) {
            close(passed);
            passed = -1;
        }
    }
    g_mutex_lock(&stream->lock);
    stream->closed = true;
    g_mutex_unlock(&stream->lock);
    VLOG("stream %d from pid %d closed", stream->id, stream->pid);
    wake();
    return nullptr;
}

static void* listenThread(void*)
{
    int fd = -1;
    // A title can only bind in a directory a payload created (the first
    // child makes /system_tmp/ps5wk, 0777).
    for (int attempt = 0; fd < 0; attempt++) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, socketPath, sizeof address.sun_path - 1);
        unlink(socketPath);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && !bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof address) && !listen(fd, 8))
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
        if (!attempt || !(attempt % 120))
            VLOG("waiting to listen on %s (errno %d)", socketPath, errno);
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }
    chmod(socketPath, 0777);
    VLOG("listening on %s", socketPath);
    for (;;) {
        int client = accept(fd, nullptr, nullptr);
        if (client < 0) {
            if (errno != EINTR) {
                struct timespec ts = { 0, 100 * 1000 * 1000 };
                nanosleep(&ts, nullptr);
            }
            continue;
        }
        auto stream = std::make_shared<PS5VideoStream>();
        stream->fd = client;
        g_mutex_lock(&streamsLock);
        stream->id = ++nextStreamID;
        streams.push_back(stream);
        g_mutex_unlock(&streamsLock);
        pthread_t thread;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 256 * 1024);
        auto* arg = new std::shared_ptr<PS5VideoStream>(stream);
        if (pthread_create(&thread, &attributes, receiverThread, arg)) {
            delete arg;
            g_mutex_lock(&stream->lock);
            stream->closed = true;
            g_mutex_unlock(&stream->lock);
        } else
            pthread_detach(thread);
    }
    return nullptr;
}

void start(void (*wakeCallback)())
{
    if (socketPath[0])
        return;
    wakeFunction = wakeCallback;
    snprintf(socketPath, sizeof socketPath, PS5WK_VIDEO_SOCKET_FORMAT, static_cast<int>(getpid()));
    pthread_t thread;
    if (!pthread_create(&thread, nullptr, listenThread, nullptr))
        pthread_detach(thread);
}

unsigned streamCount()
{
    g_mutex_lock(&streamsLock);
    unsigned n = streams.size();
    g_mutex_unlock(&streamsLock);
    return n;
}

int64_t nextDeadline()
{
    int64_t deadline = INT64_MAX;
    g_mutex_lock(&streamsLock);
    for (auto& stream : streams) {
        g_mutex_lock(&stream->lock);
        if (stream->closed)
            deadline = 0;
        if (stream->hasPending)
            deadline = std::min(deadline, stream->pendingSinceUS + geometryCommitDelayUS);
        if (!stream->queue.empty()) {
            int64_t target = stream->queue.front().targetUS;
            deadline = std::min(deadline, target > nowUS() + 2000000 ? 0 : target);
        }
        g_mutex_unlock(&stream->lock);
    }
    g_mutex_unlock(&streamsLock);
    return deadline;
}

static void logStream(PS5VideoStream& stream, int64_t now)
{
    if (!stream.windowStartUS)
        stream.windowStartUS = now;
    double seconds = (now - stream.windowStartUS) / 1e6;
    if (seconds < 5)
        return;
    uint32_t received = stream.received - stream.windowReceived;
    uint32_t displayed = stream.displayed - stream.windowDisplayed;
    uint32_t dropped = stream.dropped - stream.windowDropped;
    if (received || displayed)
        VLOG("stream %d (pid %d): %.1f frames/s received, %.1f/s shown, %u dropped (%u in all), shown %.1f ms after due on average; "
             "held 1/2/3/4+ vsyncs: %u/%u/%u/%u, last holds %s; %s %dx%d+%d+%d",
            stream.id, stream.pid, received / seconds, displayed / seconds, dropped, stream.dropped,
            displayed ? stream.windowLateMS / displayed : 0.0, stream.holdHistogram[0], stream.holdHistogram[1], stream.holdHistogram[2],
            stream.holdHistogram[3], stream.holdPattern, stream.geometry.visible ? "at" : "hidden", stream.geometry.width,
            stream.geometry.height, stream.geometry.x, stream.geometry.y);
    memset(stream.holdHistogram, 0, sizeof stream.holdHistogram);
    stream.windowStartUS = now;
    stream.windowReceived = stream.received;
    stream.windowDisplayed = stream.displayed;
    stream.windowDropped = stream.dropped;
    stream.windowLateMS = 0;
}

bool consume(int64_t displayUS, int64_t slackUS, bool commitGeometry, const Consumer& consumer)
{
    std::vector<std::shared_ptr<PS5VideoStream>> current;
    g_mutex_lock(&streamsLock);
    current = streams;
    g_mutex_unlock(&streamsLock);

    int64_t now = nowUS();
    bool active = false;
    for (auto& stream : current) {
        g_mutex_lock(&stream->lock);
        if (stream->closed) {
            g_mutex_unlock(&stream->lock);
            consumer(*stream, nullptr, nullptr);
            g_mutex_lock(&streamsLock);
            streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
            g_mutex_unlock(&streamsLock);
            g_mutex_lock(&stream->lock);
            for (auto& frame : stream->queue) {
                if (frame.remote)
                    PS5VideoDecoder::release(frame.decoder, frame.token, false);
            }
            stream->queue.clear();
            if (stream->shm)
                munmap(stream->shm, stream->shmBytes);
            stream->shm = nullptr;
            if (stream->fd >= 0)
                close(stream->fd);
            stream->fd = -1;
            g_mutex_unlock(&stream->lock);
            VLOG("stream %d removed: %u frames received, %u shown, %u dropped", stream->id, stream->received, stream->displayed, stream->dropped);
            continue;
        }

        if (stream->hasPending && (commitGeometry || now - stream->pendingSinceUS >= geometryCommitDelayUS)) {
            bool moved = stream->pending.x != stream->geometry.x || stream->pending.y != stream->geometry.y
                || stream->pending.width != stream->geometry.width || stream->pending.height != stream->geometry.height
                || stream->pending.visible != stream->geometry.visible;
            stream->geometry = stream->pending;
            stream->hasPending = false;
            if (moved && now - stream->lastGeometryLogUS > 1000000) {
                stream->lastGeometryLogUS = now;
                VLOG("stream %d: %s %dx%d+%d+%d%s", stream->id, stream->geometry.visible ? "at" : "hidden", stream->geometry.width,
                    stream->geometry.height, stream->geometry.x, stream->geometry.y, commitGeometry ? " (with a page)" : " (timed)");
            }
        }

        // The newest frame due by then; the ones before it are dropped.
        std::vector<PS5VideoFrame> skipped;
        PS5VideoFrame chosen;
        bool haveChosen = false;
        while (!stream->queue.empty()) {
            auto& front = stream->queue.front();
            // A target far ahead means a broken clock mapping: show it now.
            bool due = !front.targetUS || front.targetUS <= displayUS + slackUS || front.targetUS > displayUS + 2000000;
            // Hysteresis: replace an already chosen frame (dropping it) only
            // if the newer one is clearly due. Frames due right at the
            // threshold, with a millisecond or two of jitter, otherwise got
            // dropped and then repeated (a hitch at 60 fps on 60 Hz).
            if (due && haveChosen && front.targetUS && front.targetUS <= displayUS + 2000000
                && front.targetUS > displayUS + slackUS - slackUS / 2)
                due = false;
            if (!due)
                break;
            if (haveChosen)
                skipped.push_back(chosen);
            chosen = front;
            haveChosen = true;
            stream->queue.pop_front();
            stream->received++;
        }
        if (!stream->queue.empty())
            active = true;
        bool pinned = false;
        if (haveChosen && chosen.remote) {
            pinned = PS5VideoDecoder::pin(chosen.decoder, chosen.token, chosen.picture);
            if (!pinned) {
                skipped.push_back(chosen); // the decoder no longer has it (flushed)
                haveChosen = false;
            }
        }
        if (haveChosen && (chosen.remote || (chosen.generation == stream->generation && stream->shm))) {
            const uint8_t* data = chosen.remote ? nullptr : stream->shm + static_cast<size_t>(chosen.slot) * stream->slotBytes;
            consumer(*stream, &chosen, data); // copies while the mapping is held (shared memory) or draws from the decoder's buffer
            if (pinned) {
                g_mutex_lock(&pinnedLock);
                pinnedPictures.emplace_back(chosen.decoder, chosen.token);
                g_mutex_unlock(&pinnedLock);
            }
            if (stream->hasFrame && stream->holdVsyncs) {
                unsigned hold = stream->holdVsyncs;
                stream->holdHistogram[std::min(hold, 4u) - 1]++;
                if (stream->holdPatternLength == 16) {
                    memmove(stream->holdPattern, stream->holdPattern + 1, 15);
                    stream->holdPatternLength = 15;
                }
                stream->holdPattern[stream->holdPatternLength++] = hold < 10 ? static_cast<char>('0' + hold) : '+';
                stream->holdPattern[stream->holdPatternLength] = 0;
            }
            stream->holdVsyncs = 0;
            stream->displayed++;
            stream->hasFrame = true;
            stream->lastFrameUS = now;
            if (chosen.targetUS)
                stream->windowLateMS += std::max<int64_t>(0, displayUS - chosen.targetUS) / 1000.0;
        } else
            consumer(*stream, nullptr, nullptr);
        if (stream->hasFrame)
            stream->holdVsyncs++;
        stream->dropped += skipped.size();
        g_mutex_unlock(&stream->lock);

        for (auto& frame : skipped)
            discardFrame(*stream, frame);
        if (haveChosen)
            sendRelease(*stream, chosen.remote ? chosen.token : chosen.slot, chosen.remote ? 0xffffffffu : chosen.generation);
        if (now - stream->lastFrameUS < activeAfterFrameUS)
            active = true;
        logStream(*stream, now);
    }
    return active;
}

void presented()
{
    std::vector<std::pair<uint32_t, uint32_t>> done;
    g_mutex_lock(&pinnedLock);
    done.swap(pinnedPictures);
    g_mutex_unlock(&pinnedLock);
    for (auto& [decoder, token] : done)
        PS5VideoDecoder::release(decoder, token, true);
}

} // namespace PS5VideoServer

// ps5wk-shot: a 60 Hz null consumer.
static void* nullConsumerThread(void*)
{
    const int64_t period = 16667;
    int64_t next = g_get_monotonic_time();
    for (;;) {
        next += period;
        int64_t wait = next - g_get_monotonic_time();
        if (wait > 0) {
            struct timespec ts = { static_cast<time_t>(wait / 1000000), static_cast<long>(wait % 1000000) * 1000 };
            nanosleep(&ts, nullptr);
        } else if (wait < -100000)
            next = g_get_monotonic_time();
        PS5VideoServer::consume(next, period / 2, true, [](PS5VideoStream&, const PS5VideoFrame*, const uint8_t*) { });
        PS5VideoServer::presented();
    }
    return nullptr;
}

void ps5wk_video_server_start_null(void)
{
    // PS5WK_FAKE_VDEC=1: the fake hardware decoder (empty pictures in
    // display order), so payload-only runs exercise ps5vdecproxy.
    if (!g_strcmp0(g_getenv("PS5WK_FAKE_VDEC"), "1"))
        ps5wk_video_set_decoder_backend(ps5wk_video_fake_decoder_backend());
    PS5VideoServer::start(nullptr);
    pthread_t thread;
    if (!pthread_create(&thread, nullptr, nullConsumerThread, nullptr))
        pthread_detach(thread);
}
