#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

extern "C"
{
#include <libavcodec/packet.h>
}

struct MediaPacketQueue
{
  std::deque<AVPacket*> packets;
  std::mutex mutex;
  std::condition_variable condition;
  bool finished = false;
  bool stopped = false;
};

bool pushMediaPacket(
  MediaPacketQueue* queue,
  const AVPacket* packet,
  size_t maxPacketCount);

AVPacket* popMediaPacket(MediaPacketQueue* queue);

void finishMediaPacketQueue(MediaPacketQueue* queue);
void stopMediaPacketQueue(MediaPacketQueue* queue);
void clearMediaPacketQueue(MediaPacketQueue* queue);
