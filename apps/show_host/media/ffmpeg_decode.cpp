#include "ffmpeg_decode.h"
#include "../audio/decoded_buffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string_view>

#include <windows.h>
#include <d3d11.h>
#include <mfobjects.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

std::string utf8(const std::wstring& text) {
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

bool lgpl_build() {
    // Refuse accidental use of a GPL/nonfree SDK, including transitive options.
    for (const auto* config : {avcodec_configuration(), avformat_configuration(),
            avutil_configuration(), swscale_configuration(), swresample_configuration()}) {
        const std::string_view value(config);
        if (value.find("--enable-gpl") != value.npos
                || value.find("--enable-nonfree") != value.npos) return false;
    }
    return true;
}

class Input {
public:
    ~Input() { close(); }
    void close() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        stream = nullptr;
        draining = false;
    }
    void deadline() {
        expires = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    static int interrupt(void* opaque) {
        return std::chrono::steady_clock::now() > static_cast<Input*>(opaque)->expires;
    }
    static AVPixelFormat hardware_format(AVCodecContext* context, const AVPixelFormat* formats) {
        for (auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == AV_PIX_FMT_D3D11) return *format;
        }
        return avcodec_default_get_format(context, formats);
    }
    bool open(const std::wstring& source, AVMediaType type, bool restricted,
            ID3D11Device* device = nullptr) {
        close();
        if (!lgpl_build()) return false;
        auto path = utf8(source);
        if (path.empty()) return false;
        format = avformat_alloc_context();
        if (!format) return false;
        format->interrupt_callback = {interrupt, this};
        deadline();
        if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return false;
        deadline();
        if (avformat_find_stream_info(format, nullptr) < 0) return false;
        index = av_find_best_stream(format, type, -1, -1, nullptr, 0);
        if (index < 0) return false;
        stream = format->streams[index];
        const auto id = stream->codecpar->codec_id;
        if (restricted && id != AV_CODEC_ID_AV1 && id != AV_CODEC_ID_HEVC) return false;
        const AVCodec* decoder = nullptr;
        if (id == AV_CODEC_ID_AV1) {
            if (device) {
                decoder = avcodec_find_decoder_by_name("av1");
            } else {
                decoder = avcodec_find_decoder_by_name("libdav1d");
                if (!decoder) decoder = avcodec_find_decoder_by_name("libaom-av1");
            }
        } else {
            decoder = avcodec_find_decoder(id);
        }
        // The native AV1 decoder requires hardware; we require a software one
        // so support is also available on machines without AV1 acceleration.
        if (!decoder) return false;
        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0) return false;
        codec->pkt_timebase = stream->time_base;
        codec->thread_count = 0;
        if (device) {
            bool supported = false;
            for (int config_index = 0; ; ++config_index) {
                const auto* config = avcodec_get_hw_config(decoder, config_index);
                if (!config) break;
                if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA
                        && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                    supported = true;
                    break;
                }
            }
            if (!supported) return false;
            auto* hardware = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!hardware) return false;
            auto* context = reinterpret_cast<AVHWDeviceContext*>(hardware->data);
            auto* d3d = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
            device->AddRef();
            d3d->device = device;
            if (av_hwdevice_ctx_init(hardware) < 0) {
                av_buffer_unref(&hardware);
                return false;
            }
            codec->hw_device_ctx = hardware;
            codec->get_format = hardware_format;
        }
        if (avcodec_open2(codec, decoder, nullptr) < 0) return false;
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        return frame && packet;
    }
    int read() {
        av_frame_unref(frame);
        for (;;) {
            const auto received = avcodec_receive_frame(codec, frame);
            if (received != AVERROR(EAGAIN)) return received;
            if (draining) return AVERROR_EOF;
            int result;
            do {
                av_packet_unref(packet);
                deadline();
                result = av_read_frame(format, packet);
            } while (result >= 0 && packet->stream_index != index);
            if (result == AVERROR_EOF) {
                result = avcodec_send_packet(codec, nullptr);
                if (result < 0 && result != AVERROR_EOF) return result;
                draining = true;
            } else if (result < 0) {
                return result;
            } else {
                // receive_frame returned EAGAIN, so send_packet must accept
                // input. Keep the packet referenced until it has been sent.
                result = avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
                if (result < 0) return result;
            }
        }
    }
    double start_seconds() const {
        return stream->start_time == AV_NOPTS_VALUE ? 0.0
            : stream->start_time * av_q2d(stream->time_base);
    }
    double duration() const {
        if (stream && stream->duration != AV_NOPTS_VALUE)
            return std::max(0.0, stream->duration * av_q2d(stream->time_base));
        return format && format->duration != AV_NOPTS_VALUE
            ? std::max(0.0, static_cast<double>(format->duration) / AV_TIME_BASE) : 0.0;
    }
    AVFormatContext* format{};
    AVCodecContext* codec{};
    AVStream* stream{};
    AVFrame* frame{};
    AVPacket* packet{};
    int index{-1};
    bool draining{};
    std::chrono::steady_clock::time_point expires;
};

