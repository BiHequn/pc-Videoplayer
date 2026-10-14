#include <filesystem>
#include <iostream>
#include <cmath>
#include <cstring>
#include <vector>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
void check(int result) { if(result < 0) throw std::runtime_error("fixture mux failed"); }
void generate(const std::filesystem::path& path, bool audio) {
    AVFormatContext* mux = nullptr;
    const auto name = path.u8string();
    check(avformat_alloc_output_context2(&mux, nullptr, "avi", name.c_str()));
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if(!codec) throw std::runtime_error("MPEG4 fixture encoder unavailable");
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    encoder->width=320; encoder->height=180; encoder->pix_fmt=AV_PIX_FMT_YUV420P;
    encoder->time_base={1,25}; encoder->framerate={25,1}; encoder->gop_size=75; encoder->max_b_frames=0;
    encoder->bit_rate=600000; check(avcodec_open2(encoder,codec,nullptr));
    AVStream* video = avformat_new_stream(mux, nullptr);
    video->time_base = {1,25};
    check(avcodec_parameters_from_context(video->codecpar,encoder));
    AVStream* sound = nullptr;
    if(audio) {
        sound=avformat_new_stream(mux,nullptr); sound->time_base={1,48000};
        auto* p=sound->codecpar; p->codec_type=AVMEDIA_TYPE_AUDIO; p->codec_id=AV_CODEC_ID_PCM_S16LE;
        p->format=AV_SAMPLE_FMT_S16; p->sample_rate=48000; p->bits_per_coded_sample=16;
        p->block_align=2; p->bit_rate=48000*16; av_channel_layout_default(&p->ch_layout,1);
    }
    check(avio_open(&mux->pb,name.c_str(),AVIO_FLAG_WRITE)); check(avformat_write_header(mux,nullptr));
    AVFrame* image=av_frame_alloc(); image->format=AV_PIX_FMT_YUV420P; image->width=320; image->height=180;
    check(av_frame_get_buffer(image,32));
    auto receive=[&] {
        AVPacket* packet=av_packet_alloc();
        while(avcodec_receive_packet(encoder,packet)>=0) {
            packet->stream_index=video->index; av_packet_rescale_ts(packet,encoder->time_base,video->time_base);
            check(av_interleaved_write_frame(mux,packet)); av_packet_unref(packet);
        }
        av_packet_free(&packet);
    };
    for(int frame=0;frame<100;++frame) {
        check(av_frame_make_writable(image));
        for(int y=0;y<180;++y) for(int x=0;x<320;++x)
            image->data[0][y*image->linesize[0]+x]=static_cast<unsigned char>(32+(x+y+frame*2)%190);
        for(int y=0;y<90;++y) for(int x=0;x<160;++x) {
            image->data[1][y*image->linesize[1]+x]=static_cast<unsigned char>(80+frame%70);
            image->data[2][y*image->linesize[2]+x]=static_cast<unsigned char>(140+x%50);
        }
        image->pts=frame; check(avcodec_send_frame(encoder,image)); receive();
        if(sound) {
            AVPacket* packet=av_packet_alloc(); check(av_new_packet(packet,1920*2));
            for(int sample=0;sample<1920;++sample) {
                const auto value=static_cast<int16_t>(1500*std::sin((frame*1920+sample)*2*3.141592653589793*440/48000));
                std::memcpy(packet->data+sample*2,&value,2);
            }
            packet->stream_index=sound->index; packet->pts=packet->dts=frame*1920; packet->duration=1920;
            av_packet_rescale_ts(packet,{1,48000},sound->time_base);
            check(av_interleaved_write_frame(mux,packet)); av_packet_free(&packet);
        }
    }
    check(avcodec_send_frame(encoder,nullptr)); receive();
    check(av_write_trailer(mux)); avio_closep(&mux->pb); avformat_free_context(mux);
    av_frame_free(&image); avcodec_free_context(&encoder);
}
int main(int argc,char** argv) {
    try { if(argc!=2) return 2; std::filesystem::path dir=std::filesystem::u8path(argv[1]);
      std::filesystem::create_directories(dir); generate(dir/"audiovisual.avi",true); generate(dir/"silent.avi",false);
      std::cout << "Generated 4-second audio/video and silent fixtures\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
