#pragma once
#include "video-frame-queue.h"
bool copyVideoFrame(const DecodedVideoFrame& source, QueuedVideoFrame* destination);
void decodedFrameView(const QueuedVideoFrame& source, DecodedVideoFrame* destination);
