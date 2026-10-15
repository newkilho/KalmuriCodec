/* Kalmuri.dll: H.264/AAC MP4 encoder on top of static FFmpeg (LGPL-2.1-or-later) */
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define KFF_API __declspec(dllexport)

typedef void (*kff_log_fn)(int level, const char *text);

typedef struct KffEnc {
    AVFormatContext *fmt;
    AVCodecContext *venc, *aenc;
    AVStream *vst, *ast;
    struct SwsContext *sws;
    SwrContext *swr;
    AVAudioFifo *fifo;
    AVFrame *vframe, *aframe;
    AVPacket *pkt;
    int64_t apts;
    int width, height;
    int header_done;
} KffEnc;

static kff_log_fn g_log;

static void log_cb(void *avcl, int level, const char *fmt, va_list vl) {
    static int prefix = 1;
    char line[1024];
    kff_log_fn f = g_log;
    if (!f || level > AV_LOG_INFO) return;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &prefix);
    f(level, line);
}

KFF_API void kff_set_log(kff_log_fn f) {
    g_log = f;
    av_log_set_level(f ? AV_LOG_INFO : AV_LOG_QUIET);
    av_log_set_callback(f ? log_cb : av_log_default_callback);
}

KFF_API int kff_check(void) {
    return AV_VERSION_MAJOR(avcodec_version()) == LIBAVCODEC_VERSION_MAJOR
        && AV_VERSION_MAJOR(avformat_version()) == LIBAVFORMAT_VERSION_MAJOR
        && AV_VERSION_MAJOR(avutil_version()) == LIBAVUTIL_VERSION_MAJOR
        && AV_VERSION_MAJOR(swscale_version()) == LIBSWSCALE_VERSION_MAJOR
        && AV_VERSION_MAJOR(swresample_version()) == LIBSWRESAMPLE_VERSION_MAJOR;
}

static void set_err(char *err, int len, const char *what, int code) {
    char msg[AV_ERROR_MAX_STRING_SIZE] = "";
    if (code) av_strerror(code, msg, sizeof msg);
    snprintf(err, len, code ? "%s (%s)" : "%s", what, msg);
}

static void tune(AVCodecContext *c, const char *name, int fps) {
    if (!strcmp(name, "h264_nvenc")) {
        av_opt_set(c->priv_data, "preset", "p4", 0);
        av_opt_set(c->priv_data, "rc", "vbr", 0);
        av_opt_set_int(c->priv_data, "cq", 23, 0);
        c->bit_rate = 0;
    } else if (!strcmp(name, "h264_qsv")) {
        av_opt_set(c->priv_data, "preset", "medium", 0);
        c->global_quality = 23;
    } else if (!strcmp(name, "h264_amf")) {
        av_opt_set(c->priv_data, "usage", "transcoding", 0);
        av_opt_set(c->priv_data, "rc", "cqp", 0);
        av_opt_set_int(c->priv_data, "qp_i", 21, 0);
        av_opt_set_int(c->priv_data, "qp_p", 23, 0);
    } else {
        av_opt_set_int(c->priv_data, "hw_encoding", 1, 0);
        av_opt_set(c->priv_data, "rate_control", "u_vbr", 0);
        av_opt_set(c->priv_data, "scenario", "display_remoting", 0);
        c->bit_rate = (int64_t)c->width * c->height * fps / 10;
    }
}

