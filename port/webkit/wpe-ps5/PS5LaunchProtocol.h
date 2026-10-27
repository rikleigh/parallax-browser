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

/* How the title (UIProcess) starts a WebKit child process as an elfldr
 * payload and hands it its IPC socket. Shared by WPEProcessManagerPS5.cpp
 * (title) and apps/child/ps5wk-child.cpp (payload). Spike 8 measured every
 * step on 13.60.
 *
 *  1. The title connects to the local elfldr (127.0.0.1:9021) and sends the
 *     child ELF, followed on the same stream by one header line:
 *       PS5WK-LAUNCH 1 <web|network> <process id> <socket path>\n
 *     elfldr reads exactly the ELF (it sizes it from the ELF headers) and
 *     dup2()s the connection onto the payload's stdin/stdout/stderr, so the
 *     payload reads the header from stdin and its output comes back to the
 *     title on that connection.
 *  2. A title may not connect() to a payload's unix socket (EPERM) and may
 *     only bind() in a 0777 directory that a payload created. So the payload
 *     creates PS5WK_LAUNCH_DIR (0777) and connects to the title's socket in
 *     it, retrying while the title waits for the directory to appear.
 *  3. The payload sends a ps5wk_hello; the title answers with a
 *     ps5wk_welcome carrying the client end of WebKit's IPC socketpair as
 *     SCM_RIGHTS. The rendezvous socket is then closed and WebKit's
 *     IPC::Connection runs over the passed socket, unchanged.
 */
#ifndef PS5LaunchProtocol_h
#define PS5LaunchProtocol_h

#include <stdint.h>

#define PS5WK_ELFLDR_PORT 9021
#define PS5WK_LAUNCH_DIR "/system_tmp/ps5wk"
#define PS5WK_LAUNCH_HEADER "PS5WK-LAUNCH"
#define PS5WK_LAUNCH_VERSION 1

#define PS5WK_HELLO_MAGIC 0x4f4c4c4548575350ull /* "PSWHELLO" */
#define PS5WK_WELCOME_MAGIC 0x454d4f434c455750ull /* "PWELCOME" */

struct ps5wk_hello {
    uint64_t magic;
    uint64_t processIdentifier;
    int32_t pid;
    int32_t version;
};

struct ps5wk_welcome {
    uint64_t magic;
    int32_t status; /* 0: the IPC fd is attached */
    int32_t reserved;
};

#endif /* PS5LaunchProtocol_h */
