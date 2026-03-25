/*
 * Structor — Musique concrète sound deconstructor/reconstructor
 * Audio FX for Ableton Move via Schwung (audio_fx_api_v2)
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
#include "pffft.h"

#define FFT_SIZE 256

/* ========================================================================
   Schwung API (must match chain_host ABI exactly)
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
#define BASE_GRAIN_SIZE 512
#define MAX_MODES       8
#define MAX_ARP         6
#define MAX_OCTAVE_FOLD 5
#define BLOCK_SIZE      128
#define DETECT_WINDOW   8192    /* ~186 ms detection window */
#define RESCAN_BLOCKS   128     /* re-detect every ~0.37 sec (128 * 128 = 16384 samples) */
#define RECON_GAIN      3.75f   /* makeup gain for reconstruction (compensates Hann window avg + tanhf compression) */

/* Schroeder reverb delay line max lengths (prime-based) */
#define REV_COMB_MAX_0  2048
#define REV_COMB_MAX_1  2048
#define REV_COMB_MAX_2  2048
#define REV_COMB_MAX_3  2048
#define REV_AP_MAX_0    1024
#define REV_AP_MAX_1    1024

/* Schroeder reverb base delay lengths at size=1.0 */
static const uint32_t REV_COMB_BASE[4] = {1557, 1617, 1491, 1422};
static const uint32_t REV_AP_BASE[2]   = {225, 556};

#define NUM_STRUCTOR_PRESETS 20

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
    float sort_key;
    uint8_t type;
} StructorEvent;

typedef enum {
    MODE_RANDOM = 0,
    MODE_PITCH_UP,
    MODE_PITCH_DOWN,
    MODE_DENSITY_UP,
    MODE_TIME_WARP,
    MODE_DENSITY_ARP,
    MODE_DELTARUPT,
    MODE_SPECTRAL_DENSITY
} ReconMode;

typedef enum {
    ARP_UP = 0,
    ARP_DOWN,
    ARP_UP_DOWN,
    ARP_DOWN_UP,
    ARP_RANDOM,
    ARP_CASCADE
} ArpPattern;

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} biquad_t;

/* ========================================================================
   Instance
   ======================================================================== */

