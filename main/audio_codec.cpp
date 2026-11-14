// Audio backend for the M5Stack Tab5.
//
// extern "C" interface: audio_init / audio_write_s32 / the counters. The emulator on one
// core converts and enqueues, a task on the other drains into the codec with a
// blocking write so the emulator never stalls on audio.
//
// The Tab5 does not have a bare I2S amplifier
// on three GPIOs, it has a codec behind I2C with the speaker enable on an I/O
// expander. All of that is the BSP's job — bsp_audio_init() brings up I2S and
// bsp_audio_codec_speaker_init() returns a handle that takes PCM. So there is
// no pin table here at all.

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"  // xStreamBufferCreateWithCaps
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

#include "board_pins.h"
#include "pie_simd.h"  // falls back to plain C off Xtensa/S3

extern "C" int ets_printf(const char *fmt, ...);

static bool s_ok = false;
static int s_bufframes = 0;
static int16_t *s_buf = nullptr;                // conversion scratch (stereo int16)
static int16_t *s_dry16 = nullptr;              // conversion scratch (the beep)
static int16_t *s_frames = nullptr;             // the two, as ring frames
static StreamBufferHandle_t s_ring = nullptr;   // core1 -> core0 PCM stream (bytes)
// A ring frame is the FM mix and the beep side by side - L, R, beep L, beep R -
// so the effects can be put on the one and not the other: the machine's own
// beep (the boot "pipo", a game's key clicks) is not part of the FM sound.
#define RING_FRAME 8
static esp_codec_dev_handle_t s_spk = nullptr;  // BSP speaker codec
static int s_volume = 70;                       // ES8388 output level, 0-100


// ---- sound effects ----------------------------------------------------------
// The FM chip's output reaches the speaker as hard as it left the chip: at a
// 22kHz mix its upper harmonics fold back as a gritty edge, and the SSG's
// square waves keep their corners. These are optional colourings, applied
// here on the way to the codec - on this core, so the emulator pays nothing:
//
//   0  Original     native FM
//   1  retired      old Warm settings normalize to Original
//   2  Hall         filtered native FM with stereo room ambience
//   3  Synthesizer  procedural instrument replacement with ambience
//   4  Orchestra    an orchestra's instruments in place of the chip's, in a concert hall
extern "C" void fm_orchestra_enable(int on);
static volatile int s_fx = 0;
#define PRIME_MS 80
#define RING_TARGET_MS 40   // what the ring is kept at, on top of the codec's own buffer
static float s_target_frames = 22050.0f * RING_TARGET_MS / 1000;
static size_t s_prime_bytes = 22050 * RING_FRAME * PRIME_MS / 1000;
static int s_fx_rate = 22050;

struct biquad {
    float b0, b1, b2, a1, a2;
    float z1[2], z2[2];
};

static void bq_set(biquad &f, float b0, float b1, float b2, float a0, float a1, float a2) {
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
    f.z1[0] = f.z1[1] = f.z2[0] = f.z2[1] = 0.0f;
}

