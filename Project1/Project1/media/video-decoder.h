#pragma once

#include<cstddef>
#include<cstdint>
#include<array>
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

enum class VideoPixelFormat { Bgra, Yuv420p };
enum class VideoColorRange { Unspecified, Limited, Full };
enum class VideoColorSpace { Unspecified, Bt601, Bt709, Bt2020 };

struct VideoDecoderConfig
{
	VideoCodec codec = VideoCodec::Unknown;
	int ffmpegCodecId = 0;
	int width = 0;
	int height = 0;
	VideoTimeBase timeBase;
	//H.264的SPS/PPS 或 H.265的VPS/SPS/PPS
	std::vector<std::uint8_t> extraData;
	bool nativeYuv = false;
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
	VideoPixelFormat pixelFormat = VideoPixelFormat::Bgra;
	// Borrowed views valid until the next receiveFrame, flush or close.
	// data/stride alias plane 0; YUV chroma size is ceil(width/2), ceil(height/2).
	std::array<const unsigned char*, 3> planes{};
	std::array<int, 3> strides{};
	VideoColorRange colorRange = VideoColorRange::Unspecified;
	VideoColorSpace colorSpace = VideoColorSpace::Unspecified;
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
