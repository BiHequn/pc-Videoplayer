#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "app/playback-session.h"
#include "media/ffmpeg-decoder.h"
#include "media/loopback-http.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void require(bool condition,const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::string avError(int value) { char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(value,text,sizeof(text)); return text; }
void checked(int value,const char* operation) { require(value >= 0,std::string(operation)+": "+avError(value)); }
template<class Predicate> void until(Predicate predicate,std::chrono::milliseconds timeout,const std::string& message) {
    const auto end = Clock::now()+timeout;
    while (!predicate()) { require(Clock::now()<end,message); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
void healthy(PlaybackSession& player) {
    const auto s = player.snapshot(); require(s.state != PlaybackState::Error,"session error: "+s.error);
}
std::shared_ptr<const PlaybackFrame> nativeFrame(PlaybackSession& player) {
    healthy(player); const auto frame = player.frame();
    require(frame && frame->pixelFormat == VideoPixelFormat::Yuv420p && frame->width == 320 && frame->height == 180,
        "session did not publish native YUV420P frame");
    for (int plane=0;plane<3;++plane) {
        const auto width = plane ? 160 : 320, height = plane ? 90 : 180;
        require(frame->planeStrides[plane] >= width && frame->planeOffsets[plane]+std::size_t(frame->planeStrides[plane])*height <= frame->pixels.size(),
            "native YUV plane is not fully owned/in bounds");
    }
    require(frame->planeOffsets[1] >= 320*180 && frame->planeOffsets[2] > frame->planeOffsets[1],"native chroma plane offsets invalid");
    return frame;
}
class HttpOrigin {
public:
    struct FlvLayout {
        std::uint64_t size=0,lastVideoEnd=0;
        unsigned videoTags=0,audioTags=0;
        double lastVideoPts=0;
    };
    explicit HttpOrigin(const fs::path& directory) {
        require(fs::exists(directory/"hls/index.m3u8"),"missing generated HLS fixture; run make-stream-media first");
        for (const auto& entry : fs::recursive_directory_iterator(directory)) if(entry.is_regular_file()) {
            std::ifstream input(entry.path(),std::ios::binary);
            files.emplace("/"+fs::relative(entry.path(),directory).generic_u8string(),
                std::vector<unsigned char>{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()});
        }
        require(server.start([this](const LocalHttpRequest& req,LocalHttpResponse& out){serve(req,out);}),"HTTP origin failed");
    }
    ~HttpOrigin() { stopping=true; held=false; server.stop(); }
    std::string url(const std::string& path="/hls/index.m3u8") const { return "http://127.0.0.1:"+std::to_string(server.port())+path; }
    FlvLayout flvLayout() const {
        const auto entry=files.find("/stream.flv");
        require(entry!=files.end(),"missing generated stream.flv with PCM audio");
        const auto& b=entry->second;
        require(b.size()>13&&b[0]=='F'&&b[1]=='L'&&b[2]=='V'&&(b[4]&5)==5,"fixture is not audio/video FLV");
        auto be24=[&](std::size_t p){return (std::uint32_t(b[p])<<16)|(std::uint32_t(b[p+1])<<8)|b[p+2];};
        auto be32=[&](std::size_t p){return (std::uint32_t(b[p])<<24)|(std::uint32_t(b[p+1])<<16)|(std::uint32_t(b[p+2])<<8)|b[p+3];};
        FlvLayout result;result.size=b.size();
        for(std::size_t pos=be32(5)+4;pos<b.size();) {
            require(pos+15<=b.size(),"truncated FLV tag header");
            const auto payload=be24(pos+1);const auto end=pos+15+payload;
            require(end<=b.size()&&be32(end-4)==payload+11,"invalid FLV tag boundary");
            if(b[pos]==9) {
                require(payload>0,"empty FLV video tag");++result.videoTags;result.lastVideoEnd=end;
                result.lastVideoPts=((std::uint32_t(b[pos+7])<<24)|be24(pos+4))/1000.0;
            } else if(b[pos]==8) {
                require(payload>1&&(b[pos+11]>>4)==3,"FLV audio must be builtin PCM S16LE");++result.audioTags;
            }
            pos=end;
        }
        require(result.videoTags==300&&result.audioTags==300&&result.lastVideoEnd<result.size&&result.lastVideoPts>=11.9,
            "FLV fixture lacks expected 12-second video and trailing real PCM audio");
        return result;
    }
    std::atomic<bool> held{false};
    // Gate exactly after the final video tag, before the trailing audio/EOF.
    std::atomic<std::uint64_t> flvHoldAt{UINT64_MAX};
    std::atomic<bool> flvTailWaiting{false};
    std::atomic<int> delayMs{2};
    std::atomic<std::uint64_t> sent{0};
private:
    std::map<std::string,std::vector<unsigned char>> files;
    LoopbackHttp server; std::atomic<bool> stopping{false};
    void serve(const LocalHttpRequest& req,LocalHttpResponse& out) {
        const auto entry=files.find(req.path);
        if(entry==files.end()){out.headers(404,0,"text/plain");return;}
        const auto& bytes=entry->second;
        std::uint64_t first=0,last=bytes.size()-1;
        if(!req.range.empty()) {
            try {const auto dash=req.range.find('-');first=std::stoull(req.range.substr(6,dash-6));
                if(dash+1<req.range.size())last=std::min<std::uint64_t>(last,std::stoull(req.range.substr(dash+1)));
                require(first<=last,"invalid range");
            }catch(...){out.headers(416,0,"text/plain");return;}
        }
        const bool partial=!req.range.empty(),playlist=req.path.find("m3u8")!=std::string::npos;
        const auto extra="ETag: \"network-fixture-"+std::to_string(bytes.size())+"\"\r\nAccept-Ranges: bytes\r\n"+
            (partial?"Content-Range: bytes "+std::to_string(first)+"-"+std::to_string(last)+"/"+std::to_string(bytes.size())+"\r\n":"");
        const bool flv=req.path=="/stream.flv";
        if(!out.headers(partial?206:200,last-first+1,playlist?"application/vnd.apple.mpegurl":(flv?"video/x-flv":"video/mp2t"),extra)||req.method=="HEAD")return;
        for(auto pos=first;pos<=last&&!stopping;) {
            while(held&&!playlist&&!stopping)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if(stopping)return;
            if(flv&&pos>=flvHoldAt.load()) {
                flvTailWaiting=true;
                while(pos>=flvHoldAt.load()&&!stopping)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                flvTailWaiting=false;
                if(stopping)return;
            }
            auto count=std::min<std::uint64_t>(8192,last-pos+1);
            if(flv)count=std::min(count,flvHoldAt.load()-pos);
            if(!out.write(bytes.data()+pos,static_cast<std::size_t>(count))) {
                if(flv)std::cerr<<"FLV origin write failed offset="<<pos<<" range="<<req.range<<'\n';
                return;
            }
            sent+=count;pos+=count;
            if(!playlist)std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
        }
    }
};
void directFlvAudio(const fs::path& fixtures) {
    HttpOrigin origin(fixtures);const auto layout=origin.flvLayout();PlaybackSession player;
    const auto source=origin.url("/stream.flv");const auto started=Clock::now();
    player.open(source,VideoDecoderBackend::Ffmpeg,true);
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Playing&&player.frame();},
        std::chrono::seconds(10),"direct FLV audio did not play");
    const auto retained=nativeFrame(player);const auto pixels=retained->pixels;
    require(player.snapshot().source==source&&player.snapshot().backend==VideoDecoderBackend::Ffmpeg&&player.snapshot().rate==1.0,
        "FLV did not retain direct FFmpeg source at audible 1x");
    require(player.snapshot().duration>=11.9&&player.snapshot().duration<=12.1,"FLV audio/video duration incorrect");
    require(origin.sent<layout.lastVideoEnd,"FLV downloaded its tail before the progressive first frame");
    origin.flvHoldAt=layout.lastVideoEnd;
    // This accessor reads the backend's atomic WOM_DONE counter. A media audio
    // stream alone is insufficient: actual device completions must be observed.
    until([&]{healthy(player);return getFfmpegAudioCompletionCount()>=4;},std::chrono::seconds(3),
        "FLV PCM did not produce actual WaveOut completion callbacks");
    std::cout<<"FLV direct native + PCM first frame/audio in "<<
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()<<"ms; callbacks="<<getFfmpegAudioCompletionCount()<<'\n';
    origin.held=true;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Buffering;},std::chrono::seconds(6),
        "FLV audio network hold did not enter Buffering");
    const double frozen=player.snapshot().position;const auto serial=player.frame()->serial;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto frozenCallbacks=getFfmpegAudioCompletionCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    require(player.snapshot().state==PlaybackState::Buffering&&std::abs(player.snapshot().position-frozen)<0.05,
        "FLV audio buffering did not freeze playback position");
    require(getFfmpegAudioCompletionCount()==frozenCallbacks,"WaveOut kept draining while FLV was buffering");
    origin.delayMs=0;origin.held=false;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Playing&&player.frame()->serial>serial&&
        getFfmpegAudioCompletionCount()>frozenCallbacks;},std::chrono::seconds(4),"FLV did not resume native video and real audio after release");
    std::cout<<"FLV audio buffering/resume position="<<frozen<<" -> "<<player.snapshot().position<<
        " callbacks="<<frozenCallbacks<<" -> "<<getFfmpegAudioCompletionCount()<<'\n';
    auto progress=Clock::now();
    until([&]{healthy(player);if(Clock::now()-progress>std::chrono::seconds(2)) {
            progress=Clock::now();std::cout<<"FLV tail progress state="<<int(player.snapshot().state)<<" position="<<player.snapshot().position<<
                " sent="<<origin.sent<<" callbacks="<<getFfmpegAudioCompletionCount()<<'\n';
        }return origin.flvTailWaiting.load();},std::chrono::seconds(15),"FLV origin never held after last video tag");
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Buffering;},std::chrono::seconds(4),
        "FLV final-video hold did not enter Buffering before audio tail/EOF");
    const auto tail=nativeFrame(player);const auto tailCallbacks=getFfmpegAudioCompletionCount();
    const double tailPosition=player.snapshot().position;
    require(tailPosition>=layout.lastVideoPts-.05,"FLV tail hold happened before final video presentation");
    require(origin.sent==layout.lastVideoEnd,"FLV tail hold leaked trailing audio bytes");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    require(player.snapshot().state==PlaybackState::Buffering&&std::abs(player.snapshot().position-tailPosition)<.05,
        "FLV final-video buffering was not held stably");
    const auto released=Clock::now();origin.flvHoldAt=UINT64_MAX;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Ended;},std::chrono::seconds(4),
        "FLV EOF while Buffering did not restart and drain tail audio to Ended");
    require(player.frame()&&player.frame()->serial==tail->serial,"FLV EOF recovery required an unexpected new video frame");
    require(player.snapshot().position>=11.9&&player.snapshot().rate==1.0&&origin.sent==layout.size,
        "FLV audio EOF truncated media or bypassed audio through a playback rate change");
    require(getFfmpegAudioCompletionCount()>tailCallbacks,"FLV trailing PCM did not complete after buffered EOF release");
    require(retained->pixels==pixels,"FLV retained native YUV planes changed");
    std::cout<<"PASS direct FLV real PCM/native YUV, buffering/audio resume, final-video -> Buffering -> audio-only EOF/Ended; position="<<
        tailPosition<<" -> "<<player.snapshot().position<<" tailCallbacks="<<tailCallbacks<<" -> "<<getFfmpegAudioCompletionCount()<<
        " EOF release="<<std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-released).count()<<"ms\n";
}
void directHls(const fs::path& fixtures) {
    HttpOrigin origin(fixtures); PlaybackSession player;
    const auto source=origin.url(); const auto started=Clock::now();
    player.open(source,VideoDecoderBackend::Ffmpeg,true);
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Playing&&player.frame();},std::chrono::seconds(12),"direct HLS did not play");
    const auto retained=nativeFrame(player); const auto pixels=retained->pixels;
    require(player.snapshot().source==source&&player.snapshot().backend==VideoDecoderBackend::Ffmpeg,"session did not retain direct network source/backend");
    std::cout<<"HLS direct native first frame in "<<std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()<<"ms\n";
    player.pause(true);until([&]{healthy(player);return player.snapshot().state==PlaybackState::Paused;},std::chrono::seconds(2),"HLS pause failed");
    const auto serial=player.frame()->serial;player.seek(3.013);
    until([&]{healthy(player);return player.frame()&&player.frame()->serial>serial;},std::chrono::seconds(7),"HLS paused preview failed");
    nativeFrame(player);require(player.snapshot().state==PlaybackState::Paused&&player.snapshot().position>=3.013&&player.snapshot().position<3.12,"HLS paused seek position/state incorrect");
    const double paused=player.snapshot().position;std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(std::abs(player.snapshot().position-paused)<0.01&&retained->pixels==pixels,"paused clock advanced or retained YUV pixels changed");
    std::cout<<"HLS paused preview position="<<paused<<" nativeSerial="<<player.frame()->serial<<'\n';
    origin.held=true;player.pause(false);
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Buffering;},std::chrono::seconds(8),"HLS network starvation did not enter Buffering");
    const double bufferPosition=player.snapshot().position;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    require(player.snapshot().state==PlaybackState::Buffering&&std::abs(player.snapshot().position-bufferPosition)<0.05,"buffering failed to freeze clock");
    const auto bufferedSerial=player.frame()->serial;origin.held=false;origin.delayMs=0;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Playing&&player.frame()->serial>bufferedSerial;},std::chrono::seconds(5),"HLS did not resume after origin release");
    std::cout<<"HLS buffering/resume position="<<bufferPosition<<" -> "<<player.snapshot().position<<'\n';
    player.open(source,VideoDecoderBackend::Ffmpeg,true);
    until([&]{healthy(player);return player.frame()&&player.frame()->serial>bufferedSerial&&player.snapshot().position<0.5;},std::chrono::seconds(7),"HLS reopen did not reset playback");
    nativeFrame(player);player.setRate(4.0);
    const auto endStarted=Clock::now();
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Ended;},std::chrono::seconds(7),"direct HLS did not reach Ended at 4x");
    require(player.snapshot().position>=11.9,"direct HLS EOF position truncated");
    std::cout<<"HLS direct 4x EOF position="<<player.snapshot().position<<" elapsed="<<
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-endStarted).count()<<"ms\n";
    player.open(source,VideoDecoderBackend::Ffmpeg,true);
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Playing;},std::chrono::seconds(6),"HLS stop scenario reopen failed");
    origin.held=true;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Buffering;},std::chrono::seconds(8),"HLS stop test never blocked in network buffering");
    const auto stopStarted=Clock::now();player.stop();
    until([&]{return player.snapshot().state==PlaybackState::Idle;},std::chrono::seconds(2),"stop did not interrupt held HLS read promptly");
    require(!player.frame(),"HLS stop retained frame");
    std::cout<<"PASS direct HLS native YUV, paused seek, retained planes, buffering/resume, reopen/4x/end; held stop="<<
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-stopStarted).count()<<"ms\n";
}