std::uint32_t primaries(AVColorPrimaries value) {
    switch (value) {
        case AVCOL_PRI_BT709: return MFVideoPrimaries_BT709;
        case AVCOL_PRI_BT2020: return MFVideoPrimaries_BT2020;
        case AVCOL_PRI_BT470BG: return MFVideoPrimaries_BT470_2_SysBG;
        case AVCOL_PRI_SMPTE170M: return MFVideoPrimaries_SMPTE170M;
        default: return MFVideoPrimaries_Unknown;
    }
}
std::uint32_t transfer(AVColorTransferCharacteristic value) {
    switch (value) {
        case AVCOL_TRC_BT709: return MFVideoTransFunc_709;
        case AVCOL_TRC_SMPTE2084: return MFVideoTransFunc_2084;
        case AVCOL_TRC_ARIB_STD_B67: return MFVideoTransFunc_HLG;
        case AVCOL_TRC_SMPTE170M: return MFVideoTransFunc_709;
        default: return MFVideoTransFunc_Unknown;
    }
}

} // namespace

struct FfmpegVideoDecoder::Impl {
    Input input;
    SwsContext* scaler{};
    MfD3D11VideoPixelFormat output{};
    int width{}, height{}, pixel_format{-1};
    std::uint64_t changes{};
    double next_timestamp{};
    std::wstring source;
    bool hardware_requested{};
    ~Impl() { sws_freeContext(scaler); }
};

