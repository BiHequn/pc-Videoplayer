#pragma once
#include "video-decoder.h"
#include<condition_variable>
#include<cstddef>
#include<deque>
#include<mutex>
#include<vector>

struct QueuedVideoFrame
{
	std::vector<unsigned char> pixels;
	int width = 0;
	int height = 0;
	int stride = 0;
	double ptsSeconds = -1.0;
	VideoPixelFormat pixelFormat = VideoPixelFormat::Bgra;
	// One owned allocation; offsets address top rows in pixels, strides are positive.
	std::array<std::size_t, 3> planeOffsets{};
	std::array<int, 3> planeStrides{};
	VideoColorRange colorRange = VideoColorRange::Unspecified;
	VideoColorSpace colorSpace = VideoColorSpace::Unspecified;
};

struct VideoFrameQueue
{
	std::deque<QueuedVideoFrame> frames;
	std::mutex mutex;
	std::condition_variable condition;
	bool finished = false;
	bool stopped = false;
};

bool pushVideoFrame(VideoFrameQueue* queue, const unsigned char* data, int width, int height, int stride, double ptsSeconds, size_t maxFrameCount);
bool pushVideoFrame(VideoFrameQueue* queue, const DecodedVideoFrame& source, size_t maxFrameCount);
bool tryPopVideoFrame(VideoFrameQueue* queue, QueuedVideoFrame* output);

void finishVideoFrameQueue(VideoFrameQueue* queue);
void stopVideoFrameQueue(VideoFrameQueue* queue);
void clearVideoFrameQueue(VideoFrameQueue* queue);
