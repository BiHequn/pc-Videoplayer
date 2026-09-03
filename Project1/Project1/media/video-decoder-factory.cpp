#include"video-decoder-factory.h"

#if NP_ENABLE_FFMPEG_VIDEO_DECODER
#include"ffmpeg-video-decoder.h"
#endif

#if NP_ENABLE_MF_VIDEO_DECODER
#include"mf-video-decoder.h"
#endif

std::unique_ptr<IVideoDecoder> createVideoDecoder(VideoDecoderBackend backend)
{
    switch (backend)
    {
#if NP_ENABLE_FFMPEG_VIDEO_DECODER
    case VideoDecoderBackend::Ffmpeg:
        return std::make_unique<FfmpegVideoDecoder>();
#endif
#if NP_ENABLE_MF_VIDEO_DECODER
    case VideoDecoderBackend::MediaFoundation:
        return std::make_unique<MfVideoDecoder>();
#endif
    case VideoDecoderBackend::Auto:
#if NP_ENABLE_MF_VIDEO_DECODER
        return std::make_unique<MfVideoDecoder>();
#elif NP_ENABLE_FFMPEG_VIDEO_DECODER
        return std::make_unique<FfmpegVideoDecoder>();
#else
        return nullptr;
#endif
    default:
        return nullptr;
    }
}
