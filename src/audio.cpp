//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "tusb.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/flash.h" // flash_safe_execute_core_init(): park core1 during config_save
#include "pico/util/queue.h"
#include "config.h"
#include "state_mgr.h"
#include "usb.h"

#define INPUT_CHANNELS    4
#define SAMPLE_SIZE       64
#define REPORT_SIZE       398
#define REPORT_ID         0x36
// #define VOLUME_GAIN       2
// #define BUFFER_LENGTH     48 — replaced by config().audio_buffer_length
#define MIC_CHANNELS      2
#define MIC_FRAMES        480
#define MIC_OPUS_SIZE     71

using std::clamp;
using std::max;

static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;
static bool plug_headset = false;
alignas(8) static uint32_t audio_core1_stack[8192];
queue_t audio_fifo;
static uint8_t opus_buf[200];
critical_section_t opus_cs;
queue_t mic_fifo;        // BT-encoded mic frames pending Opus decode (core0 -> core1)
queue_t mic_decode_fifo; // PCM mic frames pending USB write (core1 -> core0)

struct audio_raw_element {
    float data[512 * 2];
};
struct mic_element {
    uint8_t data[MIC_OPUS_SIZE];
};
struct mic_decode_element {
    int16_t data[MIC_FRAMES * MIC_CHANNELS];
    uint16_t len;
};

void set_headset(bool state) {
    plug_headset = state;
}

void audio_loop() {
    // Sync UAC mute status to local state if host changes it
    if (mute[1] != g_last_uac_mute) {
        g_last_uac_mute = mute[1];
        g_firmware_mic_muted = (mute[1] != 0);
        state_set_local_mute(g_firmware_mic_muted);
        state_push_to_bt();
    }

    static mic_decode_element mic_element{};
    static uint16_t mic_write_pos = 0;
    static uint16_t mic_write_len = 0;
    if (mic_write_pos == mic_write_len) {
        if (queue_try_remove(&mic_decode_fifo, &mic_element)) {
            mic_write_pos = 0;
            mic_write_len = mic_element.len;
        }
    }
    if (mic_write_pos < mic_write_len) {
        const auto *data = reinterpret_cast<const uint8_t *>(mic_element.data);
        const uint16_t remaining = mic_write_len - mic_write_pos;
        const uint16_t written = tud_audio_write(data + mic_write_pos, remaining);
        mic_write_pos += written;
    }

    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }

    // Accumulate directly into the staging queue element so the only
    // copy needed is the queue's internal memcpy at queue_try_add. Was:
    // accumulate into audio_buf, memcpy 4 KB into element, queue copies
    // another 4 KB into its slot — two memcpys per push (~760 KB/sec of
    // pure-overhead memcpy at 93 push/sec).
    static audio_raw_element staging{};
    static uint audio_buf_pos = 0;

    extern float volume[2];
    static float cached_audio_gain = 0.0f;
    static float cached_speaker_volume = 1.0f; // impossible value -> force first compute
    static bool cached_mute = true;
    if (volume[0] != cached_speaker_volume || mute[0] != cached_mute) {
        cached_speaker_volume = volume[0];
        cached_mute = mute[0];
        cached_audio_gain = cached_mute ? 0.0f : powf(10.0f, cached_speaker_volume / 20.0f);
    }
    constexpr float INV_INT16 = 1.0f / 32768.0f;
    const float audio_scale = cached_audio_gain * INV_INT16;
#if !DISABLE_SPEAKER_PROC
    for (int i = 0; i < frames; i++) {
        staging.data[audio_buf_pos++] = raw[i * INPUT_CHANNELS] * audio_scale;
        staging.data[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1] * audio_scale;
        if (audio_buf_pos == 512 * 2) {
            if (queue_is_full(&audio_fifo)) {
                queue_try_remove(&audio_fifo, NULL);
            }
            if (!queue_try_add(&audio_fifo, &staging)) {
                printf("[Audio] Warning: audio_fifo add failed\n");
            }
            __sev(); // Notify Core 1
            audio_buf_pos = 0;
        }
    }
