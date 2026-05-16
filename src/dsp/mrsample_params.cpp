#include "mrsample_params.h"

#include <string.h>

#define SAMPLES_ROOT "/data/UserData/UserLibrary/Samples"

static const mrsample_param_desc_t kParams[] = {
    /* sample */
    {"sample_path",   "Sample",       "filepath", 0.0f, 0.0f,   0.0f,   0.0f,  "",      NULL, SAMPLES_ROOT, ".wav", SAMPLES_ROOT, 0},
    {"sample_start",  "Start",        "float",    0.0f, 1.0f,   0.01f,  0.0f,  NULL,    NULL, NULL,         NULL,   NULL,         1},
    {"loop_mode",     "Loop",         "enum",     0.0f, 0.0f,   0.0f,   0.0f,  "off",   "[\"off\",\"on\"]", NULL, NULL, NULL, 0},
    {"loop_start",    "Loop Start",   "float",    0.0f, 1.0f,   0.01f,  0.0f,  NULL,    NULL, NULL,         NULL,   NULL,         1},
    {"loop_end",      "Loop End",     "float",    0.0f, 1.0f,   0.01f,  1.0f,  NULL,    NULL, NULL,         NULL,   NULL,         1},
    {"loop_xfade_ms", "Loop Xfade",   "float",    0.0f, 500.0f, 1.0f,   10.0f, NULL,    NULL, NULL,         NULL,   NULL,         0},

    /* tuning */
    {"root_note",     "Root Note",    "int",      0.0f, 127.0f, 1.0f,   60.0f, NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"transpose",     "Transpose",    "int",     -48.0f, 48.0f, 1.0f,   0.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"fine_tune",     "Fine Tune",    "int",    -100.0f, 100.0f,1.0f,   0.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},

    /* amp env */
    {"attack_ms",     "Attack",       "float",    0.0f, 10000.0f, 1.0f, 5.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"decay_ms",      "Decay",        "float",    0.0f, 10000.0f, 5.0f, 200.0f,NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"sustain",       "Sustain",      "float",    0.0f, 1.0f,   0.01f,  1.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"release_ms",    "Release",      "float",    0.0f, 10000.0f, 5.0f, 200.0f,NULL,    NULL, NULL,         NULL,   NULL,         0},

    /* filter */
    {"filter_type",   "Type",         "enum",     0.0f, 0.0f,   0.0f,   0.0f,  "lp",    "[\"lp\",\"bp\",\"hp\"]", NULL, NULL, NULL, 0},
    {"filter_cutoff", "Cutoff",       "float",    0.0f, 1.0f,   0.01f,  1.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"filter_res",    "Resonance",    "float",    0.0f, 1.0f,   0.01f,  0.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"filter_env_amt","Env Amt",      "float",   -1.0f, 1.0f,   0.01f,  0.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},

    /* lfo */
    {"lfo_rate_hz",   "LFO Rate",     "float",    0.05f, 20.0f, 0.05f,  1.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"lfo_depth",     "LFO Depth",    "float",    0.0f, 1.0f,   0.01f,  0.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},

    /* global */
    {"gain",          "Gain",         "float",    0.0f, 2.0f,   0.01f,  1.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
    {"polyphony",     "Polyphony",    "int",      1.0f, 16.0f,  1.0f,   8.0f,  NULL,    NULL, NULL,         NULL,   NULL,         0},
};

#define PARAM_COUNT ((int)(sizeof(kParams) / sizeof(kParams[0])))

const mrsample_param_desc_t *mrsample_params(int *count_out) {
    if (count_out) *count_out = PARAM_COUNT;
    return kParams;
}

const mrsample_param_desc_t *mrsample_find_param(const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < PARAM_COUNT; i++) {
        if (strcmp(key, kParams[i].key) == 0) return &kParams[i];
    }
    return NULL;
}
