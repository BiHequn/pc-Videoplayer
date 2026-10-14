#include "media/ffmpeg-video-decoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifndef NP_TEST_NATIVE_CONTRACT
#define NP_TEST_NATIVE_CONTRACT 1
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static void check(int code) { require(code>=0,"FFmpeg fixture operation failed"); }
static void decodeFixture(AVPixelFormat format,bool native) {
    const AVCodec* codec=avcodec_find_encoder(AV_CODEC_ID_FFV1); require(codec!=nullptr,"FFV1 encoder unavailable");
    AVCodecContext* encoder=avcodec_alloc_context3(codec);
    encoder->width=5; encoder->height=3; encoder->pix_fmt=format; encoder->time_base={1,25};
    encoder->color_range=AVCOL_RANGE_MPEG; encoder->colorspace=AVCOL_SPC_BT709;
    check(avcodec_open2(encoder,codec,nullptr));
    VideoDecoderConfig config; config.ffmpegCodecId=AV_CODEC_ID_FFV1; config.width=5; config.height=3; config.timeBase={1,25};
    if(encoder->extradata_size>0) config.extraData.assign(encoder->extradata,encoder->extradata+encoder->extradata_size);
#if NP_TEST_NATIVE_CONTRACT
    config.nativeYuv=native;
#endif
    FfmpegVideoDecoder decoder; require(decoder.initialize(config),"decoder initialize failed");
    AVFrame* input=av_frame_alloc(); input->format=format; input->width=5; input->height=3;
    input->color_range=AVCOL_RANGE_MPEG; input->colorspace=AVCOL_SPC_BT709;
    check(av_frame_get_buffer(input,32));
    for(int frameNo=0;frameNo<2;++frameNo) {
        check(av_frame_make_writable(input));
        for(int y=0;y<3;++y) for(int x=0;x<5;++x) input->data[0][y*input->linesize[0]+x]=static_cast<unsigned char>(60+frameNo*20+x+y*5);
        const int cw=format==AV_PIX_FMT_YUV420P?3:5, ch=format==AV_PIX_FMT_YUV420P?2:3;
        for(int p=1;p<3;++p) for(int y=0;y<ch;++y) std::memset(input->data[p]+y*input->linesize[p],p==1?90:240,cw);
        input->pts=frameNo; check(avcodec_send_frame(encoder,input));
        AVPacket* packet=av_packet_alloc(); check(avcodec_receive_packet(encoder,packet));
        EncodedVideoPacket encoded; encoded.data=packet->data; encoded.size=packet->size; encoded.pts=frameNo; encoded.dts=frameNo; encoded.hasPts=encoded.hasDts=true;
        require(decoder.sendPacket(encoded)==VideoDecoderResult::Ok,"send fixture failed"); av_packet_free(&packet);
        DecodedVideoFrame output; require(decoder.receiveFrame(&output)==VideoDecoderResult::FrameReady,"receive fixture failed");
        require(output.width==5&&output.height==3,"decoded dimensions changed");
        require(std::abs(output.ptsSeconds-frameNo/25.0)<.000001,"timestamp changed");
        if(native) {
#if NP_TEST_NATIVE_CONTRACT
            require(output.pixelFormat==VideoPixelFormat::Yuv420p,"native decode still returns BGRA");
            require(output.data==output.planes[0]&&output.stride==output.strides[0],"legacy plane aliases wrong");
            for(int p=0;p<3;++p) require(output.planes[p]!=nullptr&&output.strides[p]>=(p?3:5),"planar pointer/stride missing");
            // FFV1 losslessly reproduces luma: verifies decode exposes Y rather than CPU RGB.
            for(int y=0;y<3;++y) for(int x=0;x<5;++x) require(output.planes[0][y*output.strides[0]+x]==60+frameNo*20+x+y*5,"native luma differs from encoded Y");
            require(output.planes[1][output.strides[1]+2]==90&&output.planes[2][output.strides[2]+2]==240,"odd chroma last row/column missing");
#else
            require(output.data[0]==60+frameNo*20,"native luma differs: decoder still converts to BGRA");
#endif
        } else {
#if NP_TEST_NATIVE_CONTRACT
            require(output.pixelFormat==VideoPixelFormat::Bgra,"default decode changed from BGRA");
            require(output.planes[0]==output.data&&output.strides[0]==output.stride,"BGRA aliases missing");
#endif
            require(output.stride==20&&output.data[3]==255,"BGRA fallback layout changed");
        }
    }
    require(decoder.drain()==VideoDecoderResult::Ok,"drain failed");
    DecodedVideoFrame output; output.data=input->data[0];
    require(decoder.receiveFrame(&output)==VideoDecoderResult::EndOfStream,"EOF failed");
    require(output.data==nullptr,"EOF retained an old public pixel pointer");
    decoder.flush(); decoder.close(); decoder.close();
    output.data=input->data[0];
    require(decoder.receiveFrame(&output)==VideoDecoderResult::Error,"closed decoder accepted receive");
    require(output.data==nullptr,"error retained an old public pixel pointer");
    av_frame_free(&input); avcodec_free_context(&encoder);
}
int main() {
    try {
        decodeFixture(AV_PIX_FMT_YUV420P,true);
        decodeFixture(AV_PIX_FMT_YUV444P,true);
        decodeFixture(AV_PIX_FMT_YUV420P,false);
        std::cout<<"PASS: native YUV420P, YUV444P conversion, odd chroma, frame lifetime, BGRA compatibility\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