// RBJ cookbook.
static void bq_lowpass(biquad &f, float fs, float fc, float q) {
    const float w = 2.0f * (float)M_PI * fc / fs, c = cosf(w), al = sinf(w) / (2.0f * q);
    bq_set(f, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

static void bq_lowshelf(biquad &f, float fs, float fc, float db) {
    const float A = powf(10.0f, db / 40.0f), w = 2.0f * (float)M_PI * fc / fs;
    const float c = cosf(w), sn = sinf(w), al = sn / 2.0f * sqrtf(2.0f);
    const float sa = 2.0f * sqrtf(A) * al;
    bq_set(f, A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c),
           A * ((A + 1) - (A - 1) * c - sa), (A + 1) + (A - 1) * c + sa,
           -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}

static inline float bq_run(biquad &f, int ch, float x) {
    const float y = f.b0 * x + f.z1[ch];
    f.z1[ch] = f.b1 * x - f.a1 * y + f.z2[ch];
    f.z2[ch] = f.b2 * x - f.a2 * y;
    return y;
}

// Freeverb's structure (parallel damped combs, then series allpasses), four
// combs a side, delays halved for 22kHz from its 44.1kHz tuning.
#define FX_COMBS 4
#define FX_ALLPS 2
#define FX_SPREAD 23                    // samples between the two sides' tunings
#define FX_WIDE_LEN 331                 // 15ms at 22kHz: the pseudo-stereo delay
static const int k_comb_len[FX_COMBS] = {558, 594, 639, 678};
static const int k_allp_len[FX_ALLPS] = {278, 220};

struct fx_line {
    float *buf;
    int    len, pos;
    float  store;
};

static fx_line s_comb[2][FX_COMBS];
static fx_line s_allp[2][FX_ALLPS];

// Orchestra's own room: a concert hall, not the studio above. Freeverb's full
// eight combs a side, half as long again, behind a 20ms pre-delay: a tail of
// about two seconds, which is what makes separate players one orchestra.
#define HALL_COMBS 8
#define HALL_ALLPS 4
#define HALL_PRE   441
static const int k_hall_comb[HALL_COMBS] = {809, 877, 937, 1009, 1069, 1151, 1223, 1297};
static const int k_hall_allp[HALL_ALLPS] = {278, 220, 170, 112};
static fx_line s_hcomb[2][HALL_COMBS];
static fx_line s_hallp[2][HALL_ALLPS];
static fx_line s_hpre;
static biquad  s_lp, s_shelf;
static bool    s_fx_ready = false;
static float  *s_wide;                  // FX_WIDE_LEN samples of the mono signal
static int     s_wide_pos;

static inline float comb_run(fx_line &c, float x, float fb, float damp) {
    const float y = c.buf[c.pos];
    c.store = y * (1.0f - damp) + c.store * damp;
    c.buf[c.pos] = x + c.store * fb;
    if (++c.pos >= c.len) {
        c.pos = 0;
    }
    return y;
}

static inline float allp_run(fx_line &a, float x) {
    const float b = a.buf[a.pos];
    a.buf[a.pos] = x + b * 0.5f;
    if (++a.pos >= a.len) {
        a.pos = 0;
    }
    return b - x;
}

static void line_clear(fx_line &c) {
    memset(c.buf, 0, sizeof(float) * c.len);
    c.pos = 0;
    c.store = 0.0f;
}

static bool line_alloc(fx_line &c, int len) {
    c.len = len;
    c.buf = (float *)heap_caps_malloc(sizeof(float) * len, MALLOC_CAP_SPIRAM);
    return c.buf != nullptr;
}

static void fx_reset(void) {
    const float fs = (float)s_fx_rate;
    bq_lowpass(s_lp, fs, 6500.0f, 0.6f);
    bq_lowshelf(s_shelf, fs, 180.0f, 5.0f);
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < FX_COMBS; k++) {
            line_clear(s_comb[ch][k]);
        }
        for (int k = 0; k < FX_ALLPS; k++) {
            line_clear(s_allp[ch][k]);
        }
        for (int k = 0; k < HALL_COMBS; k++) {
            line_clear(s_hcomb[ch][k]);
        }
        for (int k = 0; k < HALL_ALLPS; k++) {
            line_clear(s_hallp[ch][k]);
        }
    }
    line_clear(s_hpre);
    memset(s_wide, 0, sizeof(float) * FX_WIDE_LEN);
    s_wide_pos = 0;
}

