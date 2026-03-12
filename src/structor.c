/*
 * Structor — Musique concrète sound deconstructor/reconstructor
 * Audio FX for Ableton Move via Move-Anything (audio_fx_api_v2)
 *
 * Original DSP: envelope follower + onset/peak/ZC detection → mode-dependent
 * reordering → grain-based reconstruction with Hann/Deltarupt envelopes.
 *
 * MIT License — fillioning 2025
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ========================================================================
   Move-Anything API (must match chain_host ABI exactly)
   ======================================================================== */

typedef int (*move_mod_emit_value_fn)(void *ctx,
                                      const char *source_id,
                                      const char *target,
                                      const char *param,
                                      float signal,
                                      float depth,
                                      float offset,
                                      int bipolar,
                                      int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
} host_api_v1_t;

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

/* ========================================================================
   Constants
   ======================================================================== */

#define BUFFER_SIZE     131072  /* ~3 sec at 44.1 kHz */
#define MAX_EVENTS      2048
#define BASE_GRAIN_SIZE 256
#define MAX_MODES       7
#define MAX_ARP         6
#define BLOCK_SIZE      128

/* ========================================================================
   Types
   ======================================================================== */

typedef enum {
    EVENT_ONSET,
    EVENT_PEAK,
    EVENT_ZERO_CROSSING
} EventType;

typedef struct {
    uint32_t sample_index;
    float amplitude;
    float frequency_center;
    float density;
    uint8_t type;
} StructorEvent;

typedef enum {
    MODE_RANDOM = 0,
    MODE_PITCH_UP,
    MODE_PITCH_DOWN,
    MODE_DENSITY_UP,
    MODE_TIME_WARP,
    MODE_DENSITY_ARP,
    MODE_DELTARUPT
} ReconMode;

typedef enum {
    ARP_UP = 0,
    ARP_DOWN,
    ARP_UP_DOWN,
    ARP_DOWN_UP,
    ARP_RANDOM,
    ARP_CASCADE
} ArpPattern;

/* ========================================================================
   Instance
   ======================================================================== */

typedef struct {
    /* Circular buffer (stereo, int16 converted to float internally) */
    float buffer_l[BUFFER_SIZE];
    float buffer_r[BUFFER_SIZE];
    uint32_t buf_write_pos;

    /* Event detection */
    StructorEvent events[MAX_EVENTS];
    uint32_t event_count;
    int playback_order[MAX_EVENTS];

    /* Grain playback state */
    uint32_t playback_index;
    float playback_position;

    /* Reconstruction output buffers (pre-allocated, no malloc in render) */
    float recon_l[BLOCK_SIZE];
    float recon_r[BLOCK_SIZE];

    /* Envelope follower */
    float env_follower;
    float env_attack;
    float env_release;

    /* Parameters — 6 global */
    float detection;        /* 0.0-1.0, default 0.1 */
    float density;          /* 0.1-1.0, default 0.7 */
    float grain_size;       /* 0.5-2.0, default 1.0 */
    float time_warp;        /* 0.5-2.0, default 1.0 */
    float mix;              /* 0.0-1.0, default 0.5 */
    float feedback;         /* 0.0-0.95, default 0.2 */

    /* Mode */
    int mode;               /* 0-6 */

    /* Mode-specific params */
    float reshuffle;        /* mode 0: 0.0-1.0 */
    float scatter;          /* mode 1,2: 0.0-1.0 */
    float curve;            /* mode 3: 0.0-1.0 */
    float drift;            /* mode 4: 0.0-1.0 */
    int   arp_pattern;      /* mode 5: 0-5 */
    float deltarupt_attack; /* mode 6: 0.0-1.0 */

    /* Frame counter (for reshuffle timing) */
    uint32_t frame_count;

    /* Simple PRNG state (no stdlib rand() in render path) */
    uint32_t rng;
} StructorInstance;

static const host_api_v1_t *g_host = NULL;

