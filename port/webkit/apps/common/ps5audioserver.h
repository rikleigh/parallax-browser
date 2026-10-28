#pragma once
// Audio server for the WebProcess payloads' ps5audiosink (see ps5audioserver.c).
// The wire format is duplicated in port/deps/gst/gstps5audiosink.c: keep both in sync.
#include <stdint.h>

#define PS5WK_AUDIO_SOCKET_FORMAT "/system_tmp/ps5wk/audio-%d.sock" /* %d = UIProcess pid */
#define PS5WK_AUDIO_MAGIC 0x41355350u /* "PS5A" */
#define PS5WK_AUDIO_VERSION 1

struct ps5wk_audio_hello {
    uint32_t magic;
    uint32_t version;
    uint32_t rate;     /* 48000 */
    uint32_t channels; /* 1 or 2, S16 interleaved */
    int32_t pid;       /* client's pid, for the log */
    uint32_t reserved;
};

enum { PS5WK_AUDIO_SCE = 1, PS5WK_AUDIO_NULL = 2, PS5WK_AUDIO_SILENT = 3 };

#ifdef __cplusplus
extern "C" {
#endif
void ps5wk_audio_server_start(int mode);
void ps5wk_audio_server_stop(void);
int ps5wk_audio_server_active_streams(void);
void ps5wk_audio_server_selftest(void);
#ifdef __cplusplus
}
#endif