static bool fx_init(int rate) {
    s_fx_rate = rate;
    for (int ch = 0; ch < 2; ch++) {
        const int sp = ch ? FX_SPREAD : 0;   // the right side tuned a little apart from the left
        for (int k = 0; k < FX_COMBS; k++) {
            if (!line_alloc(s_comb[ch][k], k_comb_len[k] + sp)) {
                return false;
            }
        }
        for (int k = 0; k < FX_ALLPS; k++) {
            if (!line_alloc(s_allp[ch][k], k_allp_len[k] + sp)) {
                return false;
            }
        }
        for (int k = 0; k < HALL_COMBS; k++) {
            if (!line_alloc(s_hcomb[ch][k], k_hall_comb[k] + sp)) {
                return false;
            }
        }
        for (int k = 0; k < HALL_ALLPS; k++) {
            if (!line_alloc(s_hallp[ch][k], k_hall_allp[k] + sp)) {
                return false;
            }
        }
    }
    if (!line_alloc(s_hpre, HALL_PRE)) {
        return false;
    }
    s_wide = (float *)heap_caps_malloc(sizeof(float) * FX_WIDE_LEN, MALLOC_CAP_SPIRAM);
    if (!s_wide) {
        return false;
    }
    fx_reset();
    s_fx_ready = true;
    return true;
}

// A hard clip is a click every time a peak touches the top; the boost at the
// bottom and the reverb's tail make that common. Above 70% of full scale the
// curve bends over instead, smoothly, and never quite reaches it.
static inline int16_t fx_clip(float v) {
    float n = v * (1.0f / 32768.0f);
    const float a = fabsf(n);
    if (a > 0.7f) {
        const float e = a - 0.7f;
        n = copysignf(0.7f + 0.3f * e / (e + 0.3f), n);
    }
    return (int16_t)(n * 32767.0f);
}

// From ring frames (in) to stereo s16 frames (out): the effects on the FM mix,
// and the beep added after them untouched.
static void fx_process(const int16_t *p, int16_t *o, int frames, int mode) {
    // Headroom for the shelf's boost and the reverb's tail; the codec's own
    // volume is where loudness belongs.
    const float pre = 0.6f;
    // The beep at the level the FM mix's direct sound comes out at, so the
    // two keep the balance they have with the effects off.
    const float dry = (mode >= 2) ? pre * 0.8f : pre;
    for (int i = 0; i < frames; i++) {
        float l = bq_run(s_lp, 0, p[4 * i] * pre);
        float r = bq_run(s_lp, 1, p[4 * i + 1] * pre);
        // Acoustic bass already has a strong fundamental. Keep Orchestra flat
        // below 180 Hz instead of inheriting the legacy 5 dB bass boost.
        if (mode != 4) {
            l = bq_run(s_shelf, 0, l);
            r = bq_run(s_shelf, 1, r);
        }
        if (mode == 4) {
            // The hall: the whole orchestra into one room, a moment late.
            const float in = s_hpre.buf[s_hpre.pos];
            s_hpre.buf[s_hpre.pos] = (l + r) * 0.015f;
            if (++s_hpre.pos >= s_hpre.len) {
                s_hpre.pos = 0;
            }
            float wl = 0.0f, wr = 0.0f;
            for (int k = 0; k < HALL_COMBS; k++) {
                wl += comb_run(s_hcomb[0][k], in, 0.87f, 0.40f);
                wr += comb_run(s_hcomb[1][k], in, 0.87f, 0.40f);
            }
            for (int k = 0; k < HALL_ALLPS; k++) {
                wl = allp_run(s_hallp[0][k], wl);
                wr = allp_run(s_hallp[1][k], wr);
            }
            l = l * 0.85f + wl * 0.85f + wr * 0.15f;
            r = r * 0.85f + wr * 0.85f + wl * 0.15f;
        } else if (mode >= 2) {
            const float in = (l + r) * 0.015f;
            float wl = 0.0f, wr = 0.0f;
            for (int k = 0; k < FX_COMBS; k++) {
                wl += comb_run(s_comb[0][k], in, 0.83f, 0.35f);
                wr += comb_run(s_comb[1][k], in, 0.83f, 0.35f);
            }
            for (int k = 0; k < FX_ALLPS; k++) {
                wl = allp_run(s_allp[0][k], wl);
                wr = allp_run(s_allp[1][k], wr);
            }
            // Pseudo-stereo (Lauridsen): the signal 15ms late, added to one
            // side and taken from the other. Each frequency leans left or
            // right by a different amount, so a mono chip opens out - and the
            // two sides still add back up to the original.
            const float m = (l + r) * 0.5f;
            const float d = s_wide[s_wide_pos];
            s_wide[s_wide_pos] = m;
            if (++s_wide_pos >= FX_WIDE_LEN) {
                s_wide_pos = 0;
            }
            // The reverb wider than freeverb's 1.0 as well: each side gets a
            // little of the other's tail, inverted.
            const float w1 = 1.25f, w2 = -0.25f;
            if (mode >= 3) {
                l = l * 0.8f + (wl * 0.9f + wr * 0.1f) * 1.5f;
                r = r * 0.8f + (wr * 0.9f + wl * 0.1f) * 1.5f;
            } else {
                l = l * 0.8f + d * 0.35f + (wl * w1 + wr * w2) * 1.3f;
                r = r * 0.8f - d * 0.35f + (wr * w1 + wl * w2) * 1.3f;
            }
        }
        o[2 * i] = fx_clip(l + p[4 * i + 2] * dry);
        o[2 * i + 1] = fx_clip(r + p[4 * i + 3] * dry);
    }
}

