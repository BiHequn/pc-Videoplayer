#include "ffmpeg-video-decoder.h"

#include <cstring>
#include <iostream>
#include <limits>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

static AVCodecID toFfmpegCodecId(VideoCodec codec)
{
    switch (codec)
    {
    case VideoCodec::H264:
        return AV_CODEC_ID_H264;

    case VideoCodec::Hevc:
        return AV_CODEC_ID_HEVC;

    default:
        return AV_CODEC_ID_NONE;
    }
}

static void printDecoderError(
    const char* operation,
    int errorCode)
{
    char message[AV_ERROR_MAX_STRING_SIZE] = {};

    av_strerror(
        errorCode,
        message,
        sizeof(message));

    std::cerr
        << operation
        << " failed: "
        << message
        << std::endl;
}

FfmpegVideoDecoder::~FfmpegVideoDecoder()
{
    close();
}

bool FfmpegVideoDecoder::initialize(
    const VideoDecoderConfig& config)
{
    close();

    if (config.width <= 0 ||
        config.height <= 0 ||
        config.timeBase.numerator <= 0 ||
        config.timeBase.denominator <= 0)
    {
        return false;
    }

    AVCodecID codecId =
        static_cast<AVCodecID>(config.ffmpegCodecId);

    if (codecId == AV_CODEC_ID_NONE)
        codecId = toFfmpegCodecId(config.codec);

    if (codecId == AV_CODEC_ID_NONE)
        return false;

    const AVCodec* decoder =
        avcodec_find_decoder(codecId);

    if (!decoder)
    {
        std::cerr
            << "FFmpeg video decoder not found."
            << std::endl;

        return false;
    }

    codecContext_ =
        avcodec_alloc_context3(decoder);

    if (!codecContext_)
        return false;

    codecContext_->width = config.width;
    codecContext_->height = config.height;

    codecContext_->pkt_timebase = {
        config.timeBase.numerator,
        config.timeBase.denominator
    };

    if (!config.extraData.empty())
    {
        if (config.extraData.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max() -
                AV_INPUT_BUFFER_PADDING_SIZE))
        {
            close();
            return false;
        }

        const std::size_t allocationSize =
            config.extraData.size() +
            AV_INPUT_BUFFER_PADDING_SIZE;

        codecContext_->extradata =
            static_cast<std::uint8_t*>(
                av_mallocz(allocationSize));

        if (!codecContext_->extradata)
        {
            close();
            return false;
        }

        std::memcpy(
            codecContext_->extradata,
            config.extraData.data(),
            config.extraData.size());

        codecContext_->extradata_size =
            static_cast<int>(
                config.extraData.size());
    }

    int result = avcodec_open2(
        codecContext_,
        decoder,
        nullptr);

    if (result < 0)
    {
        printDecoderError(
            "avcodec_open2",
            result);

        close();
        return false;
    }

    decodedFrame_ = av_frame_alloc();

    if (!decodedFrame_)
    {
        close();
        return false;
    }

    timeBase_ = config.timeBase;
    nativeYuv_ = config.nativeYuv;
    if (nativeYuv_) {
        nativeFrame_ = av_frame_alloc();
        if (!nativeFrame_) { close(); return false; }
    }
    return true;
}