static AVCodecContext *open_video(KffEnc *e, int fps, int only, char *name, int namelen) {
    static const char *const order[] = { "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "h264_mf" };
    for (int i = 0; i < 5; i++) {
        const AVCodec *codec = avcodec_find_encoder_by_name(order[i]);
        AVCodecContext *c;
        if (only >= 0 && i != only) continue;
        if (!codec || !(c = avcodec_alloc_context3(codec))) continue;
        c->width = e->width;
        c->height = e->height;
        c->pix_fmt = AV_PIX_FMT_NV12;
        c->time_base = (AVRational){ 1, fps };
        c->framerate = (AVRational){ fps, 1 };
        c->gop_size = fps * 2;
        c->max_b_frames = 0;
        c->color_range = AVCOL_RANGE_MPEG;
        c->colorspace = AVCOL_SPC_BT709;
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc = AVCOL_TRC_BT709;
        if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        tune(c, order[i], fps);
        if (i == 4) av_opt_set_int(c->priv_data, "hw_encoding", 0, 0);
        if (avcodec_open2(c, codec, NULL) >= 0) {
            snprintf(name, namelen, "%s%s", order[i], i == 4 ? " (sw)" : "");
            return c;
        }
        avcodec_free_context(&c);
    }
    return NULL;
}

static int open_audio(KffEnc *e, int rate, int channels, int sfmt, char *err, int errlen) {
    static const enum AVSampleFormat fmts[] = { AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S32 };
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVChannelLayout layout;
    int r;
    if (!codec || !(e->aenc = avcodec_alloc_context3(codec))) { set_err(err, errlen, "aac", 0); return -1; }
    av_channel_layout_default(&layout, channels);
    av_channel_layout_copy(&e->aenc->ch_layout, &layout);
    e->aenc->sample_rate = rate;
    e->aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    e->aenc->bit_rate = 192000;
    e->aenc->time_base = (AVRational){ 1, rate };
    if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER) e->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((r = avcodec_open2(e->aenc, codec, NULL)) < 0) { set_err(err, errlen, "aac", r); return -1; }
    r = swr_alloc_set_opts2(&e->swr, &layout, AV_SAMPLE_FMT_FLTP, rate, &layout, fmts[sfmt], rate, 0, NULL);
    if (r < 0 || (r = swr_init(e->swr)) < 0) { set_err(err, errlen, "swresample", r); return -1; }
    e->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels, e->aenc->frame_size * 4);
    e->aframe = av_frame_alloc();
    if (!e->fifo || !e->aframe) { set_err(err, errlen, "memory", 0); return -1; }
    e->ast = avformat_new_stream(e->fmt, NULL);
    if (!e->ast) { set_err(err, errlen, "stream", 0); return -1; }
    e->ast->time_base = e->aenc->time_base;
    avcodec_parameters_from_context(e->ast->codecpar, e->aenc);
    return 0;
}

KFF_API void kff_close(KffEnc *e);

KFF_API KffEnc *kff_open(const char *path, int width, int height, int fps, int rate, int channels, int sfmt, int only,
                 char *err, int errlen, char *codec, int codeclen) {
    KffEnc *e = av_mallocz(sizeof *e);
    int r;
    if (!e) { set_err(err, errlen, "memory", 0); return NULL; }
    e->width = width;
    e->height = height;
    if ((r = avformat_alloc_output_context2(&e->fmt, NULL, "mp4", path)) < 0) { set_err(err, errlen, "mp4", r); goto fail; }
    if (!(e->venc = open_video(e, fps, only, codec, codeclen))) { set_err(err, errlen, "H.264", AVERROR_ENCODER_NOT_FOUND); goto fail; }
    if (!(e->vst = avformat_new_stream(e->fmt, NULL))) { set_err(err, errlen, "stream", 0); goto fail; }
    e->vst->time_base = e->venc->time_base;
    avcodec_parameters_from_context(e->vst->codecpar, e->venc);
    if (channels > 0 && open_audio(e, rate, channels, sfmt, err, errlen) < 0) goto fail;

    e->sws = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!e->sws) { set_err(err, errlen, "swscale", 0); goto fail; }
    {
        const int *coef = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(e->sws, coef, 1, coef, 0, 0, 1 << 16, 1 << 16);
    }
    e->vframe = av_frame_alloc();
    e->pkt = av_packet_alloc();
    if (!e->vframe || !e->pkt) { set_err(err, errlen, "memory", 0); goto fail; }
    e->vframe->format = AV_PIX_FMT_NV12;
    e->vframe->width = width;
    e->vframe->height = height;

    if ((r = avio_open(&e->fmt->pb, path, AVIO_FLAG_WRITE)) < 0) { set_err(err, errlen, path, r); goto fail; }
    {
        AVDictionary *opt = NULL;
        av_dict_set(&opt, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
        r = avformat_write_header(e->fmt, &opt);
        av_dict_free(&opt);
        if (r < 0) { set_err(err, errlen, "mp4", r); goto fail; }
    }
    e->header_done = 1;
    return e;
fail:
    kff_close(e);
    return NULL;
}