#endif

    // Accumulate haptic samples (channels 3 & 4) into a staging buffer
    static int16_t haptic_staging[(512 + 64) * 2] = {};
    static int input_frames_acc = 0;
    for (int i = 0; i < frames; i++) {
        haptic_staging[(input_frames_acc + i) * 2 + 0] = raw[i * INPUT_CHANNELS + 2];
        haptic_staging[(input_frames_acc + i) * 2 + 1] = raw[i * INPUT_CHANNELS + 3];
    }

    input_frames_acc += frames;
    if (input_frames_acc < 512) return;
    input_frames_acc -= 512;
    {
        // 16:1 Decimation filter: average 16 samples of 48kHz to produce 1 sample of 3kHz
        static int8_t haptic_buf[SAMPLE_SIZE];
        for (int k = 0; k < 32; k++) {
            int32_t sum_l = 0;
            int32_t sum_r = 0;
            for (int j = 0; j < 16; j++) {
                sum_l += haptic_staging[(k * 16 + j) * 2 + 0];
                sum_r += haptic_staging[(k * 16 + j) * 2 + 1];
            }
            int32_t val_l = sum_l / 3072;
            int32_t val_r = sum_r / 3072;
            if (val_l < -128) val_l = -128;
            else if (val_l > 127) val_l = 127;
            if (val_r < -128) val_r = -128;
            else if (val_r > 127) val_r = 127;
            haptic_buf[k * 2 + 0] = static_cast<int8_t>(val_l);
            haptic_buf[k * 2 + 1] = static_cast<int8_t>(val_r);
        }

        if (input_frames_acc > 0) {
            memmove(haptic_staging, haptic_staging + 512 * 2, input_frames_acc * 2 * sizeof(int16_t));
        }

        // pkt is static. Constant fields (type/length bytes) are written
        // once on first call; only per-packet-varying bytes are touched
        // in the hot path. Variable rare-change bytes (buf_len, headset
        // type) are refreshed only when their inputs change.
        static uint8_t pkt[REPORT_SIZE] = {};
        static bool pkt_init = false;
        if (!pkt_init) {
            pkt[0] = REPORT_ID;
            pkt[2] = 0x11 | 0 << 6 | 1 << 7;
            pkt[3] = 7;
            // bit 0 of pkt[4] enables the controller's mic upload.
            pkt[4] = 0b11111111;
            pkt[11] = 0x10 | 0 << 6 | 1 << 7; // SetStateData type
            pkt[12] = 63;
            pkt[76] = 0x12 | 0 << 6 | 1 << 7; // Haptics Audio Data type
            pkt[77] = SAMPLE_SIZE;
#if !DISABLE_SPEAKER_PROC
            pkt[143] = 200;                   // Speaker payload length
#endif
            pkt_init = true;
        }
        // buf_len: refresh only when config value changes (writes to pkt[5..9]).
        static uint8_t cached_buf_len = 0xFF;
        const auto &cfg = get_config();
        const auto buf_len = cfg.audio_buffer_length;
        if (buf_len != cached_buf_len) {
            pkt[5] = pkt[6] = pkt[7] = pkt[8] = pkt[9] = buf_len;
            cached_buf_len = buf_len;
        }
#if !DISABLE_SPEAKER_PROC
        // Speaker/headset type byte changes only when headset is plugged/unplugged.
        static bool cached_plug_headset = !plug_headset; // force first write
        if (plug_headset != cached_plug_headset) {
            pkt[142] = (plug_headset ? 0x16 : 0x13) | 0 << 6 | 1 << 7;
            cached_plug_headset = plug_headset;
        }
#endif
        // Per-packet variable fields.
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[10] = packetCounter++;
        // Audio serves the USB-exposed slot only (the tier manager's
        // designated audio slot once slots fan out).
        state_get(BT_USB_SLOT, pkt + 13, 63);
        memcpy(pkt + 78, haptic_buf, SAMPLE_SIZE);
#if !DISABLE_SPEAKER_PROC
        critical_section_enter_blocking(&opus_cs);
        memcpy(pkt + 144, opus_buf, 200);
        critical_section_exit(&opus_cs);
#endif

        bt_write(BT_USB_SLOT, pkt, sizeof(pkt), /*kick=*/false);
    }
}

void audio_init() {
#if !DISABLE_SPEAKER_PROC
    queue_init(&audio_fifo, sizeof(audio_raw_element), 6);
    critical_section_init(&opus_cs);
    queue_init(&mic_fifo, sizeof(mic_element), 2);
    queue_init(&mic_decode_fifo, sizeof(mic_decode_element), 2);
    multicore_launch_core1_with_stack(core1_entry, audio_core1_stack, sizeof(audio_core1_stack));
#endif
}

static OpusEncoder *encoder;
static OpusDecoder *decoder; // mic decoder

