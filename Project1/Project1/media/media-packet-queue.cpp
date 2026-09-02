#include "media-packet-queue.h"

bool pushMediaPacket(
  MediaPacketQueue* queue,
  const AVPacket* packet,
  size_t maxPacketCount)
{
  if (!queue || !packet)
    return false;

  AVPacket* copiedPacket = av_packet_clone(packet);
  if (!copiedPacket)
    return false;

  std::unique_lock<std::mutex> lock(queue->mutex);

  queue->condition.wait(
    lock,
    [queue, maxPacketCount]()
    {
      return queue->stopped ||
        maxPacketCount == 0 ||
        queue->packets.size() < maxPacketCount;
    });

  if (queue->stopped)
  {
    lock.unlock();
    av_packet_free(&copiedPacket);
    return false;
  }

  queue->packets.push_back(copiedPacket);
  lock.unlock();
  queue->condition.notify_all();
  return true;
}

AVPacket* popMediaPacket(MediaPacketQueue* queue)
{
  if (!queue)
    return nullptr;

  std::unique_lock<std::mutex> lock(queue->mutex);

  queue->condition.wait(
    lock,
    [queue]()
    {
      return queue->stopped ||
        queue->finished ||
        !queue->packets.empty();
    });

  if (queue->packets.empty())
    return nullptr;

  AVPacket* packet = queue->packets.front();
  queue->packets.pop_front();

  lock.unlock();
  queue->condition.notify_all();
  return packet;
}

void finishMediaPacketQueue(MediaPacketQueue* queue)
{
  if (!queue)
    return;

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->finished = true;
  }

  queue->condition.notify_all();
}

void stopMediaPacketQueue(MediaPacketQueue* queue)
{
  if (!queue)
    return;

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->stopped = true;
  }

  queue->condition.notify_all();
}

void clearMediaPacketQueue(MediaPacketQueue* queue)
{
  if (!queue)
    return;

  std::lock_guard<std::mutex> lock(queue->mutex);

  while (!queue->packets.empty())
  {
    AVPacket* packet = queue->packets.front();
    queue->packets.pop_front();
    av_packet_free(&packet);
  }

  queue->finished = false;
  queue->stopped = false;
}