static int drain(KffEnc *e, AVCodecContext *c, AVStream *st) {
    int r;
    while ((r = avcodec_receive_packet(c, e->pkt)) >= 0) {
        if (!e->pkt->duration && c == e->venc) e->pkt->duration = 1;
        av_packet_rescale_ts(e->pkt, c->time_base, st->time_base);
        e->pkt->stream_index = st->index;
        if ((r = av_interleaved_write_frame(e->fmt, e->pkt)) < 0) return r;
    }
    return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
}

KFF_API int kff_video(KffEnc *e, const uint8_t *bgra, int stride, int64_t pts) {
    const uint8_t *src[1] = { bgra };
    int srcstride[1] = { stride };
    int r;
    if ((r = av_frame_make_writable(e->vframe)) < 0 && (r = av_frame_get_buffer(e->vframe, 0)) < 0) return r;
    sws_scale(e->sws, src, srcstride, 0, e->height, e->vframe->data, e->vframe->linesize);
    e->vframe->pts = pts;
    if ((r = avcodec_send_frame(e->venc, e->vframe)) < 0) return r;
    return drain(e, e->venc, e->vst);
}

static int audio_frames(KffEnc *e, int flush) {
    int r, size = e->aenc->frame_size;
    while (av_audio_fifo_size(e->fifo) >= size || (flush && av_audio_fifo_size(e->fifo) > 0)) {
        int n = FFMIN(size, av_audio_fifo_size(e->fifo));
        av_frame_unref(e->aframe);
        e->aframe->nb_samples = n;
        e->aframe->format = AV_SAMPLE_FMT_FLTP;
        e->aframe->sample_rate = e->aenc->sample_rate;
        av_channel_layout_copy(&e->aframe->ch_layout, &e->aenc->ch_layout);
        if ((r = av_frame_get_buffer(e->aframe, 0)) < 0) return r;
        av_audio_fifo_read(e->fifo, (void **)e->aframe->data, n);
        e->aframe->pts = e->apts;
        e->apts += n;
        if ((r = avcodec_send_frame(e->aenc, e->aframe)) < 0) return r;
        if ((r = drain(e, e->aenc, e->ast)) < 0) return r;
    }
    return 0;
}

KFF_API int kff_audio(KffEnc *e, const uint8_t *data, int frames) {
    uint8_t **out = NULL;
    int r, n;
    if (!e->aenc || frames <= 0) return 0;
    if ((r = av_samples_alloc_array_and_samples(&out, NULL, e->aenc->ch_layout.nb_channels, frames, AV_SAMPLE_FMT_FLTP, 0)) < 0) return r;
    n = swr_convert(e->swr, out, frames, &data, frames);
    r = n < 0 ? n : av_audio_fifo_write(e->fifo, (void **)out, n);
    av_freep(&out[0]);
    av_freep(&out);
    return r < 0 ? r : audio_frames(e, 0);
}

KFF_API int kff_finish(KffEnc *e) {
    int r = 0, t;
    if (e->aenc) {
        if ((t = audio_frames(e, 1)) < 0) r = t;
        avcodec_send_frame(e->aenc, NULL);
        if ((t = drain(e, e->aenc, e->ast)) < 0) r = t;
    }
    avcodec_send_frame(e->venc, NULL);
    if ((t = drain(e, e->venc, e->vst)) < 0) r = t;
    if ((t = av_write_trailer(e->fmt)) < 0) r = t;
    return r;
}

KFF_API void kff_close(KffEnc *e) {
    if (!e) return;
    if (e->fmt && e->fmt->pb) avio_closep(&e->fmt->pb);
    avformat_free_context(e->fmt);
    avcodec_free_context(&e->venc);
    avcodec_free_context(&e->aenc);
    sws_freeContext(e->sws);
    swr_free(&e->swr);
    if (e->fifo) av_audio_fifo_free(e->fifo);
    av_frame_free(&e->vframe);
    av_frame_free(&e->aframe);
    av_packet_free(&e->pkt);
    av_free(e);
}

KFF_API void kff_strerror(int code, char *buf, int len) {
    av_strerror(code, buf, len);
}
