#include "video.hpp"
#ifdef SWITCH_HERO_VIDEO
#include "background.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#endif

namespace fret {
#ifdef SWITCH_HERO_VIDEO
namespace {
// Frames decoded ahead of the playhead. Enough to ride out one slow frame;
// more only costs memory (a 1080p frame is 3 MB).
constexpr size_t QueueDepth = 3;
// A frame this far behind the playhead is dropped instead of shown.
constexpr double LateBy = .1;
// After this many late frames in a row the decoder stops running the VP9/VP8
// loop filter: a softer picture, but a good deal less work per frame.
constexpr int LateFramesBeforeCheap = 8;

struct Frame {
    AVFrame *frame = nullptr;
    double pts = 0;
};
} // namespace

struct BackgroundVideo::Impl {
    fs::path file;
    std::thread worker;

    // Shared with the decoder thread, under `mutex`.
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Frame> queue;
    bool stop = false, failed = false, opened = false, ended = false;
    bool seekWanted = false;
    double seekTo = 0;
    double target = 0;  // the playhead, as of the last frame() call
    double cursor = 0;  // newest frame decoded, or where the last seek landed
    double seekBase = 0; // where decoding last started from
    double duration = 0;

    // Main thread only.
    SDL_Texture *texture = nullptr;
    int textureW = 0, textureH = 0;
    double shown = 0;
    bool haveShown = false;

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
        for (auto &f : queue)
            av_frame_free(&f.frame);
        if (texture)
            SDL_DestroyTexture(texture);
    }

    void fail(const std::string &why) {
        std::cerr << "Switch Hero: background video " << file.string() << ": " << why << '\n';
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
    }

    void run() {
        // Its own spare core at loader priority: FFmpeg's own threads would
        // start on the render core at the render thread's priority, and
        // Horizon does not time-slice equal priorities, so decoding there
        // would stall the frame. One thread, placed by us, instead.
        runInBackground(Work::Loader);
        AVFormatContext *format = nullptr;
        AVCodecContext *codec = nullptr;
        AVPacket *packet = av_packet_alloc();
        AVFrame *decoded = av_frame_alloc();
        SwsContext *scaler = nullptr;
        decodeAll(format, codec, packet, decoded, scaler);
        sws_freeContext(scaler);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    void decodeAll(AVFormatContext *&format, AVCodecContext *&codec, AVPacket *packet, AVFrame *decoded,
                   SwsContext *&scaler) {
        if (!packet || !decoded)
            return fail("out of memory");
        // "file:" stops FFmpeg reading the "sdmc:" in a console path as a protocol.
        const std::string url = "file:" + file.string();
        if (avformat_open_input(&format, url.c_str(), nullptr, nullptr) < 0)
            return fail("cannot open");
        if (avformat_find_stream_info(format, nullptr) < 0)
            return fail("cannot read stream info");
        const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0)
            return fail("no video stream");
        AVStream *stream = format->streams[index];
        for (unsigned i = 0; i < format->nb_streams; ++i)
            if (int(i) != index)
                format->streams[i]->discard = AVDISCARD_ALL; // the song plays its own audio
        // FFmpeg's built-in AV1 decoder only drives hardware; dav1d decodes in software.
        const AVCodec *decoder =
            stream->codecpar->codec_id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libdav1d") : nullptr;
        if (!decoder)
            decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder)
            return fail("no decoder for this format");
        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0)
            return fail("cannot set up the decoder");
        codec->thread_count = 1; // see run()
        codec->thread_type = 0;
        if (avcodec_open2(codec, decoder, nullptr) < 0)
            return fail("cannot open the decoder");

        const double timeBase = av_q2d(stream->time_base);
        const int64_t start = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        {
            std::lock_guard<std::mutex> lock(mutex);
            duration = format->duration > 0  ? double(format->duration) / AV_TIME_BASE
                       : stream->duration > 0 ? double(stream->duration) * timeBase
                                              : 0;
            opened = true;
        }

        double skipUntil = -1e9; // after a seek: frames before the target are not wanted
        int late = 0;
        bool cheap = false, draining = false;
        for (;;) {
            bool seek = false;
            double seekPosition = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stop || seekWanted || (!ended && queue.size() < QueueDepth); });
                if (stop)
                    return;
                if (seekWanted) {
                    seek = true, seekPosition = seekTo;
                    seekWanted = false, ended = false;
                    for (auto &f : queue)
                        av_frame_free(&f.frame);
                    queue.clear();
                }
            }
            if (seek) {
                const int64_t ts = start + int64_t(std::max(0.0, seekPosition) / timeBase);
                av_seek_frame(format, index, ts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(codec);
                skipUntil = seekPosition, late = 0, draining = false;
            }
            if (!draining) {
                if (av_read_frame(format, packet) < 0) {
                    draining = true; // end of file (or unreadable): flush what is left
                    avcodec_send_packet(codec, nullptr);
                } else {
                    if (packet->stream_index == index)
                        avcodec_send_packet(codec, packet);
                    av_packet_unref(packet);
                }
            }
            int r;
            while ((r = avcodec_receive_frame(codec, decoded)) >= 0) {
                int64_t ts = decoded->best_effort_timestamp;
                if (ts == AV_NOPTS_VALUE)
                    ts = decoded->pts;
                const double pts = ts == AV_NOPTS_VALUE ? 0 : double(ts - start) * timeBase;
                double playhead;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    playhead = target;
                }
                if (pts < skipUntil - LateBy) {
                    // Decoding forward from the keyframe before a seek target.
                } else if (pts < playhead - LateBy) {
                    if (++late >= LateFramesBeforeCheap && !cheap) {
                        codec->skip_loop_filter = AVDISCARD_ALL;
                        cheap = true;
                    }
                } else {
                    late = 0;
                    if (AVFrame *out = toYuv420(decoded, scaler)) {
                        std::lock_guard<std::mutex> lock(mutex);
                        queue.push_back({out, pts});
                        cursor = pts;
                    }
                }
                av_frame_unref(decoded);
            }
            if (r == AVERROR_EOF) {
                std::lock_guard<std::mutex> lock(mutex);
                ended = true;
            }
        }
    }

    // SDL takes planar 4:2:0 directly; anything else is converted once here,
    // on the decoder thread, rather than on the main one.
    static AVFrame *toYuv420(AVFrame *in, SwsContext *&scaler) {
        if (in->format == AV_PIX_FMT_YUV420P || in->format == AV_PIX_FMT_YUVJ420P)
            return av_frame_clone(in);
        scaler = sws_getCachedContext(scaler, in->width, in->height, AVPixelFormat(in->format), in->width,
                                      in->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        AVFrame *out = av_frame_alloc();
        if (!scaler || !out)
            return av_frame_free(&out), nullptr;
        out->format = AV_PIX_FMT_YUV420P;
        out->width = in->width, out->height = in->height;
        if (av_frame_get_buffer(out, 0) < 0)
            return av_frame_free(&out), nullptr;
        sws_scale(scaler, in->data, in->linesize, 0, in->height, out->data, out->linesize);
        return out;
    }
};

