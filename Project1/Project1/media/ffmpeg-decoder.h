#pragma once
#include"video-decoder.h"
#include"video-decoder-factory.h"

#include<memory>

bool openFfmpegVideo(const char* source,std::unique_ptr<IVideoDecoder> videoDecoder,VideoDecoderBackend backend);
bool decodeNextFfmpegFrame(DecodedVideoFrame* output);
bool isFfmpegVideoFinished();
bool isFfmpegPlaybackFinished();
void closeFfmpegVideo();

unsigned int getFfmpegFrameIntervalMs();
void startFfmpegPlayback();
void setFfmpegAudioPaused(bool paused);
double getFfmpegAudioClockSeconds();