VideoDecoderResult FfmpegVideoDecoder::sendPacket(
    const EncodedVideoPacket& packet)
{
    if (!codecContext_ ||
        !packet.data ||
        packet.size == 0 ||
        packet.size >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max()))
    {
        return VideoDecoderResult::Error;
    }

    AVPacket* avPacket = av_packet_alloc();

    if (!avPacket)
        return VideoDecoderResult::Error;

    int result = av_new_packet(
        avPacket,
        static_cast<int>(packet.size));

    if (result < 0)
    {
        av_packet_free(&avPacket);
        return VideoDecoderResult::Error;
    }

    std::memcpy(
        avPacket->data,
        packet.data,
        packet.size);

    avPacket->pts =
        packet.hasPts
        ? packet.pts
        : AV_NOPTS_VALUE;

    avPacket->dts =
        packet.hasDts
        ? packet.dts
        : AV_NOPTS_VALUE;

    avPacket->duration = packet.duration;

    if (packet.keyFrame)
        avPacket->flags |= AV_PKT_FLAG_KEY;

    result = avcodec_send_packet(
        codecContext_,
        avPacket);

    av_packet_free(&avPacket);

    if (result == AVERROR(EAGAIN))
        return VideoDecoderResult::TryAgain;

    if (result == AVERROR_EOF)
        return VideoDecoderResult::EndOfStream;

    if (result < 0)
    {
        printDecoderError(
            "avcodec_send_packet",
            result);

        return VideoDecoderResult::Error;
    }

    return VideoDecoderResult::Ok;
}

VideoDecoderResult FfmpegVideoDecoder::receiveFrame(
    DecodedVideoFrame* frame)
{
    if (frame)
        *frame = {};
    if (!codecContext_ ||
        !decodedFrame_ ||
        !frame)
    {
        return VideoDecoderResult::Error;
    }

    int result = avcodec_receive_frame(
        codecContext_,
        decodedFrame_);

    if (result == AVERROR(EAGAIN))
        return VideoDecoderResult::TryAgain;

    if (result == AVERROR_EOF)
        return VideoDecoderResult::EndOfStream;

    if (result < 0)
    {
        printDecoderError(
            "avcodec_receive_frame",
            result);

        return VideoDecoderResult::Error;
    }

    if (!(nativeYuv_ ? exposeFrameAsYuv(decodedFrame_, frame)
                     : convertFrameToBgra(decodedFrame_, frame)))
    {
        av_frame_unref(decodedFrame_);
        *frame = {};
        return VideoDecoderResult::Error;
    }

    if (decodedFrame_->best_effort_timestamp !=
        AV_NOPTS_VALUE)
    {
        frame->ptsSeconds =
            static_cast<double>(
                decodedFrame_->
                best_effort_timestamp) *
            static_cast<double>(
                timeBase_.numerator) /
            static_cast<double>(
                timeBase_.denominator);
    }
    else
    {
        frame->ptsSeconds = -1.0;
    }

    av_frame_unref(decodedFrame_);
    return VideoDecoderResult::FrameReady;
}

VideoDecoderResult FfmpegVideoDecoder::drain()
{
    if (!codecContext_)
        return VideoDecoderResult::Error;

    int result = avcodec_send_packet(
        codecContext_,
        nullptr);

    if (result == AVERROR(EAGAIN))
        return VideoDecoderResult::TryAgain;

    if (result == AVERROR_EOF)
        return VideoDecoderResult::EndOfStream;

    if (result < 0)
    {
        printDecoderError(
            "avcodec_send_packet drain",
            result);

        return VideoDecoderResult::Error;
    }

    return VideoDecoderResult::Ok;
}

void FfmpegVideoDecoder::flush()
{
    if (codecContext_)
        avcodec_flush_buffers(codecContext_);

    if (decodedFrame_)
        av_frame_unref(decodedFrame_);
    if (nativeFrame_)
        av_frame_unref(nativeFrame_);
}

void FfmpegVideoDecoder::close()
{
    if (scaleContext_)
    {
        sws_freeContext(scaleContext_);
        scaleContext_ = nullptr;
    }

    if (decodedFrame_)
        av_frame_free(&decodedFrame_);
    if (nativeFrame_)
        av_frame_free(&nativeFrame_);

    if (codecContext_)
        avcodec_free_context(&codecContext_);

    bgraBuffer_.clear();
    timeBase_ = {};
    nativeYuv_ = false;
}

