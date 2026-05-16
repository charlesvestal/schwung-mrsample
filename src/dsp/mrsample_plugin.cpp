#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include "mrsample_engine.h"
#include "mrsample_params.h"

extern "C" {
#define MOVE_PLUGIN_API_VERSION_2 2

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
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;
}

static const host_api_v1_t *g_host = NULL;

static void plugin_log(const char *msg) {
    if (!msg) return;
    if (g_host && g_host->log) {
        char line[256];
        snprintf(line, sizeof(line), "[mrsample] %s", msg);
        g_host->log(line);
    }
}

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

typedef struct {
    float *data;        /* interleaved if channels==2, else mono */
    int length;         /* frames */
    int sample_rate;
    int channels;
    /* loop region from WAV smpl chunk, normalized 0..1; -1 if absent */
    float smpl_loop_start;
    float smpl_loop_end;
    int   smpl_root_note;  /* midi_unity_note from smpl chunk; -1 if absent */
} sample_buffer_t;

typedef struct {
    char module_dir[512];
    char last_error[256];

    /* persisted-ish UI state */
    char ui_last_sample_dir[512];

    /* the live sample path */
    char sample_path[512];

    /* loaded sample data */
    sample_buffer_t sample;

    mrsample_engine_t engine;
} mrsample_instance_t;

static void set_error(mrsample_instance_t *inst, const char *msg) {
    if (!inst) return;
    if (!msg) { inst->last_error[0] = '\0'; return; }
    snprintf(inst->last_error, sizeof(inst->last_error), "%s", msg);
}

static void free_sample(sample_buffer_t *s) {
    if (!s) return;
    free(s->data);
    s->data = NULL;
    s->length = 0;
    s->sample_rate = 0;
    s->channels = 0;
    s->smpl_loop_start = -1.0f;
    s->smpl_loop_end   = -1.0f;
    s->smpl_root_note  = -1;
}

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i24_le(const uint8_t *p) {
    int32_t v = ((int32_t)p[0]) | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (v & 0x00800000) v |= ~0x00FFFFFF;
    return v;
}

/* Load WAV — interleaved float, preserves stereo. Parses smpl chunk if present. */
static int load_wav(const char *path, sample_buffer_t *out, char *err, int err_len) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->smpl_loop_start = -1.0f;
    out->smpl_loop_end   = -1.0f;
    out->smpl_root_note  = -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, err_len, "Could not open: %s", path);
        return -1;
    }

    uint8_t riff[12];
    if (fread(riff, 1, 12, fp) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(fp);
        snprintf(err, err_len, "Not a RIFF/WAVE file");
        return -1;
    }

    int have_fmt = 0, have_data = 0;
    uint16_t audio_format = 0, channels = 0, block_align = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0;
    long data_offset = 0;
    uint32_t smpl_loop_start_frames = 0, smpl_loop_end_frames = 0;
    int have_smpl_loop = 0;
    int smpl_unity_note = -1;

    while (!feof(fp)) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, fp) != 8) break;
        uint32_t chunk_size = rd_u32_le(chdr + 4);
        long chunk_data_pos = ftell(fp);

        if (memcmp(chdr, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t want = (chunk_size < sizeof(fmt)) ? chunk_size : (uint32_t)sizeof(fmt);
            if (fread(fmt, 1, want, fp) != want || want < 16) {
                fclose(fp);
                snprintf(err, err_len, "Corrupt fmt chunk");
                return -1;
            }
            audio_format = rd_u16_le(fmt + 0);
            channels = rd_u16_le(fmt + 2);
            sample_rate = rd_u32_le(fmt + 4);
            block_align = rd_u16_le(fmt + 12);
            bits_per_sample = rd_u16_le(fmt + 14);
            have_fmt = 1;
        } else if (memcmp(chdr, "data", 4) == 0) {
            data_size = chunk_size;
            data_offset = chunk_data_pos;
            have_data = 1;
        } else if (memcmp(chdr, "smpl", 4) == 0) {
            /* parse smpl chunk: 36 bytes fixed header, then loops */
            if (chunk_size >= 36) {
                uint8_t smpl[36];
                if (fread(smpl, 1, 36, fp) == 36) {
                    smpl_unity_note = (int)rd_u32_le(smpl + 12);
                    uint32_t num_loops = rd_u32_le(smpl + 28);
                    if (num_loops > 0) {
                        uint8_t loop[24];
                        if (fread(loop, 1, 24, fp) == 24) {
                            smpl_loop_start_frames = rd_u32_le(loop + 8);
                            smpl_loop_end_frames   = rd_u32_le(loop + 12);
                            have_smpl_loop = 1;
                        }
                    }
                }
            }
        }

        long next = chunk_data_pos + (long)chunk_size + (chunk_size & 1u);
        if (fseek(fp, next, SEEK_SET) != 0) break;
    }

    if (!have_fmt || !have_data) {
        fclose(fp);
        snprintf(err, err_len, "WAV missing fmt/data chunk");
        return -1;
    }
    if (channels < 1 || channels > 2 || block_align < 1 || data_size < (uint32_t)block_align) {
        fclose(fp);
        snprintf(err, err_len, "Invalid WAV layout (need 1 or 2 channels)");
        return -1;
    }
    if (!((audio_format == 1 && (bits_per_sample == 8 || bits_per_sample == 16 ||
                                 bits_per_sample == 24 || bits_per_sample == 32)) ||
          (audio_format == 3 && bits_per_sample == 32))) {
        fclose(fp);
        snprintf(err, err_len, "Unsupported WAV (need PCM 8/16/24/32 or float32)");
        return -1;
    }

    if (fseek(fp, data_offset, SEEK_SET) != 0) {
        fclose(fp);
        snprintf(err, err_len, "Failed to seek WAV data");
        return -1;
    }

    uint8_t *raw = (uint8_t *)malloc(data_size);
    if (!raw) {
        fclose(fp);
        snprintf(err, err_len, "Out of memory");
        return -1;
    }
    if (fread(raw, 1, data_size, fp) != data_size) {
        free(raw);
        fclose(fp);
        snprintf(err, err_len, "Failed to read WAV data");
        return -1;
    }
    fclose(fp);

    int frames = (int)(data_size / block_align);
    if (frames <= 0) {
        free(raw);
        snprintf(err, err_len, "Empty WAV data");
        return -1;
    }

    out->data = (float *)malloc((size_t)frames * channels * sizeof(float));
    if (!out->data) {
        free(raw);
        snprintf(err, err_len, "Out of memory allocating samples");
        return -1;
    }

    int bps = bits_per_sample / 8;
    for (int i = 0; i < frames; i++) {
        const uint8_t *frame_ptr = raw + (size_t)i * block_align;
        for (int ch = 0; ch < channels; ch++) {
            const uint8_t *sp = frame_ptr + ch * bps;
            float v = 0.0f;
            if (audio_format == 1) {
                if (bits_per_sample == 8) {
                    v = ((int)sp[0] - 128) / 128.0f;
                } else if (bits_per_sample == 16) {
                    v = (int16_t)rd_u16_le(sp) / 32768.0f;
                } else if (bits_per_sample == 24) {
                    v = (float)rd_i24_le(sp) / 8388608.0f;
                } else {
                    v = (float)(int32_t)rd_u32_le(sp) / 2147483648.0f;
                }
            } else {
                float x;
                memcpy(&x, sp, sizeof(float));
                v = x;
            }
            out->data[i * channels + ch] = clampf(v, -1.0f, 1.0f);
        }
    }
    free(raw);

    out->length = frames;
    out->sample_rate = (int)sample_rate;
    out->channels = channels;

    if (have_smpl_loop && smpl_loop_end_frames > smpl_loop_start_frames &&
        smpl_loop_end_frames < (uint32_t)frames) {
        out->smpl_loop_start = (float)smpl_loop_start_frames / (float)(frames - 1);
        out->smpl_loop_end   = (float)smpl_loop_end_frames   / (float)(frames - 1);
    }
    out->smpl_root_note = smpl_unity_note;

    return 0;
}