typedef struct {
    /* Circular buffer (stereo, heap-allocated to avoid huge struct) */
    float *buffer_l;
    float *buffer_r;
    uint32_t buf_write_pos;

    /* Event detection */
    StructorEvent events[MAX_EVENTS];
    uint32_t event_count;
    int playback_order[MAX_EVENTS];

    /* Grain playback state — 2-slot overlap for click-free transitions */
    uint32_t playback_index;
    float playback_position;
    uint32_t locked_grain_size;   /* grain size locked at grain start, prevents mid-grain jumps */
    /* Crossfade grain (slot B): fading-out previous grain */
    int xf_active;               /* 1 if crossfade grain is rendering */
    float xf_position;           /* playback position in crossfade grain */
    uint32_t xf_grain_start;     /* sample index of crossfade grain's event */
    uint32_t xf_grain_size;      /* locked grain size of crossfade grain */
    float xf_speed;              /* playback speed of crossfade grain */
    float xf_pan;                /* pan of crossfade grain */

    /* Reconstruction output buffers (pre-allocated, no malloc in render) */
    float recon_l[BLOCK_SIZE];
    float recon_r[BLOCK_SIZE];

    /* Envelope follower */
    float env_follower;
    float env_attack;
    float env_release;

    /* Parameters — 7 global */
    float detection;        /* 0.0-1.0, default 0.1 (internal, not on knob) */
    float envelope;         /* 0.0-1.0, default 0.5 (grain shape: 0=rect, 0.5=Hann, 1=narrow) */
    float density;          /* 0.05-4.0, default 0.7 */
    float grain_size;       /* 0.01-10.0, default 1.0 */
    float time_warp;        /* 0.25-8.0, default 1.0 */
    float mix;              /* 0.0-1.0, default 0.5 */
    float feedback;         /* 0.0-0.95, default 0.2 */

    /* Mode */
    int mode;               /* 0-7 */

    /* Current UI page (0=Structor, 1=Randomize) */
    int current_page;

    /* Mode-specific params (Knob 8) */
    float shuffle_bias;          /* mode 0: 0.0-1.0 */
    float pitch_range_window;    /* mode 1: 0.0-1.0 */
    int   octave_fold;           /* mode 2: 0-4 */
    float density_curve;         /* mode 3: 0.0-1.0 */
    float speed_curve_exp;       /* mode 4: 0.5-2.0 */
    int   arp_pattern;           /* mode 5: 0-5 */
    float deltarupt_attack;      /* mode 6: 0.0-1.0 */
    float density_morphing;      /* mode 7: 0.0-1.0 */

    /* Randomize page params */
    float rnd_envelope;     /* 0-1 (0-100% random depth) */
    float rnd_density;      /* 0-1 */
    float rnd_grain;        /* 0-1 */
    float rnd_time;         /* 0-1 */
    float rnd_pan;          /* 0-1 (0=center, 1=full random stereo) */
    int   seq_on;           /* 0=Off, 1=On */
    float seq_time_ms;      /* 10-1000 ms */
    int   seq_mult;         /* 0-8 index into multiplier table */

    /* Sequencer state */
    uint32_t seq_samples_elapsed;  /* samples since last random seed change */
    float seq_env_offset;          /* current random offsets (updated on seq tick) */
    float seq_env_smooth;          /* smoothed envelope offset (avoids clicks) */
    float seq_den_offset;
    float seq_grain_offset;
    float seq_time_offset;         /* quantized octave index: -2,-1,0,1,2 → 0.25,0.5,1,2,4 */
    float seq_pan_offset;
    float seq_filter_offset;
    float current_grain_pan;       /* per-grain random pan: -1 (full L) to +1 (full R) */
    int   rnd_filter;              /* 0=off, 1-100 = % chance of random cutoff change per grain */
    float grain_filter_cutoff;     /* current grain filter cutoff: 0-100 (0-49=LPF, 50=bypass, 51-100=HPF) */

    /* Per-grain Isolator3 filter (3-stage cascade, stereo) */
    biquad_t grain_filt_l[3];
    biquad_t grain_filt_r[3];

    /* Per-grain reverb send flag (set at grain transition) */
    int grain_reverb_send;

    /* DC-blocking filter state (removes low-freq thuds from buffer position jumps) */
    float dc_l_prev_in, dc_l_prev_out;
    float dc_r_prev_in, dc_r_prev_out;

    /* ---- Page 3: Presets ---- */
    int preset;                    /* 0-9 */
    float master_filter;           /* 0-1: <0.49=LP, 0.49-0.51=bypass, >0.51=HP */
    biquad_t master_filt_l[3];     /* Isolator3 master output filter */
    biquad_t master_filt_r[3];
    float master_filt_prev;        /* track changes to avoid recalc */
    float master_filt_smooth;      /* per-sample smoothed filter value */

    /* Random reverb */
    float rnd_reverb;              /* 0-1: chance per grain to send to reverb */
    float rev_mix;                 /* 0-1: reverb wet/dry */
    float rev_size;                /* 0-1: room size */
    float rev_decay;               /* 0-1: feedback */
    float rev_damp;                /* 0-1: damping (LP on comb feedback) */

    /* Schroeder reverb state */
    float *rev_comb_buf[4];        /* 4 comb filter delay lines (heap) */
    uint32_t rev_comb_len[4];      /* current delay lengths */
    uint32_t rev_comb_pos[4];      /* write positions */
    float rev_comb_filt[4];        /* comb LP filter state */
    float *rev_ap_buf[2];          /* 2 allpass delay lines (heap) */
    uint32_t rev_ap_len[2];
    uint32_t rev_ap_pos[2];
    float rev_bus[BLOCK_SIZE];     /* reverb input accumulator per block */
    float rev_out_l[BLOCK_SIZE];   /* reverb stereo output */
    float rev_out_r[BLOCK_SIZE];

    /* Frame counter (for reshuffle timing) */
    uint32_t frame_count;

    /* Detection scheduling */
    uint32_t blocks_since_detect;   /* blocks since last detection pass */
    uint32_t total_samples_written; /* how many samples written to buffer total */

    /* Simple PRNG state (no stdlib rand() in render path) */
    uint32_t rng;

    /* FFT state (pre-allocated, no malloc in render path) */
    PFFFT_Setup *fft_setup;
    float *fft_input;       /* aligned, FFT_SIZE floats */
    float *fft_output;      /* aligned, FFT_SIZE floats */
    float *fft_work;        /* aligned, FFT_SIZE floats — avoid stack alloc in pffft */
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

/* Random float in [-1, +1] */
static inline float rand_bipolar(uint32_t *rng) {
    return ((float)(xorshift32(rng) & 0xFFFF) / 32768.0f) - 1.0f;
}

/* Random float in [0, 1] */
static inline float rand_unipolar(uint32_t *rng) {
    return (float)(xorshift32(rng) & 0xFFFF) / 65536.0f;
}

/* ---- Fast math approximations (per-sample hot path) ---- */

/* Fast tanh: rational approximation, max error ~0.001 in [-3,3].
 * Uses x*(27+x²)/(27+9x²) — Padé [1,2] variant.
 * Perceptually transparent on audio signals. */
static inline float fast_tanhf(float x) {
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Fast sin(x) for x in [0, pi]: parabolic approximation.
 * 4x(pi-x) / (pi²) with one correction term. Max error ~0.001.
 * Used for Hann window where x = phase * pi. */
static inline float fast_sinf_0pi(float x) {
    /* x in [0, pi] */
    float y = 4.0f * x * (3.14159265f - x) / (3.14159265f * 3.14159265f);
    /* Correction for better accuracy: */
    return y * (0.775f + 0.225f * y);
}

/* Fast powf for small integer-ish exponents (1-5 range).
 * Uses exp2(exp * log2(base)) with fast approximations.
 * For the envelope narrowing path where base is in [0,1] and exp in [1,5]. */
static inline float fast_powf_01(float base, float exp) {
    if (base <= 0.0f) return 0.0f;
    if (base >= 1.0f) return 1.0f;
    /* log2 approximation via float bit manipulation */
    union { float f; uint32_t i; } u = { .f = base };
    float log2_base = (float)((int32_t)u.i - 0x3f800000) * 5.9604645e-08f;
    /* exp2 approximation via float bit trick */
    float val = log2_base * exp;
    float result = val * 0.6931472f; /* log2 → ln */
    /* Fast exp approximation: (1 + x/256)^256 via repeated squaring */
    result = 1.0f + result / 256.0f;
    result *= result; result *= result; result *= result; result *= result;
    result *= result; result *= result; result *= result; result *= result;
    return clampf(result, 0.0f, 1.0f);
}

/* ---- Biquad filter (Isolator3-inspired, from Octocosme) ---- */

#define SAMPLE_RATE 44100.0f
#define TWO_PI_F    6.2831853f
#define DENORM_F    1e-20f

/* ---- Schroeder Reverb Engine ---- */

static void reverb_init(StructorInstance *inst) {
    /* Allocate delay lines */
    for (int i=0;i<4;i++) {
        uint32_t sz = (i==0)?REV_COMB_MAX_0:(i==1)?REV_COMB_MAX_1:(i==2)?REV_COMB_MAX_2:REV_COMB_MAX_3;
        inst->rev_comb_buf[i] = (float*)calloc(sz, sizeof(float));
        inst->rev_comb_pos[i] = 0;
        inst->rev_comb_filt[i] = 0.0f;
        inst->rev_comb_len[i] = REV_COMB_BASE[i];
    }
    for (int i=0;i<2;i++) {
        uint32_t sz = (i==0)?REV_AP_MAX_0:REV_AP_MAX_1;
        inst->rev_ap_buf[i] = (float*)calloc(sz, sizeof(float));
        inst->rev_ap_pos[i] = 0;
        inst->rev_ap_len[i] = REV_AP_BASE[i];
    }
}

static void reverb_free(StructorInstance *inst) {
    for (int i=0;i<4;i++) { free(inst->rev_comb_buf[i]); inst->rev_comb_buf[i]=NULL; }
    for (int i=0;i<2;i++) { free(inst->rev_ap_buf[i]); inst->rev_ap_buf[i]=NULL; }
}

static void reverb_update_params(StructorInstance *inst) {
    /* Scale delay lengths by size (0.3x at size=0, 1.0x at size=1) */
    float scale = 0.3f + inst->rev_size * 0.7f;
    for (int i=0;i<4;i++) {
        uint32_t len = (uint32_t)(REV_COMB_BASE[i] * scale);
        uint32_t maxlen = (i==0)?REV_COMB_MAX_0:(i==1)?REV_COMB_MAX_1:(i==2)?REV_COMB_MAX_2:REV_COMB_MAX_3;
        if (len < 16) len = 16;
        if (len > maxlen) len = maxlen;
        inst->rev_comb_len[i] = len;
    }
    for (int i=0;i<2;i++) {
        uint32_t len = (uint32_t)(REV_AP_BASE[i] * scale);
        uint32_t maxlen = (i==0)?REV_AP_MAX_0:REV_AP_MAX_1;
        if (len < 8) len = 8;
        if (len > maxlen) len = maxlen;
        inst->rev_ap_len[i] = len;
    }
}

static void reverb_process(StructorInstance *inst, int frames) {
    /* Feedback gain: 0.5 (tight ambiance) to 0.88 (long space).
     * Kept below 0.9 to prevent instability with 4 parallel combs. */
    float fb = 0.50f + inst->rev_decay * 0.38f;
    /* Damping LPF coefficient: 0=bright (no damping), 1=very dark */
    float damp = inst->rev_damp;
    float damp1 = 1.0f - damp;
    /* Input attenuation: prevent overdriving the comb network */
    float in_gain = 0.3f;

    memset(inst->rev_out_l, 0, frames * sizeof(float));
    memset(inst->rev_out_r, 0, frames * sizeof(float));

    for (int i = 0; i < frames; i++) {
        float in = inst->rev_bus[i] * in_gain;
        float comb_sum = 0.0f;

        /* 4 parallel comb filters with LP-filtered feedback + soft clip */
        for (int c = 0; c < 4; c++) {
            float *buf = inst->rev_comb_buf[c];
            uint32_t len = inst->rev_comb_len[c];
            uint32_t pos = inst->rev_comb_pos[c];
            uint32_t rd = (pos >= len) ? (pos - len) : (pos + REV_COMB_MAX_0 - len);
            rd %= REV_COMB_MAX_0;

            float delayed = buf[rd];
            /* One-pole LP in feedback for damping */
            inst->rev_comb_filt[c] = delayed * damp1 + inst->rev_comb_filt[c] * damp;
            /* Soft clip inside feedback loop to prevent runaway */
            float fb_sig = inst->rev_comb_filt[c] * fb;
            if (fb_sig > 1.0f) fb_sig = 1.0f;
            else if (fb_sig < -1.0f) fb_sig = -1.0f;
            buf[pos] = in + fb_sig;

            inst->rev_comb_pos[c] = (pos + 1) % REV_COMB_MAX_0;
            comb_sum += delayed;
        }
        comb_sum *= 0.25f; /* normalize */

        /* 2 series allpass filters for diffusion */
        float ap = comb_sum;
        for (int a = 0; a < 2; a++) {
            float *buf = inst->rev_ap_buf[a];
            uint32_t len = inst->rev_ap_len[a];
            uint32_t pos = inst->rev_ap_pos[a];
            uint32_t maxlen = (a==0)?REV_AP_MAX_0:REV_AP_MAX_1;
            uint32_t rd = (pos >= len) ? (pos - len) : (pos + maxlen - len);
            rd %= maxlen;

            float delayed = buf[rd];
            float g = 0.5f;
            buf[pos] = ap + delayed * g;
            ap = delayed - ap * g;
            inst->rev_ap_pos[a] = (pos + 1) % maxlen;
        }

        /* Stereo decorrelation: slight phase offset between L/R */
        inst->rev_out_l[i] = ap;
        inst->rev_out_r[i] = (i > 0) ? inst->rev_out_l[i-1] : ap;
    }

    /* Clear reverb bus for next block */
    memset(inst->rev_bus, 0, frames * sizeof(float));
}

/* ---- Preset Table ---- */
typedef struct {
    float envelope, density, grain_size, time_warp, mix, feedback;
    int mode;
    /* Randomize page */
    float rnd_envelope, rnd_density, rnd_grain, rnd_time, rnd_pan;
    int rnd_filter;
    /* Presets page */
    float master_filter, rnd_reverb, rev_mix, rev_size, rev_decay, rev_damp;
} structor_preset_t;

/*                       env   den   grain tw    mix   fb    mode re    rd    rg    rt    rp   rf   mfilt rrv   rmix  rsz   rdcy  rdmp */
static const structor_preset_t STRUCTOR_PRESETS[NUM_STRUCTOR_PRESETS] = {
    {0.50f,5.00f,5.00f,1.00f,0.50f,0.20f, 0, 0.0f,0.0f,0.0f,0.0f,0.25f, 0, 0.50f,0.00f,0.00f,0.50f,0.50f,0.50f}, /* 0 Init */
    {0.40f,0.50f,0.60f,1.50f,0.60f,0.30f, 0, 0.3f,0.4f,0.3f,0.2f,0.50f,30, 0.50f,0.40f,0.30f,0.40f,0.60f,0.40f}, /* 1 Scatter */
    {0.55f,0.80f,1.20f,0.50f,0.55f,0.15f, 1, 0.2f,0.3f,0.2f,0.1f,0.30f, 0, 0.50f,0.20f,0.25f,0.60f,0.70f,0.50f}, /* 2 Ascend */
    {0.55f,0.80f,1.20f,0.60f,0.55f,0.15f, 2, 0.2f,0.3f,0.2f,0.1f,0.30f, 0, 0.50f,0.20f,0.25f,0.60f,0.70f,0.50f}, /* 3 Descend */
    {0.30f,0.60f,0.40f,1.00f,0.65f,0.25f, 5, 0.4f,0.5f,0.3f,0.5f,0.40f,20, 0.50f,0.30f,0.20f,0.30f,0.40f,0.30f}, /* 4 Pulse */
    {0.15f,0.90f,0.30f,1.50f,0.70f,0.40f, 6, 0.5f,0.6f,0.4f,0.3f,0.20f,50, 0.35f,0.00f,0.00f,0.20f,0.30f,0.60f}, /* 5 Tape Cut */
    {0.60f,0.40f,2.00f,0.70f,0.50f,0.10f, 7, 0.3f,0.2f,0.3f,0.2f,0.50f,40, 0.50f,0.60f,0.50f,0.80f,0.80f,0.60f}, /* 6 Spectral */
    {0.70f,0.30f,3.00f,0.40f,0.45f,0.35f, 0, 0.6f,0.4f,0.5f,0.4f,0.60f, 0, 0.50f,0.50f,0.60f,0.90f,0.90f,0.70f}, /* 7 Ambient */
    {0.25f,0.95f,0.50f,2.00f,0.75f,0.50f, 0, 0.8f,0.7f,0.8f,0.7f,0.80f,60, 0.50f,0.70f,0.40f,0.50f,0.50f,0.30f}, /* 8 Chaos */
    {0.65f,0.35f,2.50f,0.50f,0.55f,0.20f, 7, 0.4f,0.3f,0.4f,0.3f,0.40f, 0, 0.60f,0.80f,0.70f,1.00f,0.95f,0.85f}, /* 9 Dark Hall */
    {0.80f,0.20f,8.00f,0.30f,0.40f,0.50f, 0, 0.5f,0.3f,0.5f,0.3f,0.30f, 0, 0.50f,0.30f,0.40f,0.70f,0.80f,0.80f}, /* 10 Frozen */
    {0.20f,1.50f,0.20f,3.00f,0.70f,0.10f, 4, 0.7f,0.8f,0.6f,0.8f,0.60f,70, 0.50f,0.10f,0.10f,0.20f,0.30f,0.20f}, /* 11 Glitch */
    {0.90f,0.15f,10.0f,0.25f,0.35f,0.60f, 7, 0.3f,0.2f,0.3f,0.2f,0.20f, 0, 0.50f,0.70f,0.50f,1.00f,0.85f,0.90f}, /* 12 Drone */
    {0.35f,0.80f,0.50f,1.00f,0.60f,0.30f, 5, 0.5f,0.6f,0.4f,0.6f,0.50f,30, 0.50f,0.50f,0.35f,0.50f,0.60f,0.40f}, /* 13 Sequenced */
    {0.10f,2.00f,0.15f,4.00f,0.80f,0.00f, 6, 0.6f,0.7f,0.5f,0.4f,0.30f,80, 0.50f,0.00f,0.00f,0.30f,0.40f,0.50f}, /* 14 Shatter */
    {0.60f,0.40f,3.00f,0.50f,0.45f,0.40f, 1, 0.3f,0.3f,0.4f,0.2f,0.40f,20, 0.50f,0.40f,0.45f,0.80f,0.75f,0.65f}, /* 15 Rise */
    {0.45f,0.60f,1.50f,1.20f,0.55f,0.20f, 3, 0.4f,0.5f,0.3f,0.3f,0.50f,40, 0.50f,0.60f,0.40f,0.60f,0.55f,0.45f}, /* 16 Thick */
    {0.70f,0.25f,6.00f,0.35f,0.40f,0.70f, 0, 0.5f,0.4f,0.6f,0.3f,0.40f, 0, 0.40f,0.20f,0.30f,0.40f,0.50f,0.70f}, /* 17 Lo-Fi */
    {0.50f,0.50f,2.00f,1.50f,0.60f,0.15f, 2, 0.3f,0.4f,0.3f,0.4f,0.50f, 0, 0.50f,0.80f,0.60f,0.90f,0.80f,0.50f}, /* 18 Cathedral */
    {0.30f,1.00f,0.80f,2.00f,0.75f,0.35f, 4, 0.6f,0.5f,0.5f,0.5f,0.60f,50, 0.50f,0.40f,0.25f,0.35f,0.45f,0.35f}, /* 19 Warp */
};
static const char *STRUCTOR_PRESET_NAMES[NUM_STRUCTOR_PRESETS] = {
    "Init","Scatter","Ascend","Descend","Pulse",
    "Tape Cut","Spectral","Ambient","Chaos","Dark Hall",
    "Frozen","Glitch","Drone","Sequenced","Shatter",
    "Rise","Thick","Lo-Fi","Cathedral","Warp"
};

static void apply_structor_preset(void *instance, int idx) {
    StructorInstance *inst = (StructorInstance*)instance;
    if (idx < 0 || idx >= NUM_STRUCTOR_PRESETS) return;
    const structor_preset_t *p = &STRUCTOR_PRESETS[idx];
    inst->envelope = p->envelope; inst->density = p->density;
    inst->grain_size = p->grain_size; inst->time_warp = p->time_warp;
    inst->mix = p->mix; inst->feedback = p->feedback;
    inst->mode = p->mode;
    /* Randomize page */
    inst->rnd_envelope = p->rnd_envelope; inst->rnd_density = p->rnd_density;
    inst->rnd_grain = p->rnd_grain; inst->rnd_time = p->rnd_time;
    inst->rnd_pan = p->rnd_pan; inst->rnd_filter = p->rnd_filter;
    /* Presets page */
    inst->master_filter = p->master_filter;
    inst->rnd_reverb = p->rnd_reverb; inst->rev_mix = p->rev_mix;
    inst->rev_size = p->rev_size; inst->rev_decay = p->rev_decay;
    inst->rev_damp = p->rev_damp;
    inst->preset = idx;
}

static void randomize_preset(StructorInstance *inst) {
    /* Structor page */
    inst->envelope = 0.1f + rand_unipolar(&inst->rng) * 0.8f;
    inst->density = 0.2f + rand_unipolar(&inst->rng) * 0.8f;
    inst->grain_size = 0.3f + rand_unipolar(&inst->rng) * 4.0f;
    inst->time_warp = 0.25f + rand_unipolar(&inst->rng) * 3.75f;
    inst->mix = 0.3f + rand_unipolar(&inst->rng) * 0.5f;
    inst->feedback = rand_unipolar(&inst->rng) * 0.6f;
    inst->mode = (int)(rand_unipolar(&inst->rng) * MAX_MODES) % MAX_MODES;
    /* Randomize page — this is where the magic happens */
    inst->rnd_envelope = rand_unipolar(&inst->rng) * 0.7f;
    inst->rnd_density = rand_unipolar(&inst->rng) * 0.6f;
    inst->rnd_grain = rand_unipolar(&inst->rng) * 0.7f;
    inst->rnd_time = rand_unipolar(&inst->rng) * 0.6f;
    inst->rnd_pan = rand_unipolar(&inst->rng) * 0.8f;
    inst->rnd_filter = (int)(rand_unipolar(&inst->rng) * 60.0f);
    /* Presets page */
    inst->master_filter = 0.5f; /* always bypass — avoids gain/distortion bugs */
    inst->rnd_reverb = rand_unipolar(&inst->rng) * 0.8f;
    inst->rev_mix = rand_unipolar(&inst->rng) * 0.7f;
    inst->rev_size = rand_unipolar(&inst->rng);
    inst->rev_decay = 0.2f + rand_unipolar(&inst->rng) * 0.7f;
    inst->rev_damp = rand_unipolar(&inst->rng) * 0.8f;
}

static void biquad_lpf(biquad_t *f, float freq, float q) {
    float w0 = TWO_PI_F * clampf(freq, 20.0f, SAMPLE_RATE * 0.49f) / SAMPLE_RATE;
    float alpha = sinf(w0) / (2.0f * clampf(q, 0.1f, 20.0f));
    float cosw = cosf(w0);
    float a0_inv = 1.0f / (1.0f + alpha);
    f->b0 = (1.0f - cosw) * 0.5f * a0_inv;
    f->b1 = (1.0f - cosw) * a0_inv;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw * a0_inv;
    f->a2 = (1.0f - alpha) * a0_inv;
}

static void biquad_hpf(biquad_t *f, float freq, float q) {
    float w0 = TWO_PI_F * clampf(freq, 20.0f, SAMPLE_RATE * 0.49f) / SAMPLE_RATE;
    float alpha = sinf(w0) / (2.0f * clampf(q, 0.1f, 20.0f));
    float cosw = cosf(w0);
    float a0_inv = 1.0f / (1.0f + alpha);
    f->b0 = (1.0f + cosw) * 0.5f * a0_inv;
    f->b1 = -(1.0f + cosw) * a0_inv;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw * a0_inv;
    f->a2 = (1.0f - alpha) * a0_inv;
}

static inline float biquad_process(biquad_t *f, float in) {
    float out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2
              - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = in;
    f->y2 = f->y1; f->y1 = out;
    return out + DENORM_F;
}

static inline void biquad_reset(biquad_t *f) {
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
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
   Spectral estimation (FFT-based via pffft)
   ======================================================================== */

static float estimate_spectral_center(StructorInstance *inst,
                                       uint32_t window_start,
                                       uint32_t window_size) {
    if (!inst->fft_setup || !inst->fft_input || !inst->fft_output || !inst->fft_work)
        return 1000.0f;  /* fallback */

    float *buf = inst->buffer_l;
    float *in = inst->fft_input;
    float *out = inst->fft_output;

    /* Copy samples into aligned FFT input buffer (use FFT_SIZE, not window_size) */
    uint32_t n = (window_size < FFT_SIZE) ? window_size : FFT_SIZE;
    for (uint32_t i = 0; i < n; i++)
        in[i] = buf[(window_start + i) % BUFFER_SIZE];
    /* Zero-pad if window is shorter than FFT_SIZE */
    for (uint32_t i = n; i < FFT_SIZE; i++)
        in[i] = 0.0f;

    /* Forward FFT (ordered output: interleaved real/imag) */
    /* Use pre-allocated work buffer — never use stack (audio thread has limited stack) */
    pffft_transform_ordered(inst->fft_setup, in, out, inst->fft_work, PFFFT_FORWARD);

    /* Find peak magnitude bin (skip DC at bin 0)
       Output layout for real FFT ordered:
         out[0] = DC (real), out[1] = Nyquist (real)
         out[2k], out[2k+1] = Re(bin k), Im(bin k)  for k = 1..N/2-1 */
    float max_mag = 0.0f;
    int peak_bin = 1;
    for (int k = 1; k < FFT_SIZE / 2; k++) {
        float re = out[2 * k];
        float im = out[2 * k + 1];
        float mag = re * re + im * im;  /* skip sqrt — just comparing */
        if (mag > max_mag) {
            max_mag = mag;
            peak_bin = k;
        }
    }

    /* Parabolic interpolation for sub-bin accuracy */
    float true_bin = (float)peak_bin;
    if (peak_bin > 1 && peak_bin < FFT_SIZE / 2 - 1) {
        float re_m = out[2 * (peak_bin - 1)];
        float im_m = out[2 * (peak_bin - 1) + 1];
        float re_p = out[2 * (peak_bin + 1)];
        float im_p = out[2 * (peak_bin + 1) + 1];
        float alpha = re_m * re_m + im_m * im_m;
        float beta  = max_mag;
        float gamma = re_p * re_p + im_p * im_p;
        float denom = alpha - 2.0f * beta + gamma;
        if (fabsf(denom) > 1e-10f) {
            float delta = 0.5f * (alpha - gamma) / denom;
            true_bin += clampf(delta, -0.5f, 0.5f);
        }
    }

    /* Convert bin to frequency: freq = bin * sample_rate / fft_size */
    float freq = true_bin * 44100.0f / (float)FFT_SIZE;
    return clampf(freq, 20.0f, 20000.0f);
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

        /* Onset: magnitude above threshold & envelope
           detection param 0.0 = very sensitive, 1.0 = only loud events */
        float thresh = inst->detection * 0.5f + 0.005f;  /* range ~0.005 to ~0.505 */
        if (mag > thresh && mag > inst->env_follower * 0.5f) {
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
            evt->amplitude = clampf(mag * 0.5f, 0.0f, 1.0f);  /* normalize: mag = |L|+|R|, max ~2.0 */
            evt->frequency_center = estimate_spectral_center(inst, pos, BASE_GRAIN_SIZE);
            evt->density = evt->amplitude;
            evt->sort_key = 0.0f;
            evt->type = is_onset ? EVENT_ONSET :
                       (is_peak ? EVENT_PEAK : EVENT_ZERO_CROSSING);

            last_event_pos = pos;
        }
    }

    /* Density culling: keep top N by amplitude (with random offset, max -50% down)
     * density 0.05-1.0: cull events (keep top N%)
     * density >1.0: keep all events (no culling) — higher density affects grain overlap later */
    if (inst->event_count > 0) {
        float den_offset = inst->seq_den_offset;
        if (den_offset < -0.5f) den_offset = -0.5f;
        float eff_density = inst->density * (1.0f + den_offset);
        if (eff_density < 0.05f) eff_density = 0.05f;
        float cull_density = clampf(eff_density, 0.05f, 1.0f);
        uint32_t target = (uint32_t)(inst->event_count * cull_density);
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
   Comparators for qsort
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

static int cmp_sort_key_asc(const void *a, const void *b) {
    float ka = ((const StructorEvent*)a)->sort_key;
    float kb = ((const StructorEvent*)b)->sort_key;
    return (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
}

static int cmp_sort_key_desc(const void *a, const void *b) {
    float ka = ((const StructorEvent*)a)->sort_key;
    float kb = ((const StructorEvent*)b)->sort_key;
    return (ka > kb) ? -1 : (ka < kb) ? 1 : 0;
}

/* ========================================================================
   Build playback order (mode-dependent)
   ======================================================================== */

static void build_playback_order(StructorInstance *inst) {
    uint32_t n = inst->event_count;
    if (n == 0) return;

    switch (inst->mode) {

        /* ---- Mode 0: Random Remix ---- */
        /* Shuffle Bias: 0.0 = pure random, 1.0 = constrained (nearby swaps) */
        case MODE_RANDOM: {
            float bias = inst->shuffle_bias;
            for (uint32_t i = n - 1; i > 0; i--) {
                uint32_t j;
                if (bias < 0.01f) {
                    /* Pure Fisher-Yates */
                    j = xorshift32(&inst->rng) % (i + 1);
                } else {
                    /* Constrained: max swap distance shrinks with bias */
                    int max_dist = (int)((1.0f - bias) * (float)i) + 1;
                    int offset = (int)(xorshift32(&inst->rng) % (uint32_t)(max_dist * 2 + 1)) - max_dist;
                    j = (uint32_t)clampi((int)i + offset, 0, (int)i);
                }
                StructorEvent tmp = inst->events[i];
                inst->events[i] = inst->events[j];
                inst->events[j] = tmp;
            }
            break;
        }

        /* ---- Mode 1: Pitch Sort Ascending ---- */
        /* Pitch Range Window: 0.0 = strict sort, 1.0 = coarse quantization */
        case MODE_PITCH_UP: {
            float window = inst->pitch_range_window;
            if (window < 0.01f) {
                qsort(inst->events, n, sizeof(StructorEvent), cmp_freq_asc);
            } else {
                /* Quantize frequencies into bins, then sort */
                float num_bins = 100.0f * (1.0f - window) + 2.0f;
                float bin_width = 7800.0f / num_bins;  /* 200-8000 Hz range */
                for (uint32_t i = 0; i < n; i++) {
                    float freq = inst->events[i].frequency_center;
                    float quantized = floorf((freq - 200.0f) / bin_width) * bin_width + 200.0f;
                    /* Jitter within bin for variation */
                    float jitter = (float)(xorshift32(&inst->rng) & 0xFFFF) / 65536.0f * bin_width;
                    inst->events[i].sort_key = quantized + jitter;
                }
                qsort(inst->events, n, sizeof(StructorEvent), cmp_sort_key_asc);
            }
            break;
        }

        /* ---- Mode 2: Pitch Sort Descending ---- */
        /* Octave Fold: 0=None, 1=Fold octave, 2=Mirror, 3=Harmonic, 4=Inharmonic */
        case MODE_PITCH_DOWN: {
            int fold = inst->octave_fold;
            float ref_freq = 440.0f;
            if (fold == 0) {
                qsort(inst->events, n, sizeof(StructorEvent), cmp_freq_desc);
            } else {
                for (uint32_t i = 0; i < n; i++) {
                    float freq = inst->events[i].frequency_center;
                    if (freq < 1.0f) freq = 1.0f;  /* guard against log of 0 */
                    switch (fold) {
                        case 1: {
                            /* Fold to single octave: sort by pitch class */
                            float octave = logf(freq / ref_freq) * 1.4426950f;
                            inst->events[i].sort_key = (octave - floorf(octave)) * 12.0f;
                            break;
                        }
                        case 2: {
                            /* Fold + reflect: mirror around octave boundaries */
                            float octave = logf(freq / ref_freq) * 1.4426950f;
                            inst->events[i].sort_key = fabsf(octave - roundf(octave));
                            break;
                        }
                        case 3: {
                            /* Harmonic collapse: fold down to fundamental */
                            float fund = freq;
                            while (fund > ref_freq && fund > 1.0f) fund *= 0.5f;
                            inst->events[i].sort_key = fund;
                            break;
                        }
                        case 4: {
                            /* Inharmonic spread: golden ratio wrapping */
                            float ratio = freq / ref_freq;
                            inst->events[i].sort_key = fmodf(ratio * 1.6180339f, 1.0f);
                            break;
                        }
                    }
                }
                qsort(inst->events, n, sizeof(StructorEvent), cmp_sort_key_desc);
            }
            break;
        }

        /* ---- Mode 3: Density Ascending ---- */
        /* Density Curve: 0.0 = linear, 0.5 = sigmoid, 1.0 = exponential */
        case MODE_DENSITY_UP: {
            qsort(inst->events, n, sizeof(StructorEvent), cmp_density_asc);
            float crv = inst->density_curve;
            if (crv > 0.01f) {
                for (uint32_t i = 0; i < n; i++) {
                    float pos = (n > 1) ? (float)i / (float)(n - 1) : 0.5f;
                    float scale;
                    if (crv < 0.33f) {
                        /* Linear-ish: slight curve */
                        float t = crv / 0.33f;  /* 0-1 within linear zone */
                        scale = lerpf(pos, pos, t);  /* identity */
                    } else if (crv < 0.66f) {
                        /* Sigmoid: S-curve */
                        scale = 1.0f / (1.0f + expf(-10.0f * (pos - 0.5f)));
                    } else {
                        /* Exponential: slow start, rapid end */
                        float t = (crv - 0.66f) / 0.34f;
                        float exp_val = pos * pos;
                        scale = lerpf(pos, exp_val, t);
                    }
                    inst->events[i].amplitude *= (0.2f + 0.8f * scale);
                }
            }
            break;
        }

        /* ---- Mode 4: Time Warp ---- */
        /* Speed Curve Exponent applied in reconstruct, not in sort */
        case MODE_TIME_WARP:
            /* Keep original order — speed varies in reconstruct */
            break;

        /* ---- Mode 5: Density Arpeggiator ---- */
        case MODE_DENSITY_ARP: {
            qsort(inst->events, n, sizeof(StructorEvent), cmp_density_asc);
            switch (inst->arp_pattern) {
                case ARP_UP:
                    break;
                case ARP_DOWN:
                    qsort(inst->events, n, sizeof(StructorEvent), cmp_density_desc);
                    break;
                case ARP_UP_DOWN: {
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

        /* ---- Mode 6: Deltarupt ---- */
        case MODE_DELTARUPT:
            qsort(inst->events, n, sizeof(StructorEvent), cmp_density_asc);
            break;

        /* ---- Mode 7: Spectral Density Warp ---- */
        /* Density Morphing: 0.0 = pure pitch sort, 1.0 = pure density sort */
        case MODE_SPECTRAL_DENSITY: {
            float morph = inst->density_morphing;
            float min_freq = 8000.0f, max_freq = 200.0f;
            float min_amp = 1.0f, max_amp = 0.0f;

            /* Find min/max for normalization */
            for (uint32_t i = 0; i < n; i++) {
                float f = inst->events[i].frequency_center;
                float a = inst->events[i].amplitude;
                if (f < min_freq) min_freq = f;
                if (f > max_freq) max_freq = f;
                if (a < min_amp) min_amp = a;
                if (a > max_amp) max_amp = a;
            }

            float freq_range = max_freq - min_freq;
            float amp_range = max_amp - min_amp;
            if (freq_range < 1.0f) freq_range = 1.0f;
            if (amp_range < 0.001f) amp_range = 0.001f;

            /* Compute blended sort key */
            for (uint32_t i = 0; i < n; i++) {
                float freq_norm = (inst->events[i].frequency_center - min_freq) / freq_range;
                float amp_norm = (inst->events[i].amplitude - min_amp) / amp_range;
                inst->events[i].sort_key = lerpf(freq_norm, amp_norm, morph);
            }

            qsort(inst->events, n, sizeof(StructorEvent), cmp_sort_key_asc);
            break;
        }
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

    /* Apply random offsets to grain parameters.
       Asymmetric clamping prevents glitchy artifacts:
       - Grain: max -30% down (prevents near-zero grains)
       - Envelope: clamp min 0.1 (prevents hard rectangle = clicks)
       - Density/Time: max -50% down */
    float grain_offset = inst->seq_grain_offset;
    if (grain_offset < -0.3f) grain_offset = -0.3f;
    float eff_grain = inst->grain_size * (1.0f + grain_offset);

    /* Smooth the envelope offset to prevent clicks from abrupt window shape changes.
       ~50ms slew (coeff 0.002 at 44.1kHz/128 = ~344 blocks/sec → tau ≈ 1.4s at block rate,
       but applied per-sample inside the block so tau ≈ 50ms) */
    float env_target = inst->seq_env_offset;
    inst->seq_env_smooth += 0.05f * (env_target - inst->seq_env_smooth);
    float env_offset = inst->seq_env_smooth;
    if (env_offset < -0.5f) env_offset = -0.5f;
    float eff_envelope = clampf(inst->envelope + env_offset * 0.5f, 0.15f, 0.95f);

    /* Rnd Time: seq_time_offset is now an absolute octave multiplier (0.25, 0.5, 1, 2, 4) */
    float time_octave = inst->seq_time_offset;
    if (time_octave < 0.1f) time_octave = 1.0f;  /* safety: default to 1x if unset */
    float eff_time_warp = inst->time_warp * time_octave;
    if (eff_grain < 0.1f) eff_grain = 0.1f;
    if (eff_time_warp < 0.1f) eff_time_warp = 0.1f;

    /* Density > 1.0: scale up grain size for more overlap (denser texture) */
    float density_scale = inst->density > 1.0f ? inst->density : 1.0f;
    uint32_t new_grain_size = (uint32_t)(BASE_GRAIN_SIZE * eff_grain * density_scale);
    if (new_grain_size < 4) new_grain_size = 4;
    if (new_grain_size > 65536) new_grain_size = 65536;

    /* Lock grain_size at grain start — changing mid-grain causes window phase jumps = clicks */
    if (inst->locked_grain_size == 0 || inst->playback_position < 1.0f)
        inst->locked_grain_size = new_grain_size;
    uint32_t grain_size = inst->locked_grain_size;

    /* Crossfade length: 128 samples (~3ms) or grain_size/3, whichever is smaller.
     * Longer crossfade masks transient attacks from drums/clicky sources. */
    uint32_t xf_len = (grain_size < 384) ? grain_size / 3 : 128;
    if (xf_len < 8) xf_len = 8;

    for (uint32_t out_i = 0; out_i < num_samples; out_i++) {

        /* ---- Render crossfade grain (slot B) if active ---- */
        if (inst->xf_active) {
            uint32_t xf_src = (inst->xf_grain_start + (uint32_t)inst->xf_position) % BUFFER_SIZE;
            /* Pure fade-out ramp only — no Hann window. The Hann is near zero at
             * the grain boundary where the crossfade starts, so applying it would
             * make the crossfade grain silent (defeating the purpose). */
            uint32_t xf_remaining = inst->xf_grain_size - (uint32_t)inst->xf_position;
            float t = clampf((float)xf_remaining / (float)xf_len, 0.0f, 1.0f);
            float xf_win = fast_sinf_0pi(t * 1.5707963f); /* equal-power cos fade-out */

            float xf_frac = inst->xf_position - (uint32_t)inst->xf_position;
            uint32_t xp0=(xf_src-1+BUFFER_SIZE)%BUFFER_SIZE, xp1=xf_src;
            uint32_t xp2=(xf_src+1)%BUFFER_SIZE, xp3=(xf_src+2)%BUFFER_SIZE;
            float xs_l = xf_win * hermite_interp(buf_l[xp0],buf_l[xp1],buf_l[xp2],buf_l[xp3],xf_frac);
            float xs_r = xf_win * hermite_interp(buf_r[xp0],buf_r[xp1],buf_r[xp2],buf_r[xp3],xf_frac);
            float xpan_l = 1.0f - clampf(inst->xf_pan, 0.0f, 1.0f);
            float xpan_r = 1.0f + clampf(inst->xf_pan, -1.0f, 0.0f);
            out_l[out_i] += xs_l * xpan_l;
            out_r[out_i] += xs_r * xpan_r;

            inst->xf_position += inst->xf_speed;
            if (inst->xf_position >= (float)inst->xf_grain_size)
                inst->xf_active = 0;
        }

        /* ---- Render main grain (slot A) ---- */
        uint32_t evt_idx = inst->playback_index % inst->event_count;
        StructorEvent *evt = &inst->events[evt_idx];

        uint32_t grain_start = evt->sample_index;
        if (grain_start > grain_size / 2)
            grain_start -= grain_size / 2;

        uint32_t src_pos = (grain_start + (uint32_t)inst->playback_position) % BUFFER_SIZE;

        /* Window envelope */
        float window_phase = inst->playback_position / (float)grain_size;
        window_phase = clampf(window_phase, 0.0f, 1.0f);
        float window = 1.0f;

        if (inst->mode == MODE_DELTARUPT) {
            float attack_dur = inst->deltarupt_attack;
            if (attack_dur < 0.01f) {
                window = 1.0f;
            } else if (window_phase < attack_dur) {
                float phase_in_attack = window_phase / attack_dur;
                window = phase_in_attack * phase_in_attack;
            } else {
                window = 1.0f;
            }
        } else {
            float env = eff_envelope;
            float hann_w = fast_sinf_0pi(window_phase * 3.14159265f);
            float hann = hann_w * hann_w;

            if (env <= 0.5f) {
                float t = env * 2.0f;
                window = (1.0f - t) * 1.0f + t * hann;
            } else {
                float t = (env - 0.5f) * 2.0f;
                float narrow_exp = 1.0f + t * 4.0f;
                float narrow = fast_powf_01(hann, narrow_exp);
                window = (1.0f - t) * hann + t * narrow;
            }
        }

        /* Equal-power fade-in at grain start (complements crossfade fade-out) */
        if (inst->mode != MODE_DELTARUPT) {
            uint32_t pos = (uint32_t)inst->playback_position;
            if (pos < xf_len) {
                float t = (float)pos / (float)xf_len; /* 0→1 */
                float fade_in = fast_sinf_0pi(t * 1.5707963f); /* sin fade-in */
                window *= fade_in;
            }
        }

        /* Hermite interpolation */
        uint32_t p0 = (src_pos - 1 + BUFFER_SIZE) % BUFFER_SIZE;
        uint32_t p1 = src_pos;
        uint32_t p2 = (src_pos + 1) % BUFFER_SIZE;
        uint32_t p3 = (src_pos + 2) % BUFFER_SIZE;

        float frac = inst->playback_position - (uint32_t)inst->playback_position;

        float samp_l = window * hermite_interp(
            buf_l[p0], buf_l[p1], buf_l[p2], buf_l[p3], frac);
        float samp_r = window * hermite_interp(
            buf_r[p0], buf_r[p1], buf_r[p2], buf_r[p3], frac);

        /* Per-grain Isolator3 filter */
        if (inst->rnd_filter > 0) {
            float filt_param = inst->grain_filter_cutoff / 100.0f;
            if (filt_param < 0.49f || filt_param > 0.51f) {
                for (int s = 0; s < 3; s++) {
                    samp_l = biquad_process(&inst->grain_filt_l[s], samp_l);
                    samp_r = biquad_process(&inst->grain_filt_r[s], samp_r);
                }
            }
        }

        /* Per-grain pan */
        float pan = inst->current_grain_pan;
        float pan_l = 1.0f - clampf(pan, 0.0f, 1.0f);
        float pan_r = 1.0f + clampf(pan, -1.0f, 0.0f);
        out_l[out_i] += samp_l * pan_l;
        out_r[out_i] += samp_r * pan_r;

        /* Reverb send: accumulate grain audio into reverb bus if flagged */
        if (inst->grain_reverb_send && inst->rnd_reverb > 0.0f)
            inst->rev_bus[out_i] += (samp_l + samp_r) * 0.5f;

        /* Playback speed */
        float speed = eff_time_warp;
        if (inst->mode == MODE_TIME_WARP) {
            float norm_amp = clampf(evt->density, 0.001f, 1.0f);
            float speed_factor = fast_powf_01(norm_amp, inst->speed_curve_exp);
            speed = lerpf(0.25f, 2.0f, speed_factor) * eff_time_warp;
        } else if (inst->mode == MODE_DENSITY_ARP) {
            speed = lerpf(0.8f, 1.2f, evt->density) * eff_time_warp;
        }

        inst->playback_position += speed;

        /* ---- Grain transition: move current to crossfade slot, start next ---- */
        if (inst->playback_position >= (float)grain_size) {
            /* Save current grain state into crossfade slot B */
            inst->xf_active = 1;
            inst->xf_position = inst->playback_position; /* continue from where we are */
            inst->xf_grain_start = grain_start;
            inst->xf_grain_size = grain_size + xf_len; /* extend tail for fade-out */
            inst->xf_speed = speed;
            inst->xf_pan = inst->current_grain_pan;

            /* Start new grain in slot A */
            inst->playback_position = 0.0f;
            inst->playback_index++;
            /* New random pan */
            float eff_pan = clampf(inst->rnd_pan + inst->seq_pan_offset, 0.0f, 1.0f);
            inst->current_grain_pan = rand_bipolar(&inst->rng) * eff_pan;
            /* Reverb send: stochastic per-grain decision */
            inst->grain_reverb_send = (inst->rnd_reverb > 0.0f && rand_unipolar(&inst->rng) < inst->rnd_reverb) ? 1 : 0;
            /* New random filter cutoff */
            if (inst->rnd_filter > 0) {
                float eff_chance = clampf((float)inst->rnd_filter / 100.0f + inst->seq_filter_offset, 0.0f, 1.0f);
                if (rand_unipolar(&inst->rng) < eff_chance) {
                    float base_cutoff = rand_unipolar(&inst->rng) * 100.0f;
                    inst->grain_filter_cutoff = clampf(base_cutoff + inst->seq_filter_offset * 50.0f, 0.0f, 100.0f);
                }
                float fc = inst->grain_filter_cutoff;
                float filt_param = fc / 100.0f;
                if (filt_param < 0.49f) {
                    float t = (0.49f - filt_param) / 0.49f;
                    float lp_f = 18000.0f * powf(200.0f / 18000.0f, t);
                    for (int s = 0; s < 3; s++) {
                        biquad_lpf(&inst->grain_filt_l[s], lp_f, 0.707f);
                        biquad_lpf(&inst->grain_filt_r[s], lp_f, 0.707f);
                    }
                } else if (filt_param > 0.51f) {
                    float t = (filt_param - 0.51f) / 0.49f;
                    float hp_f = 20.0f * powf(400.0f, t);
                    for (int s = 0; s < 3; s++) {
                        biquad_hpf(&inst->grain_filt_l[s], hp_f, 0.707f);
                        biquad_hpf(&inst->grain_filt_r[s], hp_f, 0.707f);
                    }
                }
                for (int s = 0; s < 3; s++) {
                    biquad_reset(&inst->grain_filt_l[s]);
                    biquad_reset(&inst->grain_filt_r[s]);
                }
            }
            inst->locked_grain_size = new_grain_size;
            grain_size = new_grain_size;
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

    /* Heap-allocate circular buffers (~1MB total — too large for struct embedding) */
    inst->buffer_l = (float*)calloc(BUFFER_SIZE, sizeof(float));
    inst->buffer_r = (float*)calloc(BUFFER_SIZE, sizeof(float));
    if (!inst->buffer_l || !inst->buffer_r) {
        free(inst->buffer_l);
        free(inst->buffer_r);
        free(inst);
        return NULL;
    }

    /* Defaults */
    inst->detection = 1.0f;
    inst->envelope = 0.5f;
    inst->density = 5.0f;
    inst->grain_size = 5.0f;
    inst->time_warp = 1.0f;
    inst->mix = 0.5f;
    inst->feedback = 0.2f;
    inst->mode = MODE_RANDOM;

    inst->shuffle_bias = 0.5f;
    inst->pitch_range_window = 0.0f;
    inst->octave_fold = 0;
    inst->density_curve = 0.0f;
    inst->speed_curve_exp = 1.0f;
    inst->arp_pattern = ARP_UP;
    inst->deltarupt_attack = 0.05f;
    inst->density_morphing = 0.0f;

    /* Randomize page defaults */
    inst->rnd_envelope = 0.0f;
    inst->rnd_density = 0.0f;
    inst->rnd_grain = 0.0f;
    inst->rnd_time = 0.0f;
    inst->rnd_pan = 0.25f;
    inst->current_grain_pan = 0.0f;
    inst->rnd_filter = 0;
    inst->grain_filter_cutoff = 50.0f;  /* bypass */
    for (int i = 0; i < 3; i++) {
        biquad_reset(&inst->grain_filt_l[i]);
        biquad_reset(&inst->grain_filt_r[i]);
    }
    inst->seq_on = 0;
    inst->seq_time_ms = 200.0f;
    inst->seq_mult = 4;  /* 1x */
    inst->seq_samples_elapsed = 0;
    inst->seq_env_offset = 0.0f;
    inst->seq_env_smooth = 0.0f;
    inst->seq_den_offset = 0.0f;
    inst->seq_grain_offset = 0.0f;
    inst->seq_time_offset = 1.0f;  /* 1.0 = no change (octave multiplier) */
    inst->seq_pan_offset = 0.0f;
    inst->seq_filter_offset = 0.0f;

    /* Page 3: Presets */
    inst->preset = 0;
    inst->master_filter = 0.5f;
    inst->master_filt_prev = -1.0f;
    inst->master_filt_smooth = 0.5f;
    for (int i = 0; i < 3; i++) {
        biquad_reset(&inst->master_filt_l[i]);
        biquad_reset(&inst->master_filt_r[i]);
    }
    inst->rnd_reverb = 0.0f;
    inst->rev_mix = 0.3f;
    inst->rev_size = 0.5f;
    inst->rev_decay = 0.5f;
    inst->rev_damp = 0.5f;
    inst->grain_reverb_send = 0;
    reverb_init(inst);
    reverb_update_params(inst);

    /* Envelope follower time constants (10 ms attack, 50 ms release at 44.1 kHz) */
    inst->env_attack = 1.0f - expf(-1.0f / (44100.0f * 0.01f));
    inst->env_release = 1.0f - expf(-1.0f / (44100.0f * 0.05f));

    /* PRNG seed */
    inst->rng = 0x12345678;

    /* Detection scheduling — force first detection after enough data accumulated */
    inst->blocks_since_detect = RESCAN_BLOCKS;  /* trigger on first eligible block */
    inst->total_samples_written = 0;

    /* FFT setup (pre-allocate — no malloc in render path) */
    inst->fft_setup = pffft_new_setup(FFT_SIZE, PFFFT_REAL);
    inst->fft_input = (float*)pffft_aligned_calloc(FFT_SIZE, sizeof(float));
    inst->fft_output = (float*)pffft_aligned_calloc(FFT_SIZE, sizeof(float));
    inst->fft_work = (float*)pffft_aligned_calloc(FFT_SIZE, sizeof(float));

    /* Apply preset 0 (Init) — overrides the hardcoded defaults above
     * with the preset table values for envelope/density/grain/tw/mix/fb/mode/filter/reverb */
    apply_structor_preset(inst, 0);

    if (g_host && g_host->log) g_host->log("[structor] instance created");
    return inst;
}

static void structor_destroy(void *instance) {
    if (instance) {
        StructorInstance *inst = (StructorInstance*)instance;
        if (inst->fft_setup) pffft_destroy_setup(inst->fft_setup);
        if (inst->fft_input) pffft_aligned_free(inst->fft_input);
        if (inst->fft_output) pffft_aligned_free(inst->fft_output);
        if (inst->fft_work) pffft_aligned_free(inst->fft_work);
        free(inst->buffer_l);
        free(inst->buffer_r);
        reverb_free(inst);
        free(inst);
    }
}

#define MAX_SEQ_MULT 9
static const float SEQ_MULT_VALUES[MAX_SEQ_MULT] = {
    0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f
};
static const char *SEQ_MULT_NAMES[MAX_SEQ_MULT] = {
    "1/8", "1/4", "1/2", "3/4", "1x", "1.5x", "2x", "3x", "4x"
};

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
        /* Feedback: blend new input with decayed previous content */
        inst->buffer_l[pos] = l + inst->recon_l[i] * inst->feedback;
        inst->buffer_r[pos] = r + inst->recon_r[i] * inst->feedback;

        inst->buf_write_pos = (pos + 1) % BUFFER_SIZE;
    }
    inst->total_samples_written += frames;

    /* Periodic detection: only re-detect every RESCAN_BLOCKS blocks,
       and only once we have enough data in the buffer */
    inst->blocks_since_detect++;
    if (inst->blocks_since_detect >= RESCAN_BLOCKS &&
        inst->total_samples_written >= DETECT_WINDOW) {

        uint32_t detect_win = DETECT_WINDOW;
        /* Don't detect more than what we've written */
        if (detect_win > inst->total_samples_written)
            detect_win = inst->total_samples_written;
        if (detect_win > BUFFER_SIZE)
            detect_win = BUFFER_SIZE;

        uint32_t window_start = (inst->buf_write_pos - detect_win + BUFFER_SIZE) % BUFFER_SIZE;
        detect_events(inst, window_start, detect_win);
        build_playback_order(inst);

        /* Reset playback to start of new order */
        inst->playback_index = 0;
        inst->playback_position = 0.0f;
        inst->blocks_since_detect = 0;
    }

    /* Sequencer tick — update random offsets at seq_time_ms intervals */
    if (inst->seq_on) {
        float mult = SEQ_MULT_VALUES[clampi(inst->seq_mult, 0, MAX_SEQ_MULT - 1)];
        uint32_t seq_period = (uint32_t)(inst->seq_time_ms * mult * 44.1f);
        if (seq_period < 441) seq_period = 441;  /* min 10ms */
        inst->seq_samples_elapsed += frames;
        if (inst->seq_samples_elapsed >= seq_period) {
            inst->seq_samples_elapsed = 0;
            inst->seq_env_offset    = rand_bipolar(&inst->rng) * inst->rnd_envelope;
            inst->seq_den_offset    = rand_bipolar(&inst->rng) * inst->rnd_density;
            inst->seq_grain_offset  = rand_bipolar(&inst->rng) * inst->rnd_grain;
            /* Rnd Time: quantized octave selection (0.25, 0.5, 1, 2, 4) */
            {
                static const float octaves[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
                int max_spread = (int)(inst->rnd_time * 2.0f + 0.5f);  /* 0→0, 0.5→1, 1.0→2 octaves */
                if (max_spread > 2) max_spread = 2;
                int idx = 2 + (max_spread > 0 ? ((int)(xorshift32(&inst->rng) % (uint32_t)(max_spread * 2 + 1)) - max_spread) : 0);
                if (idx < 0) idx = 0; if (idx > 4) idx = 4;
                inst->seq_time_offset = octaves[idx];
            }
            inst->seq_pan_offset    = rand_bipolar(&inst->rng) * inst->rnd_pan;
            inst->seq_filter_offset = rand_bipolar(&inst->rng) * ((float)inst->rnd_filter / 100.0f);
        }
    } else {
        /* Continuous: new random offsets every block */
        inst->seq_env_offset    = rand_bipolar(&inst->rng) * inst->rnd_envelope;
        inst->seq_den_offset    = rand_bipolar(&inst->rng) * inst->rnd_density;
        inst->seq_grain_offset  = rand_bipolar(&inst->rng) * inst->rnd_grain;
        /* Rnd Time: quantized octave selection (0.25, 0.5, 1, 2, 4) */
        {
            static const float octaves[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
            int max_spread = (int)(inst->rnd_time * 2.0f + 0.5f);
            if (max_spread > 2) max_spread = 2;
            int idx = 2 + (max_spread > 0 ? ((int)(xorshift32(&inst->rng) % (uint32_t)(max_spread * 2 + 1)) - max_spread) : 0);
            if (idx < 0) idx = 0; if (idx > 4) idx = 4;
            inst->seq_time_offset = octaves[idx];
        }
        inst->seq_pan_offset    = rand_bipolar(&inst->rng) * inst->rnd_pan;
        inst->seq_filter_offset = rand_bipolar(&inst->rng) * ((float)inst->rnd_filter / 100.0f);
    }

    /* Reconstruct */
    reconstruct(inst, frames);

    /* DC-blocking filter on reconstruction output: y[n] = x[n] - x[n-1] + 0.997*y[n-1]
     * Removes DC offset jumps between grains from different buffer positions */
    for (int i = 0; i < frames; i++) {
        float xl = inst->recon_l[i];
        float xr = inst->recon_r[i];
        float yl = xl - inst->dc_l_prev_in + 0.997f * inst->dc_l_prev_out;
        float yr = xr - inst->dc_r_prev_in + 0.997f * inst->dc_r_prev_out;
        inst->dc_l_prev_in = xl; inst->dc_l_prev_out = yl;
        inst->dc_r_prev_in = xr; inst->dc_r_prev_out = yr;
        inst->recon_l[i] = yl;
        inst->recon_r[i] = yr;
    }

    /* Process reverb bus and mix into reconstruction output */
    if (inst->rnd_reverb > 0.0f || inst->rev_mix > 0.0f) {
        reverb_update_params(inst);
        reverb_process(inst, frames);
        float rmix = inst->rev_mix;
        for (int i = 0; i < frames; i++) {
            inst->recon_l[i] += inst->rev_out_l[i] * rmix;
            inst->recon_r[i] += inst->rev_out_r[i] * rmix;
        }
    }

    /* Mix wet/dry with equal-power crossfade (no volume dip at 50%) */
    float mix_angle = inst->mix * 1.5707963f;  /* 0..pi/2 */
    float dry = cosf(mix_angle);  /* once per block — keep precise */
    float wet = sinf(mix_angle);
    /* Per-sample smoothing coefficient for filter (~10ms tau at 44.1kHz) */
    float filt_alpha = 1.0f - expf(-1.0f / (44100.0f * 0.010f));
    float mf_target = inst->master_filter;

    for (int i = 0; i < frames; i++) {
        /* Smooth filter parameter per-sample (eliminates zipper noise) */
        inst->master_filt_smooth += filt_alpha * (mf_target - inst->master_filt_smooth);
        float mf = inst->master_filt_smooth;

        /* Update filter coefficients when smoothed value changes */
        if (fabsf(mf - inst->master_filt_prev) > 0.0005f) {
            inst->master_filt_prev = mf;
            if (mf < 0.49f) {
                float t = (0.49f - mf) / 0.49f;
                float lp_f = 18000.0f * powf(200.0f / 18000.0f, t);
                for (int s = 0; s < 3; s++) {
                    biquad_lpf(&inst->master_filt_l[s], lp_f, 0.707f);
                    biquad_lpf(&inst->master_filt_r[s], lp_f, 0.707f);
                }
            } else if (mf > 0.51f) {
                float t = (mf - 0.51f) / 0.49f;
                float hp_f = 20.0f * powf(400.0f, t);
                for (int s = 0; s < 3; s++) {
                    biquad_hpf(&inst->master_filt_l[s], hp_f, 0.707f);
                    biquad_hpf(&inst->master_filt_r[s], hp_f, 0.707f);
                }
            }
        }
        int mf_active = (mf < 0.49f || mf > 0.51f);

        float in_l = audio_inout[i * 2]     / 32768.0f;
        float in_r = audio_inout[i * 2 + 1] / 32768.0f;

        float out_l = dry * in_l + wet * inst->recon_l[i] * RECON_GAIN;
        float out_r = dry * in_r + wet * inst->recon_r[i] * RECON_GAIN;

        /* Master DJ filter (Isolator3 3-stage cascade, smoothed) */
        if (mf_active) {
            for (int s = 0; s < 3; s++) {
                out_l = biquad_process(&inst->master_filt_l[s], out_l);
                out_r = biquad_process(&inst->master_filt_r[s], out_r);
            }
        }

        /* Soft limiting (fast approximation) */
        out_l = fast_tanhf(out_l);
        out_r = fast_tanhf(out_r);

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
   Mode names and display constants
   ======================================================================== */

static const char *MODE_NAMES[MAX_MODES] = {
    "Random", "Pitch Up", "Pitch Down", "Density Up",
    "Time Warp", "Dens Arp", "Deltarupt", "Spec Warp"
};

static const char *ARP_NAMES[MAX_ARP] = {
    "Up", "Down", "Up-Down", "Down-Up", "Random", "Cascade"
};

static const char *OCTAVE_FOLD_NAMES[MAX_OCTAVE_FOLD] = {
    "None", "1 Oct", "Mirror", "Harmonic", "Inharm"
};

static const char *KNOB8_LABELS[MAX_MODES] = {
    "Shfl Bias", "Pitch Win", "Oct Fold", "Dens Crv",
    "Spd Curve", "Arp Ptrn", "Attack", "Morphing"
};

/* ========================================================================
   Helper: get/set the current Knob 8 value based on mode
   ======================================================================== */

static float get_knob8_value(StructorInstance *inst) {
    switch (inst->mode) {
        case MODE_RANDOM:           return inst->shuffle_bias;
        case MODE_PITCH_UP:         return inst->pitch_range_window;
        case MODE_PITCH_DOWN:       return (float)inst->octave_fold;
        case MODE_DENSITY_UP:       return inst->density_curve;
        case MODE_TIME_WARP:        return inst->speed_curve_exp;
        case MODE_DENSITY_ARP:      return (float)inst->arp_pattern;
        case MODE_DELTARUPT:        return inst->deltarupt_attack;
        case MODE_SPECTRAL_DENSITY: return inst->density_morphing;
        default:                    return 0.0f;
    }
}

static void set_knob8_value(StructorInstance *inst, float val) {
    switch (inst->mode) {
        case MODE_RANDOM:           inst->shuffle_bias = clampf(val, 0.0f, 1.0f); break;
        case MODE_PITCH_UP:         inst->pitch_range_window = clampf(val, 0.0f, 1.0f); break;
        case MODE_PITCH_DOWN:       inst->octave_fold = clampi((int)val, 0, MAX_OCTAVE_FOLD - 1); break;
        case MODE_DENSITY_UP:       inst->density_curve = clampf(val, 0.0f, 1.0f); break;
        case MODE_TIME_WARP:        inst->speed_curve_exp = clampf(val, 0.5f, 2.0f); break;
        case MODE_DENSITY_ARP:      inst->arp_pattern = clampi((int)val, 0, MAX_ARP - 1); break;
        case MODE_DELTARUPT:        inst->deltarupt_attack = clampf(val, 0.0f, 1.0f); break;
        case MODE_SPECTRAL_DENSITY: inst->density_morphing = clampf(val, 0.0f, 1.0f); break;
    }
}

/* Normalized 0-1 getter/setter for chain_param "Special" (menu editing).
   Maps each mode's raw range to 0-1 so the hierarchy editor doesn't clamp. */
static float get_knob8_normalized(StructorInstance *inst) {
    switch (inst->mode) {
        case MODE_RANDOM:           return inst->shuffle_bias;
        case MODE_PITCH_UP:         return inst->pitch_range_window;
        case MODE_PITCH_DOWN:       return (float)inst->octave_fold / (float)(MAX_OCTAVE_FOLD - 1);
        case MODE_DENSITY_UP:       return inst->density_curve;
        case MODE_TIME_WARP:        return (inst->speed_curve_exp - 0.5f) / 1.5f;
        case MODE_DENSITY_ARP:      return (float)inst->arp_pattern / (float)(MAX_ARP - 1);
        case MODE_DELTARUPT:        return inst->deltarupt_attack;
        case MODE_SPECTRAL_DENSITY: return inst->density_morphing;
        default:                    return 0.0f;
    }
}

static void set_knob8_normalized(StructorInstance *inst, float norm) {
    norm = clampf(norm, 0.0f, 1.0f);
    switch (inst->mode) {
        case MODE_RANDOM:           inst->shuffle_bias = norm; break;
        case MODE_PITCH_UP:         inst->pitch_range_window = norm; break;
        case MODE_PITCH_DOWN:       inst->octave_fold = clampi((int)(norm * (MAX_OCTAVE_FOLD - 1) + 0.5f), 0, MAX_OCTAVE_FOLD - 1); break;
        case MODE_DENSITY_UP:       inst->density_curve = norm; break;
        case MODE_TIME_WARP:        inst->speed_curve_exp = clampf(0.5f + norm * 1.5f, 0.5f, 2.0f); break;
        case MODE_DENSITY_ARP:      inst->arp_pattern = clampi((int)(norm * (MAX_ARP - 1) + 0.5f), 0, MAX_ARP - 1); break;
        case MODE_DELTARUPT:        inst->deltarupt_attack = norm; break;
        case MODE_SPECTRAL_DENSITY: inst->density_morphing = norm; break;
    }
}

/* ========================================================================
   API: set_param
   ======================================================================== */

static void structor_set_param(void *instance, const char *key, const char *val) {
    StructorInstance *inst = (StructorInstance*)instance;
    if (!inst || !key || !val) return;

    /* Page tracking — Shadow UI sends _level when user navigates */
    if (strcmp(key, "_level") == 0) {
        if (strcmp(val, "Randomize") == 0) inst->current_page = 1;
        else if (strcmp(val, "Presets") == 0) inst->current_page = 2;
        else inst->current_page = 0;  /* "Structor" or root */
        return;
    }

    /* Direct parameter keys */
    if (strcmp(key, "detection") == 0) {
        inst->detection = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "envelope") == 0) {
        inst->envelope = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "density") == 0) {
        inst->density = clampf(atof(val), 0.05f, 20.0f);
    } else if (strcmp(key, "grain_size") == 0) {
        inst->grain_size = clampf(atof(val), 0.1f, 40.0f);
    } else if (strcmp(key, "time_warp") == 0) {
        inst->time_warp = clampf(atof(val), 0.25f, 8.0f);
    } else if (strcmp(key, "mix") == 0) {
        inst->mix = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "feedback") == 0) {
        inst->feedback = clampf(atof(val), 0.0f, 0.95f);
    } else if (strcmp(key, "mode") == 0) {
        /* Accept mode name string or integer index */
        int found = 0;
        for (int i = 0; i < MAX_MODES; i++) {
            if (strcmp(val, MODE_NAMES[i]) == 0) { inst->mode = i; found = 1; break; }
        }
        if (!found) inst->mode = clampi(atoi(val), 0, MAX_MODES - 1);
    } else if (strcmp(key, "Special") == 0 || strcmp(key, "special") == 0) {
        set_knob8_normalized(inst, atof(val));
    } else if (strcmp(key, "shuffle_bias") == 0) {
        inst->shuffle_bias = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "pitch_range_window") == 0) {
        inst->pitch_range_window = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "octave_fold") == 0) {
        for (int i = 0; i < MAX_OCTAVE_FOLD; i++) {
            if (strcmp(val, OCTAVE_FOLD_NAMES[i]) == 0) { inst->octave_fold = i; return; }
        }
        inst->octave_fold = clampi(atoi(val), 0, MAX_OCTAVE_FOLD - 1);
    } else if (strcmp(key, "density_curve") == 0) {
        inst->density_curve = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "speed_curve_exp") == 0) {
        inst->speed_curve_exp = clampf(atof(val), 0.5f, 2.0f);
    } else if (strcmp(key, "arp_pattern") == 0) {
        for (int i = 0; i < MAX_ARP; i++) {
            if (strcmp(val, ARP_NAMES[i]) == 0) { inst->arp_pattern = i; return; }
        }
        inst->arp_pattern = clampi(atoi(val), 0, MAX_ARP - 1);
    } else if (strcmp(key, "deltarupt_attack") == 0) {
        inst->deltarupt_attack = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "density_morphing") == 0) {
        inst->density_morphing = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rnd_envelope") == 0) {
        inst->rnd_envelope = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rnd_density") == 0) {
        inst->rnd_density = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rnd_grain") == 0) {
        inst->rnd_grain = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rnd_time") == 0) {
        inst->rnd_time = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rnd_pan") == 0) {
        float v = atof(val);
        if (v > 1.0f) v = v / 100.0f;  /* accept both 0-100 int and 0-1 float */
        inst->rnd_pan = clampf(v, 0.0f, 1.0f);
    } else if (strcmp(key, "rnd_filter") == 0) {
        float v = atof(val);
        if (v <= 1.0f && v > 0.0f && strchr(val, '.') != NULL) {
            /* Float 0-1 from hierarchy editor */
            inst->rnd_filter = clampi((int)(v * 100.0f + 0.5f), 0, 100);
        } else {
            inst->rnd_filter = clampi(atoi(val), 0, 100);
        }
        if (inst->rnd_filter == 0) inst->grain_filter_cutoff = 50.0f;  /* reset to bypass */
    } else if (strcmp(key, "seq_on") == 0) {
        if (strcmp(val, "On") == 0) inst->seq_on = 1;
        else if (strcmp(val, "Off") == 0) inst->seq_on = 0;
        else inst->seq_on = clampi(atoi(val), 0, 1);
    } else if (strcmp(key, "seq_time") == 0) {
        inst->seq_time_ms = clampf(atof(val), 10.0f, 1000.0f);
    } else if (strcmp(key, "seq_mult") == 0) {
        /* Accept name string or integer index */
        for (int i = 0; i < MAX_SEQ_MULT; i++) {
            if (strcmp(val, SEQ_MULT_NAMES[i]) == 0) { inst->seq_mult = i; return; }
        }
        inst->seq_mult = clampi(atoi(val), 0, MAX_SEQ_MULT - 1);
    }
    /* ---- Page 3 params ---- */
    else if (strcmp(key, "preset") == 0) {
        int p = atoi(val);
        if (p >= 0 && p < NUM_STRUCTOR_PRESETS) apply_structor_preset(instance, p);
    } else if (strcmp(key, "rnd_preset") == 0) {
        /* Trigger on any non-zero value (int or float from jog menu) */
        float v = atof(val);
        if (v != 0.0f) randomize_preset(inst);
    } else if (strcmp(key, "master_filter") == 0) {
        inst->master_filter = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rnd_reverb") == 0) {
        inst->rnd_reverb = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rev_mix") == 0) {
        inst->rev_mix = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rev_size") == 0) {
        inst->rev_size = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rev_decay") == 0) {
        inst->rev_decay = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "rev_damp") == 0) {
        inst->rev_damp = clampf(atof(val), 0.0f, 1.0f);
    }
    /* ---- Knob overlay: knob_N_adjust (page-aware) ---- */
    else if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int knob_num = atoi(key + 5);  /* 1-indexed */
        int delta = atoi(val);

        if (inst->current_page == 2) {
            /* Presets page */
            switch (knob_num) {
                case 1: {
                    int p = inst->preset + delta;
                    if (p >= NUM_STRUCTOR_PRESETS) p = 0;
                    if (p < 0) p = NUM_STRUCTOR_PRESETS - 1;
                    apply_structor_preset(instance, p);
                    break;
                }
                case 2: if (delta != 0) { randomize_preset(inst); inst->preset = -1; } break;
                case 3: inst->master_filter = clampf(inst->master_filter + delta * 0.01f, 0.0f, 1.0f); break;
                case 4: inst->rnd_reverb = clampf(inst->rnd_reverb + delta * 0.01f, 0.0f, 1.0f); break;
                case 5: inst->rev_mix = clampf(inst->rev_mix + delta * 0.01f, 0.0f, 1.0f); break;
                case 6: inst->rev_size = clampf(inst->rev_size + delta * 0.01f, 0.0f, 1.0f); break;
                case 7: inst->rev_decay = clampf(inst->rev_decay + delta * 0.01f, 0.0f, 1.0f); break;
                case 8: inst->rev_damp = clampf(inst->rev_damp + delta * 0.01f, 0.0f, 1.0f); break;
            }
        } else if (inst->current_page == 1) {
            /* Randomize page */
            switch (knob_num) {
                case 1: inst->rnd_envelope = clampf(inst->rnd_envelope + delta * 0.02f, 0.0f, 1.0f); break;
                case 2: inst->rnd_density = clampf(inst->rnd_density + delta * 0.02f, 0.0f, 1.0f); break;
                case 3: inst->rnd_grain = clampf(inst->rnd_grain + delta * 0.02f, 0.0f, 1.0f); break;
                case 4: inst->rnd_time = clampf(inst->rnd_time + delta * 0.02f, 0.0f, 1.0f); break;
                case 5: inst->rnd_pan = clampf(inst->rnd_pan + delta * 0.01f, 0.0f, 1.0f); break;
                case 6: inst->seq_on = inst->seq_on ? 0 : (delta != 0 ? 1 : 0); break;
                case 7: inst->seq_time_ms = clampf(inst->seq_time_ms + delta * 5.0f, 10.0f, 1000.0f); break;
                case 8: {
                    int new_mult = inst->seq_mult + delta;
                    if (new_mult >= MAX_SEQ_MULT) new_mult = 0;
                    if (new_mult < 0) new_mult = MAX_SEQ_MULT - 1;
                    inst->seq_mult = new_mult;
                    break;
                }
            }
        } else {
            /* Structor page */
            switch (knob_num) {
                case 1: inst->envelope = clampf(inst->envelope + delta * 0.02f, 0.0f, 1.0f); break;
                case 2: inst->density = clampf(inst->density + delta * 0.05f, 0.05f, 20.0f); break;
                case 3: inst->grain_size = clampf(inst->grain_size + delta * 0.2f, 0.1f, 40.0f); break;
                case 4: inst->time_warp = clampf(inst->time_warp + delta * 0.05f, 0.25f, 8.0f); break;
                case 5: inst->feedback = clampf(inst->feedback + delta * 0.01f, 0.0f, 0.95f); break;
                case 6: {
                    int new_mode = inst->mode + delta;
                    if (new_mode > (MAX_MODES - 1)) new_mode = 0;
                    if (new_mode < 0) new_mode = MAX_MODES - 1;
                    inst->mode = new_mode;
                    break;
                }
                case 7: {
                    /* Context-sensitive Special knob */
                    if (inst->mode == MODE_DENSITY_ARP) {
                        int new_arp = inst->arp_pattern + delta;
                        inst->arp_pattern = clampi(new_arp, 0, MAX_ARP - 1);
                    } else if (inst->mode == MODE_PITCH_DOWN) {
                        int new_fold = inst->octave_fold + delta;
                        inst->octave_fold = clampi(new_fold, 0, MAX_OCTAVE_FOLD - 1);
                    } else if (inst->mode == MODE_TIME_WARP) {
                        inst->speed_curve_exp = clampf(inst->speed_curve_exp + delta * 0.01f, 0.5f, 2.0f);
                    } else {
                        float cur = get_knob8_value(inst);
                        set_knob8_value(inst, cur + delta * 0.01f);
                    }
                    break;
                }
                case 8: inst->mix = clampf(inst->mix + delta * 0.01f, 0.0f, 1.0f); break;
            }
        }
    }
    /* State serialization */
    else if (strcmp(key, "state") == 0) {
        int m = 0, ap = 0, of = 0, sqon = 0;
        float det = 0.1f, env = 0.2f, den = 0.7f, gs = 1.0f, tw = 1.0f;
        float mx = 0.5f, fb = 0.2f;
        float sb = 0.5f, prw = 0.0f, dc = 0.0f, sce = 1.0f, da = 0.05f, dm = 0.0f;
        float re = 0.0f, rd = 0.0f, rg = 0.0f, rt = 0.0f, rp = 0.25f, stms = 200.0f;
        int sqmult = 4, rndf = 0;
        sscanf(val,
            "{\"detection\":%f,\"envelope\":%f,\"density\":%f,\"grain_size\":%f,\"time_warp\":%f,"
            "\"mix\":%f,\"feedback\":%f,\"mode\":%d,"
            "\"shuffle_bias\":%f,\"pitch_range_window\":%f,\"octave_fold\":%d,"
            "\"density_curve\":%f,\"speed_curve_exp\":%f,"
            "\"arp_pattern\":%d,\"deltarupt_attack\":%f,\"density_morphing\":%f,"
            "\"rnd_envelope\":%f,\"rnd_density\":%f,\"rnd_grain\":%f,\"rnd_time\":%f,"
            "\"rnd_pan\":%f,"
            "\"seq_on\":%d,\"seq_time\":%f,\"seq_mult\":%d,\"rnd_filter\":%d}",
            &det, &env, &den, &gs, &tw, &mx, &fb, &m,
            &sb, &prw, &of, &dc, &sce, &ap, &da, &dm,
            &re, &rd, &rg, &rt, &rp, &sqon, &stms, &sqmult, &rndf);
        inst->detection = clampf(det, 0.0f, 1.0f);
        inst->envelope = clampf(env, 0.0f, 1.0f);
        inst->density = clampf(den, 0.05f, 20.0f);
        inst->grain_size = clampf(gs, 0.1f, 40.0f);
        inst->time_warp = clampf(tw, 0.25f, 8.0f);
        inst->mix = clampf(mx, 0.0f, 1.0f);
        inst->feedback = clampf(fb, 0.0f, 0.95f);
        inst->mode = clampi(m, 0, MAX_MODES - 1);
        inst->shuffle_bias = clampf(sb, 0.0f, 1.0f);
        inst->pitch_range_window = clampf(prw, 0.0f, 1.0f);
        inst->octave_fold = clampi(of, 0, MAX_OCTAVE_FOLD - 1);
        inst->density_curve = clampf(dc, 0.0f, 1.0f);
        inst->speed_curve_exp = clampf(sce, 0.5f, 2.0f);
        inst->arp_pattern = clampi(ap, 0, MAX_ARP - 1);
        inst->deltarupt_attack = clampf(da, 0.0f, 1.0f);
        inst->density_morphing = clampf(dm, 0.0f, 1.0f);
        inst->rnd_envelope = clampf(re, 0.0f, 1.0f);
        inst->rnd_density = clampf(rd, 0.0f, 1.0f);
        inst->rnd_grain = clampf(rg, 0.0f, 1.0f);
        inst->rnd_time = clampf(rt, 0.0f, 1.0f);
        inst->rnd_pan = clampf(rp, 0.0f, 1.0f);
        inst->seq_on = clampi(sqon, 0, 1);
        inst->seq_time_ms = clampf(stms, 10.0f, 1000.0f);
        inst->seq_mult = clampi(sqmult, 0, MAX_SEQ_MULT - 1);
        inst->rnd_filter = clampi(rndf, 0, 100);
    }
}

/* ========================================================================
   API: get_param
   ======================================================================== */

static int structor_get_param(void *instance, const char *key, char *buf, int buf_len) {
    StructorInstance *inst = (StructorInstance*)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Structor");
    } else if (strcmp(key, "chain_params") == 0) {
        static const char *cp =
            "["
            "{\"key\":\"envelope\",\"name\":\"Envelope\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02},"
            "{\"key\":\"density\",\"name\":\"Density\",\"type\":\"float\",\"min\":0.05,\"max\":20,\"step\":0.05},"
            "{\"key\":\"grain_size\",\"name\":\"Grain Size\",\"type\":\"float\",\"min\":0.1,\"max\":40,\"step\":0.2},"
            "{\"key\":\"time_warp\",\"name\":\"Time Warp\",\"type\":\"float\",\"min\":0.25,\"max\":8,\"step\":0.05},"
            "{\"key\":\"feedback\",\"name\":\"Feedback\",\"type\":\"float\",\"min\":0,\"max\":0.95,\"step\":0.01},"
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"Random\",\"Pitch Up\",\"Pitch Down\",\"Density Up\",\"Time Warp\",\"Dens Arp\",\"Deltarupt\",\"Spec Warp\"],\"default\":0},"
            "{\"key\":\"Special\",\"name\":\"Special\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rnd_envelope\",\"name\":\"Rnd Env\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02},"
            "{\"key\":\"rnd_density\",\"name\":\"Rnd Density\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02},"
            "{\"key\":\"rnd_grain\",\"name\":\"Rnd Grain\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02},"
            "{\"key\":\"rnd_time\",\"name\":\"Rnd Time\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02},"
            "{\"key\":\"rnd_pan\",\"name\":\"Rnd Pan\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02},"
            "{\"key\":\"seq_on\",\"name\":\"Sequence\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":0},"
            "{\"key\":\"seq_time\",\"name\":\"Seq Time\",\"type\":\"int\",\"min\":10,\"max\":1000,\"step\":5},"
            "{\"key\":\"seq_mult\",\"name\":\"Seq Mult\",\"type\":\"enum\",\"options\":[\"1/8\",\"1/4\",\"1/2\",\"3/4\",\"1x\",\"1.5x\",\"2x\",\"3x\",\"4x\"],\"default\":4},"
            "{\"key\":\"detection\",\"name\":\"Detection\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rnd_filter\",\"name\":\"Rnd Filter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":19,\"step\":1},"
            "{\"key\":\"rnd_preset\",\"name\":\"Rnd Preset\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"master_filter\",\"name\":\"Filter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rnd_reverb\",\"name\":\"Rnd Reverb\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rev_mix\",\"name\":\"Rev Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rev_size\",\"name\":\"Rev Size\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rev_decay\",\"name\":\"Rev Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rev_damp\",\"name\":\"Rev Damp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}"
            "]";
        int len = (int)strlen(cp);
        if (len >= buf_len) return -1;
        memcpy(buf, cp, len + 1);
        return len;
    } else if (strcmp(key, "ui_hierarchy") == 0) {
        static const char *hier =
            "{\"modes\":null,\"levels\":{"
            "\"root\":{\"name\":\"Structor\","
            "\"knobs\":[\"envelope\",\"density\",\"grain_size\",\"time_warp\",\"feedback\",\"mode\",\"Special\",\"mix\"],"
            "\"params\":[{\"level\":\"Structor\",\"label\":\"Structor\"},{\"level\":\"Randomize\",\"label\":\"Randomize\"},{\"level\":\"Presets\",\"label\":\"Presets\"}]},"
            "\"Structor\":{\"label\":\"Structor\","
            "\"knobs\":[\"envelope\",\"density\",\"grain_size\",\"time_warp\",\"feedback\",\"mode\",\"Special\",\"mix\"],"
            "\"params\":[\"envelope\",\"density\",\"grain_size\",\"time_warp\",\"feedback\",\"mode\",\"Special\",\"mix\"]},"
            "\"Randomize\":{\"label\":\"Randomize\","
            "\"knobs\":[\"rnd_envelope\",\"rnd_density\",\"rnd_grain\",\"rnd_time\",\"rnd_pan\",\"seq_on\",\"seq_time\",\"seq_mult\"],"
            "\"params\":[\"rnd_envelope\",\"rnd_density\",\"rnd_grain\",\"rnd_time\",\"rnd_pan\",\"seq_on\",\"seq_time\",\"seq_mult\",\"detection\",\"rnd_filter\"]},"
            "\"Presets\":{\"label\":\"Presets\","
            "\"knobs\":[\"preset\",\"rnd_preset\",\"master_filter\",\"rnd_reverb\",\"rev_mix\",\"rev_size\",\"rev_decay\",\"rev_damp\"],"
            "\"params\":[\"preset\",\"rnd_preset\",\"master_filter\",\"rnd_reverb\",\"rev_mix\",\"rev_size\",\"rev_decay\",\"rev_damp\"]}"
            "}}";
        int len = (int)strlen(hier);
        if (len >= buf_len) return -1;
        memcpy(buf, hier, len + 1);
        return len;
    } else if (strcmp(key, "detection") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->detection);
    } else if (strcmp(key, "envelope") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->envelope);
    } else if (strcmp(key, "density") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->density);
    } else if (strcmp(key, "grain_size") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->grain_size);
    } else if (strcmp(key, "time_warp") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->time_warp);
    } else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->mix);
    } else if (strcmp(key, "feedback") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->feedback);
    } else if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%s", MODE_NAMES[clampi(inst->mode, 0, MAX_MODES - 1)]);
    } else if (strcmp(key, "Special") == 0 || strcmp(key, "special") == 0) {
        return snprintf(buf, buf_len, "%.4f", get_knob8_normalized(inst));
    } else if (strcmp(key, "shuffle_bias") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->shuffle_bias);
    } else if (strcmp(key, "pitch_range_window") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->pitch_range_window);
    } else if (strcmp(key, "octave_fold") == 0) {
        return snprintf(buf, buf_len, "%s", OCTAVE_FOLD_NAMES[clampi(inst->octave_fold, 0, MAX_OCTAVE_FOLD - 1)]);
    } else if (strcmp(key, "density_curve") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->density_curve);
    } else if (strcmp(key, "speed_curve_exp") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->speed_curve_exp);
    } else if (strcmp(key, "arp_pattern") == 0) {
        return snprintf(buf, buf_len, "%s", ARP_NAMES[clampi(inst->arp_pattern, 0, MAX_ARP - 1)]);
    } else if (strcmp(key, "deltarupt_attack") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->deltarupt_attack);
    } else if (strcmp(key, "density_morphing") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->density_morphing);
    } else if (strcmp(key, "rnd_envelope") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rnd_envelope);
    } else if (strcmp(key, "rnd_density") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rnd_density);
    } else if (strcmp(key, "rnd_grain") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rnd_grain);
    } else if (strcmp(key, "rnd_time") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rnd_time);
    } else if (strcmp(key, "rnd_pan") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rnd_pan);
    } else if (strcmp(key, "rnd_filter") == 0) {
        return snprintf(buf, buf_len, "%.4f", (float)inst->rnd_filter / 100.0f);
    } else if (strcmp(key, "seq_on") == 0) {
        return snprintf(buf, buf_len, "%s", inst->seq_on ? "On" : "Off");
    } else if (strcmp(key, "seq_time") == 0) {
        return snprintf(buf, buf_len, "%d", (int)inst->seq_time_ms);
    } else if (strcmp(key, "seq_mult") == 0) {
        return snprintf(buf, buf_len, "%s", SEQ_MULT_NAMES[clampi(inst->seq_mult, 0, MAX_SEQ_MULT - 1)]);
    } else if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->preset);
    } else if (strcmp(key, "rnd_preset") == 0) {
        return snprintf(buf, buf_len, "0");  /* always reads 0; any set triggers randomize */
    } else if (strcmp(key, "master_filter") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->master_filter);
    } else if (strcmp(key, "rnd_reverb") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rnd_reverb);
    } else if (strcmp(key, "rev_mix") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rev_mix);
    } else if (strcmp(key, "rev_size") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rev_size);
    } else if (strcmp(key, "rev_decay") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rev_decay);
    } else if (strcmp(key, "rev_damp") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rev_damp);
    }

    /* ---- Knob overlay: knob_N_name (page-aware) ---- */
    else if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int knob_num = atoi(key + 5);
        if (inst->current_page == 2) {
            switch (knob_num) {
                case 1: return snprintf(buf, buf_len, "Preset");
                case 2: return snprintf(buf, buf_len, "Rnd Preset");
                case 3: return snprintf(buf, buf_len, "Filter");
                case 4: return snprintf(buf, buf_len, "Rnd Reverb");
                case 5: return snprintf(buf, buf_len, "Rev Mix");
                case 6: return snprintf(buf, buf_len, "Rev Size");
                case 7: return snprintf(buf, buf_len, "Rev Decay");
                case 8: return snprintf(buf, buf_len, "Rev Damp");
            }
        } else if (inst->current_page == 1) {
            switch (knob_num) {
                case 1: return snprintf(buf, buf_len, "Rnd Env");
                case 2: return snprintf(buf, buf_len, "Rnd Density");
                case 3: return snprintf(buf, buf_len, "Rnd Grain");
                case 4: return snprintf(buf, buf_len, "Rnd Time");
                case 5: return snprintf(buf, buf_len, "Rnd Pan");
                case 6: return snprintf(buf, buf_len, "Sequence");
                case 7: return snprintf(buf, buf_len, "Seq Time");
                case 8: return snprintf(buf, buf_len, "Seq Mult");
            }
        } else {
            switch (knob_num) {
                case 1: return snprintf(buf, buf_len, "Envelope");
                case 2: return snprintf(buf, buf_len, "Density");
                case 3: return snprintf(buf, buf_len, "Grain Size");
                case 4: return snprintf(buf, buf_len, "Time Warp");
                case 5: return snprintf(buf, buf_len, "Feedback");
                case 6: return snprintf(buf, buf_len, "Mode");
                case 7: return snprintf(buf, buf_len, "%s", KNOB8_LABELS[clampi(inst->mode, 0, MAX_MODES - 1)]);
                case 8: return snprintf(buf, buf_len, "Mix");
            }
        }
        return 0;
    }

    /* ---- Knob overlay: knob_N_value (page-aware) ---- */
    else if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int knob_num = atoi(key + 5);
        if (inst->current_page == 2) {
            switch (knob_num) {
                case 1: return snprintf(buf, buf_len, "%d", inst->preset);
                case 2: return snprintf(buf, buf_len, "Go!");
                case 3: {
                    float mf = inst->master_filter;
                    if (mf < 0.49f) return snprintf(buf, buf_len, "LP %d%%", (int)((0.49f - mf) / 0.49f * 100));
                    if (mf > 0.51f) return snprintf(buf, buf_len, "HP %d%%", (int)((mf - 0.51f) / 0.49f * 100));
                    return snprintf(buf, buf_len, "Off");
                }
                case 4: return snprintf(buf, buf_len, "%d%%", (int)(inst->rnd_reverb * 100.0f));
                case 5: return snprintf(buf, buf_len, "%d%%", (int)(inst->rev_mix * 100.0f));
                case 6: return snprintf(buf, buf_len, "%d%%", (int)(inst->rev_size * 100.0f));
                case 7: return snprintf(buf, buf_len, "%d%%", (int)(inst->rev_decay * 100.0f));
                case 8: return snprintf(buf, buf_len, "%d%%", (int)(inst->rev_damp * 100.0f));
            }
        } else if (inst->current_page == 1) {
            switch (knob_num) {
                case 1: return snprintf(buf, buf_len, "%d%%", (int)(inst->rnd_envelope * 100.0f));
                case 2: return snprintf(buf, buf_len, "%d%%", (int)(inst->rnd_density * 100.0f));
                case 3: return snprintf(buf, buf_len, "%d%%", (int)(inst->rnd_grain * 100.0f));
                case 4: return snprintf(buf, buf_len, "%d%%", (int)(inst->rnd_time * 100.0f));
                case 5: return snprintf(buf, buf_len, "%d%%", (int)(inst->rnd_pan * 100.0f));
                case 6: return snprintf(buf, buf_len, "%s", inst->seq_on ? "On" : "Off");
                case 7: return snprintf(buf, buf_len, "%dms", (int)inst->seq_time_ms);
                case 8: return snprintf(buf, buf_len, "%s", SEQ_MULT_NAMES[clampi(inst->seq_mult, 0, MAX_SEQ_MULT - 1)]);
            }
        } else {
            switch (knob_num) {
                case 1: return snprintf(buf, buf_len, "%d%%", (int)(inst->envelope * 100.0f));
                case 2: return snprintf(buf, buf_len, "%.2f", inst->density);
                case 3: return snprintf(buf, buf_len, "%.2f", inst->grain_size);
                case 4: return snprintf(buf, buf_len, "%.2f", inst->time_warp);
                case 5: return snprintf(buf, buf_len, "%d%%", (int)(inst->feedback * 100.0f));
                case 6: return snprintf(buf, buf_len, "%s", MODE_NAMES[clampi(inst->mode, 0, MAX_MODES - 1)]);
                case 7: {
                    if (inst->mode == MODE_DENSITY_ARP) {
                        return snprintf(buf, buf_len, "%s", ARP_NAMES[clampi(inst->arp_pattern, 0, MAX_ARP - 1)]);
                    } else if (inst->mode == MODE_PITCH_DOWN) {
                        return snprintf(buf, buf_len, "%s", OCTAVE_FOLD_NAMES[clampi(inst->octave_fold, 0, MAX_OCTAVE_FOLD - 1)]);
                    } else if (inst->mode == MODE_TIME_WARP) {
                        return snprintf(buf, buf_len, "%.1fx", inst->speed_curve_exp);
                    } else {
                        float v = get_knob8_value(inst);
                        return snprintf(buf, buf_len, "%d%%", (int)(v * 100.0f));
                    }
                }
                case 8: return snprintf(buf, buf_len, "%d%%", (int)(inst->mix * 100.0f));
            }
        }
        return 0;
    }

    /* ---- State serialization ---- */
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"detection\":%.4f,\"envelope\":%.4f,\"density\":%.4f,\"grain_size\":%.4f,\"time_warp\":%.4f,"
            "\"mix\":%.4f,\"feedback\":%.4f,\"mode\":%d,"
            "\"shuffle_bias\":%.4f,\"pitch_range_window\":%.4f,\"octave_fold\":%d,"
            "\"density_curve\":%.4f,\"speed_curve_exp\":%.4f,"
            "\"arp_pattern\":%d,\"deltarupt_attack\":%.4f,\"density_morphing\":%.4f,"
            "\"rnd_envelope\":%.4f,\"rnd_density\":%.4f,\"rnd_grain\":%.4f,\"rnd_time\":%.4f,"
            "\"rnd_pan\":%.4f,"
            "\"seq_on\":%d,\"seq_time\":%d,\"seq_mult\":%d,\"rnd_filter\":%d}",
            inst->detection, inst->envelope, inst->density, inst->grain_size, inst->time_warp,
            inst->mix, inst->feedback, inst->mode,
            inst->shuffle_bias, inst->pitch_range_window, inst->octave_fold,
            inst->density_curve, inst->speed_curve_exp,
            inst->arp_pattern, inst->deltarupt_attack, inst->density_morphing,
            inst->rnd_envelope, inst->rnd_density, inst->rnd_grain, inst->rnd_time,
            inst->rnd_pan,
            inst->seq_on, (int)inst->seq_time_ms, inst->seq_mult, inst->rnd_filter);
    }

    return -1;
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