bool FfmpegVideoDecoder::convertFrameToBgra(
    const AVFrame* source,
    DecodedVideoFrame* output)
{
    if (!source ||
        !output ||
        source->width <= 0 ||
        source->height <= 0)
    {
        return false;
    }

    if (source->width > std::numeric_limits<int>::max() / 4)
        return false;

    scaleContext_ = sws_getCachedContext(
        scaleContext_,
        source->width,
        source->height,
        static_cast<AVPixelFormat>(
            source->format),
        source->width,
        source->height,
        AV_PIX_FMT_BGRA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!scaleContext_)
        return false;

    const int stride = source->width * 4;

    const std::size_t bufferSize =
        static_cast<std::size_t>(stride) *
        static_cast<std::size_t>(
            source->height);

    bgraBuffer_.resize(bufferSize);

    std::uint8_t* destinationData[4] = {
        bgraBuffer_.data(),
        nullptr,
        nullptr,
        nullptr
    };

    int destinationStride[4] = {
        stride,
        0,
        0,
        0
    };

    int convertedHeight = sws_scale(
        scaleContext_,
        source->data,
        source->linesize,
        0,
        source->height,
        destinationData,
        destinationStride);

    if (convertedHeight != source->height)
        return false;

    output->data = bgraBuffer_.data();
    output->width = source->width;
    output->height = source->height;
    output->stride = stride;
    output->planes[0] = output->data;
    output->strides[0] = stride;

    return true;
}

namespace {
VideoColorSpace videoSpace(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709: return VideoColorSpace::Bt709;
    case AVCOL_SPC_BT2020_NCL: return VideoColorSpace::Bt2020;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return VideoColorSpace::Bt601;
    default: return VideoColorSpace::Unspecified;
    }
}
int swsSpace(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
    default: return SWS_CS_ITU601;
    }
}
}

bool FfmpegVideoDecoder::exposeFrameAsYuv(const AVFrame* source, DecodedVideoFrame* output) {
    if (!source || !output || !nativeFrame_ || source->width <= 0 || source->height <= 0)
        return false;
    av_frame_unref(nativeFrame_);
    const auto format = static_cast<AVPixelFormat>(source->format);
    // Keep FFmpeg's reference-counted plane allocation alive after decodedFrame_ is unref'd.
    // YUV420P (including full-range legacy YUVJ420P) takes no swscale or RGB path.
    if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) {
        if (av_frame_ref(nativeFrame_, source) < 0) return false;
    } else {
        nativeFrame_->format = AV_PIX_FMT_YUV420P;
        nativeFrame_->width = source->width;
        nativeFrame_->height = source->height;
        if (av_frame_get_buffer(nativeFrame_, 32) < 0) return false;
        scaleContext_ = sws_getCachedContext(scaleContext_, source->width, source->height, format,
            source->width, source->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaleContext_) return false;
        const bool full = source->color_range == AVCOL_RANGE_JPEG ||
            format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P;
        const int* coefficients = sws_getCoefficients(swsSpace(source->colorspace));
        if (sws_setColorspaceDetails(scaleContext_, coefficients, full, coefficients, full,
                                    0, 1 << 16, 1 << 16) < 0) return false;
        if (sws_scale(scaleContext_, source->data, source->linesize, 0, source->height,
                      nativeFrame_->data, nativeFrame_->linesize) != source->height) return false;
        nativeFrame_->color_range = full ? AVCOL_RANGE_JPEG : source->color_range;
        nativeFrame_->colorspace = source->colorspace;
    }
    output->pixelFormat = VideoPixelFormat::Yuv420p;
    output->width = source->width; output->height = source->height;
    for (int plane = 0; plane < 3; ++plane) {
        output->planes[plane] = nativeFrame_->data[plane];
        output->strides[plane] = nativeFrame_->linesize[plane];
        if (!output->planes[plane] || !output->strides[plane]) return false;
    }
    output->data = output->planes[0]; output->stride = output->strides[0];
    output->colorRange = (nativeFrame_->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P)
        ? VideoColorRange::Full : (nativeFrame_->color_range == AVCOL_RANGE_MPEG
                                  ? VideoColorRange::Limited : VideoColorRange::Unspecified);
    output->colorSpace = videoSpace(nativeFrame_->colorspace);
    return true;
}