enum {
    FMT_UNKNOWN = 0,
    FMT_WAV,
    FMT_MP3,
    FMT_FLAC,
    FMT_AIFF
};

static int classify_extension(const char *path) {
    if (!path) return FMT_UNKNOWN;
    const char *dot = strrchr(path, '.');
    if (!dot) return FMT_UNKNOWN;
    if (strcasecmp(dot, ".wav") == 0)  return FMT_WAV;
    if (strcasecmp(dot, ".mp3") == 0)  return FMT_MP3;
    if (strcasecmp(dot, ".flac") == 0) return FMT_FLAC;
    if (strcasecmp(dot, ".aif") == 0 || strcasecmp(dot, ".aiff") == 0) return FMT_AIFF;
    return FMT_UNKNOWN;
}

static int load_mp3(const char *path, sample_buffer_t *out, char *err, int err_len) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->smpl_loop_start = -1.0f;
    out->smpl_loop_end   = -1.0f;
    out->smpl_root_note  = -1;

    drmp3_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    drmp3_uint64 total_frames = 0;
    float *pcm = drmp3_open_file_and_read_pcm_frames_f32(path, &cfg, &total_frames, NULL);
    if (!pcm) {
        snprintf(err, err_len, "Could not decode MP3: %s", path);
        return -1;
    }
    if (cfg.channels < 1 || cfg.channels > 2) {
        drmp3_free(pcm, NULL);
        snprintf(err, err_len, "Unsupported MP3 channels: %u", (unsigned)cfg.channels);
        return -1;
    }
    out->data = pcm;
    out->length = (int)total_frames;
    out->sample_rate = (int)cfg.sampleRate;
    out->channels = (int)cfg.channels;
    return 0;
}

static int load_flac(const char *path, sample_buffer_t *out, char *err, int err_len) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->smpl_loop_start = -1.0f;
    out->smpl_loop_end   = -1.0f;
    out->smpl_root_note  = -1;

    unsigned int channels = 0;
    unsigned int sample_rate = 0;
    drflac_uint64 total_frames = 0;
    float *pcm = drflac_open_file_and_read_pcm_frames_f32(path, &channels, &sample_rate,
                                                          &total_frames, NULL);
    if (!pcm) {
        snprintf(err, err_len, "Could not decode FLAC: %s", path);
        return -1;
    }
    if (channels < 1 || channels > 2) {
        drflac_free(pcm, NULL);
        snprintf(err, err_len, "Unsupported FLAC channels: %u", channels);
        return -1;
    }
    out->data = pcm;
    out->length = (int)total_frames;
    out->sample_rate = (int)sample_rate;
    out->channels = (int)channels;
    return 0;
}

