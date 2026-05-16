#include "mrsample_engine.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void mrsample_engine_init(mrsample_engine_t *e) {
    if (!e) return;
    memset(e, 0, sizeof(*e));

    e->sample_rate_out   = MRSAMPLE_ENGINE_SAMPLE_RATE;
    e->polyphony        = 8;
    e->voice_age_counter = 1;

    e->sample_start = 0.0f;
    e->loop_start   = 0.0f;
    e->loop_end     = 1.0f;
    e->loop_xfade_ms = 10.0f;

    e->root_note       = 60;
    e->transpose       = 0;
    e->fine_tune_cents = 0;

    e->loop_mode  = MRSAMPLE_LOOP_OFF;
    e->attack_ms  = 5.0f;
    e->decay_ms   = 200.0f;
    e->sustain    = 1.0f;
    e->release_ms = 200.0f;

    e->filter_type    = MRSAMPLE_FILTER_LP;
    e->filter_cutoff  = 1.0f;
    e->filter_res     = 0.0f;
    e->filter_env_amt = 0.0f;

    e->lfo_rate_hz = 1.0f;
    e->lfo_depth   = 0.0f;
    e->lfo_phase   = 0.0f;

    e->master_gain = 1.0f;
}

void mrsample_engine_set_sample(mrsample_engine_t *e,
                                const float *data, int len,
                                int sample_rate, int channels) {
    if (!e) return;
    e->sample_data    = data;
    e->sample_len     = (len > 0) ? len : 0;
    e->sample_rate    = (sample_rate > 1000) ? sample_rate : MRSAMPLE_ENGINE_SAMPLE_RATE;
    e->sample_channels = (channels == 2) ? 2 : 1;
}

static int find_voice_slot(mrsample_engine_t *e) {
    int poly = clampi(e->polyphony, 1, MRSAMPLE_ENGINE_MAX_VOICES);
    for (int i = 0; i < poly; i++) {
        if (!e->voices[i].active) return i;
    }
    int oldest = 0;
    uint32_t oldest_age = e->voices[0].age;
    for (int i = 1; i < poly; i++) {
        if (e->voices[i].age < oldest_age) {
            oldest_age = e->voices[i].age;
            oldest = i;
        }
    }
    return oldest;
}

void mrsample_engine_note_on(mrsample_engine_t *e, int note, int velocity) {
    if (!e) return;
    if (!e->sample_data || e->sample_len < 2) return;

    int slot = find_voice_slot(e);
    mrsample_voice_t *v = &e->voices[slot];
    memset(v, 0, sizeof(*v));

    v->active = 1;
    v->age = e->voice_age_counter++;
    v->note = note;
    v->gate = 1;

    /* start pos */
    int frames = e->sample_len;
    float start = clampf(e->sample_start, 0.0f, 1.0f);
    v->sample_pos = (double)start * (double)(frames - 1);

    /* pitch */
    double semis = (double)(note - e->root_note + e->transpose)
                 + (double)e->fine_tune_cents / 100.0;
    double pitch = pow(2.0, semis / 12.0);
    double sr_ratio = (double)e->sample_rate / (double)e->sample_rate_out;
    double inc = sr_ratio * pitch;
    if (inc < 0.001) inc = 0.001;
    if (inc > 64.0)  inc = 64.0;
    v->sample_inc = inc;

    /* gain from velocity, master gain applied at render */
    float vel = clampf((float)velocity / 127.0f, 0.0f, 1.0f);
    v->gain = vel;

    /* amp env init */
    float fs = (float)e->sample_rate_out;
    float a_samples = clampf(e->attack_ms,  0.0f, 100000.0f) * 0.001f * fs;
    float d_samples = clampf(e->decay_ms,   0.0f, 100000.0f) * 0.001f * fs;
    float r_samples = clampf(e->release_ms, 0.0f, 100000.0f) * 0.001f * fs;
    if (a_samples < 1.0f) a_samples = 1.0f;
    if (d_samples < 1.0f) d_samples = 1.0f;
    if (r_samples < 1.0f) r_samples = 1.0f;

    v->attack_inc  = 1.0f / a_samples;
    v->decay_inc   = 1.0f / d_samples;
    v->release_inc = 1.0f / r_samples;
    v->sustain_level = clampf(e->sustain, 0.0f, 1.0f);

    if (e->attack_ms <= 0.0f) {
        v->env = 1.0f;
        v->env_stage = MRSAMPLE_ENV_DECAY;
    } else {
        v->env = 0.0f;
        v->env_stage = MRSAMPLE_ENV_ATTACK;
    }
}

