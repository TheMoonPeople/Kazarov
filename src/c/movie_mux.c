#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t position;
    int writable;
} MemoryFile;

static int memory_read(void *opaque, uint8_t *buffer, int size) {
    MemoryFile *file = opaque;
    if (file->position >= file->size)
        return AVERROR_EOF;
    size_t count = file->size - file->position;
    if (count > (size_t)size)
        count = (size_t)size;
    memcpy(buffer, file->data + file->position, count);
    file->position += count;
    return (int)count;
}

static int memory_write(void *opaque, const uint8_t *buffer, int size) {
    MemoryFile *file = opaque;
    if (!file->writable || size < 0 || (size_t)size > SIZE_MAX - file->position)
        return AVERROR(EINVAL);
    size_t end = file->position + (size_t)size;
    if (end > file->capacity) {
        size_t capacity = file->capacity ? file->capacity : 65536;
        while (capacity < end) {
            if (capacity > SIZE_MAX / 2)
                return AVERROR(ENOMEM);
            capacity *= 2;
        }
        uint8_t *resized = realloc(file->data, capacity);
        if (!resized)
            return AVERROR(ENOMEM);
        file->data = resized;
        file->capacity = capacity;
    }
    if (file->position > file->size)
        memset(file->data + file->size, 0, file->position - file->size);
    memcpy(file->data + file->position, buffer, (size_t)size);
    file->position = end;
    if (end > file->size)
        file->size = end;
    return size;
}

static int64_t memory_seek(void *opaque, int64_t offset, int whence) {
    MemoryFile *file = opaque;
    if (whence == AVSEEK_SIZE)
        return (int64_t)file->size;
    int basis = whence & ~AVSEEK_FORCE;
    int64_t base = basis == SEEK_SET ? 0 : basis == SEEK_CUR ? (int64_t)file->position
                                       : basis == SEEK_END   ? (int64_t)file->size
                                                             : -1;
    if (base < 0 || offset < -base || offset > INT64_MAX - base)
        return AVERROR(EINVAL);
    int64_t position = base + offset;
    if (!file->writable && (uint64_t)position > file->size)
        return AVERROR(EINVAL);
    file->position = (size_t)position;
    return position;
}

static AVIOContext *memory_io(MemoryFile *file) {
    uint8_t *buffer = av_malloc(32768);
    if (!buffer)
        return NULL;
    AVIOContext *io = avio_alloc_context(buffer, 32768, file->writable, file,
                                         memory_read, file->writable ? memory_write : NULL, memory_seek);
    if (!io)
        av_free(buffer);
    return io;
}

static uint16_t little16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t little32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int drain_audio(AVCodecContext *encoder, AVFormatContext *output, AVStream *stream, AVPacket *packet) {
    int result;
    while ((result = avcodec_receive_packet(encoder, packet)) >= 0) {
        av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
        packet->stream_index = stream->index;
        result = av_write_frame(output, packet);
        av_packet_unref(packet);
        if (result < 0)
            return result;
    }
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}

static int encode_wav(const uint8_t *wav, size_t wav_size, AVFormatContext *output,
                      AVCodecContext *encoder, AVStream *stream) {
    if (wav_size < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVEfmt ", 8) ||
        little16(wav + 20) != 1 || little16(wav + 34) != 16 || memcmp(wav + 36, "data", 4))
        return AVERROR_INVALIDDATA;
    int channels = little16(wav + 22);
    if (channels < 1 || channels > 2 || channels != encoder->ch_layout.nb_channels)
        return AVERROR_INVALIDDATA;
    size_t bytes = little32(wav + 40);
    if (bytes > wav_size - 44)
        return AVERROR_INVALIDDATA;
    size_t samples = bytes / (2 * (size_t)channels);
    const uint8_t *pcm = wav + 44;
    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return AVERROR(ENOMEM);
    int result = 0;
    size_t frame_size = encoder->frame_size > 0 ? (size_t)encoder->frame_size : 4096;
    for (size_t start = 0; start < samples; start += frame_size) {
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            result = AVERROR(ENOMEM);
            break;
        }
        frame->format = encoder->sample_fmt;
        frame->sample_rate = encoder->sample_rate;
        frame->nb_samples = (int)((samples - start) < frame_size ? samples - start : frame_size);
        frame->pts = (int64_t)start;
        result = av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
        if (result >= 0)
            result = av_frame_get_buffer(frame, 0);
        if (result >= 0) {
            for (int sample = 0; sample < frame->nb_samples; ++sample) {
                for (int channel = 0; channel < channels; ++channel) {
                    const uint8_t *value = pcm + (start + (size_t)sample) * (size_t)channels * 2 + (size_t)channel * 2;
                    if (encoder->sample_fmt == AV_SAMPLE_FMT_S16P) {
                        memcpy(frame->data[channel] + sample * 2, value, 2);
                    } else if (encoder->sample_fmt == AV_SAMPLE_FMT_S32P) {
                        int32_t expanded = (int32_t)(int16_t)little16(value) * 65536;
                        memcpy(frame->data[channel] + sample * 4, &expanded, 4);
                    } else {
                        result = AVERROR(ENOSYS);
                        break;
                    }
                }
                if (result < 0)
                    break;
            }
        }
        if (result >= 0)
            result = avcodec_send_frame(encoder, frame);
        av_frame_free(&frame);
        if (result >= 0)
            result = drain_audio(encoder, output, stream, packet);
        if (result < 0)
            break;
    }
    if (result >= 0)
        result = avcodec_send_frame(encoder, NULL);
    if (result >= 0)
        result = drain_audio(encoder, output, stream, packet);
    av_packet_free(&packet);
    return result;
}

