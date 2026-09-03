#pragma once
#include"video-decoder.h"
#include<memory>

enum class VideoDecoderBackend
{
	Auto,
	Ffmpeg,
	MediaFoundation
};

std::unique_ptr<IVideoDecoder> createVideoDecoder(VideoDecoderBackend backend);