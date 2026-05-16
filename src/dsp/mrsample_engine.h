#ifndef MRSAMPLE_ENGINE_H
#define MRSAMPLE_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MRSAMPLE_ENGINE_SAMPLE_RATE = 44100,
    MRSAMPLE_ENGINE_MAX_VOICES  = 16
};

typedef enum {
    MRSAMPLE_LOOP_OFF = 0,
    MRSAMPLE_LOOP_ON  = 1
} mrsample_loop_mode_t;

typedef enum {
    MRSAMPLE_FILTER_LP = 0,
    MRSAMPLE_FILTER_BP = 1,
    MRSAMPLE_FILTER_HP = 2
} mrsample_filter_type_t;

typedef enum {
    MRSAMPLE_ENV_OFF = 0,
    MRSAMPLE_ENV_ATTACK,
    MRSAMPLE_ENV_DECAY,
    MRSAMPLE_ENV_SUSTAIN,
    MRSAMPLE_ENV_RELEASE
} mrsample_env_stage_t;

typedef struct {
    int active;
    uint32_t age;
    int note;
    int gate;

    double sample_pos;     /* float frames into mono buffer */
    double sample_inc;

    /* amp env (linear) */
    int env_stage;
    float env;
    float attack_inc;
    float decay_inc;
    float sustain_level;
    float release_inc;
    float release_start;

    float gain;            /* note velocity * master gain */

    /* per-voice SVF state */
    float svf_ic1;
    float svf_ic2;
} mrsample_voice_t;

typedef struct {
    /* sample data — engine does not own; caller swaps pointer atomically */
    const float *sample_data;
    int sample_len;
    int sample_rate;
    int sample_channels;   /* 1 = mono, 2 = stereo interleaved */

    int polyphony;
    uint32_t voice_age_counter;

    /* params (cooked) */
    float sample_start;
    float loop_start;
    float loop_end;
    float loop_xfade_ms;

    int   root_note;
    int   transpose;
    int   fine_tune_cents;

    int   loop_mode;
    float attack_ms;
    float decay_ms;
    float sustain;
    float release_ms;

    int   filter_type;
    float filter_cutoff;   /* 0..1 normalized */
    float filter_res;
    float filter_env_amt;

    float lfo_rate_hz;
    float lfo_depth;
    float lfo_phase;       /* 0..1 */

    float master_gain;

    int sample_rate_out;

    mrsample_voice_t voices[MRSAMPLE_ENGINE_MAX_VOICES];
} mrsample_engine_t;

void mrsample_engine_init(mrsample_engine_t *e);

void mrsample_engine_set_sample(mrsample_engine_t *e,
                                const float *data, int len,
                                int sample_rate, int channels);

void mrsample_engine_note_on(mrsample_engine_t *e, int note, int velocity);
void mrsample_engine_note_off(mrsample_engine_t *e, int note);
void mrsample_engine_all_notes_off(mrsample_engine_t *e);

void mrsample_engine_render(mrsample_engine_t *e,
                            float *out_l, float *out_r, int frames);

#ifdef __cplusplus
}
#endif

#endif