/* Returns malloc-owned MP4 bytes; no source, intermediate, or final movie touches disk. */
int kazarov_mux_movie(const uint8_t *video, size_t video_size, const uint8_t *wav, size_t wav_size,
                      uint8_t **result_bytes, size_t *result_size, char *error, size_t error_size) {
    MemoryFile input_file = {(uint8_t *)video, video_size, video_size, 0, 0};
    MemoryFile output_file = {0};
    output_file.writable = 1;
    AVIOContext *input_io = NULL, *output_io = NULL;
    AVFormatContext *input = NULL, *output = NULL;
    AVCodecContext *encoder = NULL;
    AVPacket *packet = NULL;
    int result = AVERROR_UNKNOWN;
    *result_bytes = NULL;
    *result_size = 0;
    input_io = memory_io(&input_file);
    output_io = memory_io(&output_file);
    if (!input_io || !output_io) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    input = avformat_alloc_context();
    if (!input) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    input->pb = input_io;
    input->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_GENPTS;
    result = avformat_open_input(&input, NULL, NULL, NULL);
    if (result < 0)
        goto done;
    result = avformat_find_stream_info(input, NULL);
    if (result < 0)
        goto done;
    int video_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_index < 0) {
        result = video_index;
        goto done;
    }
    result = avformat_alloc_output_context2(&output, NULL, "mp4", NULL);
    if (result < 0 || !output)
        goto done;
    output->pb = output_io;
    output->flags |= AVFMT_FLAG_CUSTOM_IO;
    AVStream *video_stream = avformat_new_stream(output, NULL);
    if (!video_stream) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    result = avcodec_parameters_copy(video_stream->codecpar, input->streams[video_index]->codecpar);
    if (result < 0)
        goto done;
    video_stream->codecpar->codec_tag = 0;
    video_stream->time_base = input->streams[video_index]->time_base;
    AVStream *audio_stream = NULL;
    if (wav && wav_size) {
        if (wav_size < 44 || little32(wav + 24) == 0 || little16(wav + 22) == 0) {
            result = AVERROR_INVALIDDATA;
            goto done;
        }
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_ALAC);
        if (!codec) {
            result = AVERROR_ENCODER_NOT_FOUND;
            goto done;
        }
        encoder = avcodec_alloc_context3(codec);
        if (!encoder) {
            result = AVERROR(ENOMEM);
            goto done;
        }
        encoder->sample_fmt = AV_SAMPLE_FMT_S16P;
        encoder->sample_rate = (int)little32(wav + 24);
        encoder->time_base = (AVRational){1, encoder->sample_rate};
        av_channel_layout_default(&encoder->ch_layout, little16(wav + 22));
        encoder->thread_count = 1;
        if (output->oformat->flags & AVFMT_GLOBALHEADER)
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        result = avcodec_open2(encoder, codec, NULL);
        if (result < 0)
            goto done;
        audio_stream = avformat_new_stream(output, NULL);
        if (!audio_stream) {
            result = AVERROR(ENOMEM);
            goto done;
        }
        audio_stream->time_base = encoder->time_base;
        result = avcodec_parameters_from_context(audio_stream->codecpar, encoder);
        if (result < 0)
            goto done;
    }
    result = avformat_write_header(output, NULL);
    if (result < 0)
        goto done;
    packet = av_packet_alloc();
    if (!packet) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    while ((result = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == video_index) {
            av_packet_rescale_ts(packet, input->streams[video_index]->time_base, video_stream->time_base);
            packet->stream_index = video_stream->index;
            result = av_write_frame(output, packet);
        }
        av_packet_unref(packet);
        if (result < 0)
            goto done;
    }
    if (result != AVERROR_EOF)
        goto done;
    result = 0;
    if (encoder)
        result = encode_wav(wav, wav_size, output, encoder, audio_stream);
    if (result < 0)
        goto done;
    result = av_write_trailer(output);
    if (result < 0)
        goto done;
    *result_bytes = output_file.data;
    *result_size = output_file.size;
    output_file.data = NULL;
done:
    if (result < 0 && error && error_size)
        av_strerror(result, error, error_size);
    av_packet_free(&packet);
    avcodec_free_context(&encoder);
    avformat_close_input(&input);
    avformat_free_context(output);
    avio_context_free(&input_io);
    avio_context_free(&output_io);
    free(output_file.data);
    return result;
}

void kazarov_free_movie(uint8_t *buffer) { free(buffer); }
