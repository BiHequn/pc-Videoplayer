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
double getFfmpegPlaybackClockSeconds();
void advanceFfmpegPlaybackClock();
void setFfmpegPlaybackRate(double rate);
double getFfmpegPlaybackRate();
bool seekFfmpegBySeconds(double deltaSeconds);

// Lifecycle, getters and controls belong to the playback worker. Only the
// atomic abort request may be called concurrently (including while opening).
// Install the probe on the worker before opening; clear after its final close.
// It is called by I/O/decode threads, must be thread safe, and must not call
// backend APIs. close/open/seek never reset the caller's cancellation state.
void setFfmpegCancellationProbe(bool (*probe)(void*), void* user);
double getFfmpegDurationSeconds();
bool seekFfmpegToPosition(double seconds);
// Decode seek preroll without starting audio, then leave playback paused.
// Frame storage follows decodeNextFfmpegFrame's lifetime; bounded to 5 seconds.
bool previewFfmpegAtPosition(double seconds, DecodedVideoFrame* frame);
void setFfmpegVolume(float normalized);
VideoDecoderBackend getFfmpegEffectiveBackend();
void requestFfmpegAbort();
void anchorFfmpegVideoClock(double ptsSeconds);
