#include "video-frame-queue.h"
#include<utility>


bool pushVideoFrame(VideoFrameQueue* queue, const unsigned char* data, int width, int height, int stride, double ptsSeconds, size_t maxFrameCount)
{
	if (!queue || !data || width <= 0 || height <= 0 || stride <= 0 || maxFrameCount == 0)
	{
		return false;
	}
	QueuedVideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.stride = stride;
	frame.ptsSeconds = ptsSeconds;

	const size_t dataSize = static_cast<size_t>(stride) * static_cast<size_t>(height);
	//必须复制BGRA数据，原始数据指向解码器复用的g_bgraBuffer，下一帧解码时会被覆盖
	frame.pixels.assign(data, data + dataSize);
	std::unique_lock<std::mutex> lock(queue->mutex);

	//队列已满时让视频解码线程等待窗口线程消费
	queue->condition.wait(lock, [queue, maxFrameCount]()
		{
			return queue->stopped || queue->frames.size() < maxFrameCount;
		});
	if (queue->stopped)
	{
		return false;
	}
	queue->frames.push_back(std::move(frame));
	lock.unlock();
	queue->condition.notify_all();
	return true;
	}

bool tryPopVideoFrame(VideoFrameQueue* queue, QueuedVideoFrame* output)
{
	if (!queue || !output)
	{
		return false;
	}
	std::unique_lock<std::mutex> lock(queue->mutex);
	//窗口定时器不能阻塞，没有帧时直接返回
		if (queue->frames.empty())
		{
			return false;
	}
		*output = std::move(queue->frames.front());
		queue->frames.pop_front();
		lock.unlock();
		queue->condition.notify_all();
		
		return true;
}

void finishVideoFrameQueue(VideoFrameQueue* queue)
{
	if (!queue)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue->mutex);
		queue->finished = true;
	}
	queue->condition.notify_all();
}

void stopVideoFrameQueue(VideoFrameQueue* queue)
{
	if (!queue)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue->mutex);
		queue->stopped = true;
	}
	queue->condition.notify_all();
}

void clearVideoFrameQueue(VideoFrameQueue* queue)
{
	if (!queue)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue->mutex);
		queue->frames.clear();
		queue->finished = false;
		queue->stopped = false;
	}
	queue->condition.notify_all();
}
