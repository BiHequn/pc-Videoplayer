#include "ffmpeg-video-decoder.h"

#include <cstring>
#include <iostream>
#include <limits>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
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
        toFfmpegCodecId(config.codec);

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

    if (!convertFrameToBgra(
        decodedFrame_,
        frame))
    {
        av_frame_unref(decodedFrame_);
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

    if (codecContext_)
        avcodec_free_context(&codecContext_);

    bgraBuffer_.clear();
    timeBase_ = {};
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

    return true;
}