FfmpegVideoDecoder::FfmpegVideoDecoder() : impl_(std::make_unique<Impl>()) {}
FfmpegVideoDecoder::~FfmpegVideoDecoder() = default;
void FfmpegVideoDecoder::close() { impl_ = std::make_unique<Impl>(); }
bool FfmpegVideoDecoder::open(const std::wstring& source,
        MfD3D11VideoPixelFormat output, bool restricted, void* device) {
    close();
    impl_->output = output;
    impl_->source = source;
    impl_->hardware_requested = device != nullptr;
    if (impl_->input.open(source, AVMEDIA_TYPE_VIDEO, restricted,
            static_cast<ID3D11Device*>(device))) return true;
    impl_->hardware_requested = false;
    if (device && impl_->input.open(source, AVMEDIA_TYPE_VIDEO, restricted)) return true;
    close();
    return false;
}
bool FfmpegVideoDecoder::seek(double seconds) {
    auto& input = impl_->input;
    if (!input.codec || !std::isfinite(seconds)) return false;
    seconds = std::max(0.0, seconds);
    const auto position = (seconds + input.start_seconds()) / av_q2d(input.stream->time_base);
    if (position < static_cast<double>(std::numeric_limits<std::int64_t>::min())
            || position >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) return false;
    input.deadline();
    if (av_seek_frame(input.format, input.index, static_cast<std::int64_t>(position),
            AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(input.codec);
    av_packet_unref(input.packet);
    av_frame_unref(input.frame);
    input.draining = false;
    impl_->next_timestamp = seconds;
    return true;
}
double FfmpegVideoDecoder::duration() const noexcept { return impl_->input.duration(); }
std::uint64_t FfmpegVideoDecoder::format_change_count() const noexcept { return impl_->changes; }

FfmpegReadStatus FfmpegVideoDecoder::read(FfmpegVideoFrame& result) {
    result = {};
    auto& state = *impl_;
    auto& input = state.input;
    if (!input.codec) return FfmpegReadStatus::fatal_error;
    auto status = input.read();
    // Some drivers only reject the codec profile after the first packet. AV1's
    // native hardware decoder has no software implementation, so reopen with
    // dav1d and resume at the requested timestamp instead of failing playback.
    if (status < 0 && status != AVERROR_EOF && status != AVERROR(EAGAIN)
            && status != AVERROR_EXIT && state.hardware_requested) {
        state.hardware_requested = false;
        const auto resume = state.next_timestamp;
        if (input.open(state.source, AVMEDIA_TYPE_VIDEO, false) && seek(resume)) status = input.read();
    }
    if (status == AVERROR_EOF) return FfmpegReadStatus::end_of_stream;
    if (status < 0) return status == AVERROR(EAGAIN) || status == AVERROR_EXIT
        || status == AVERROR(EIO) ? FfmpegReadStatus::retryable_error : FfmpegReadStatus::fatal_error;
    const auto* frame = input.frame;
    if (frame->width <= 0 || frame->height <= 0) return FfmpegReadStatus::fatal_error;
    const bool nv12 = state.output == MfD3D11VideoPixelFormat::nv12;
    const bool hardware = frame->format == AV_PIX_FMT_D3D11;
    // D3D11 NV12 resources require even dimensions. Keep the visible aperture.
    const auto width = nv12 ? (frame->width + 1) & ~1 : frame->width;
    const auto height = nv12 ? (frame->height + 1) & ~1 : frame->height;
    const auto target = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_BGRA;
    const auto bytes = av_image_get_buffer_size(target, width, height, 1);
    if (bytes < 0) return FfmpegReadStatus::fatal_error;
    if (hardware) {
        auto* retained = av_frame_clone(input.frame);
        if (!retained) return FfmpegReadStatus::fatal_error;
        result.hardware_retention = std::shared_ptr<void>(retained, [](void* value) {
            auto* owned = static_cast<AVFrame*>(value);
            av_frame_free(&owned);
        });
        result.hardware_texture = frame->data[0];
        result.hardware_subresource = static_cast<unsigned int>(reinterpret_cast<std::intptr_t>(frame->data[1]));
    } else {
        result.pixels.resize(bytes);
    }
    std::uint8_t* planes[4]{};
    int strides[4]{};
    if (!hardware && av_image_fill_arrays(planes, strides, result.pixels.data(), target,
            width, height, 1) < 0) return FfmpegReadStatus::fatal_error;
    if (!hardware) {
        state.scaler = sws_getCachedContext(state.scaler, frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
            target, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!state.scaler) return FfmpegReadStatus::fatal_error;
    }
    auto colour = frame->colorspace;
    if (colour == AVCOL_SPC_UNSPECIFIED) colour = input.codec->colorspace;
    const auto matrix = colour == AVCOL_SPC_BT470BG || colour == AVCOL_SPC_SMPTE170M
        ? SWS_CS_ITU601 : colour == AVCOL_SPC_BT2020_NCL || colour == AVCOL_SPC_BT2020_CL
        ? SWS_CS_BT2020 : SWS_CS_ITU709;
    const bool full = frame->color_range == AVCOL_RANGE_JPEG;
    result.hardware_full_range = full;
    const auto* coefficients = sws_getCoefficients(matrix);
    if (!hardware && (sws_setColorspaceDetails(state.scaler, coefficients, full, coefficients,
            nv12 ? full : true, 0, 1 << 16, 1 << 16) < 0
            || sws_scale(state.scaler, frame->data, frame->linesize, 0,
                frame->height, planes, strides) != frame->height))
        return FfmpegReadStatus::fatal_error;
    if (state.width && (state.width != frame->width || state.height != frame->height
            || state.pixel_format != frame->format)) ++state.changes;
    state.width = frame->width;
    state.height = frame->height;
    state.pixel_format = frame->format;
    auto& format = result.format;
    format.pixel_format = state.output;
    format.width = width;
    format.height = height;
    format.clean_aperture_width = frame->width;
    format.clean_aperture_height = frame->height;
    const auto aspect = av_guess_sample_aspect_ratio(input.format, input.stream, input.frame);
    if (aspect.num > 0 && aspect.den > 0) {
        format.pixel_aspect_ratio_num = aspect.num;
        format.pixel_aspect_ratio_den = aspect.den;
    }
    const auto rate = av_guess_frame_rate(input.format, input.stream, input.frame);
    if (rate.num > 0 && rate.den > 0) {
        format.frame_rate_num = rate.num;
        format.frame_rate_den = rate.den;
    }
    format.interlace_mode = frame->flags & AV_FRAME_FLAG_INTERLACED
        ? (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST
            ? MFVideoInterlace_FieldInterleavedUpperFirst : MFVideoInterlace_FieldInterleavedLowerFirst)
        : MFVideoInterlace_Progressive;
    format.video_primaries = primaries(frame->color_primaries);
    format.transfer_function = transfer(frame->color_trc);
    format.yuv_matrix = matrix == SWS_CS_ITU601 ? MFVideoTransferMatrix_BT601
        : matrix == SWS_CS_BT2020 ? MFVideoTransferMatrix_BT2020_10 : MFVideoTransferMatrix_BT709;
    format.nominal_range = !nv12 || full ? MFNominalRange_0_255 : MFNominalRange_16_235;
    const auto* display = av_frame_get_side_data(input.frame, AV_FRAME_DATA_DISPLAYMATRIX);
    const auto* stream_display = av_packet_side_data_get(input.stream->codecpar->coded_side_data,
        input.stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    const auto* matrix_data = display && display->size >= 9 * sizeof(std::int32_t)
        ? display->data : stream_display && stream_display->size >= 9 * sizeof(std::int32_t)
        ? stream_display->data : nullptr;
    if (matrix_data) {
        const auto rotation = -av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix_data));
        if (std::isfinite(rotation)) {
            const auto degrees = (static_cast<int>(std::lround(rotation)) % 360 + 360) % 360;
            if (degrees % 90 == 0) format.rotation_degrees = degrees;
        }
    }
    result.row_pitch = strides[0];
    result.timestamp_seconds = frame->best_effort_timestamp != AV_NOPTS_VALUE
        ? frame->best_effort_timestamp * av_q2d(input.stream->time_base) - input.start_seconds()
        : state.next_timestamp;
    result.duration_seconds = frame->duration > 0
        ? frame->duration * av_q2d(input.stream->time_base)
        : rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 1.0 / 30.0;
    state.next_timestamp = result.timestamp_seconds + result.duration_seconds;
    return FfmpegReadStatus::frame;
}

bool ffmpeg_decode_audio(const std::wstring& source, DecodedAudioBuffer& output) {
    output = {};
    Input input;
    if (!input.open(source, AVMEDIA_TYPE_AUDIO, false)) return false;
    DecodedAudioBuffer decoded;
    decoded.sample_rate = input.codec->sample_rate;
    decoded.channels = 2;
    if (decoded.sample_rate <= 0) return false;
    SwrContext* resampler{};
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    const auto configured = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT,
        decoded.sample_rate, &input.codec->ch_layout, input.codec->sample_fmt,
        input.codec->sample_rate, 0, nullptr);
    if (configured < 0 || swr_init(resampler) < 0) {
        swr_free(&resampler);
        return false;
    }
    const auto convert = [&](const std::uint8_t** data, int samples) {
        const int capacity = swr_get_out_samples(resampler, samples);
        if (capacity < 0) return -1;
        std::vector<float> block(static_cast<std::size_t>(capacity) * 2);
        auto* bytes = reinterpret_cast<std::uint8_t*>(block.data());
        const auto count = swr_convert(resampler, &bytes, capacity, data, samples);
        if (count > 0) decoded.samples.insert(decoded.samples.end(),
            block.begin(), block.begin() + static_cast<std::size_t>(count) * 2);
        return count;
    };
    bool succeeded = true;
    for (;;) {
        const auto result = input.read();
        if (result == AVERROR_EOF) break;
        if (result < 0 || input.frame->sample_rate != decoded.sample_rate
                || input.frame->format != input.codec->sample_fmt
                || av_channel_layout_compare(&input.frame->ch_layout, &input.codec->ch_layout) != 0
                || convert(const_cast<const std::uint8_t**>(input.frame->extended_data),
                    input.frame->nb_samples) < 0) {
            succeeded = false;
            break;
        }
    }
    if (succeeded) {
        int count;
        do { count = convert(nullptr, 0); } while (count > 0);
        succeeded = count == 0;
    }
    swr_free(&resampler);
    if (!succeeded || decoded.samples.empty()) return false;
    output = std::move(decoded);
    return true;
}

bool ffmpeg_probe_video(const std::wstring& source, FfmpegMediaInfo& info) {
    info = {};
    Input input;
    if (!input.open(source, AVMEDIA_TYPE_VIDEO, false)) return false;
    info.duration_seconds = input.duration();
    info.has_audio = av_find_best_stream(input.format, AVMEDIA_TYPE_AUDIO,
        -1, -1, nullptr, 0) >= 0;
    return true;
}