// Effects off: the FM mix and the beep added back together.
static void mix_plain(const int16_t *p, int16_t *o, int frames) {
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            const int v = p[4 * i + ch] + p[4 * i + 2 + ch];
            o[2 * i + ch] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
}

extern "C" void audio_set_fx(int mode) {
    s_fx = (mode >= 0 && mode <= 4 && mode != 1) ? mode : 0;
    fm_orchestra_enable(s_fx == 4 ? 2 : s_fx == 3 ? 1 : 0);
}

extern "C" int audio_get_fx(void) {
    return s_fx;
}

// ---- clock drift -------------------------------------------------------------
// The emulator and the codec run on different clocks, so what is waiting in the ring drifts up or down. Correcting
// that by dropping or inserting whole frames is a small step in the wave each time - a crackle. Instead each block is
// resampled, a fraction of a percent faster or slower, which is smooth: the ring is held at RING_TARGET_MS and the
// codec's own buffer never runs dry.
struct resampler {
    float phase;        // where the next output frame falls, in input frames from the start of the block
    float fill_avg;     // the ring's fill, smoothed, in frames
    int16_t last[2];    // the previous block's last frame: the left end of the first interpolation
};

// How fast to read the coming block: above 1 while the ring holds more than the target, below while it holds less.
static float rs_step(resampler &rs, size_t ring_bytes) {
    rs.fill_avg += ((float)ring_bytes / RING_FRAME - rs.fill_avg) * (1.0f / 32.0f);
    const float step = 1.0f + (rs.fill_avg - s_target_frames) / s_target_frames * 0.002f;
    return step > 1.003f ? 1.003f : step < 0.997f ? 0.997f : step;
}

// Linear interpolation over [the previous block's last frame, this block]; returns the frames written to out.
static int rs_run(resampler &rs, const int16_t *in, int frames, float step, int16_t *out) {
    int n = 0;
    while (rs.phase < (float)frames) {
        const int i = (int)rs.phase;
        const float t = rs.phase - (float)i;
        const float l0 = i ? in[2 * (i - 1)] : rs.last[0], r0 = i ? in[2 * (i - 1) + 1] : rs.last[1];
        out[2 * n] = (int16_t)(l0 + (in[2 * i] - l0) * t);
        out[2 * n + 1] = (int16_t)(r0 + (in[2 * i + 1] - r0) * t);
        n++;
        rs.phase += step;
    }
    rs.phase -= (float)frames;
    rs.last[0] = in[2 * (frames - 1)];
    rs.last[1] = in[2 * (frames - 1) + 1];
    return n;
}