/* ========================================================================
   Utilities
   ======================================================================== */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int clampi(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

/* Fast PRNG (xorshift32) — no global state, no stdlib dependency */
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Hermite 4-point interpolation */
static inline float hermite_interp(float y0, float y1, float y2, float y3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2*t3 - 3*t2 + 1;
    float h10 = t3 - 2*t2 + t;
    float h01 = -2*t3 + 3*t2;
    float h11 = t3 - t2;
    return h00*y1 + h10*(y2-y0)*0.5f + h01*y2 + h11*(y3-y1)*0.5f;
}

static inline float smooth_env(float current, float target, float attack, float release) {
    if (target > current)
        return current + (target - current) * attack;
    else
        return current - (current - target) * release;
}

/* ========================================================================
   Spectral estimation (ZCR-based, rough — Phase 2: FFT)
   ======================================================================== */

static float estimate_spectral_center(StructorInstance *inst,
                                       uint32_t window_start,
                                       uint32_t window_size) {
    float *buf = inst->buffer_l;
    uint32_t zc_count = 0;

    for (uint32_t i = 0; i < window_size - 1; i++) {
        float s1 = buf[(window_start + i) % BUFFER_SIZE];
        float s2 = buf[(window_start + i + 1) % BUFFER_SIZE];
        if ((s1 >= 0 && s2 < 0) || (s1 < 0 && s2 >= 0))
            zc_count++;
    }

    float zcr = (float)zc_count / (float)window_size;
    return 200.0f + zcr * 7800.0f;
}

/* ========================================================================
   Event detection
   ======================================================================== */

static void detect_events(StructorInstance *inst,
                           uint32_t window_start,
                           uint32_t window_size) {
    float *buf_l = inst->buffer_l;
    float *buf_r = inst->buffer_r;

    inst->event_count = 0;

    uint32_t min_spacing = (uint32_t)(44100.0f * 0.01f);  /* 10 ms */
    uint32_t last_event_pos = 0;

    for (uint32_t i = 0; i < window_size - 1 && inst->event_count < MAX_EVENTS; i++) {
        uint32_t pos = (window_start + i) % BUFFER_SIZE;
        uint32_t pos_next = (pos + 1) % BUFFER_SIZE;

        float sl = buf_l[pos];
        float sr = buf_r[pos];
        float sl_next = buf_l[pos_next];
        float sr_next = buf_r[pos_next];

        float mag = fabsf(sl) + fabsf(sr);
        float mag_next = fabsf(sl_next) + fabsf(sr_next);

        inst->env_follower = smooth_env(inst->env_follower, mag,
                                         inst->env_attack, inst->env_release);

        int is_onset = 0, is_peak = 0, is_zc = 0;

        /* Onset: magnitude above threshold & envelope */
        if (mag > inst->detection && mag > inst->env_follower * 0.5f) {
            if (mag > mag_next || i == 0)
                is_onset = 1;
        }

        /* Peak: local maximum */
        if (i > 0 && i < window_size - 2) {
            uint32_t pos_prev = (pos - 1 + BUFFER_SIZE) % BUFFER_SIZE;
            float mag_prev = fabsf(buf_l[pos_prev]) + fabsf(buf_r[pos_prev]);
            uint32_t pos_next2 = (pos_next + 1) % BUFFER_SIZE;
            float mag_next2 = fabsf(buf_l[pos_next2]) + fabsf(buf_r[pos_next2]);
            if (mag > mag_prev && mag > mag_next && mag > mag_next2)
                is_peak = 1;
        }

        /* Zero-crossing */
        if ((sl >= 0 && sl_next < 0) || (sl < 0 && sl_next >= 0))
            is_zc = 1;

        if ((is_onset || is_peak || is_zc) &&
            (pos >= last_event_pos + min_spacing)) {

            StructorEvent *evt = &inst->events[inst->event_count++];
            evt->sample_index = pos;
            evt->amplitude = mag;
            evt->frequency_center = estimate_spectral_center(inst, pos, BASE_GRAIN_SIZE);
            evt->density = clampf(mag, 0.0f, 1.0f);
            evt->type = is_onset ? EVENT_ONSET :
                       (is_peak ? EVENT_PEAK : EVENT_ZERO_CROSSING);

            last_event_pos = pos;
        }
    }

    /* Density culling: keep top N by amplitude */
    if (inst->event_count > 0) {
        uint32_t target = (uint32_t)(inst->event_count * inst->density);
        if (target < 4) target = 4;
        if (target < inst->event_count) {
            /* Simple selection sort for top-N (avoid qsort overhead in render) */
            for (uint32_t i = 0; i < target && i < inst->event_count - 1; i++) {
                uint32_t max_idx = i;
                for (uint32_t j = i + 1; j < inst->event_count; j++) {
                    if (inst->events[j].amplitude > inst->events[max_idx].amplitude)
                        max_idx = j;
                }
                if (max_idx != i) {
                    StructorEvent tmp = inst->events[i];
                    inst->events[i] = inst->events[max_idx];
                    inst->events[max_idx] = tmp;
                }
            }
            inst->event_count = target;
        }
    }
}

/* ========================================================================
   Comparators for qsort (used in build_playback_order)
   ======================================================================== */

static int cmp_freq_asc(const void *a, const void *b) {
    float fa = ((const StructorEvent*)a)->frequency_center;
    float fb = ((const StructorEvent*)b)->frequency_center;
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

static int cmp_freq_desc(const void *a, const void *b) {
    float fa = ((const StructorEvent*)a)->frequency_center;
    float fb = ((const StructorEvent*)b)->frequency_center;
    return (fa > fb) ? -1 : (fa < fb) ? 1 : 0;
}

static int cmp_density_asc(const void *a, const void *b) {
    float da = ((const StructorEvent*)a)->density;
    float db = ((const StructorEvent*)b)->density;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

static int cmp_density_desc(const void *a, const void *b) {
    float da = ((const StructorEvent*)a)->density;
    float db = ((const StructorEvent*)b)->density;
    return (da > db) ? -1 : (da < db) ? 1 : 0;
}

/* ========================================================================
   Build playback order (mode-dependent)
   ======================================================================== */

static void build_playback_order(StructorInstance *inst) {
    uint32_t n = inst->event_count;
    if (n == 0) return;

    /* Work directly on events array — reorder in place */
    switch (inst->mode) {
        case MODE_RANDOM: {
            /* Fisher-Yates shuffle */
            for (uint32_t i = n - 1; i > 0; i--) {
                uint32_t j = xorshift32(&inst->rng) % (i + 1);
                StructorEvent tmp = inst->events[i];
                inst->events[i] = inst->events[j];
                inst->events[j] = tmp;
            }
            break;
        }
        case MODE_PITCH_UP:
            qsort(inst->events, n, sizeof(StructorEvent), cmp_freq_asc);
            /* Apply scatter: swap random pairs based on scatter amount */
            if (inst->scatter > 0.01f) {
                uint32_t swaps = (uint32_t)(n * inst->scatter * 0.5f);
                for (uint32_t s = 0; s < swaps; s++) {
                    uint32_t a = xorshift32(&inst->rng) % n;
                    uint32_t b = xorshift32(&inst->rng) % n;
                    StructorEvent tmp = inst->events[a];
                    inst->events[a] = inst->events[b];
                    inst->events[b] = tmp;
                }
            }
            break;

        case MODE_PITCH_DOWN:
            qsort(inst->events, n, sizeof(StructorEvent), cmp_freq_desc);
            if (inst->scatter > 0.01f) {
                uint32_t swaps = (uint32_t)(n * inst->scatter * 0.5f);
                for (uint32_t s = 0; s < swaps; s++) {
                    uint32_t a = xorshift32(&inst->rng) % n;
                    uint32_t b = xorshift32(&inst->rng) % n;
                    StructorEvent tmp = inst->events[a];
                    inst->events[a] = inst->events[b];
                    inst->events[b] = tmp;
                }
            }
            break;

        case MODE_DENSITY_UP:
            qsort(inst->events, n, sizeof(StructorEvent), cmp_density_asc);
            break;

        case MODE_TIME_WARP:
            /* Keep original order — speed varies in reconstruct */
            break;

        case MODE_DENSITY_ARP: {
            /* Sort by density ascending first */
            qsort(inst->events, n, sizeof(StructorEvent), cmp_density_asc);

            switch (inst->arp_pattern) {
                case ARP_UP:
                    /* Already ascending */
                    break;
                case ARP_DOWN:
                    qsort(inst->events, n, sizeof(StructorEvent), cmp_density_desc);
                    break;
                case ARP_UP_DOWN: {
                    /* Already ascending; reverse second half */
                    uint32_t half = n / 2;
                    for (uint32_t i = 0; i < (n - half) / 2; i++) {
                        StructorEvent tmp = inst->events[half + i];
                        inst->events[half + i] = inst->events[n - 1 - i];
                        inst->events[n - 1 - i] = tmp;
                    }
                    break;
                }
                case ARP_DOWN_UP: {
                    qsort(inst->events, n, sizeof(StructorEvent), cmp_density_desc);
                    uint32_t half = n / 2;
                    for (uint32_t i = 0; i < (n - half) / 2; i++) {
                        StructorEvent tmp = inst->events[half + i];
                        inst->events[half + i] = inst->events[n - 1 - i];
                        inst->events[n - 1 - i] = tmp;
                    }
                    break;
                }
                case ARP_RANDOM: {
                    for (uint32_t i = n - 1; i > 0; i--) {
                        uint32_t j = xorshift32(&inst->rng) % (i + 1);
                        StructorEvent tmp = inst->events[i];
                        inst->events[i] = inst->events[j];
                        inst->events[j] = tmp;
                    }
                    break;
                }
                case ARP_CASCADE:
                    qsort(inst->events, n, sizeof(StructorEvent), cmp_density_desc);
                    break;
            }
            break;
        }

        case MODE_DELTARUPT:
            qsort(inst->events, n, sizeof(StructorEvent), cmp_density_asc);
            break;
    }

    /* Build identity playback order (events are now reordered in place) */
    for (uint32_t i = 0; i < n; i++)
        inst->playback_order[i] = i;
}

/* ========================================================================
   Grain-based reconstruction
   ======================================================================== */

static void reconstruct(StructorInstance *inst, uint32_t num_samples) {
    float *out_l = inst->recon_l;
    float *out_r = inst->recon_r;

    memset(out_l, 0, num_samples * sizeof(float));
    memset(out_r, 0, num_samples * sizeof(float));

    if (inst->event_count == 0) return;

    float *buf_l = inst->buffer_l;
    float *buf_r = inst->buffer_r;

    uint32_t grain_size = (uint32_t)(BASE_GRAIN_SIZE * inst->grain_size);
    if (grain_size < 64) grain_size = 64;
    if (grain_size > 4096) grain_size = 4096;

    for (uint32_t out_i = 0; out_i < num_samples; out_i++) {
        uint32_t evt_idx = inst->playback_index % inst->event_count;
        StructorEvent *evt = &inst->events[evt_idx];

        /* Grain start centered on event */
        uint32_t grain_start = evt->sample_index;
        if (grain_start > grain_size / 2)
            grain_start -= grain_size / 2;

        uint32_t src_pos = (grain_start + (uint32_t)inst->playback_position) % BUFFER_SIZE;

        /* Window envelope */
        float window_phase = inst->playback_position / (float)grain_size;
        window_phase = clampf(window_phase, 0.0f, 1.0f);
        float window = 1.0f;

        if (inst->mode == MODE_DELTARUPT) {
            /* Deltarupt: quadratic attack + instant cutoff */
            float attack_dur = inst->deltarupt_attack;
            if (attack_dur < 0.01f) {
                window = (window_phase < 0.05f) ? 1.0f : 0.0f;
            } else if (window_phase < attack_dur) {
                float phase_in_attack = window_phase / attack_dur;
                window = phase_in_attack * phase_in_attack;  /* quadratic */
            } else {
                window = 0.0f;
            }
        } else {
            /* Hann window */
            float w = sinf(window_phase * 3.14159265f);
            window = w * w;
        }

        /* Hermite interpolation */
        uint32_t p0 = (src_pos - 1 + BUFFER_SIZE) % BUFFER_SIZE;
        uint32_t p1 = src_pos;
        uint32_t p2 = (src_pos + 1) % BUFFER_SIZE;
        uint32_t p3 = (src_pos + 2) % BUFFER_SIZE;

        float frac = inst->playback_position - (uint32_t)inst->playback_position;

        out_l[out_i] += window * hermite_interp(
            buf_l[p0], buf_l[p1], buf_l[p2], buf_l[p3], frac) * evt->amplitude;
        out_r[out_i] += window * hermite_interp(
            buf_r[p0], buf_r[p1], buf_r[p2], buf_r[p3], frac) * evt->amplitude;

        /* Playback speed */
        float speed = 1.0f;
        if (inst->mode == MODE_TIME_WARP) {
            speed = lerpf(0.5f, 2.0f, evt->density) * inst->time_warp;
            /* Apply drift variation */
            float drift_mod = (float)(xorshift32(&inst->rng) & 0xFFFF) / 65536.0f;
            speed += (drift_mod - 0.5f) * inst->drift * 0.5f;
        } else if (inst->mode == MODE_DENSITY_ARP) {
            speed = lerpf(0.2f, 1.5f, evt->density) * inst->time_warp;
        }

        inst->playback_position += speed;

        /* Advance to next event when grain exhausted */
        if (inst->playback_position >= (float)grain_size) {
            inst->playback_position = 0.0f;
            inst->playback_index++;
            if (inst->playback_index >= inst->event_count)
                inst->playback_index = 0;
        }
    }
}

/* ========================================================================
   API: create / destroy
   ======================================================================== */

static void* structor_create(const char *module_dir, const char *config_json) {
    StructorInstance *inst = (StructorInstance*)calloc(1, sizeof(StructorInstance));
    if (!inst) return NULL;

    /* Defaults */
    inst->detection = 0.1f;
    inst->density = 0.7f;
    inst->grain_size = 1.0f;
    inst->time_warp = 1.0f;
    inst->mix = 0.5f;
    inst->feedback = 0.2f;
    inst->mode = MODE_RANDOM;

    inst->reshuffle = 0.5f;
    inst->scatter = 0.0f;
    inst->curve = 0.0f;
    inst->drift = 0.3f;
    inst->arp_pattern = ARP_UP;
    inst->deltarupt_attack = 0.05f;

    /* Envelope follower time constants (10 ms attack, 50 ms release at 44.1 kHz) */
    inst->env_attack = 1.0f - expf(-1.0f / (44100.0f * 0.01f));
    inst->env_release = 1.0f - expf(-1.0f / (44100.0f * 0.05f));

    /* PRNG seed */
    inst->rng = 0x12345678;

    if (g_host && g_host->log) g_host->log("[structor] instance created");
    return inst;
}

static void structor_destroy(void *instance) {
    if (instance) free(instance);
}

/* ========================================================================
   API: process_block (in-place stereo int16, 128 frames)
   ======================================================================== */

static void structor_process(void *instance, int16_t *audio_inout, int frames) {
    StructorInstance *inst = (StructorInstance*)instance;
    if (!inst || frames <= 0) return;
    if (frames > BLOCK_SIZE) frames = BLOCK_SIZE;

    /* Convert int16 → float and write to circular buffer with feedback */
    for (int i = 0; i < frames; i++) {
        float l = audio_inout[i * 2]     / 32768.0f;
        float r = audio_inout[i * 2 + 1] / 32768.0f;

        uint32_t pos = inst->buf_write_pos;
        inst->buffer_l[pos] = l + inst->buffer_l[pos] * inst->feedback;
        inst->buffer_r[pos] = r + inst->buffer_r[pos] * inst->feedback;

        inst->buf_write_pos = (pos + 1) % BUFFER_SIZE;
    }

    /* Detect events in recent window */
    uint32_t grain_sz = (uint32_t)(BASE_GRAIN_SIZE * inst->grain_size);
    if (grain_sz < 64) grain_sz = 64;
    uint32_t window_start = (inst->buf_write_pos - grain_sz + BUFFER_SIZE) % BUFFER_SIZE;
    detect_events(inst, window_start, grain_sz);

    /* Build playback order */
    build_playback_order(inst);

    /* Reconstruct */
    reconstruct(inst, frames);

    /* Mix wet/dry and convert back to int16 */
    float wet = inst->mix;
    float dry = 1.0f - wet;

    for (int i = 0; i < frames; i++) {
        float in_l = audio_inout[i * 2]     / 32768.0f;
        float in_r = audio_inout[i * 2 + 1] / 32768.0f;

        float out_l = dry * in_l + wet * inst->recon_l[i];
        float out_r = dry * in_r + wet * inst->recon_r[i];

        /* Soft limiting */
        out_l = tanhf(out_l);
        out_r = tanhf(out_r);

        /* Convert to int16 */
        int32_t il = (int32_t)(out_l * 32767.0f);
        int32_t ir = (int32_t)(out_r * 32767.0f);
        if (il > 32767) il = 32767;
        if (il < -32768) il = -32768;
        if (ir > 32767) ir = 32767;
        if (ir < -32768) ir = -32768;
        audio_inout[i * 2]     = (int16_t)il;
        audio_inout[i * 2 + 1] = (int16_t)ir;
    }

    inst->frame_count += frames;
}

/* ========================================================================
   Mode names and arp pattern names (for get_param display)
   ======================================================================== */

static const char *MODE_NAMES[MAX_MODES] = {
    "Random", "Pitch Up", "Pitch Down", "Density Up",
    "Time Warp", "Density Arp", "Deltarupt"
};

static const char *ARP_NAMES[MAX_ARP] = {
    "Up", "Down", "Up-Down", "Down-Up", "Random", "Cascade"
};

/* Knob 8 label per mode */
static const char *KNOB8_LABELS[MAX_MODES] = {
    "Reshuffle", "Scatter", "Scatter", "Curve",
    "Drift", "Arp Ptrn", "Attack"
};

/* ========================================================================
   Helper: get the current Knob 8 value based on mode
   ======================================================================== */

static float get_knob8_value(StructorInstance *inst) {
    switch (inst->mode) {
        case MODE_RANDOM:      return inst->reshuffle;
        case MODE_PITCH_UP:    return inst->scatter;
        case MODE_PITCH_DOWN:  return inst->scatter;
        case MODE_DENSITY_UP:  return inst->curve;
        case MODE_TIME_WARP:   return inst->drift;
        case MODE_DENSITY_ARP: return (float)inst->arp_pattern;
        case MODE_DELTARUPT:   return inst->deltarupt_attack;
        default:               return 0.0f;
    }
}

static void set_knob8_value(StructorInstance *inst, float val) {
    switch (inst->mode) {
        case MODE_RANDOM:      inst->reshuffle = clampf(val, 0.0f, 1.0f); break;
        case MODE_PITCH_UP:    inst->scatter = clampf(val, 0.0f, 1.0f); break;
        case MODE_PITCH_DOWN:  inst->scatter = clampf(val, 0.0f, 1.0f); break;
        case MODE_DENSITY_UP:  inst->curve = clampf(val, 0.0f, 1.0f); break;
        case MODE_TIME_WARP:   inst->drift = clampf(val, 0.0f, 1.0f); break;
        case MODE_DENSITY_ARP: inst->arp_pattern = clampi((int)val, 0, 5); break;
        case MODE_DELTARUPT:   inst->deltarupt_attack = clampf(val, 0.0f, 1.0f); break;
    }
}

/* ========================================================================
   API: set_param
   ======================================================================== */

static void structor_set_param(void *instance, const char *key, const char *val) {
    StructorInstance *inst = (StructorInstance*)instance;
    if (!inst || !key || !val) return;

    /* Direct parameter keys */
    if (strcmp(key, "detection") == 0) {
        inst->detection = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "density") == 0) {
        inst->density = clampf(atof(val), 0.1f, 1.0f);
    } else if (strcmp(key, "grain_size") == 0) {
        inst->grain_size = clampf(atof(val), 0.5f, 2.0f);
    } else if (strcmp(key, "time_warp") == 0) {
        inst->time_warp = clampf(atof(val), 0.5f, 2.0f);
    } else if (strcmp(key, "mix") == 0) {
        inst->mix = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "feedback") == 0) {
        inst->feedback = clampf(atof(val), 0.0f, 0.95f);
    } else if (strcmp(key, "mode") == 0) {
        inst->mode = clampi(atoi(val), 0, MAX_MODES - 1);
    } else if (strcmp(key, "special") == 0) {
        /* Knob 8 "special" — route to mode-specific param */
        set_knob8_value(inst, atof(val));
    } else if (strcmp(key, "reshuffle") == 0) {
        inst->reshuffle = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "scatter") == 0) {
        inst->scatter = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "curve") == 0) {
        inst->curve = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "drift") == 0) {
        inst->drift = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "arp_pattern") == 0) {
        inst->arp_pattern = clampi(atoi(val), 0, 5);
    } else if (strcmp(key, "deltarupt_attack") == 0) {
        inst->deltarupt_attack = clampf(atof(val), 0.0f, 1.0f);
    }
    /* ---- Knob overlay: knob_N_adjust ---- */
    else if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int knob_num = atoi(key + 5);  /* 1-indexed */
        int delta = atoi(val);

        switch (knob_num) {
            case 1: inst->detection = clampf(inst->detection + delta * 0.01f, 0.0f, 1.0f); break;
            case 2: inst->density = clampf(inst->density + delta * 0.01f, 0.1f, 1.0f); break;
            case 3: inst->grain_size = clampf(inst->grain_size + delta * 0.01f, 0.5f, 2.0f); break;
            case 4: inst->time_warp = clampf(inst->time_warp + delta * 0.01f, 0.5f, 2.0f); break;
            case 5: inst->mix = clampf(inst->mix + delta * 0.01f, 0.0f, 1.0f); break;
            case 6: inst->feedback = clampf(inst->feedback + delta * 0.01f, 0.0f, 0.95f); break;
            case 7: {
                /* Mode: wrap around 0-6 */
                int new_mode = inst->mode + delta;
                if (new_mode > (MAX_MODES - 1)) new_mode = 0;
                if (new_mode < 0) new_mode = MAX_MODES - 1;
                inst->mode = new_mode;
                break;
            }
            case 8: {
                /* Context-sensitive Knob 8 */
                float cur = get_knob8_value(inst);
                if (inst->mode == MODE_DENSITY_ARP) {
                    /* Integer arp pattern */
                    int new_arp = inst->arp_pattern + delta;
                    inst->arp_pattern = clampi(new_arp, 0, 5);
                } else {
                    set_knob8_value(inst, cur + delta * 0.01f);
                }
                break;
            }
        }
    }
    /* State serialization */
    else if (strcmp(key, "state") == 0) {
        int m = 0, ap = 0;
        float det = 0.1f, den = 0.7f, gs = 1.0f, tw = 1.0f;
        float mx = 0.5f, fb = 0.2f;
        float resh = 0.5f, scat = 0.0f, crv = 0.0f, dft = 0.3f, da = 0.05f;
        sscanf(val,
            "{\"detection\":%f,\"density\":%f,\"grain_size\":%f,\"time_warp\":%f,"
            "\"mix\":%f,\"feedback\":%f,\"mode\":%d,"
            "\"reshuffle\":%f,\"scatter\":%f,\"curve\":%f,\"drift\":%f,"
            "\"arp_pattern\":%d,\"deltarupt_attack\":%f}",
            &det, &den, &gs, &tw, &mx, &fb, &m,
            &resh, &scat, &crv, &dft, &ap, &da);
        inst->detection = clampf(det, 0.0f, 1.0f);
        inst->density = clampf(den, 0.1f, 1.0f);
        inst->grain_size = clampf(gs, 0.5f, 2.0f);
        inst->time_warp = clampf(tw, 0.5f, 2.0f);
        inst->mix = clampf(mx, 0.0f, 1.0f);
        inst->feedback = clampf(fb, 0.0f, 0.95f);
        inst->mode = clampi(m, 0, MAX_MODES - 1);
        inst->reshuffle = clampf(resh, 0.0f, 1.0f);
        inst->scatter = clampf(scat, 0.0f, 1.0f);
        inst->curve = clampf(crv, 0.0f, 1.0f);
        inst->drift = clampf(dft, 0.0f, 1.0f);
        inst->arp_pattern = clampi(ap, 0, 5);
        inst->deltarupt_attack = clampf(da, 0.0f, 1.0f);
    }
}