/* big-endian readers for AIFF */
static uint16_t rd_u16_be(const uint8_t *p) { return ((uint16_t)p[0] << 8) | (uint16_t)p[1]; }
static uint32_t rd_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static int32_t rd_i24_be(const uint8_t *p) {
    int32_t v = ((int32_t)p[0] << 16) | ((int32_t)p[1] << 8) | (int32_t)p[2];
    if (v & 0x00800000) v |= ~0x00FFFFFF;
    return v;
}

/* 80-bit IEEE 754 extended → uint32, for AIFF sample rate */
static uint32_t rd_extended_be(const uint8_t *p) {
    uint16_t expon = ((uint16_t)(p[0] & 0x7F) << 8) | p[1];
    uint64_t hi_mantissa = ((uint64_t)p[2] << 56) | ((uint64_t)p[3] << 48) |
                           ((uint64_t)p[4] << 40) | ((uint64_t)p[5] << 32) |
                           ((uint64_t)p[6] << 24) | ((uint64_t)p[7] << 16) |
                           ((uint64_t)p[8] << 8)  |  (uint64_t)p[9];
    if (expon == 0 && hi_mantissa == 0) return 0;
    int e = (int)expon - 16383 - 63;
    if (e > 0) return (uint32_t)(hi_mantissa << e);
    if (e < 0 && -e < 64) return (uint32_t)(hi_mantissa >> (-e));
    return (uint32_t)hi_mantissa;
}

static int load_aif(const char *path, sample_buffer_t *out, char *err, int err_len) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->smpl_loop_start = -1.0f;
    out->smpl_loop_end   = -1.0f;
    out->smpl_root_note  = -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, err_len, "Could not open: %s", path); return -1; }

    uint8_t form[12];
    if (fread(form, 1, 12, fp) != 12 ||
        memcmp(form, "FORM", 4) != 0 ||
        (memcmp(form + 8, "AIFF", 4) != 0 && memcmp(form + 8, "AIFC", 4) != 0)) {
        fclose(fp);
        snprintf(err, err_len, "Not an AIFF file");
        return -1;
    }
    int is_aifc = (memcmp(form + 8, "AIFC", 4) == 0);

    int have_comm = 0, have_ssnd = 0;
    uint16_t channels = 0, bits_per_sample = 0;
    uint32_t num_frames = 0, sample_rate = 0;
    long ssnd_data_offset = 0;
    uint32_t ssnd_size = 0;
    int is_float = 0;

    while (!feof(fp)) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, fp) != 8) break;
        uint32_t chunk_size = rd_u32_be(chdr + 4);
        long chunk_data_pos = ftell(fp);

        if (memcmp(chdr, "COMM", 4) == 0) {
            uint8_t comm[40];
            uint32_t want = (chunk_size < sizeof(comm)) ? chunk_size : (uint32_t)sizeof(comm);
            if (fread(comm, 1, want, fp) != want || want < 18) {
                fclose(fp);
                snprintf(err, err_len, "Corrupt AIFF COMM");
                return -1;
            }
            channels        = rd_u16_be(comm + 0);
            num_frames      = rd_u32_be(comm + 2);
            bits_per_sample = rd_u16_be(comm + 6);
            sample_rate     = rd_extended_be(comm + 8);
            if (is_aifc && want >= 22) {
                if (memcmp(comm + 18, "fl32", 4) == 0 || memcmp(comm + 18, "FL32", 4) == 0 ||
                    memcmp(comm + 18, "fl64", 4) == 0) {
                    is_float = 1;
                }
            }
            have_comm = 1;
        } else if (memcmp(chdr, "SSND", 4) == 0) {
            uint8_t ssnd_hdr[8];
            if (fread(ssnd_hdr, 1, 8, fp) != 8) {
                fclose(fp);
                snprintf(err, err_len, "Corrupt AIFF SSND");
                return -1;
            }
            uint32_t ssnd_offset = rd_u32_be(ssnd_hdr + 0);
            ssnd_data_offset = chunk_data_pos + 8 + (long)ssnd_offset;
            ssnd_size = chunk_size > (8 + ssnd_offset) ? chunk_size - 8 - ssnd_offset : 0;
            have_ssnd = 1;
        }

        long next = chunk_data_pos + (long)chunk_size + (chunk_size & 1u);
        if (fseek(fp, next, SEEK_SET) != 0) break;
    }

    if (!have_comm || !have_ssnd) {
        fclose(fp);
        snprintf(err, err_len, "AIFF missing COMM/SSND");
        return -1;
    }
    if (channels < 1 || channels > 2) {
        fclose(fp);
        snprintf(err, err_len, "Unsupported AIFF channels (need 1 or 2)");
        return -1;
    }
    if (!is_float && bits_per_sample != 8 && bits_per_sample != 16 &&
        bits_per_sample != 24 && bits_per_sample != 32) {
        fclose(fp);
        snprintf(err, err_len, "Unsupported AIFF bit depth: %u", (unsigned)bits_per_sample);
        return -1;
    }

    if (fseek(fp, ssnd_data_offset, SEEK_SET) != 0) {
        fclose(fp);
        snprintf(err, err_len, "Failed to seek AIFF data");
        return -1;
    }

    int bytes_per_sample = bits_per_sample / 8;
    int block_align = bytes_per_sample * channels;
    if (num_frames == 0) num_frames = ssnd_size / (uint32_t)block_align;
    uint32_t total_bytes = (uint32_t)num_frames * (uint32_t)block_align;
    if (total_bytes == 0) {
        fclose(fp);
        snprintf(err, err_len, "Empty AIFF data");
        return -1;
    }

    uint8_t *raw = (uint8_t *)malloc(total_bytes);
    if (!raw) { fclose(fp); snprintf(err, err_len, "Out of memory"); return -1; }
    if (fread(raw, 1, total_bytes, fp) != total_bytes) {
        free(raw); fclose(fp);
        snprintf(err, err_len, "Failed to read AIFF data");
        return -1;
    }
    fclose(fp);

    out->data = (float *)malloc((size_t)num_frames * channels * sizeof(float));
    if (!out->data) { free(raw); snprintf(err, err_len, "Out of memory"); return -1; }

    for (uint32_t i = 0; i < num_frames; i++) {
        const uint8_t *frame_ptr = raw + (size_t)i * block_align;
        for (int ch = 0; ch < channels; ch++) {
            const uint8_t *sp = frame_ptr + ch * bytes_per_sample;
            float v = 0.0f;
            if (is_float) {
                if (bits_per_sample == 32) {
                    uint32_t u = rd_u32_be(sp);
                    memcpy(&v, &u, 4);
                }
            } else {
                if (bits_per_sample == 8) {
                    v = (int8_t)sp[0] / 128.0f;
                } else if (bits_per_sample == 16) {
                    v = (int16_t)rd_u16_be(sp) / 32768.0f;
                } else if (bits_per_sample == 24) {
                    v = (float)rd_i24_be(sp) / 8388608.0f;
                } else {
                    v = (float)(int32_t)rd_u32_be(sp) / 2147483648.0f;
                }
            }
            out->data[i * channels + ch] = clampf(v, -1.0f, 1.0f);
        }
    }
    free(raw);

    out->length = (int)num_frames;
    out->sample_rate = (int)sample_rate;
    out->channels = (int)channels;
    return 0;
}