void mic_proc() {
    static mic_element mic_packet{};
    if (!queue_try_remove(&mic_fifo,&mic_packet)) {
        return;
    }
    // Decode straight into the queue element to avoid an intermediate
    // ~960 B memcpy on the hot path. decode_element is the staging buffer
    // we'd be copying into anyway.
    static mic_decode_element decode_element{};
    auto decoded_samples = opus_decode(decoder, mic_packet.data, MIC_OPUS_SIZE,
                                       decode_element.data, MIC_FRAMES, false);
    if (decoded_samples <= 0) {
        printf("[Audio] OpusDecoder decode failed: %d\n", decoded_samples);
        return;
    }
    decode_element.len = decoded_samples * MIC_CHANNELS * sizeof(int16_t);
    if (g_firmware_mic_muted) {
        memset(decode_element.data, 0, decode_element.len);
    }
    if (queue_is_full(&mic_decode_fifo)) {
        queue_try_remove(&mic_decode_fifo,NULL);
    }
    queue_try_add(&mic_decode_fifo,&decode_element);
}

// speaker_proc was awalol's helper using opus_fifo/opus_element; we keep
// our existing opus_buf + opus_cs pipeline inline in core1_entry instead.

// Fast Linear Interpolator to convert 512 frames (10.66ms) down to 480 frames (10.0ms)
// Uses <1% of the CPU compared to WDL_Resampler
static void fast_resample_512_to_480(const float* in, float* out) {
    const float ratio = 512.0f / 480.0f;
    for (int i = 0; i < 480; i++) {
        float src_pos = i * ratio;
        int idx = static_cast<int>(src_pos);
        float frac = src_pos - idx;
        int idx_next = (idx < 511) ? idx + 1 : 511;

        out[i * 2 + 0] = in[idx * 2 + 0] * (1.0f - frac) + in[idx_next * 2 + 0] * frac;
        out[i * 2 + 1] = in[idx * 2 + 1] * (1.0f - frac) + in[idx_next * 2 + 1] * frac;
    }
}

void core1_entry() {
    // Register core1 as a flash-safe victim so core0's flash_safe_execute()
    // (config_save) actually parks this core while flash is erased/programmed,
    // instead of letting it fault on XIP. Requires PICO_FLASH_ASSUME_CORE1_SAFE=0.
    flash_safe_execute_core_init();
    int error = 0;
    encoder = opus_encoder_create(48000, 2,OPUS_APPLICATION_AUDIO, &error);
    if (error != 0) {
        printf("[Audio] OpusEncoder create failed\n");
        return;
    }
    opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
    opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0)); // max 4
    decoder = opus_decoder_create(48000, MIC_CHANNELS, &error);
    if (error != 0) {
        printf("[Audio] OpusDecoder create failed\n");
    }

    while (true) {
        bool worked = false;
        // Speaker (host -> DS5) encode path. Use try_remove so mic_proc isn't starved.
        static audio_raw_element audio_element{};
        if (queue_try_remove(&audio_fifo, &audio_element)) {
            worked = true;
            static float out_buf[480 * 2];
            fast_resample_512_to_480(audio_element.data, out_buf);

            static uint8_t out[200];
            const opus_int32 encoded = opus_encode_float(encoder, out_buf, 480, out, sizeof(out));
            if (encoded < 0) {
                continue;
            }
            if (static_cast<size_t>(encoded) < sizeof(out)) {
                memset(out + encoded, 0, sizeof(out) - static_cast<size_t>(encoded));
            }
            critical_section_enter_blocking(&opus_cs);
            memcpy(opus_buf, out, 200);
            critical_section_exit(&opus_cs);
        }
        if (!queue_is_empty(&mic_fifo)) {
            mic_proc();
            worked = true;
        }
        if (!worked) {
            // Clear event register and sleep until the next __sev() event
            __wfe();
        }
    }
}

extern bool mic_active; // set by tud_audio_set_itf_cb in main.cpp

void mic_add_queue(uint8_t *data) {
    // Don't decode mic frames if the host isn't streaming the mic interface.
    // Decode load on core1 degrades speaker quality (see findings memo); when
    // nobody is listening we skip the work entirely.
    if (!mic_active) return;
    static mic_element mic_packet{};
    memcpy(mic_packet.data,data,MIC_OPUS_SIZE);
    if (queue_is_full(&mic_fifo)) {
        queue_try_remove(&mic_fifo,NULL);
    }
    queue_try_add(&mic_fifo,&mic_packet);
    __sev(); // Notify Core 1
}
