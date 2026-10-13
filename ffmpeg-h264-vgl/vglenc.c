/*
 * FFmpeg encoder: h264_vgl - H.264 encoding via virtio-gpu video passthrough
 * (host-side VCN hardware, virgl channel, see docs/WINDOWS_IMPL_DESIGN.md §4).
 *
 * This file is staged here; at M2 it moves into the FFmpeg tree built by
 * rustdesk-org/externals (see README.md in this directory).  FFmpeg 7.x
 * encoder API (encode2 receive_packet path).
 *
 * Key contract points:
 *  - input: AV_PIX_FMT_NV12 system memory only (no hwdevice, no D3D11);
 *  - output: Annex-B packets with in-band SPS/PPS/AUD synthesized by the
 *    host - NO extradata is advertised (avoids F13 dual-source hazard);
 *  - IDR frames set AV_PKT_FLAG_KEY.
 */
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/encode.h"
#include "libavutil/internal.h"

#include "vgl/vgl.h"

typedef struct VglEncContext {
    vgl_encoder_t *sess;
    int profile_high;            /* option: 1=High(wire 11), 0=Main(wire 9) */
    int zerolatency;
    AVClass *av_class;           /* required first member for option tables */
    int64_t rc_mode;             /* 0=CBR 1=VBR 2=QP */
    int force_idr_pending;
} VglEncContext;

static av_cold int vgl_enc_init(AVCodecContext *avctx)
{
    VglEncContext *vc = avctx->priv_data;

    if (vgl_open() != VGL_OK) {
        av_log(avctx, AV_LOG_ERROR, "vgl: bridge device not present\n");
        return AVERROR(EIO);
    }

    if (avctx->width <= 0 || avctx->height <= 0 ||
        (avctx->width | avctx->height) & 1) {
        av_log(avctx, AV_LOG_ERROR, "vgl: invalid dimensions %dx%d\n",
               avctx->width, avctx->height);
        return AVERROR(EINVAL);
    }

    vgl_enc_params_t p = {0};
    p.width  = avctx->width;
    p.height = avctx->height;
    p.fps    = avctx->framerate.num > 0 ? avctx->framerate.num : 30;
    p.gop    = avctx->gop_size > 0 ? avctx->gop_size : 120;
    p.bitrate_kbps = (avctx->bit_rate > 0) ? (uint32_t)(avctx->bit_rate / 1000) : 4000;
    p.profile = vc->profile_high ? VGL_PROFILE_H264_HIGH : VGL_PROFILE_H264_MAIN;
    switch (vc->rc_mode) {
    case 1:  p.rc = VGL_RC_VBR; break;
    case 2:  p.rc = VGL_RC_QP;
             p.qp = avctx->global_quality > 0 ? (uint32_t)avctx->global_quality : 26;
             break;
    default: p.rc = VGL_RC_CBR; break;
    }
    p.zerolatency = vc->zerolatency;

    vc->sess = vgl_encoder_open(&p);
    if (!vc->sess) {
        av_log(avctx, AV_LOG_ERROR, "vgl: session open failed (%s)\n",
               vgl_strerror(VGL_E_DRIVER));
        return AVERROR(EIO);
    }
    return 0;
}

static int vgl_enc_receive(AVCodecContext *avctx, AVPacket *pkt)
{
    VglEncContext *vc = avctx->priv_data;
    AVFrame *frame = NULL;
    int ret;

    ret = ff_encode_get_frame(avctx, &frame);
    if (ret < 0 && ret != AVERROR_EOF)
        return ret;

    if (frame) {
        vgl_frame_t f = {0};
        f.y_plane  = frame->data[0];
        f.uv_plane = frame->data[1];
        f.y_stride = frame->linesize[0];
        f.uv_stride= frame->linesize[1];
        f.width    = frame->width;
        f.height   = frame->height;
        f.pts_us   = frame->pts;

        if (frame->pict_type == AV_PICTURE_TYPE_I || vc->force_idr_pending) {
            vgl_encoder_force_idr(vc->sess);
            vc->force_idr_pending = 0;
        }

        vgl_packet_t out;
        ret = vgl_encode_frame(vc->sess, &f, &out);
        if (ret != VGL_OK) {
            av_log(avctx, AV_LOG_ERROR, "vgl: encode failed (%s)\n",
                   vgl_strerror(ret));
            av_frame_free(&frame);
            return AVERROR(EIO);
        }

        ret = ff_get_encode_buffer(avctx, pkt, out.len, 0);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }
        memcpy(pkt->data, out.data, out.len);
        pkt->pts = pkt->dts = frame->pts;
        if (out.keyframe)
            pkt->flags |= AV_PKT_FLAG_KEY;
        av_frame_free(&frame);
        return 0;
    }

    if (ret == AVERROR_EOF) {
        vgl_packet_t out;
        if (vgl_encoder_flush(vc->sess, &out) == VGL_OK && out.len > 0) {
            ret = ff_get_encode_buffer(avctx, pkt, out.len, 0);
            if (ret < 0)
                return ret;
            memcpy(pkt->data, out.data, out.len);
            if (out.keyframe)
                pkt->flags |= AV_PKT_FLAG_KEY;
            return 0;
        }
        return AVERROR_EOF;
    }
    return AVERROR(EAGAIN);
}

static av_cold int vgl_enc_close(AVCodecContext *avctx)
{
    VglEncContext *vc = avctx->priv_data;
    if (vc->sess) {
        vgl_encoder_close(vc->sess);
        vc->sess = NULL;
    }
    vgl_close();
    return 0;
}

#define OFFSET(x) offsetof(VglEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption vgl_options[] = {
    { "profile_high", "encode H.264 High profile (wire 11), else Main (wire 9)",
      OFFSET(profile_high), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { "zerolatency",  "slice-level output, bframes=0 (remote control default)",
      OFFSET(zerolatency), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { "rc_mode", "rate control: 0 CBR, 1 VBR, 2 QP", OFFSET(rc_mode),
      AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, 2, VE },
    { NULL },
};

static const AVClass vgl_class = {
    .class_name = "h264_vgl",
    .item_name  = av_default_item_name,
    .option     = vgl_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_vgl_encoder = {
    .p.name         = "h264_vgl",
    .p.long_name    = NULL_IF_CONFIG_SMALL("H.264 via virtio-gpu video passthrough (host hardware)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_ENCODER_FLUSH,
    .priv_data_size = sizeof(VglEncContext),
    .p.priv_class   = &vgl_class,
    .init           = vgl_enc_init,
    FF_CODEC_RECEIVE_PACKET_CB(vgl_enc_receive),
    .close          = vgl_enc_close,
    .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_NV12,
                                                     AV_PIX_FMT_NONE },
};