static int load_sample(const char *path, sample_buffer_t *out, char *err, int err_len) {
    int fmt = classify_extension(path);
    switch (fmt) {
        case FMT_WAV:  return load_wav(path, out, err, err_len);
        case FMT_MP3:  return load_mp3(path, out, err, err_len);
        case FMT_FLAC: return load_flac(path, out, err, err_len);
        case FMT_AIFF: return load_aif(path, out, err, err_len);
        default:
            snprintf(err, err_len, "Unsupported file extension (need .wav/.mp3/.flac/.aif/.aiff)");
            return -1;
    }
}

/* lightweight JSON helpers — same shape as MrDrums */
static int json_get_number(const char *json, const char *key, float *out) {
    if (!json || !key || !out) return -1;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    char *endp = NULL;
    double v = strtod(p, &endp);
    if (!endp || endp == p) return -1;
    *out = (float)v;
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len <= 1) return -1;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < out_len - 1) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return (*p == '"') ? 0 : -1;
}

static int json_escape(const char *src, char *dst, int dst_len) {
    if (!src || !dst || dst_len <= 0) return 0;
    int o = 0;
    for (int i = 0; src[i] && o < dst_len - 1; i++) {
        char c = src[i];
        if ((c == '\\' || c == '"') && o < dst_len - 2) dst[o++] = '\\';
        dst[o++] = c;
    }
    dst[o] = '\0';
    return o;
}

/* enum parse/format */
static int parse_loop_mode(const char *val) {
    if (!val) return MRSAMPLE_LOOP_OFF;
    if (strcasecmp(val, "on") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0)
        return MRSAMPLE_LOOP_ON;
    return MRSAMPLE_LOOP_OFF;
}
static const char *loop_mode_to_string(int m) {
    return (m == MRSAMPLE_LOOP_ON) ? "on" : "off";
}

static int parse_filter_type(const char *val) {
    if (!val) return MRSAMPLE_FILTER_LP;
    if (strcasecmp(val, "bp") == 0) return MRSAMPLE_FILTER_BP;
    if (strcasecmp(val, "hp") == 0) return MRSAMPLE_FILTER_HP;
    return MRSAMPLE_FILTER_LP;
}
static const char *filter_type_to_string(int t) {
    if (t == MRSAMPLE_FILTER_BP) return "bp";
    if (t == MRSAMPLE_FILTER_HP) return "hp";
    return "lp";
}

static int set_sample_path(mrsample_instance_t *inst, const char *path);

