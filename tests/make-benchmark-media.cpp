#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace {
namespace fs = std::filesystem;
void check(int result, const char* operation) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
struct Encoding {
    AVFormatContext* mux = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    ~Encoding() {
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec);
        if (mux) { if (mux->pb) avio_closep(&mux->pb); avformat_free_context(mux); }
    }
};
struct Resolution { const char* name; int width, height; };
constexpr Resolution resolutions[]{{"720p",1280,720},{"1080p",1920,1080},{"4k",3840,2160}};

void generate(const fs::path& file, Resolution size, int seconds, int fps) {
    Encoding out;
    const auto filename = file.u8string();
    check(avformat_alloc_output_context2(&out.mux, nullptr, "avi", filename.c_str()), "allocate AVI muxer");
    if (!out.mux) throw std::runtime_error("AVI muxer unavailable");
    const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!encoder) throw std::runtime_error("built-in MPEG4 encoder unavailable");
    out.codec = avcodec_alloc_context3(encoder);
    if (!out.codec) throw std::runtime_error("allocate encoder");
    auto* codec = out.codec;
    codec->width = size.width; codec->height = size.height; codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = {1,fps}; codec->framerate = {fps,1}; codec->gop_size = fps;
    codec->max_b_frames = 0; codec->thread_count = 1;
    codec->flags |= AV_CODEC_FLAG_QSCALE | AV_CODEC_FLAG_BITEXACT;
    codec->global_quality = 4 * FF_QP2LAMBDA;
    codec->color_range = AVCOL_RANGE_MPEG; codec->colorspace = AVCOL_SPC_BT709;
    if (out.mux->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    out.mux->flags |= AVFMT_FLAG_BITEXACT;
    check(avcodec_open2(codec, encoder, nullptr), "open MPEG4 encoder");
    auto* video = avformat_new_stream(out.mux, nullptr);
    if (!video) throw std::runtime_error("allocate stream");
    video->time_base = codec->time_base; video->avg_frame_rate = codec->framerate;
    check(avcodec_parameters_from_context(video->codecpar, codec), "copy stream properties");
    check(avio_open(&out.mux->pb, filename.c_str(), AVIO_FLAG_WRITE), "open fixture file");
    check(avformat_write_header(out.mux, nullptr), "write AVI header");
    out.frame = av_frame_alloc(); out.packet = av_packet_alloc();
    if (!out.frame || !out.packet) throw std::runtime_error("allocate frame/packet");
    out.frame->format = codec->pix_fmt; out.frame->width = size.width; out.frame->height = size.height;
    out.frame->quality = codec->global_quality;
    out.frame->color_range = codec->color_range; out.frame->colorspace = codec->colorspace;
    check(av_frame_get_buffer(out.frame, 32), "allocate YUV420P image");
    auto drain = [&] {
        for (;;) {
            const int result = avcodec_receive_packet(codec, out.packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            check(result, "receive MPEG4 packet");
            out.packet->stream_index = video->index;
            av_packet_rescale_ts(out.packet, codec->time_base, video->time_base);
            check(av_interleaved_write_frame(out.mux, out.packet), "write video packet");
            av_packet_unref(out.packet);
        }
    };
    // Fixed integer pattern, normalized across sizes: moving bars, checkerboard,
    // and gradients. No external media, wall clock, random seed, audio or padding.
    for (int n = 0; n < seconds * fps; ++n) {
        check(av_frame_make_writable(out.frame), "make frame writable");
        for (int y = 0; y < size.height; ++y) {
            auto* row = out.frame->data[0] + y * out.frame->linesize[0];
            for (int x = 0; x < size.width; ++x) {
                const int nx = x * 1280 / size.width, ny = y * 720 / size.height;
                const int checker = ((nx / 40 + ny / 40 + n / 5) & 1) * 32;
                row[x] = static_cast<unsigned char>(16 + (nx / 4 + ny / 3 + n * 3 + checker) % 220);
            }
        }
        for (int y = 0; y < size.height / 2; ++y) {
            auto* u = out.frame->data[1] + y * out.frame->linesize[1];
            auto* v = out.frame->data[2] + y * out.frame->linesize[2];
            for (int x = 0; x < size.width / 2; ++x) {
                u[x] = static_cast<unsigned char>(64 + (x * 256 / size.width + n * 2) % 128);
                v[x] = static_cast<unsigned char>(64 + (y * 256 / size.height + n * 3) % 128);
            }
        }
        out.frame->pts = n;
        check(avcodec_send_frame(codec, out.frame), "encode MPEG4 frame"); drain();
    }
    check(avcodec_send_frame(codec, nullptr), "flush encoder"); drain();
    check(av_write_trailer(out.mux), "finish AVI");
    check(avio_closep(&out.mux->pb), "close AVI");
    // Reopen the actual container; header fields, not filenames, define sizes.
    AVFormatContext* input = nullptr;
    check(avformat_open_input(&input, filename.c_str(), nullptr, nullptr), "reopen fixture");
    struct Close { AVFormatContext*& context; ~Close() { avformat_close_input(&context); } } close{input};
    check(avformat_find_stream_info(input, nullptr), "probe generated fixture");
    if (input->nb_streams != 1 || input->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        input->streams[0]->codecpar->codec_id != AV_CODEC_ID_MPEG4 ||
        input->streams[0]->codecpar->width != size.width || input->streams[0]->codecpar->height != size.height ||
        input->streams[0]->nb_frames != seconds * fps)
        throw std::runtime_error("generated fixture properties differ from requested video-only MPEG4");
    std::cout << "Generated " << filename << " " << size.width << 'x' << size.height
              << " " << fps << " fps " << seconds * fps << " frames, video only\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("usage: make-benchmark-media output-directory [--seconds 3..6] [--fps 1..60] [--only 720p|1080p|4k]");
        int seconds = 4, fps = 30; std::string only;
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string key = argv[i];
            if (key == "--seconds") seconds = std::stoi(argv[i+1]);
            else if (key == "--fps") fps = std::stoi(argv[i+1]);
            else if (key == "--only") only = argv[i+1];
            else throw std::runtime_error("unknown option: " + key);
        }
        if (seconds < 3 || seconds > 6 || fps < 1 || fps > 60) throw std::runtime_error("duration/fps outside supported range");
        if (!only.empty() && only != "720p" && only != "1080p" && only != "4k") throw std::runtime_error("unknown resolution");
        const auto dir = fs::absolute(fs::u8path(argv[1])); fs::create_directories(dir);
        for (auto size : resolutions) if (only.empty() || only == size.name)
            generate(dir / (std::string(size.name) + ".avi"), size, seconds, fps);
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
