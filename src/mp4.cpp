// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "../include/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef MESH_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace mesh {

bool mp4_export_supported() {
#ifdef MESH_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

#ifdef MESH_HAVE_FFMPEG

namespace {

std::string av_error_text(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(err, buf, sizeof(buf)) < 0) {
        return "FFmpeg error " + std::to_string(err);
    }
    return std::string(buf);
}

Result mp4_failure(const std::string& path, const std::string& stage, const std::string& detail) {
    return {false, "MP4 export failed (" + stage + ") for '" + path + "': " + detail, 0, 0};
}

Result mp4_failure(const std::string& path, const std::string& stage, int err) {
    return mp4_failure(path, stage, av_error_text(err));
}

// Libraries should not print encoder statistics to stderr. Keep FFmpeg error
// messages (we also report them through Result), suppress info/verbose chatter.
struct ScopedAvLogLevel {
    int previous;
    ScopedAvLogLevel() : previous(av_log_get_level()) {
        if (previous > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);
    }
    ~ScopedAvLogLevel() { av_log_set_level(previous); }
};

struct Mp4Session {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    AVStream* stream = nullptr;
    bool finished = false;
    bool file_opened = false;
    std::string path;

    ~Mp4Session() {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (sws) sws_freeContext(sws);
        if (codec) avcodec_free_context(&codec);
        if (fmt) {
            if (fmt->pb) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
        }
        if (!finished && file_opened && !path.empty()) {
            // Remove only a container this call actually created; failures
            // before avio_open (bad extension, alloc error) must never delete
            // a pre-existing file at the target path.
            std::remove(path.c_str());
        }
    }
};

const AVCodec* find_h264_encoder() {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder_by_name("libopenh264");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    return codec;
}

void composite_rgba_onto_background(const RasterImage& img, std::vector<uint8_t>& out,
                                    uint32_t out_w, uint32_t out_h,
                                    uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    out.assign(static_cast<size_t>(out_w) * out_h * 4, 0);
    for (uint32_t y = 0; y < out_h; ++y) {
        uint8_t* dst_row = &out[static_cast<size_t>(y) * out_w * 4];
        const bool inside_y = y < img.height;
        for (uint32_t x = 0; x < out_w; ++x) {
            uint8_t* dst = dst_row + static_cast<size_t>(x) * 4;
            if (inside_y && x < img.width) {
                const uint8_t* src = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
                const uint32_t a = src[3];
                const uint32_t ia = 255u - a;
                dst[0] = static_cast<uint8_t>((src[0] * a + bg_r * ia + 127u) / 255u);
                dst[1] = static_cast<uint8_t>((src[1] * a + bg_g * ia + 127u) / 255u);
                dst[2] = static_cast<uint8_t>((src[2] * a + bg_b * ia + 127u) / 255u);
            } else {
                // Odd canvas dimensions are padded with the scene background so
                // the 4:2:0 encoder always receives even width/height planes.
                dst[0] = bg_r;
                dst[1] = bg_g;
                dst[2] = bg_b;
            }
            dst[3] = 255;
        }
    }
}

Result write_encoded_packets(Mp4Session& s) {
    while (true) {
        int ret = avcodec_receive_packet(s.codec, s.packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return {true, "", 0, 0};
        if (ret < 0) return mp4_failure(s.path, "H.264 packet encoding", ret);

        // libx264 does not always attach a duration to the final packet; without
        // one the MP4 track ends one frame early (e.g. 23/24 s for 24 frames).
        if (s.packet->duration <= 0) s.packet->duration = 1;
        av_packet_rescale_ts(s.packet, s.codec->time_base, s.stream->time_base);
        s.packet->stream_index = s.stream->index;
        ret = av_interleaved_write_frame(s.fmt, s.packet);
        av_packet_unref(s.packet);
        if (ret < 0) return mp4_failure(s.path, "MP4 muxing", ret);
    }
}

} // namespace

Result render_animation_mp4(const std::string& source, const std::string& path,
                            double duration_sec, int fps, const RenderOptions& opts,
                            const std::string& base_dir) {
    if (path.empty()) {
        return {false, "MP4 export: output path is empty", 0, 0};
    }
    if (!std::isfinite(duration_sec) || duration_sec < 0.0) {
        return {false, "MP4 export: duration must be a non-negative number of seconds", 0, 0};
    }

    ScopedAvLogLevel quiet_ffmpeg;

    fps = std::clamp(fps, 1, 60);
    const double frame_count_d = std::round(duration_sec * fps);
    if (frame_count_d > 1000000.0) {
        return {false, "MP4 export: duration/fps requests too many frames (max 1,000,000)", 0, 0};
    }
    const int total_frames = std::max(1, static_cast<int>(frame_count_d));

    // Render the first frame up front: this validates the document and gives us
    // the real canvas size (including render overrides) before creating a file.
    Scene scene;
    Result pr = parse_and_evaluate(source, scene, 0.0, base_dir);
    if (!pr.ok) return pr;

    RasterImage first_frame;
    Result rr = render_scene_ex(scene, first_frame, opts);
    if (!rr.ok) return rr;
    if (first_frame.width == 0 || first_frame.height == 0) {
        return {false, "MP4 export: rendered frame has zero dimensions", 0, 0};
    }

    const uint32_t frame_w = first_frame.width;
    const uint32_t frame_h = first_frame.height;
    const uint32_t enc_w = std::max(2u, (frame_w + 1u) & ~1u);
    const uint32_t enc_h = std::max(2u, (frame_h + 1u) & ~1u);

    Mp4Session s;
    s.path = path;

    const AVCodec* codec = find_h264_encoder();
    if (!codec) {
        return {false, "MP4 export: this FFmpeg build has no H.264 encoder "
                       "(install libx264/x264 or rebuild FFmpeg with --enable-libx264)", 0, 0};
    }

    s.codec = avcodec_alloc_context3(codec);
    if (!s.codec) return mp4_failure(path, "allocating H.264 encoder context", "out of memory");

    int ret = avformat_alloc_output_context2(&s.fmt, nullptr, nullptr, path.c_str());
    if (ret < 0 || !s.fmt) {
        return mp4_failure(path, "container detection",
                           "cannot infer the output container from the file extension; use .mp4 or .mov");
    }

    s.codec->width = static_cast<int>(enc_w);
    s.codec->height = static_cast<int>(enc_h);
    s.codec->time_base = AVRational{1, fps};
    s.codec->framerate = AVRational{fps, 1};
    s.codec->pix_fmt = AV_PIX_FMT_YUV420P;
    s.codec->gop_size = std::max(1, fps * 2);
    // No B-frames: every frame's presentation timestamp is its decode timestamp,
    // which keeps the MP4 timeline exact and avoids edit lists / reorder delays.
    s.codec->max_b_frames = 0;
    s.codec->thread_count = 0;
    // MP4/MOV carry SPS/PPS in avcC, so ask the encoder for extradata.
    s.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    const bool is_x264 = std::strcmp(codec->name, "libx264") == 0;
    if (is_x264) {
        ret = av_opt_set(s.codec->priv_data, "crf", "18", 0);
        if (ret >= 0) ret = av_opt_set(s.codec->priv_data, "preset", "medium", 0);
        if (ret < 0) return mp4_failure(path, "configuring libx264 (crf 18, preset medium)", ret);
    } else {
        // Non-x264 H.264 encoders may not implement CRF; give them a sane bitrate.
        const int64_t pixels_per_sec = static_cast<int64_t>(enc_w) * enc_h * fps;
        s.codec->bit_rate = std::max<int64_t>(750000, pixels_per_sec / 8);
    }

    ret = avcodec_open2(s.codec, codec, nullptr);
    if (ret < 0) {
        return mp4_failure(path, std::string("opening H.264 encoder '") + codec->name + "'", ret);
    }
    if (s.codec->pix_fmt != AV_PIX_FMT_YUV420P) {
        return mp4_failure(path, "pixel format selection",
                           "the selected H.264 encoder did not accept yuv420p");
    }

    s.stream = avformat_new_stream(s.fmt, nullptr);
    if (!s.stream) return mp4_failure(path, "creating MP4 video stream", "out of memory");
    ret = avcodec_parameters_from_context(s.stream->codecpar, s.codec);
    if (ret < 0) return mp4_failure(path, "copying H.264 stream parameters", ret);
    s.stream->time_base = s.codec->time_base;

    ret = avio_open(&s.fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        return mp4_failure(path, "opening output file", ret);
    }
    s.file_opened = true;

    const std::string title = scene.name.empty() ? "Mesh animation" : scene.name;
    av_dict_set(&s.fmt->metadata, "title", title.c_str(), 0);
    av_dict_set(&s.fmt->metadata, "comment", "Generated by Mesh (meshc)", 0);

    AVDictionary* mux_opts = nullptr;
    av_dict_set(&mux_opts, "movflags", "+faststart", 0);
    ret = avformat_write_header(s.fmt, &mux_opts);
    av_dict_free(&mux_opts);
    if (ret < 0) return mp4_failure(path, "writing MP4 header", ret);

    s.sws = sws_getContext(static_cast<int>(enc_w), static_cast<int>(enc_h), AV_PIX_FMT_RGBA,
                           static_cast<int>(enc_w), static_cast<int>(enc_h), AV_PIX_FMT_YUV420P,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!s.sws) return mp4_failure(path, "creating RGBA -> yuv420p converter", "SWS initialization failed");

    s.frame = av_frame_alloc();
    s.packet = av_packet_alloc();
    if (!s.frame || !s.packet) return mp4_failure(path, "allocating encoder frame/packet", "out of memory");
    s.frame->format = AV_PIX_FMT_YUV420P;
    s.frame->width = static_cast<int>(enc_w);
    s.frame->height = static_cast<int>(enc_h);
    ret = av_frame_get_buffer(s.frame, 32);
    if (ret < 0) return mp4_failure(path, "allocating yuv420p frame buffer", ret);

    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    if (scene.has_background) {
        bg_r = static_cast<uint8_t>(std::round(clamp01(scene.background.r) * 255.0));
        bg_g = static_cast<uint8_t>(std::round(clamp01(scene.background.g) * 255.0));
        bg_b = static_cast<uint8_t>(std::round(clamp01(scene.background.b) * 255.0));
    }

    std::vector<uint8_t> padded;
    RasterImage frame_img;

    for (int f = 0; f < total_frames; ++f) {
        const double t = (total_frames > 1) ? (static_cast<double>(f) / fps) : 0.0;
        if (f == 0) {
            frame_img = std::move(first_frame);
        } else {
            Scene sf;
            Result pfr = parse_and_evaluate(source, sf, t, base_dir);
            if (!pfr.ok) return pfr;
            Result rfr = render_scene_ex(sf, frame_img, opts);
            if (!rfr.ok) return rfr;
        }
        if (frame_img.width != frame_w || frame_img.height != frame_h) {
            return {false, "MP4 export: frame dimensions changed during animation "
                           "(all frames must render at the same size)", 0, 0};
        }

        composite_rgba_onto_background(frame_img, padded, enc_w, enc_h, bg_r, bg_g, bg_b);

        ret = av_frame_make_writable(s.frame);
        if (ret < 0) return mp4_failure(path, "making encoder frame writable", ret);

        const uint8_t* src_data[4] = {padded.data(), nullptr, nullptr, nullptr};
        const int src_linesize[4] = {static_cast<int>(enc_w * 4), 0, 0, 0};
        const int scaled_rows = sws_scale(s.sws, src_data, src_linesize, 0,
                                          static_cast<int>(enc_h), s.frame->data, s.frame->linesize);
        if (scaled_rows <= 0) {
            return mp4_failure(path, "converting frame to yuv420p", "sws_scale produced no rows");
        }

        s.frame->pts = f;
        ret = avcodec_send_frame(s.codec, s.frame);
        if (ret < 0) return mp4_failure(path, "submitting frame to H.264 encoder", ret);

        Result wr = write_encoded_packets(s);
        if (!wr.ok) return wr;
    }

    ret = avcodec_send_frame(s.codec, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        return mp4_failure(path, "flushing H.264 encoder", ret);
    }
    Result flush_result = write_encoded_packets(s);
    if (!flush_result.ok) return flush_result;

    ret = av_write_trailer(s.fmt);
    if (ret < 0) return mp4_failure(path, "finalizing MP4 trailer", ret);

    s.finished = true;
    return {true, "", 0, 0};
}

#else // !MESH_HAVE_FFMPEG

Result render_animation_mp4(const std::string&, const std::string&, double, int,
                            const RenderOptions&, const std::string&) {
    return {false,
            "MP4 export is unavailable in this build: FFmpeg development libraries were not found. "
            "Install libavcodec/libavformat/libavutil/libswscale development packages "
            "(or rebuild with pkg-config pointing at them) and recompile Mesh.",
            0, 0};
}

#endif // MESH_HAVE_FFMPEG

} // namespace mesh