static int set_param_value(mrsample_instance_t *inst, const char *key, const char *val) {
    if (!inst || !key || !val) return 0;
    mrsample_engine_t *e = &inst->engine;

    if (strcmp(key, "sample_path") == 0)   { return set_sample_path(inst, val) >= 0 ? 1 : 1; }
    if (strcmp(key, "sample_start") == 0)  { e->sample_start    = clampf((float)atof(val), 0.0f, 1.0f); return 1; }
    if (strcmp(key, "loop_start") == 0)    { e->loop_start      = clampf((float)atof(val), 0.0f, 1.0f); return 1; }
    if (strcmp(key, "loop_end") == 0)      { e->loop_end        = clampf((float)atof(val), 0.0f, 1.0f); return 1; }
    if (strcmp(key, "loop_xfade_ms") == 0) { e->loop_xfade_ms   = clampf((float)atof(val), 0.0f, 500.0f); return 1; }

    if (strcmp(key, "root_note") == 0)     { e->root_note       = clampi(atoi(val), 0, 127); return 1; }
    if (strcmp(key, "transpose") == 0)     { e->transpose       = clampi(atoi(val), -48, 48); return 1; }
    if (strcmp(key, "fine_tune") == 0)     { e->fine_tune_cents = clampi(atoi(val), -100, 100); return 1; }

    if (strcmp(key, "loop_mode") == 0)     { e->loop_mode = parse_loop_mode(val); return 1; }
    if (strcmp(key, "attack_ms") == 0)     { e->attack_ms = clampf((float)atof(val), 0.0f, 10000.0f); return 1; }
    if (strcmp(key, "decay_ms") == 0)      { e->decay_ms  = clampf((float)atof(val), 0.0f, 10000.0f); return 1; }
    if (strcmp(key, "sustain") == 0)       { e->sustain   = clampf((float)atof(val), 0.0f, 1.0f); return 1; }
    if (strcmp(key, "release_ms") == 0)    { e->release_ms= clampf((float)atof(val), 0.0f, 10000.0f); return 1; }

    if (strcmp(key, "filter_type") == 0)   { e->filter_type    = parse_filter_type(val); return 1; }
    if (strcmp(key, "filter_cutoff") == 0) { e->filter_cutoff  = clampf((float)atof(val), 0.0f, 1.0f); return 1; }
    if (strcmp(key, "filter_res") == 0)    { e->filter_res     = clampf((float)atof(val), 0.0f, 1.0f); return 1; }
    if (strcmp(key, "filter_env_amt") == 0){ e->filter_env_amt = clampf((float)atof(val), -1.0f, 1.0f); return 1; }

    if (strcmp(key, "lfo_rate_hz") == 0)   { e->lfo_rate_hz = clampf((float)atof(val), 0.05f, 20.0f); return 1; }
    if (strcmp(key, "lfo_depth") == 0)     { e->lfo_depth   = clampf((float)atof(val), 0.0f, 1.0f); return 1; }

    if (strcmp(key, "gain") == 0)          { e->master_gain = clampf((float)atof(val), 0.0f, 2.0f); return 1; }
    if (strcmp(key, "polyphony") == 0)     { e->polyphony   = clampi(atoi(val), 1, MRSAMPLE_ENGINE_MAX_VOICES); return 1; }

    if (strcmp(key, "ui_last_sample_dir") == 0) {
        snprintf(inst->ui_last_sample_dir, sizeof(inst->ui_last_sample_dir), "%s", val);
        return 1;
    }

    return 0;
}

static int get_param_value(mrsample_instance_t *inst, const char *key, char *buf, int buf_len) {
    if (!inst || !key || !buf || buf_len <= 0) return -1;
    const mrsample_engine_t *e = &inst->engine;

    if (strcmp(key, "sample_path") == 0)   return snprintf(buf, buf_len, "%s", inst->sample_path);
    if (strcmp(key, "sample_start") == 0)  return snprintf(buf, buf_len, "%.4f", e->sample_start);
    if (strcmp(key, "loop_start") == 0)    return snprintf(buf, buf_len, "%.4f", e->loop_start);
    if (strcmp(key, "loop_end") == 0)      return snprintf(buf, buf_len, "%.4f", e->loop_end);
    if (strcmp(key, "loop_xfade_ms") == 0) return snprintf(buf, buf_len, "%.4f", e->loop_xfade_ms);

    if (strcmp(key, "root_note") == 0)     return snprintf(buf, buf_len, "%d", e->root_note);
    if (strcmp(key, "transpose") == 0)     return snprintf(buf, buf_len, "%d", e->transpose);
    if (strcmp(key, "fine_tune") == 0)     return snprintf(buf, buf_len, "%d", e->fine_tune_cents);

    if (strcmp(key, "loop_mode") == 0)     return snprintf(buf, buf_len, "%s", loop_mode_to_string(e->loop_mode));
    if (strcmp(key, "attack_ms") == 0)     return snprintf(buf, buf_len, "%.4f", e->attack_ms);
    if (strcmp(key, "decay_ms") == 0)      return snprintf(buf, buf_len, "%.4f", e->decay_ms);
    if (strcmp(key, "sustain") == 0)       return snprintf(buf, buf_len, "%.4f", e->sustain);
    if (strcmp(key, "release_ms") == 0)    return snprintf(buf, buf_len, "%.4f", e->release_ms);

    if (strcmp(key, "filter_type") == 0)   return snprintf(buf, buf_len, "%s", filter_type_to_string(e->filter_type));
    if (strcmp(key, "filter_cutoff") == 0) return snprintf(buf, buf_len, "%.4f", e->filter_cutoff);
    if (strcmp(key, "filter_res") == 0)    return snprintf(buf, buf_len, "%.4f", e->filter_res);
    if (strcmp(key, "filter_env_amt") == 0)return snprintf(buf, buf_len, "%.4f", e->filter_env_amt);

    if (strcmp(key, "lfo_rate_hz") == 0)   return snprintf(buf, buf_len, "%.4f", e->lfo_rate_hz);
    if (strcmp(key, "lfo_depth") == 0)     return snprintf(buf, buf_len, "%.4f", e->lfo_depth);

    if (strcmp(key, "gain") == 0)          return snprintf(buf, buf_len, "%.4f", e->master_gain);
    if (strcmp(key, "polyphony") == 0)     return snprintf(buf, buf_len, "%d", e->polyphony);

    if (strcmp(key, "ui_last_sample_dir") == 0) return snprintf(buf, buf_len, "%s", inst->ui_last_sample_dir);

    return -1;
}

