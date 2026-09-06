#pragma once
#include"video-decoder.h"
#include"video-decoder-factory.h"

#include<memory>

enum class PlaybackEndReason
{
    None,
    Completed,
    DemuxError,
    DecodeError
};

bool openFfmpegVideo(const char* source,std::unique_ptr<IVideoDecoder> videoDecoder,VideoDecoderBackend backend);
bool decodeNextFfmpegFrame(DecodedVideoFrame* output);
bool isFfmpegVideoFinished();
bool isFfmpegPlaybackFinished();
PlaybackEndReason getFfmpegPlaybackEndReason();
void closeFfmpegVideo();

unsigned int getFfmpegFrameIntervalMs();
void startFfmpegPlayback();
void setFfmpegAudioPaused(bool paused);
double getFfmpegAudioClockSeconds();
