/* Spike 9 software fallback: FFmpeg (h264, hevc, vp9) and dav1d (AV1)
 * decoding on the PS5's CPU, as fast as it goes, in an elfldr payload (same
 * 8 Zen 2 cores a title has; the title's ~420 MB of flexible memory would
 * also hold FFmpeg's frame pool at 4K).
 *
 * Jobs: /data/ps5webkit/hwdec/swjobs.txt, one per line:
 *   <path> <threads> [seconds]
 */
#define SPIKE_NAME "09-sw-decode"
#include "../../common/spike.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <stdlib.h>

#define DIR "/data/ps5webkit/hwdec"

static void run(const char *path, int threads, int seconds)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
        SPIKE_FAIL(path, "cannot open");
        return;
    }
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) {
        SPIKE_FAIL(path, "no video stream");
        avformat_close_input(&fmt);
        return;
    }
    const AVCodecParameters *par = fmt->streams[si]->codecpar;
    const AVCodec *codec = par->codec_id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libdav1d")
                                                             : avcodec_find_decoder(par->codec_id);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!ctx || avcodec_parameters_to_context(ctx, par) < 0) {
        SPIKE_FAIL(path, "no decoder");
        avformat_close_input(&fmt);
        return;
    }
    ctx->thread_count = threads;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        SPIKE_FAIL(path, "avcodec_open2 failed");
        return;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    unsigned frames = 0, packets = 0, loops = 0;
    const double t0 = spike_now_ms();
    int eof = 0;
    while (spike_now_ms() - t0 < seconds * 1000.0) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            loops++;
            if (frames >= 300 || eof) {
                avcodec_send_packet(ctx, NULL);
                while (avcodec_receive_frame(ctx, frame) == 0)
                    frames++;
                break;
            }
            av_seek_frame(fmt, si, 0, AVSEEK_FLAG_BACKWARD);
            continue;
        }
        if (pkt->stream_index == si) {
            packets++;
            if (avcodec_send_packet(ctx, pkt) == 0)
                while (avcodec_receive_frame(ctx, frame) == 0)
                    frames++;
        }
        av_packet_unref(pkt);
    }
    const double ms = spike_now_ms() - t0;
    SPIKE_PASS(path, "%s (%s) %dx%d %s, %d threads: %u frames (%u packets, %u passes) in %.0f ms = %.1f fps", codec->name,
               avcodec_get_name(par->codec_id), par->width, par->height,
               av_get_pix_fmt_name((enum AVPixelFormat)par->format), threads, frames, packets, loops, ms,
               frames * 1000.0 / ms);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}

int main(void)
{
    spike_log("ps5webkit spike 09 software decode (payload), pid %d, %s", (int)getpid(),
              av_version_info());
    FILE *f = fopen(DIR "/swjobs.txt", "r");
    if (!f) {
        SPIKE_FAIL("jobs", "no " DIR "/swjobs.txt");
        return 1;
    }
    char line[256], path[200];
    int threads, seconds;
    while (fgets(line, sizeof line, f)) {
        seconds = 20;
        if (line[0] == '#' || sscanf(line, "%199s %d %d", path, &threads, &seconds) < 2)
            continue;
        run(path, threads, seconds);
    }
    fclose(f);
    spike_log("software decode done");
    return 0;
}
