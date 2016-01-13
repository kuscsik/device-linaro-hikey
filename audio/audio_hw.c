/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_default"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <sound/asound.h>
#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <hardware/audio_alsaops.h>
#include <audio_effects/effect_aec.h>


#define CARD_OUT 0
#define PORT_CODEC 0
/* Minimum granularity - Arbitrary but small value */
#define CODEC_BASE_FRAME_COUNT 32

/* number of base blocks in a short period (low latency) */
#define PERIOD_MULTIPLIER 32  /* 11 ms */
/* number of frames per short period (low latency) */
#define PERIOD_SIZE (CODEC_BASE_FRAME_COUNT * PERIOD_MULTIPLIER)
/* number of pseudo periods for low latency playback */
#define PLAYBACK_PERIOD_COUNT 4
#define CODEC_SAMPLING_RATE 48000
#define MIN_WRITE_SLEEP_US      5000

struct stub_stream_in {
    struct audio_stream_in stream;
};

struct alsa_audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock;   /* see note below on mutex acquisition order */
    int mode;
    int devices;
    int in_call;
    struct alsa_stream_in *active_input;
    struct alsa_stream_out *active_output;
    bool mic_mute;
    struct echo_reference_itfe *echo_reference;
};

struct alsa_stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;   /* see note below on mutex acquisition order */
    struct pcm_config config;
    struct pcm *pcm;
    bool unavailable;
    int standby;
    struct echo_reference_itfe *echo_reference;
    struct alsa_audio_device *dev;
    int write_threshold;
    audio_format_t format;
    uint32_t channels;
    uint32_t sample_rate;

};