// ---- core 0: drain the ring into the codec --------------------------------
static void audio_task(void *arg) {
    (void)arg;
    // 4KB = 512 ring frames = ~23ms per codec write. On the S3 this was 1KB
    // because the ring behind it was small; here it just means fewer wakeups.
    static uint8_t rx[4096] __attribute__((aligned(4)));
    static int16_t mix[sizeof(rx) / RING_FRAME * 2];  // the block with the effects
    static int16_t out[sizeof(rx) / RING_FRAME * 2 + 64];  // ...resampled: what goes to the codec
    resampler rs = {0.0f, s_target_frames, {0, 0}};
    size_t carry = 0;  // bytes of a split ring frame held back from last time
    int fx_was = 0;
    bool primed = false;
    for (;;) {
        // Keep a cushion. The emulator hands over a frame's worth of sound at
        // a time, and a frame does not always take the same time; passed on
        // the moment it arrived, the codec was fed just in time, and every
        // frame that ran a little long was a gap - a click. So wait until
        // there is PRIME_MS of sound in hand before starting, and again after
        // running dry, and the jitter is absorbed by what is waiting.
        if (!primed) {
            if (xStreamBufferBytesAvailable(s_ring) < s_prime_bytes) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            primed = true;
        }
        size_t n = xStreamBufferReceive(s_ring, rx + carry, sizeof(rx) - carry, pdMS_TO_TICKS(100));
        if (n == 0) {
            primed = false;
            continue;
        }
        n += carry;
        // A ring frame is RING_FRAME bytes and the ring is byte-oriented, so
        // what comes back is not always a whole number of frames. Handing the
        // codec a partial frame shifts left and right for the rest of the
        // session — that is exactly the "clicking" bug the split build hit.
        // Carry the remainder into the next read instead.
        const size_t play = n - n % RING_FRAME;
        carry = n - play;
        if (play) {
            const int fx = s_fx;
            if (fx != fx_was) {
                if (s_fx_ready) {
                    fx_reset();          // no tail of the old setting into the new
                }
                fx_was = fx;
            }
            const int frames = (int)(play / RING_FRAME);
            const float step = rs_step(rs, xStreamBufferBytesAvailable(s_ring));
            if (fx && s_fx_ready) {
                fx_process((const int16_t *)rx, mix, frames, fx);
            } else {
                mix_plain((const int16_t *)rx, mix, frames);
            }
            esp_codec_dev_write(s_spk, out, rs_run(rs, mix, frames, step, out) * 4);
        }
        if (carry) {
            memmove(rx, rx + play, carry);
        }
    }
}

