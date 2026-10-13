// Entirely synthetic fixtures; uses only the installed FFmpeg built-in encoders.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace {
namespace fs = std::filesystem;
void checked(int result, const char* operation) {
    if (result >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, text, sizeof(text));
    throw std::runtime_error(std::string(operation) + ": " + text);
}
struct Output {
    AVFormatContext* mux = nullptr;
    AVCodecContext* encoder = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    ~Output() {
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&encoder);
        if (mux) { if (mux->pb) avio_closep(&mux->pb); avformat_free_context(mux); }
    }
};
void generate(const fs::path& file, bool hls) {
    Output out;
    const auto filename = file.u8string();
    checked(avformat_alloc_output_context2(&out.mux, nullptr, hls ? "hls" : "flv", filename.c_str()), "allocate muxer");
    const auto* codec = avcodec_find_encoder(hls ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_FLV1);
    if (!codec) throw std::runtime_error("required built-in FLV1/MPEG-2 encoder unavailable");
    out.encoder = avcodec_alloc_context3(codec);
    if (!out.encoder) throw std::runtime_error("allocate encoder");
    auto* e = out.encoder;
    e->width = 320; e->height = 180; e->pix_fmt = AV_PIX_FMT_YUV420P;
    e->time_base = {1,25}; e->framerate = {25,1}; e->gop_size = hls ? 38 : 25;
    e->max_b_frames = 0; e->bit_rate = 4000000; e->thread_count = 1;
    // A fixed quantizer keeps these moving texture fixtures large enough for a
    // deterministic progressive assertion, without padding or downloaded media.
    e->flags |= AV_CODEC_FLAG_QSCALE; e->global_quality = 4 * FF_QP2LAMBDA;
    if (out.mux->oformat->flags & AVFMT_GLOBALHEADER) e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (hls) {
        e->flags |= AV_CODEC_FLAG_CLOSED_GOP;
        checked(av_opt_set_int(e->priv_data, "sc_threshold", 1000000000, 0), "disable scene GOPs");
    }
    checked(avcodec_open2(e, codec, nullptr), "open built-in encoder");
    auto* video = avformat_new_stream(out.mux, nullptr);
    if (!video) throw std::runtime_error("allocate video stream");
    video->time_base = e->time_base; video->avg_frame_rate = e->framerate;
    checked(avcodec_parameters_from_context(video->codecpar, e), "copy video parameters");
    AVStream* audio = nullptr;
    if (!hls) {
        audio = avformat_new_stream(out.mux, nullptr);
        if (!audio) throw std::runtime_error("allocate PCM stream");
        audio->time_base = {1,44100};
        auto* p = audio->codecpar;
        p->codec_type = AVMEDIA_TYPE_AUDIO; p->codec_id = AV_CODEC_ID_PCM_S16LE;
        p->format = AV_SAMPLE_FMT_S16; p->sample_rate = 44100;
        p->bits_per_coded_sample = 16; p->block_align = 2; p->bit_rate = 44100 * 16;
        av_channel_layout_default(&p->ch_layout, 1);
    }
    AVDictionary* options = nullptr;
    if (hls) {
        av_dict_set(&options, "hls_time", "1.5", 0);
        av_dict_set(&options, "hls_list_size", "0", 0);
        av_dict_set(&options, "hls_playlist_type", "vod", 0);
        av_dict_set(&options, "hls_flags", "independent_segments", 0);
        av_dict_set(&options, "hls_segment_filename", (file.parent_path()/"segment-%02d.ts").u8string().c_str(), 0);
    }
    if (!(out.mux->oformat->flags & AVFMT_NOFILE))
        checked(avio_open(&out.mux->pb, filename.c_str(), AVIO_FLAG_WRITE), "open fixture output");
    const int header = avformat_write_header(out.mux, &options);
    av_dict_free(&options); checked(header, "write fixture header");
    out.frame = av_frame_alloc(); out.packet = av_packet_alloc();
    if (!out.frame || !out.packet) throw std::runtime_error("allocate frame/packet");
    out.frame->format = e->pix_fmt; out.frame->width = e->width; out.frame->height = e->height;
    out.frame->quality = e->global_quality;
    checked(av_frame_get_buffer(out.frame, 32), "allocate pixels");
    auto drain = [&] {
        for (;;) {
            const int result = avcodec_receive_packet(e, out.packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            checked(result, "receive encoded video");
            out.packet->stream_index = video->index;
            av_packet_rescale_ts(out.packet, e->time_base, video->time_base);
            checked(av_interleaved_write_frame(out.mux, out.packet), "mux video");
            av_packet_unref(out.packet);
        }
    };
    std::uint32_t random = 0x13579bdu;
    for (int n = 0; n < 300; ++n) {
        checked(av_frame_make_writable(out.frame), "make pixels writable");
        for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            out.frame->data[0][y*out.frame->linesize[0]+x] = static_cast<unsigned char>(32 + ((random >> 24) % 192));
        }
        for (int y = 0; y < 90; ++y) for (int x = 0; x < 160; ++x) {
            out.frame->data[1][y*out.frame->linesize[1]+x] = static_cast<unsigned char>(80 + (x+n)%80);
            out.frame->data[2][y*out.frame->linesize[2]+x] = static_cast<unsigned char>(90 + (y+n)%80);
        }
        out.frame->pts = n;
        checked(avcodec_send_frame(e, out.frame), "encode video"); drain();
        if (audio) {
            constexpr int samples = 44100 / 25;
            checked(av_new_packet(out.packet, samples*2), "allocate PCM packet");
            for (int s = 0; s < samples; ++s) {
                const auto value = static_cast<std::int16_t>(1500 * std::sin((n*samples+s)*2*3.141592653589793*440/44100));
                std::memcpy(out.packet->data+s*2, &value, 2);
            }
            out.packet->stream_index = audio->index; out.packet->pts = out.packet->dts = n*samples;
            out.packet->duration = samples;
            av_packet_rescale_ts(out.packet, {1,44100}, audio->time_base);
            checked(av_interleaved_write_frame(out.mux, out.packet), "mux PCM"); av_packet_unref(out.packet);
        }
    }
    checked(avcodec_send_frame(e, nullptr), "flush encoder"); drain();
    checked(av_write_trailer(out.mux), "finish fixture");
    std::cout << "Generated " << filename << " (320x180, 25 fps, 12 seconds)\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: make-stream-media fixture-directory");
        const auto dir = fs::absolute(fs::u8path(argv[1]));
        fs::create_directories(dir / "hls");
        generate(dir / "stream.flv", false); generate(dir / "hls" / "index.m3u8", true);
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
