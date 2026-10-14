#pragma once
#include"video-decoder.h"

struct AVCodecContext;
struct AVFrame;
struct SwsContext;

class FfmpegVideoDecoder final : public IVideoDecoder
{
public:
	FfmpegVideoDecoder() = default;
	~FfmpegVideoDecoder() override;

    bool initialize(const VideoDecoderConfig& config) override;
    VideoDecoderResult sendPacket(const EncodedVideoPacket& packet) override;
    VideoDecoderResult receiveFrame(DecodedVideoFrame* frame) override;
    VideoDecoderResult drain() override;
    void flush() override;
    void close() override;

private:
    bool convertFrameToBgra(const AVFrame* source, DecodedVideoFrame* output);
    bool exposeFrameAsYuv(const AVFrame* source, DecodedVideoFrame* output);
    AVCodecContext* codecContext_ = nullptr;
    AVFrame* decodedFrame_ = nullptr;
    AVFrame* nativeFrame_ = nullptr;
    SwsContext* scaleContext_ = nullptr;

    std::vector<unsigned char> bgraBuffer_;
    VideoTimeBase timeBase_;
    bool nativeYuv_ = false;
};