// ---- headphones -----------------------------------------------------------
// The codec drives the headphone socket and the speaker's amplifier alike, so with a plug in both played. The socket's
// switch is on the first I/O expander (0x43), pin 7, which reads 1 with a plug in (measured on the board): the
// amplifier is on only without one.
static void hp_task(void *arg) {
    (void)arg;
    esp_io_expander_handle_t io = bsp_io_expander_init();
    int state = -1, seen = -1, same = 0;
    if (io) {
        esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_INPUT);
    }
    for (;;) {
        uint32_t level = 0;
        if (io && esp_io_expander_get_level(io, IO_EXPANDER_PIN_NUM_7, &level) == ESP_OK) {
            const int in = (level & IO_EXPANDER_PIN_NUM_7) ? 1 : 0;
            if (in == seen) {
                same++;
            } else {
                seen = in;
                same = 0;
            }
            if (same >= 2 && in != state) {      // the same three times running: the plug is in, or out
                state = in;
                bsp_feature_enable(BSP_FEATURE_SPEAKER, !in);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// ---- init -----------------------------------------------------------------
// rate = sample rate, maxframes = the biggest block audio_write_s32 will get.
extern "C" bool audio_init(int rate, int maxframes) {
    if (maxframes <= 0) {
        return false;
    }
    if (bsp_audio_init(nullptr) != ESP_OK) {
        ets_printf("audio: bsp_audio_init FAIL\n");
        return false;
    }
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ets_printf("audio: codec speaker init FAIL\n");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 2;
    fs.sample_rate = (uint32_t)rate;
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {
        ets_printf("audio: codec open FAIL\n");
        return false;
    }
    esp_codec_dev_set_out_vol(s_spk, s_volume);

    s_bufframes = maxframes;
    // 16-byte align the scratch: the PIE fast path requires it on the S3, and
    // keeping the alignment costs nothing here.
    const size_t scratch = (size_t)maxframes * 2 * sizeof(int16_t) + 16;
    void *raw = heap_caps_malloc(scratch, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!raw) {
        ets_printf("audio: scratch alloc FAIL\n");
        return false;
    }
    s_buf = (int16_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    void *raw_dry = heap_caps_malloc(scratch, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_frames = (int16_t *)heap_caps_malloc((size_t)maxframes * RING_FRAME,
                                           MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!raw_dry || !s_frames) {
        ets_printf("audio: scratch alloc FAIL\n");
        return false;
    }
    s_dry16 = (int16_t *)(((uintptr_t)raw_dry + 15) & ~(uintptr_t)15);

    // 16 blocks of 20ms = ~320ms. This board
    // has 32MB with ~11MB spare after the 13MB of emulated extended memory, so
    // the ring is sized for how long a stall it should ride out rather than for
    // what fits. A shorter ring refills more often,
    // so the producer is throttled more frequently and the stutter at a BGM
    // change (where the game loads from the card and the emulator stalls) is
    // worse, not better. In PSRAM because only tasks touch it, never an ISR.
    const size_t ringbytes = (size_t)maxframes * RING_FRAME * 16;
    s_ring = xStreamBufferCreateWithCaps(ringbytes, 1, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ets_printf("audio: ring alloc FAIL (%u bytes psram)\n", (unsigned)ringbytes);
        return false;
    }

    s_prime_bytes = (size_t)rate * RING_FRAME * PRIME_MS / 1000;
    s_target_frames = (float)rate * RING_TARGET_MS / 1000;
    if (!fx_init(rate)) {
        ets_printf("audio: no memory for the effects - they stay off\n");
    }
    s_ok = true;
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 6, nullptr, 0);
    xTaskCreatePinnedToCore(hp_task, "phones", 3072, nullptr, 2, nullptr, 0);
    ets_printf("audio: codec ready rate=%d\n", rate);
    return true;
}

// ---- core 1: convert the mixer's SINT32 block and enqueue ----------------------
extern "C" void audio_write_s32(const int32_t *pcm, const int32_t *dry, int frames) {
    if (!s_ok || !s_ring || !s_buf || !pcm || frames <= 0) {
        return;
    }
    if (frames > s_bufframes) {
        frames = s_bufframes;
    }
    const int n = frames * 2;
    simd_s32_to_s16(pcm, s_buf, n, 0);
    if (dry) {
        simd_s32_to_s16(dry, s_dry16, n, 0);
    } else {
        memset(s_dry16, 0, (size_t)n * sizeof(int16_t));
    }
    for (int i = 0; i < frames; i++) {
        s_frames[4 * i] = s_buf[2 * i];
        s_frames[4 * i + 1] = s_buf[2 * i + 1];
        s_frames[4 * i + 2] = s_dry16[2 * i];
        s_frames[4 * i + 3] = s_dry16[2 * i + 1];
    }

    const size_t bytes = (size_t)frames * RING_FRAME;
    // All of the block or none of it. A send cut short at whatever byte the
    // ring had room for left the stream out of step with the frames, and
    // everything after it played as noise - left and right, high and low
    // bytes all crossed - until another cut happened to put it back.
    if (xStreamBufferSpacesAvailable(s_ring) < bytes) {
        return;
    }
    xStreamBufferSend(s_ring, s_frames, bytes, 0);
}

// ---- volume ---------------------------------------------------------------
// The ES8388's own output level, not a gain applied to the samples: the mix is
// already scaled to fit int16 (g_settings.volume_all), and attenuating it again in
// software would only cost bits.
extern "C" void audio_set_volume(int percent) {
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (s_spk) {
        esp_codec_dev_set_out_vol(s_spk, s_volume);
    }
}

extern "C" int audio_get_volume(void) {
    return s_volume;
}