/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct alsa_stream_out *out)
{
    struct alsa_audio_device *adev = out->dev;
    unsigned int card = CARD_OUT;
    unsigned int port = PORT_CODEC;

    if (out->unavailable)
        return -ENODEV;

    adev->active_output = out;

    /* default to low power: will be corrected in out_write if necessary before first write to
     * tinyalsa.
     */
    out->write_threshold = PLAYBACK_PERIOD_COUNT * PERIOD_SIZE;
    out->config.start_threshold = PERIOD_SIZE * 2;
    out->config.avail_min = PERIOD_SIZE;

    out->pcm = pcm_open(card, port, PCM_OUT | PCM_MMAP | PCM_NOIRQ, &out->config);

    if (!pcm_is_ready(out->pcm)) {
        ALOGE("cannot open pcm_out driver: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }

    if (adev->echo_reference != NULL)
        out->echo_reference = adev->echo_reference;

    return 0;
}

static int get_playback_delay(struct alsa_stream_out *out,
        size_t frames,
        struct echo_reference_buffer *buffer)
{
    unsigned int kernel_frames;
    int status;

    status = pcm_get_htimestamp(out->pcm, &kernel_frames, &buffer->time_stamp);
    if (status < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        ALOGV("get_playback_delay(): pcm_get_htimestamp error,"
                "setting playbackTimestamp to 0");
        return status;
    }

    kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

    /* adjust render time stamp with delay added by current driver buffer.
     * Add the duration of current frame as we want the render time of the last
     * sample being written. */
    buffer->delay_ns = (long)(((int64_t)(kernel_frames + frames)* 1000000000)/
            CODEC_SAMPLING_RATE);

    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return out->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGV("out_set_sample_rate: %d", 0);
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    ALOGV("out_get_buffer_size: %d", 4096);
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;

    /* return the closest majoring multiple of 16 frames, as
     * audioflinger expects audio buffers to be a multiple of 16 frames */
    size_t size = PERIOD_SIZE;
    size = ((size + 15) / 16) * 16;
    return size * audio_stream_out_frame_size((struct audio_stream_out *)stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    ALOGV("out_get_channels");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return audio_channel_out_mask_from_count(out->config.channels);
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    ALOGV("out_get_format");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return audio_format_from_pcm_format(out->config.format);
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    ALOGV("out_set_format: %d",format);
    return -ENOSYS;
}

static int do_output_standby(struct alsa_stream_out *out)
{
    struct alsa_audio_device *adev = out->dev;

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;

        adev->active_output = 0;

        /* stop writing to echo reference */
        if (out->echo_reference != NULL) {
            out->echo_reference->write(out->echo_reference, NULL);
            out->echo_reference = NULL;
        }

        out->standby = 1;
    }
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    ALOGV("out_standby");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    int status;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    status = do_output_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return status;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    ALOGV("out_dump");
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("out_set_parameters");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    struct alsa_audio_device *adev = out->dev;
    struct alsa_stream_in *in;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        if (((adev->devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
            adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
            adev->devices |= val;
        }
        pthread_mutex_unlock(&out->lock);
        pthread_mutex_unlock(&adev->lock);
    }

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    ALOGV("out_get_parameters");
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    ALOGV("out_get_latency");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return (PERIOD_SIZE * PLAYBACK_PERIOD_COUNT * 1000) / out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
        float right)
{
    ALOGV("out_set_volume: Left:%f Right:%f", left, right);
    return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
        size_t bytes)
{
#if 0
    ALOGV("out_write: bytes: %d", bytes);
    /* XXX: fake timing for audio output */
    usleep((int64_t)bytes * 1000000 / audio_stream_out_frame_size(stream) /
            out_get_sample_rate(&stream->common));
    return bytes;
#endif
    int ret;
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    struct alsa_audio_device *adev = out->dev;
    size_t frame_size = audio_stream_out_frame_size(stream);
    size_t out_frames = bytes / frame_size;
    struct alsa_stream_in *in;
    int kernel_frames;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = 0;
    }

    pthread_mutex_unlock(&adev->lock);

    if (out->echo_reference != NULL) {
        struct echo_reference_buffer b;
        b.raw = (void *)buffer;
        b.frame_count = out_frames;

        get_playback_delay(out, out_frames, &b);
        out->echo_reference->write(out->echo_reference, &b);
    }

    /* do not allow more than out->write_threshold frames in kernel pcm driver buffer */
    do {
        struct timespec time_stamp;

        if (pcm_get_htimestamp(out->pcm, (unsigned int *)&kernel_frames, &time_stamp) < 0)
            break;
        kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

        if (kernel_frames > out->write_threshold) {
            unsigned long time = (unsigned long)
                (((int64_t)(kernel_frames - out->write_threshold) * 1000000) /
                 CODEC_SAMPLING_RATE);
            if (time < MIN_WRITE_SLEEP_US)
                time = MIN_WRITE_SLEEP_US;
            usleep(time);
        }
    } while (kernel_frames > out->write_threshold);

    ret = pcm_mmap_write(out->pcm, buffer, out_frames * frame_size);

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_out_frame_size(stream) /
                out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
        uint32_t *dsp_frames)
{
    *dsp_frames = 0;
    ALOGV("out_get_render_position: dsp_frames: %p", dsp_frames);
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_add_audio_effect: %p", effect);
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_remove_audio_effect: %p", effect);
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
        int64_t *timestamp)
{
    *timestamp = 0;
    ALOGV("out_get_next_write_timestamp: %ld", (long int)(*timestamp));
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    ALOGV("in_get_sample_rate");
    return 8000;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGV("in_set_sample_rate: %d", rate);
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    ALOGV("in_get_buffer_size: %d", 320);
    return 320;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    ALOGV("in_get_channels: %d", AUDIO_CHANNEL_IN_MONO);
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    return 0;
}

static char * in_get_parameters(const struct audio_stream *stream,
        const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
        size_t bytes)
{
    ALOGV("in_read: bytes %d", bytes);
    /* XXX: fake timing for audio input */
    usleep((int64_t)bytes * 1000000 / audio_stream_in_frame_size(stream) /
            in_get_sample_rate(&stream->common));
    memset(buffer, 0, bytes);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

inline unsigned choose_param(unsigned preferred, unsigned min, unsigned max)
{
    if (max < preferred)
        return max;
    else if(min > preferred)
        return min;
    else
        return preferred;
}

inline audio_format_t choose_format(struct pcm_mask* mask)
{
    struct match {
        enum pcm_format format;
        unsigned        bit;
    };
    const struct match table[] = {
        {PCM_FORMAT_S16_LE,     SNDRV_PCM_FORMAT_S16_LE},
        {PCM_FORMAT_S24_3LE,    SNDRV_PCM_FORMAT_S24_3LE},
        {PCM_FORMAT_S24_LE,     SNDRV_PCM_FORMAT_S24_LE},
        {PCM_FORMAT_S32_LE,     SNDRV_PCM_FORMAT_S32_LE},
        {PCM_FORMAT_INVALID,    ~0u}
    };

    const unsigned bits_per_slot = sizeof(mask->bits[0]) * 8;
    const struct match *m;

    /* Search match table and return the first pcm_format supported by 'mask' */
    for (m = table; m->format != PCM_FORMAT_INVALID; m++) {
        unsigned int bits = mask->bits[m->bit / bits_per_slot];
        if(bits & (1<<(m->bit % bits_per_slot)))
            break;
    }
    return m->format;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
        audio_io_handle_t handle,
        audio_devices_t devices,
        audio_output_flags_t flags,
        struct audio_config *config,
        struct audio_stream_out **stream_out,
        const char *address __unused)
{
    ALOGV("adev_open_output_stream...");

    struct alsa_audio_device *ladev = (struct alsa_audio_device *)dev;
    struct alsa_stream_out *out;
    struct pcm_params *params;
    int ret = 0;

    params = pcm_params_get(CARD_OUT, PORT_CODEC, PCM_OUT);
    if (!params)
        return -ENOSYS;

    out = (struct alsa_stream_out *)calloc(1, sizeof(struct alsa_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->config.channels = choose_param(2,
            pcm_params_get_min(params, PCM_PARAM_CHANNELS),
            pcm_params_get_max(params, PCM_PARAM_CHANNELS));
    out->config.rate = choose_param(48000,
            pcm_params_get_min(params, PCM_PARAM_RATE),
            pcm_params_get_max(params, PCM_PARAM_RATE));

    out->config.format = choose_format(pcm_params_get_mask(params, PCM_PARAM_FORMAT));
    out->config.period_size = PERIOD_SIZE,
        out->config.period_count = PLAYBACK_PERIOD_COUNT,
        ALOGI("adev_open_output_stream selects channels=%d rate=%d format=%d",
                out->config.channels, out->config.rate, out->config.format);

    out->dev = ladev;
    out->standby = 1;
    out->unavailable = false;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
        struct audio_stream_out *stream)
{
    ALOGV("adev_close_output_stream...");
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    ALOGV("adev_set_parameters");
    return -ENOSYS;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
        const char *keys)
{
    ALOGV("adev_get_parameters");
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    ALOGV("adev_init_check");
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_voice_volume: %f", volume);
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_master_volume: %f", volume);
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
    ALOGV("adev_get_master_volume: %f", *volume);
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    ALOGV("adev_set_master_mute: %d", muted);
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    ALOGV("adev_get_master_mute: %d", *muted);
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    ALOGV("adev_set_mode: %d", mode);
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    ALOGV("adev_set_mic_mute: %d",state);
    return -ENOSYS;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    ALOGV("adev_get_mic_mute");
    return -ENOSYS;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
        const struct audio_config *config)
{
    ALOGV("adev_get_input_buffer_size: %d", 320);
    return 320;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
        audio_io_handle_t handle,
        audio_devices_t devices,
        struct audio_config *config,
        struct audio_stream_in **stream_in,
        audio_input_flags_t flags __unused,
        const char *address __unused,
        audio_source_t source __unused)
{
    ALOGV("adev_open_input_stream...");

    struct stub_audio_device *ladev = (struct stub_audio_device *)dev;
    struct stub_stream_in *in;
    int ret;

    in = (struct stub_stream_in *)calloc(1, sizeof(struct stub_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    *stream_in = &in->stream;
    return 0;

err_open:
    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
        struct audio_stream_in *in)
{
    ALOGV("adev_close_input_stream...");
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    ALOGV("adev_dump");
    return 0;
}

static int adev_close(hw_device_t *device)
{
    ALOGV("adev_close");
    free(device);
    return 0;
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *dev __unused)
{
    return (/* OUT */
            AUDIO_DEVICE_OUT_SPEAKER |
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
            AUDIO_DEVICE_OUT_DEFAULT |
            /* IN */
            AUDIO_DEVICE_IN_BUILTIN_MIC |
            AUDIO_DEVICE_IN_DEFAULT);
}

static int adev_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    ALOGV("adev_open: %s", name);

    struct alsa_audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct alsa_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.get_supported_devices = adev_get_supported_devices;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.get_master_volume = adev_get_master_volume;
    adev->hw_device.set_master_mute = adev_set_master_mute;
    adev->hw_device.get_master_mute = adev_get_master_mute;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    adev->mode = AUDIO_MODE_NORMAL;
    adev->devices = AUDIO_DEVICE_OUT_DEFAULT | AUDIO_DEVICE_IN_DEFAULT;

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Hikey audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
