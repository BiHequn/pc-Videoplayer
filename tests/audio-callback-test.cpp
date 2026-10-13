#include "media/ffmpeg-decoder.h"
#include "media/ffmpeg-runtime.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>
int main(int argc,char** argv) {
    try {
        if(argc!=2||!initializeFfmpeg())throw std::runtime_error("fixture/init");
        if(!openFfmpegVideo(argv[1],createVideoDecoder(VideoDecoderBackend::Ffmpeg),VideoDecoderBackend::Ffmpeg))throw std::runtime_error("open audio fixture");
        startFfmpegPlayback();DecodedVideoFrame frame{};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!getFfmpegAudioCompletionCount()&&std::chrono::steady_clock::now()<deadline) {
            decodeNextFfmpegFrame(&frame);advanceFfmpegPlaybackClock();std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(!getFfmpegAudioCompletionCount())throw std::runtime_error("WaveOut did not signal audio completion callback");
        if(getFfmpegAudioClockSeconds()<=0)throw std::runtime_error("device audio clock not advancing");
        std::cout<<"PASS actual WOM_DONE callback and device clock, count="<<getFfmpegAudioCompletionCount()<<'\n';
        closeFfmpegVideo();shutdownFfmpeg();return 0;
    }catch(const std::exception& e){closeFfmpegVideo();std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