/* ========================================================================
   API: get_param
   ======================================================================== */

static int structor_get_param(void *instance, const char *key, char *buf, int buf_len) {
    StructorInstance *inst = (StructorInstance*)instance;
    if (!inst || !key || !buf || buf_len < 1) return 0;

    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Structor");
    } else if (strcmp(key, "detection") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->detection);
    } else if (strcmp(key, "density") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->density);
    } else if (strcmp(key, "grain_size") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->grain_size);
    } else if (strcmp(key, "time_warp") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->time_warp);
    } else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->mix);
    } else if (strcmp(key, "feedback") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->feedback);
    } else if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%d", inst->mode);
    } else if (strcmp(key, "special") == 0) {
        return snprintf(buf, buf_len, "%.2f", get_knob8_value(inst));
    } else if (strcmp(key, "reshuffle") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->reshuffle);
    } else if (strcmp(key, "scatter") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->scatter);
    } else if (strcmp(key, "curve") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->curve);
    } else if (strcmp(key, "drift") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->drift);
    } else if (strcmp(key, "arp_pattern") == 0) {
        return snprintf(buf, buf_len, "%d", inst->arp_pattern);
    } else if (strcmp(key, "deltarupt_attack") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->deltarupt_attack);
    }

    /* ---- Knob overlay: knob_N_name ---- */
    else if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int knob_num = atoi(key + 5);
        switch (knob_num) {
            case 1: return snprintf(buf, buf_len, "Detection");
            case 2: return snprintf(buf, buf_len, "Density");
            case 3: return snprintf(buf, buf_len, "Grain Size");
            case 4: return snprintf(buf, buf_len, "Time Warp");
            case 5: return snprintf(buf, buf_len, "Mix");
            case 6: return snprintf(buf, buf_len, "Feedback");
            case 7: return snprintf(buf, buf_len, "Mode");
            case 8: return snprintf(buf, buf_len, "%s", KNOB8_LABELS[clampi(inst->mode, 0, MAX_MODES - 1)]);
        }
        return 0;
    }

    /* ---- Knob overlay: knob_N_value ---- */
    else if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int knob_num = atoi(key + 5);
        switch (knob_num) {
            case 1: return snprintf(buf, buf_len, "%d%%", (int)(inst->detection * 100.0f));
            case 2: return snprintf(buf, buf_len, "%d%%", (int)(inst->density * 100.0f));
            case 3: return snprintf(buf, buf_len, "%d%%", (int)(inst->grain_size * 100.0f));
            case 4: return snprintf(buf, buf_len, "%d%%", (int)(inst->time_warp * 100.0f));
            case 5: return snprintf(buf, buf_len, "%d%%", (int)(inst->mix * 100.0f));
            case 6: return snprintf(buf, buf_len, "%d%%", (int)(inst->feedback * 100.0f));
            case 7: return snprintf(buf, buf_len, "%s", MODE_NAMES[clampi(inst->mode, 0, MAX_MODES - 1)]);
            case 8: {
                /* Context-sensitive value display */
                if (inst->mode == MODE_DENSITY_ARP) {
                    return snprintf(buf, buf_len, "%s", ARP_NAMES[clampi(inst->arp_pattern, 0, MAX_ARP - 1)]);
                } else {
                    float v = get_knob8_value(inst);
                    return snprintf(buf, buf_len, "%d%%", (int)(v * 100.0f));
                }
            }
        }
        return 0;
    }

    /* ---- UI hierarchy ---- */
    else if (strcmp(key, "ui_hierarchy") == 0) {
        return snprintf(buf, buf_len,
            "{\"pages\":["
              "{\"name\":\"Structor\",\"params\":["
                "{\"key\":\"detection\",\"name\":\"Detection\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.1,\"step\":0.01},"
                "{\"key\":\"density\",\"name\":\"Density\",\"type\":\"float\",\"min\":0.1,\"max\":1,\"default\":0.7,\"step\":0.01},"
                "{\"key\":\"grain_size\",\"name\":\"Grain Size\",\"type\":\"float\",\"min\":0.5,\"max\":2,\"default\":1,\"step\":0.01},"
                "{\"key\":\"time_warp\",\"name\":\"Time Warp\",\"type\":\"float\",\"min\":0.5,\"max\":2,\"default\":1,\"step\":0.01},"
                "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.5,\"step\":0.01},"
                "{\"key\":\"feedback\",\"name\":\"Feedback\",\"type\":\"float\",\"min\":0,\"max\":0.95,\"default\":0.2,\"step\":0.01},"
                "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"int\",\"min\":0,\"max\":6,\"default\":0,\"step\":1},"
                "{\"key\":\"special\",\"name\":\"Special\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.5,\"step\":0.01}"
              "]}"
            "]}");
    }

    /* ---- State serialization ---- */
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"detection\":%.2f,\"density\":%.2f,\"grain_size\":%.2f,\"time_warp\":%.2f,"
            "\"mix\":%.2f,\"feedback\":%.2f,\"mode\":%d,"
            "\"reshuffle\":%.2f,\"scatter\":%.2f,\"curve\":%.2f,\"drift\":%.2f,"
            "\"arp_pattern\":%d,\"deltarupt_attack\":%.2f}",
            inst->detection, inst->density, inst->grain_size, inst->time_warp,
            inst->mix, inst->feedback, inst->mode,
            inst->reshuffle, inst->scatter, inst->curve, inst->drift,
            inst->arp_pattern, inst->deltarupt_attack);
    }

    return 0;
}

/* ========================================================================
   Static API struct + entry point
   ======================================================================== */

static audio_fx_api_v2_t g_api = {
    .api_version = 2,
    .create_instance = structor_create,
    .destroy_instance = structor_destroy,
    .process_block = structor_process,
    .set_param = structor_set_param,
    .get_param = structor_get_param,
    .on_midi = NULL,
};

__attribute__((visibility("default")))
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    if (host && host->log) host->log("[structor] Structor v0.1.0 loaded");
    return &g_api;
}