BackgroundVideo::BackgroundVideo() = default;
BackgroundVideo::~BackgroundVideo() = default;
bool BackgroundVideo::supported() { return true; }

void BackgroundVideo::open(const fs::path &file) {
    if (impl && impl->file == file && active())
        return;
    close();
    static const bool quiet = (av_log_set_level(AV_LOG_ERROR), true);
    (void)quiet;
    impl = std::make_unique<Impl>();
    impl->file = file;
    impl->worker = std::thread([p = impl.get()] { p->run(); });
}

void BackgroundVideo::close() { impl.reset(); }

bool BackgroundVideo::active() const {
    if (!impl)
        return false;
    std::lock_guard<std::mutex> lock(impl->mutex);
    return !impl->failed;
}

SDL_Texture *BackgroundVideo::frame(SDL_Renderer *renderer, double seconds) {
    if (!impl)
        return nullptr;
    Impl &v = *impl;
    Frame take;
    double duration;
    {
        std::lock_guard<std::mutex> lock(v.mutex);
        if (v.failed)
            return nullptr;
        const double t = std::max(0.0, seconds);
        v.target = t;
        if (!v.opened)
            return nullptr;
        duration = v.duration;
        const double from = v.haveShown ? v.shown : v.seekBase;
        if (!v.seekWanted && videoNeedsSeek(from, std::max(from, v.cursor), t)) {
            v.seekWanted = true;
            v.seekTo = v.seekBase = v.cursor = t;
            v.haveShown = false; // the old picture stays up until the new one lands
            v.wake.notify_all();
        }
        if (!v.seekWanted && !v.queue.empty()) {
            std::vector<double> pts;
            for (const auto &f : v.queue)
                pts.push_back(f.pts);
            const size_t due = framesToDrop(pts, t);
            if (due < v.queue.size()) {
                for (size_t i = 0; i < due; ++i) {
                    av_frame_free(&v.queue.front().frame);
                    v.queue.pop_front();
                }
                take = v.queue.front();
                v.queue.pop_front();
                v.wake.notify_all();
            }
        }
    }
    if (take.frame) {
        AVFrame *f = take.frame;
        if (!v.texture || v.textureW != f->width || v.textureH != f->height) {
            if (v.texture)
                SDL_DestroyTexture(v.texture);
            v.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width,
                                          f->height);
            v.textureW = v.texture ? f->width : 0, v.textureH = v.texture ? f->height : 0;
        }
        if (v.texture)
            SDL_UpdateYUVTexture(v.texture, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1],
                                 f->data[2], f->linesize[2]);
        v.shown = take.pts, v.haveShown = true;
        av_frame_free(&take.frame);
    }
    // Nothing before the video starts or after it ends: the room shows instead.
    if (seconds < 0 || (duration > 0 && seconds > duration + .1))
        return nullptr;
    return v.texture;
}

int BackgroundVideo::width() const { return impl ? impl->textureW : 0; }
int BackgroundVideo::height() const { return impl ? impl->textureH : 0; }

#else // Built without FFmpeg: the option is shown as unavailable.

struct BackgroundVideo::Impl {};
BackgroundVideo::BackgroundVideo() = default;
BackgroundVideo::~BackgroundVideo() = default;
bool BackgroundVideo::supported() { return false; }
void BackgroundVideo::open(const fs::path &) {}
void BackgroundVideo::close() {}
bool BackgroundVideo::active() const { return false; }
SDL_Texture *BackgroundVideo::frame(SDL_Renderer *, double) { return nullptr; }
int BackgroundVideo::width() const { return 0; }
int BackgroundVideo::height() const { return 0; }
#endif
} // namespace fret