static void clear_sample(mrsample_instance_t *inst) {
    if (!inst) return;
    mrsample_engine_set_sample(&inst->engine, NULL, 0, MRSAMPLE_ENGINE_SAMPLE_RATE, 1);
    free_sample(&inst->sample);
}

static int set_sample_path(mrsample_instance_t *inst, const char *path) {
    if (!inst || !path) return -1;
    snprintf(inst->sample_path, sizeof(inst->sample_path), "%s", path);

    if (!path[0]) {
        clear_sample(inst);
        set_error(inst, NULL);
        return 0;
    }
    if (classify_extension(path) != FMT_UNKNOWN) {
        snprintf(inst->ui_last_sample_dir, sizeof(inst->ui_last_sample_dir), "%s", path);
    } else {
        clear_sample(inst);
        set_error(inst, "Selected file must be .wav/.mp3/.flac/.aif/.aiff");
        return -1;
    }

    char resolved[1024];
    if (path[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", path);
    } else {
        snprintf(resolved, sizeof(resolved), "%s/%s", inst->module_dir, path);
    }

    sample_buffer_t loaded;
    char err[256];
    if (load_sample(resolved, &loaded, err, sizeof(err)) != 0) {
        clear_sample(inst);
        set_error(inst, err);
        return -1;
    }

    /* Swap in new buffer, free old */
    free_sample(&inst->sample);
    inst->sample = loaded;
    mrsample_engine_set_sample(&inst->engine,
                               inst->sample.data,
                               inst->sample.length,
                               inst->sample.sample_rate,
                               inst->sample.channels);

    /* Seed loop points / root note from smpl chunk if present; also auto-enable looping */
    if (loaded.smpl_loop_start >= 0.0f && loaded.smpl_loop_end > loaded.smpl_loop_start) {
        inst->engine.loop_start = loaded.smpl_loop_start;
        inst->engine.loop_end   = loaded.smpl_loop_end;
        inst->engine.loop_mode  = MRSAMPLE_LOOP_ON;
    }
    if (loaded.smpl_root_note >= 0 && loaded.smpl_root_note <= 127) {
        inst->engine.root_note = loaded.smpl_root_note;
    }

    set_error(inst, NULL);
    return 0;
}

static void apply_defaults(mrsample_instance_t *inst) {
    if (!inst) return;
    int count = 0;
    const mrsample_param_desc_t *ps = mrsample_params(&count);
    for (int i = 0; i < count; i++) {
        const mrsample_param_desc_t *p = &ps[i];
        char num_buf[64];
        const char *value = p->default_str;
        if (!value) {
            snprintf(num_buf, sizeof(num_buf), "%.6g", p->default_num);
            value = num_buf;
        }
        set_param_value(inst, p->key, value);
    }
    inst->ui_last_sample_dir[0] = '\0';
}

static void apply_state_json(mrsample_instance_t *inst, const char *json) {
    if (!inst || !json || !json[0]) return;

    int count = 0;
    const mrsample_param_desc_t *ps = mrsample_params(&count);
    for (int i = 0; i < count; i++) {
        const mrsample_param_desc_t *p = &ps[i];
        if (strcmp(p->type, "enum") == 0 || strcmp(p->type, "filepath") == 0) {
            char sv[512];
            if (json_get_string(json, p->key, sv, sizeof(sv)) == 0) {
                set_param_value(inst, p->key, sv);
                continue;
            }
            float n;
            if (json_get_number(json, p->key, &n) == 0) {
                char nb[64];
                snprintf(nb, sizeof(nb), "%.6g", n);
                set_param_value(inst, p->key, nb);
            }
        } else {
            float n;
            if (json_get_number(json, p->key, &n) == 0) {
                char nb[64];
                snprintf(nb, sizeof(nb), "%.6g", n);
                set_param_value(inst, p->key, nb);
            }
        }
    }

    char last_dir[512];
    if (json_get_string(json, "ui_last_sample_dir", last_dir, sizeof(last_dir)) == 0) {
        set_param_value(inst, "ui_last_sample_dir", last_dir);
    }
}

static void *v2_create_instance(const char *module_dir, const char *json_defaults) {
    mrsample_instance_t *inst = (mrsample_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) return NULL;

    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir ? module_dir : "");
    mrsample_engine_init(&inst->engine);
    free_sample(&inst->sample);
    set_error(inst, NULL);

    apply_defaults(inst);
    apply_state_json(inst, json_defaults);

    plugin_log("instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    mrsample_instance_t *inst = (mrsample_instance_t *)instance;
    if (!inst) return;
    free_sample(&inst->sample);
    free(inst);
    plugin_log("instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    mrsample_instance_t *inst = (mrsample_instance_t *)instance;
    if (!inst || !msg || len < 1) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t d1 = (len > 1) ? msg[1] : 0;
    uint8_t d2 = (len > 2) ? msg[2] : 0;

    if (status == 0x90) {
        if (d2 > 0) mrsample_engine_note_on(&inst->engine, d1, d2);
        else        mrsample_engine_note_off(&inst->engine, d1);
    } else if (status == 0x80) {
        mrsample_engine_note_off(&inst->engine, d1);
    } else if (status == 0xB0) {
        if (d1 == 120 || d1 == 123) mrsample_engine_all_notes_off(&inst->engine);
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    mrsample_instance_t *inst = (mrsample_instance_t *)instance;
    if (!inst || !key || !val) return;
    if (strcmp(key, "state") == 0) { apply_state_json(inst, val); return; }
    if (strcmp(key, "all_notes_off") == 0) { mrsample_engine_all_notes_off(&inst->engine); return; }
    set_param_value(inst, key, val);
}

static int append_state_kv(mrsample_instance_t *inst,
                           char *buf, int buf_len, int *off,
                           const char *key, const char *type, int *first) {
    char value[1024];
    if (get_param_value(inst, key, value, sizeof(value)) < 0) return -1;
    int o = *off;
    if (!*first) o += snprintf(buf + o, buf_len - o, ",");
    if (strcmp(type, "enum") == 0 || strcmp(type, "filepath") == 0) {
        char esc[1200];
        json_escape(value, esc, sizeof(esc));
        o += snprintf(buf + o, buf_len - o, "\"%s\":\"%s\"", key, esc);
    } else if (strcmp(type, "int") == 0) {
        o += snprintf(buf + o, buf_len - o, "\"%s\":%d", key, atoi(value));
    } else {
        o += snprintf(buf + o, buf_len - o, "\"%s\":%.6g", key, atof(value));
    }
    *off = o;
    *first = 0;
    return 0;
}

static int build_state_json(mrsample_instance_t *inst, char *buf, int buf_len) {
    if (!buf || buf_len <= 2) return -1;
    int off = 0, first = 1;
    off += snprintf(buf + off, buf_len - off, "{");
    int count = 0;
    const mrsample_param_desc_t *ps = mrsample_params(&count);
    for (int i = 0; i < count; i++) {
        append_state_kv(inst, buf, buf_len, &off, ps[i].key, ps[i].type, &first);
    }
    append_state_kv(inst, buf, buf_len, &off, "ui_last_sample_dir", "filepath", &first);
    off += snprintf(buf + off, buf_len - off, "}");
    return (off < buf_len) ? off : -1;
}

static int build_chain_params_json(mrsample_instance_t *inst, char *buf, int buf_len) {
    if (!buf || buf_len <= 2) return -1;
    int off = 0, first = 1;
    off += snprintf(buf + off, buf_len - off, "[");

    int count = 0;
    const mrsample_param_desc_t *ps = mrsample_params(&count);
    for (int i = 0; i < count; i++) {
        const mrsample_param_desc_t *p = &ps[i];
        if (!first) off += snprintf(buf + off, buf_len - off, ",");
        first = 0;

        if (strcmp(p->type, "filepath") == 0) {
            const char *start_path = (inst && inst->ui_last_sample_dir[0])
                                   ? inst->ui_last_sample_dir
                                   : (p->start_path ? p->start_path : "/data/UserData/UserLibrary/Samples");
            off += snprintf(buf + off, buf_len - off,
                "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"filepath\",\"root\":\"%s\",\"start_path\":\"%s\","
                "\"filter\":[\".wav\",\".mp3\",\".flac\",\".aif\",\".aiff\"],\"live_preview\":true}",
                p->key, p->name,
                p->root ? p->root : "/data/UserData/UserLibrary/Samples",
                start_path);
        } else if (strcmp(p->type, "enum") == 0) {
            off += snprintf(buf + off, buf_len - off,
                "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"enum\",\"options\":%s,\"default\":\"%s\"}",
                p->key, p->name,
                p->options_json ? p->options_json : "[]",
                p->default_str ? p->default_str : "");
        } else if (p->linked_to_sample) {
            const char *type = strcmp(p->type, "int") == 0 ? "int" : "float";
            off += snprintf(buf + off, buf_len - off,
                "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"ui_type\":\"wav_position\",\"mode\":\"start\",\"filepath_param\":\"sample_path\",\"min\":%g,\"max\":%g,\"step\":%g,\"shift_increment_multiplier\":0.25}",
                p->key, p->name, type, p->min_val, p->max_val,
                p->step > 0.0f ? p->step : 1.0f);
        } else {
            const char *type = strcmp(p->type, "int") == 0 ? "int" : "float";
            off += snprintf(buf + off, buf_len - off,
                "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g}",
                p->key, p->name, type, p->min_val, p->max_val,
                p->step > 0.0f ? p->step : 1.0f);
        }
    }

    off += snprintf(buf + off, buf_len - off, "]");
    return (off < buf_len) ? off : -1;
}

static int build_ui_hierarchy(mrsample_instance_t *inst, char *buf, int buf_len) {
    (void)inst;
    int n = snprintf(buf, (size_t)buf_len,
        "{"
            "\"levels\":{"
                "\"root\":{"
                    "\"name\":\"MrSample\","
                    "\"params\":["
                        "{\"label\":\"Sample\",\"level\":\"sample\"},"
                        "{\"label\":\"Amp Env\",\"level\":\"amp\"},"
                        "{\"label\":\"Filter\",\"level\":\"filter\"},"
                        "{\"label\":\"LFO\",\"level\":\"lfo\"},"
                        "{\"label\":\"Tuning\",\"level\":\"tuning\"},"
                        "{\"label\":\"Global\",\"level\":\"global\"}"
                    "],"
                    "\"knobs\":[\"sample_start\",\"attack_ms\",\"decay_ms\",\"sustain\",\"release_ms\",\"filter_cutoff\",\"filter_res\",\"gain\"]"
                "},"
                "\"sample\":{"
                    "\"name\":\"Sample\","
                    "\"params\":["
                        "\"sample_path\","
                        "\"sample_start\","
                        "\"loop_mode\","
                        "{\"key\":\"loop_start\",\"visible_if\":{\"param\":\"loop_mode\",\"equals\":\"on\"}},"
                        "{\"key\":\"loop_end\",\"visible_if\":{\"param\":\"loop_mode\",\"equals\":\"on\"}},"
                        "{\"key\":\"loop_xfade_ms\",\"visible_if\":{\"param\":\"loop_mode\",\"equals\":\"on\"}}"
                    "],"
                    "\"knobs\":[\"sample_start\",\"loop_mode\",\"loop_start\",\"loop_end\",\"loop_xfade_ms\"]"
                "},"
                "\"amp\":{"
                    "\"name\":\"Amp Env\","
                    "\"params\":[\"attack_ms\",\"decay_ms\",\"sustain\",\"release_ms\"],"
                    "\"knobs\":[\"attack_ms\",\"decay_ms\",\"sustain\",\"release_ms\"]"
                "},"
                "\"filter\":{"
                    "\"name\":\"Filter\","
                    "\"params\":[\"filter_type\",\"filter_cutoff\",\"filter_res\",\"filter_env_amt\"],"
                    "\"knobs\":[\"filter_type\",\"filter_cutoff\",\"filter_res\",\"filter_env_amt\"]"
                "},"
                "\"lfo\":{"
                    "\"name\":\"LFO\","
                    "\"params\":[\"lfo_rate_hz\",\"lfo_depth\"],"
                    "\"knobs\":[\"lfo_rate_hz\",\"lfo_depth\"]"
                "},"
                "\"tuning\":{"
                    "\"name\":\"Tuning\","
                    "\"params\":[\"root_note\",\"transpose\",\"fine_tune\"],"
                    "\"knobs\":[\"root_note\",\"transpose\",\"fine_tune\"]"
                "},"
                "\"global\":{"
                    "\"name\":\"Global\","
                    "\"params\":[\"gain\",\"polyphony\"],"
                    "\"knobs\":[\"gain\",\"polyphony\"]"
                "}"
            "}"
        "}");
    return (n >= 0 && n < buf_len) ? n : -1;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    mrsample_instance_t *inst = (mrsample_instance_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;
    if (strcmp(key, "name") == 0)          return snprintf(buf, buf_len, "MrSample");
    if (strcmp(key, "state") == 0)         return build_state_json(inst, buf, buf_len);
    if (strcmp(key, "chain_params") == 0)  return build_chain_params_json(inst, buf, buf_len);
    if (strcmp(key, "ui_hierarchy") == 0)  return build_ui_hierarchy(inst, buf, buf_len);
    return get_param_value(inst, key, buf, buf_len);
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    mrsample_instance_t *inst = (mrsample_instance_t *)instance;
    if (!inst || !buf || buf_len <= 0) return -1;
    if (!inst->last_error[0]) return 0;
    return snprintf(buf, buf_len, "%s", inst->last_error);
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    mrsample_instance_t *inst = (mrsample_instance_t *)instance;
    if (!out_interleaved_lr || frames <= 0) return;
    if (!inst) {
        memset(out_interleaved_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    std::vector<float> left((size_t)frames, 0.0f);
    std::vector<float> right((size_t)frames, 0.0f);
    mrsample_engine_render(&inst->engine, left.data(), right.data(), frames);

    for (int i = 0; i < frames; i++) {
        float l = left[(size_t)i];
        float r = right[(size_t)i];
        if (l > 0.95f || l < -0.95f) l = tanhf(l);
        if (r > 0.95f || r < -0.95f) r = tanhf(r);
        int32_t sl = (int32_t)(l * 32767.0f);
        int32_t sr = (int32_t)(r * 32767.0f);
        if (sl > 32767) sl = 32767;
        if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767;
        if (sr < -32768) sr = -32768;
        out_interleaved_lr[i * 2]     = (int16_t)sl;
        out_interleaved_lr[i * 2 + 1] = (int16_t)sr;
    }
}

static plugin_api_v2_t g_api;

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version    = MOVE_PLUGIN_API_VERSION_2;
    g_api.create_instance = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.on_midi        = v2_on_midi;
    g_api.set_param      = v2_set_param;
    g_api.get_param      = v2_get_param;
    g_api.get_error      = v2_get_error;
    g_api.render_block   = v2_render_block;
    plugin_log("plugin initialized");
    return &g_api;
}