std::uint16_t availablePort() {
    WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"socket init");
    SOCKET socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(socket!=INVALID_SOCKET,"reserve RTMP port");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"bind RTMP port");
    int size=sizeof(address);getsockname(socket,reinterpret_cast<sockaddr*>(&address),&size);
    const auto port=ntohs(address.sin_port);closesocket(socket);WSACleanup();return port;
}
class RtmpPublisher {
public:
    RtmpPublisher(const fs::path& fixture,std::string url):source(fixture.u8string()),destination(std::move(url)) {
        worker=std::thread([this]{publish();});
    }
    ~RtmpPublisher(){stop=true;held=false;if(worker.joinable())worker.join();}
    std::atomic<bool> ready{false},done{false},held{false};
    std::atomic<std::uint64_t> packets{0};
    std::string error() {std::lock_guard<std::mutex> lock(mutex);return failure;}
private:
    std::string source,destination,failure;
    std::mutex mutex;std::atomic<bool> stop{false};std::thread worker;
    void publish() {
        AVFormatContext* input=nullptr;AVFormatContext* output=nullptr;AVPacket* packet=av_packet_alloc();
        try {
            checked(avformat_open_input(&input,source.c_str(),nullptr,nullptr),"open FLV source");
            checked(avformat_find_stream_info(input,nullptr),"inspect FLV source");
            const int index=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);checked(index,"source video stream");
            checked(avformat_alloc_output_context2(&output,nullptr,"flv",destination.c_str()),"allocate live FLV output");
            auto* video=avformat_new_stream(output,nullptr);require(video!=nullptr,"allocate live video");
            checked(avcodec_parameters_copy(video->codecpar,input->streams[index]->codecpar),"copy live FLV1 codec");
            video->codecpar->codec_tag=0;video->time_base=input->streams[index]->time_base;
            output->interrupt_callback={[](void* user)->int{return static_cast<RtmpPublisher*>(user)->stop?1:0;},this};
            AVDictionary* options=nullptr;av_dict_set(&options,"listen","1",0);av_dict_set(&options,"listen_timeout","5",0);
            ready=true;
            const int opened=avio_open2(&output->pb,destination.c_str(),AVIO_FLAG_WRITE,&output->interrupt_callback,&options);
            av_dict_free(&options);checked(opened,"RTMP listen output avio_open2");
            AVDictionary* muxOptions=nullptr;av_dict_set(&muxOptions,"flvflags","no_duration_filesize",0);
            const int header=avformat_write_header(output,&muxOptions);av_dict_free(&muxOptions);checked(header,"RTMP FLV header");
            while(!stop&&av_read_frame(input,packet)>=0) {
                if(packet->stream_index==index) {
                    while(held&&!stop)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if(stop)break;
                    packet->stream_index=video->index;av_packet_rescale_ts(packet,input->streams[index]->time_base,video->time_base);
                    checked(av_interleaved_write_frame(output,packet),"write RTMP live packet");avio_flush(output->pb);++packets;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
                av_packet_unref(packet);
            }
        }catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);failure=e.what();}
        av_packet_free(&packet);avformat_close_input(&input);
        if(output){if(output->pb)avio_closep(&output->pb);avformat_free_context(output);}done=true;
    }
};
bool hasRtmp(bool output) {
    void* iterator=nullptr;const char* protocol;
    while((protocol=avio_enum_protocols(&iterator,output?1:0)))if(std::string(protocol)=="rtmp")return true;
    return false;
}
void directRtmp(const fs::path& fixtures) {
    require(hasRtmp(false)&&hasRtmp(true),"installed FFmpeg lacks built-in RTMP input/output protocol");
    const auto url="rtmp://127.0.0.1:"+std::to_string(availablePort())+"/live/fixture";
    RtmpPublisher publisher(fixtures/"stream.flv",url);PlaybackSession player;
    until([&]{return publisher.ready.load()||publisher.done.load();},std::chrono::seconds(3),"RTMP publisher startup");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    player.open(url,VideoDecoderBackend::Ffmpeg,true);
    until([&]{require(publisher.error().empty(),publisher.error());healthy(player);return player.snapshot().state==PlaybackState::Playing&&player.frame();},
        std::chrono::seconds(12),"direct RTMP listen fixture did not decode a frame");
    const auto retained=nativeFrame(player);const auto pixels=retained->pixels;
    require(player.snapshot().source==url&&publisher.packets>0,"RTMP session did not consume live publisher");
    require(player.snapshot().duration<=0,"RTMP fixture unexpectedly advertises a VOD duration");
    std::cout<<"RTMP live first native frame; packets="<<publisher.packets<<" duration="<<player.snapshot().duration<<'\n';
    publisher.held=true;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Buffering;},std::chrono::seconds(5),"RTMP live hold did not buffer");
    const double bufferPosition=player.snapshot().position;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    require(player.snapshot().state==PlaybackState::Buffering&&std::abs(player.snapshot().position-bufferPosition)<0.05,
        "RTMP buffering did not freeze the clock");
    const auto serial=player.frame()->serial;publisher.held=false;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Playing&&player.frame()->serial>serial;},std::chrono::seconds(4),"RTMP did not resume live frames");
    require(retained->pixels==pixels,"RTMP retained native planes changed after buffering/resume");
    std::cout<<"RTMP buffering/resume position="<<bufferPosition<<" -> "<<player.snapshot().position<<'\n';
    publisher.held=true;
    until([&]{healthy(player);return player.snapshot().state==PlaybackState::Buffering;},std::chrono::seconds(5),"RTMP second hold did not buffer");
    const auto before=Clock::now();player.stop();
    until([&]{return player.snapshot().state==PlaybackState::Idle;},std::chrono::seconds(2),"RTMP graceful stop did not interrupt network read");
    std::cout<<"PASS direct RTMP built-in listen server + FLV1 live packets, native YUV, buffering/resume, stop="<<
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-before).count()<<"ms\n";
}
}
int main(int argc,char** argv) {
    try {
        require(argc>=3,"usage: network-session-test generated-fixtures output-root [--hls|--rtmp|--flv-audio|--all|--rtmp-capabilities]");
        require(avformat_network_init()>=0,"network initialization");
        const std::string mode=argc>3?argv[3]:"--all";
        const auto fixtures=fs::absolute(fs::u8path(argv[1]));fs::create_directories(fs::u8path(argv[2]));
        std::cout<<"RTMP protocol input="<<hasRtmp(false)<<" output="<<hasRtmp(true)<<"; FFmpeg="<<av_version_info()<<'\n';
        require(mode=="--all"||mode=="--hls"||mode=="--rtmp"||mode=="--flv-audio"||mode=="--rtmp-capabilities","unknown scenario");
        if(mode=="--all"||mode=="--hls")directHls(fixtures);
        if(mode=="--all"||mode=="--rtmp")directRtmp(fixtures);
        if(mode=="--all"||mode=="--flv-audio")directFlvAudio(fixtures);
        avformat_network_deinit();return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';avformat_network_deinit();return 1;}
}