void mrsample_engine_note_off(mrsample_engine_t *e, int note) {
    if (!e) return;
    for (int i = 0; i < MRSAMPLE_ENGINE_MAX_VOICES; i++) {
        mrsample_voice_t *v = &e->voices[i];
        if (!v->active || v->note != note) continue;
        v->gate = 0;
        v->env_stage = MRSAMPLE_ENV_RELEASE;
        v->release_start = v->env;
    }
}

void mrsample_engine_all_notes_off(mrsample_engine_t *e) {
    if (!e) return;
    for (int i = 0; i < MRSAMPLE_ENGINE_MAX_VOICES; i++) e->voices[i].active = 0;
}

/* TPT state-variable filter — Vadim Zavalishin
 * Inputs:
 *   x   = input sample
 *   g   = tan(pi * fc / fs)
 *   k   = 2 - 2 * res    (res in 0..1, k goes 2 -> 0; lower k = more resonance)
 *   ic1, ic2 = state (pass by pointer)
 *   type 0=LP, 1=BP, 2=HP
 */
static inline float svf_process(float x, float g, float k,
                                float *ic1, float *ic2, int type) {
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float v3 = x - *ic2;
    float v1 = a1 * (*ic1) + a2 * v3;
    float v2 = *ic2 + g * v1;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    switch (type) {
        case MRSAMPLE_FILTER_BP: return v1;
        case MRSAMPLE_FILTER_HP: return x - k * v1 - v2;
        case MRSAMPLE_FILTER_LP:
        default:                 return v2;
    }
}

/* Map 0..1 cutoff to Hz: log scale 20 Hz .. 18 kHz */
static inline float cutoff_to_hz(float cutoff01) {
    cutoff01 = clampf(cutoff01, 0.0f, 1.0f);
    /* exp interp between 20 and 18000 */
    float lo = logf(20.0f);
    float hi = logf(18000.0f);
    return expf(lo + (hi - lo) * cutoff01);
}

/* Map res 0..1 to SVF k (2 -> ~0.05) */
static inline float res_to_k(float res01) {
    res01 = clampf(res01, 0.0f, 0.99f);
    return 2.0f - 1.95f * res01;
}

static inline float read_sample(const mrsample_engine_t *e, double pos, int *which) {
    /* mono mix on the fly if stereo */
    int idx = (int)pos;
    int n = e->sample_len;
    if (idx < 0) idx = 0;
    if (idx >= n - 1) idx = n - 2;
    double frac = pos - (double)idx;
    if (e->sample_channels == 2) {
        int i0 = idx * 2;
        int i1 = (idx + 1) * 2;
        float s0 = 0.5f * (e->sample_data[i0]     + e->sample_data[i0 + 1]);
        float s1 = 0.5f * (e->sample_data[i1]     + e->sample_data[i1 + 1]);
        if (which) *which = idx;
        return s0 + (float)((s1 - s0) * frac);
    }
    float s0 = e->sample_data[idx];
    float s1 = e->sample_data[idx + 1];
    if (which) *which = idx;
    return s0 + (float)((s1 - s0) * frac);
}

