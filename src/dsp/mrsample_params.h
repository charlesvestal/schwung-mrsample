#ifndef MRSAMPLE_PARAMS_H
#define MRSAMPLE_PARAMS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *name;
    const char *type;     /* "float", "int", "enum", "filepath" */
    float min_val;
    float max_val;
    float step;
    float default_num;
    const char *default_str;
    const char *options_json;
    const char *root;
    const char *filter;
    const char *start_path;
    int linked_to_sample;  /* nonzero => wav_position-style knob linked to sample_path */
} mrsample_param_desc_t;

const mrsample_param_desc_t *mrsample_params(int *count_out);
const mrsample_param_desc_t *mrsample_find_param(const char *key);

#ifdef __cplusplus
}
#endif

#endif
