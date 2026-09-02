#pragma once
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
bool tryPopVideoFrame(VideoFrameQueue* queue, QueuedVideoFrame* output);

void finishVideoFrameQueue(VideoFrameQueue* queue);
void stopVideoFrameQueue(VideoFrameQueue* queue);
void clearVideoFrameQueue(VideoFrameQueue* queue);