void mrsample_engine_render(mrsample_engine_t *e,
                            float *out_l, float *out_r, int frames) {
    if (!e || !out_l || !out_r || frames <= 0) return;

    for (int i = 0; i < frames; i++) {
        out_l[i] = 0.0f;
        out_r[i] = 0.0f;
    }

    if (!e->sample_data || e->sample_len < 2) return;

    float fs = (float)e->sample_rate_out;
    float lfo_inc = e->lfo_rate_hz / fs;
    float k = res_to_k(e->filter_res);

    /* loop boundaries in frame indices */
    int sample_len = e->sample_len;
    double loop_a = (double)clampf(e->loop_start, 0.0f, 1.0f) * (double)(sample_len - 1);
    double loop_b = (double)clampf(e->loop_end,   0.0f, 1.0f) * (double)(sample_len - 1);
    if (loop_b <= loop_a + 4.0) loop_b = loop_a + 4.0;
    if (loop_b > (double)(sample_len - 1)) loop_b = (double)(sample_len - 1);

    double xfade_frames = (double)clampf(e->loop_xfade_ms, 0.0f, 5000.0f) * 0.001 * (double)e->sample_rate;
    /* clamp xfade to half the loop length */
    double loop_len = loop_b - loop_a;
    if (xfade_frames > loop_len * 0.5) xfade_frames = loop_len * 0.5;
    if (xfade_frames < 0.0) xfade_frames = 0.0;

    for (int vi = 0; vi < MRSAMPLE_ENGINE_MAX_VOICES; vi++) {
        mrsample_voice_t *v = &e->voices[vi];
        if (!v->active) continue;

        for (int i = 0; i < frames; i++) {
            if (!v->active) break;

            /* envelope advance (always AHDSR) */
            switch (v->env_stage) {
                case MRSAMPLE_ENV_ATTACK:
                    v->env += v->attack_inc;
                    if (v->env >= 1.0f) {
                        v->env = 1.0f;
                        v->env_stage = MRSAMPLE_ENV_DECAY;
                    }
                    break;
                case MRSAMPLE_ENV_DECAY:
                    v->env -= v->decay_inc * (1.0f - v->sustain_level);
                    if (v->env <= v->sustain_level) {
                        v->env = v->sustain_level;
                        v->env_stage = MRSAMPLE_ENV_SUSTAIN;
                    }
                    break;
                case MRSAMPLE_ENV_SUSTAIN:
                    /* hold */
                    break;
                case MRSAMPLE_ENV_RELEASE:
                    v->env -= v->release_inc * v->release_start;
                    if (v->env <= 0.0f) { v->active = 0; break; }
                    break;
                default:
                    v->active = 0;
                    break;
            }
            if (!v->active) break;

            /* sample read with loop logic */
            float s = 0.0f;
            if (e->loop_mode == MRSAMPLE_LOOP_ON) {
                if (v->sample_pos >= loop_b) {
                    double over = v->sample_pos - loop_b;
                    v->sample_pos = loop_a + over;
                }
                if (xfade_frames > 0.5 && v->sample_pos > loop_b - xfade_frames) {
                    double x = (v->sample_pos - (loop_b - xfade_frames)) / xfade_frames;
                    if (x < 0.0) x = 0.0;
                    if (x > 1.0) x = 1.0;
                    double pos_a = loop_a + (v->sample_pos - (loop_b - xfade_frames));
                    float s_main = read_sample(e, v->sample_pos, NULL);
                    float s_wrap = read_sample(e, pos_a, NULL);
                    float g_main = cosf((float)x * 0.5f * (float)M_PI);
                    float g_wrap = sinf((float)x * 0.5f * (float)M_PI);
                    s = g_main * s_main + g_wrap * s_wrap;
                } else {
                    s = read_sample(e, v->sample_pos, NULL);
                }
            } else {
                if (v->sample_pos >= (double)(sample_len - 1)) {
                    v->active = 0;
                    break;
                }
                s = read_sample(e, v->sample_pos, NULL);
            }

            /* filter */
            float cutoff_mod = e->filter_cutoff
                             + e->filter_env_amt * v->env
                             + e->lfo_depth * sinf(2.0f * (float)M_PI * e->lfo_phase);
            float hz = cutoff_to_hz(cutoff_mod);
            float g = tanf((float)M_PI * hz / fs);
            float filtered = svf_process(s, g, k, &v->svf_ic1, &v->svf_ic2, e->filter_type);

            float value = filtered * v->env * v->gain * e->master_gain;
            out_l[i] += value;
            out_r[i] += value;

            v->sample_pos += v->sample_inc;
        }

        if (!v->active) continue;
    }

    /* advance global LFO once per block, in fractional form */
    float lfo_advance = lfo_inc * (float)frames;
    e->lfo_phase += lfo_advance;
    while (e->lfo_phase >= 1.0f) e->lfo_phase -= 1.0f;
}
