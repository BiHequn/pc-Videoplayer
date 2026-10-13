#pragma once

#include<cstddef>
#include<cstdint>
#include<vector>

enum class VideoCodec
{
	Unknown,
	H264,
	Hevc
};

struct VideoTimeBase
{
	int numerator = 0;
	int denominator = 1;
};

struct VideoDecoderConfig
{
	VideoCodec codec = VideoCodec::Unknown;
	int ffmpegCodecId = 0;
	int width = 0;
	int height = 0;
	VideoTimeBase timeBase;
	//H.264的SPS/PPS 或 H.265的VPS/SPS/PPS
	std::vector<std::uint8_t> extraData;
};

struct EncodedVideoPacket
{
	//数据只保证在sendPacket调用期间有效
	const std::uint8_t* data = nullptr;

	std::size_t size = 0;
	std::int64_t pts = 0;
	std::int64_t dts = 0;
	std::int64_t duration = 0;

	bool hasPts = false;
	bool hasDts = false;
	bool keyFrame = false;
};

struct DecodedVideoFrame
{
	const unsigned char* data = nullptr;

	int width = 0;
	int height = 0;
	int stride = 0;

	double ptsSeconds = 0.0;
};

enum class VideoDecoderResult
{
	Ok,
	FrameReady,
	TryAgain,
	EndOfStream,
	Error
};

class IVideoDecoder
{
public:
	virtual ~IVideoDecoder() = default;
	virtual bool initialize(const VideoDecoderConfig& config) = 0;
	virtual VideoDecoderResult sendPacket(const EncodedVideoPacket& packet) = 0;
	virtual VideoDecoderResult receiveFrame(DecodedVideoFrame* frame) = 0;
	virtual VideoDecoderResult drain() = 0;

	virtual void flush() = 0;
	virtual void close() = 0;
	
};
