/*
 * Unit test: smpl chunk parsing in MrSample's WAV loader.
 *
 * Synthesizes a known WAV with a smpl chunk in /tmp, runs the loader,
 * asserts the parsed loop points and root note match what was written.
 *
 * Build:
 *   g++ -O2 -std=c++14 tests/test_smpl.cpp -o build/test_smpl && build/test_smpl
 *
 * The parser logic is duplicated from mrsample_plugin.cpp's load_wav function
 * because that function is static there. If it diverges, the test will catch
 * regressions in this copy, not in the plugin itself — keep them in sync.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct {
    float *data;
    int length;
    int sample_rate;
    int channels;
    float smpl_loop_start;
    float smpl_loop_end;
    int   smpl_root_note;
} sample_buffer_t;

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
static uint16_t rd_u16_le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rd_i24_le(const uint8_t *p) {
    int32_t v = ((int32_t)p[0]) | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (v & 0x00800000) v |= ~0x00FFFFFF;
    return v;
}

/* ----- copy of load_wav from mrsample_plugin.cpp ----- */
static int load_wav(const char *path, sample_buffer_t *out, char *err, int err_len) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->smpl_loop_start = -1.0f;
    out->smpl_loop_end   = -1.0f;
    out->smpl_root_note  = -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, err_len, "open"); return -1; }

    uint8_t riff[12];
    if (fread(riff, 1, 12, fp) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(fp); snprintf(err, err_len, "not RIFF/WAVE"); return -1;
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
                fclose(fp); snprintf(err, err_len, "fmt"); return -1;
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

    if (!have_fmt || !have_data) { fclose(fp); snprintf(err, err_len, "missing chunks"); return -1; }
    if (channels < 1 || channels > 2) { fclose(fp); snprintf(err, err_len, "channels"); return -1; }

    if (fseek(fp, data_offset, SEEK_SET) != 0) { fclose(fp); snprintf(err, err_len, "seek"); return -1; }
    uint8_t *raw = (uint8_t *)malloc(data_size);
    if (!raw) { fclose(fp); snprintf(err, err_len, "oom"); return -1; }
    if (fread(raw, 1, data_size, fp) != data_size) { free(raw); fclose(fp); snprintf(err, err_len, "read"); return -1; }
    fclose(fp);

    int frames = (int)(data_size / block_align);
    out->data = (float *)malloc((size_t)frames * channels * sizeof(float));
    int bps = bits_per_sample / 8;
    for (int i = 0; i < frames; i++) {
        const uint8_t *frame_ptr = raw + (size_t)i * block_align;
        for (int ch = 0; ch < channels; ch++) {
            const uint8_t *sp = frame_ptr + ch * bps;
            float v = 0.0f;
            if (audio_format == 1) {
                if (bits_per_sample == 8) v = ((int)sp[0] - 128) / 128.0f;
                else if (bits_per_sample == 16) v = (int16_t)rd_u16_le(sp) / 32768.0f;
                else if (bits_per_sample == 24) v = (float)rd_i24_le(sp) / 8388608.0f;
                else v = (float)(int32_t)rd_u32_le(sp) / 2147483648.0f;
            } else {
                float x; memcpy(&x, sp, sizeof(float)); v = x;
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

/* ---------- WAV synthesizer with smpl chunk ---------- */

static void wr_u16_le(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void wr_u32_le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static int synth_wav_with_smpl(const char *path,
                               int sample_rate, int frames,
                               int loop_start, int loop_end,
                               int unity_note) {
    /* Mono 16-bit, 1-channel sine at 440 Hz */
    uint32_t data_size = frames * 2;  /* 2 bytes/frame mono */
    uint32_t smpl_size = 36 + 24;     /* fixed header + one loop */
    uint32_t riff_size = 4 + (8 + 16) + (8 + data_size) + (8 + smpl_size);

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    uint8_t hdr[12];
    memcpy(hdr, "RIFF", 4);
    wr_u32_le(hdr + 4, riff_size);
    memcpy(hdr + 8, "WAVE", 4);
    fwrite(hdr, 1, 12, fp);

    uint8_t fmt_chunk[8 + 16];
    memcpy(fmt_chunk, "fmt ", 4);
    wr_u32_le(fmt_chunk + 4, 16);
    wr_u16_le(fmt_chunk + 8,  1);   /* PCM */
    wr_u16_le(fmt_chunk + 10, 1);   /* channels */
    wr_u32_le(fmt_chunk + 12, sample_rate);
    wr_u32_le(fmt_chunk + 16, sample_rate * 2);  /* byte rate */
    wr_u16_le(fmt_chunk + 20, 2);   /* block align */
    wr_u16_le(fmt_chunk + 22, 16);  /* bits */
    fwrite(fmt_chunk, 1, sizeof(fmt_chunk), fp);

    uint8_t data_hdr[8];
    memcpy(data_hdr, "data", 4);
    wr_u32_le(data_hdr + 4, data_size);
    fwrite(data_hdr, 1, 8, fp);

    for (int i = 0; i < frames; i++) {
        double t = (double)i / (double)sample_rate;
        double s = sin(2.0 * M_PI * 440.0 * t) * 0.5;
        int16_t v = (int16_t)(s * 32767.0);
        uint8_t b[2]; wr_u16_le(b, (uint16_t)v);
        fwrite(b, 1, 2, fp);
    }

    uint8_t smpl_hdr[8 + 60];
    memcpy(smpl_hdr, "smpl", 4);
    wr_u32_le(smpl_hdr + 4, smpl_size);
    memset(smpl_hdr + 8, 0, 60);
    /* offsets within payload (smpl_hdr+8): 12 = midi_unity_note, 28 = num_loops */
    wr_u32_le(smpl_hdr + 8 + 12, (uint32_t)unity_note);
    wr_u32_le(smpl_hdr + 8 + 28, 1);  /* one loop */
    /* loop record at payload offset 36: cue_id(0..3), type(4..7), start(8..11), end(12..15), frac(16..19), play_count(20..23) */
    wr_u32_le(smpl_hdr + 8 + 36 + 8,  (uint32_t)loop_start);
    wr_u32_le(smpl_hdr + 8 + 36 + 12, (uint32_t)loop_end);
    fwrite(smpl_hdr, 1, sizeof(smpl_hdr), fp);

    fclose(fp);
    return 0;
}

static int run_case(const char *label, const char *path,
                    int sr, int frames,
                    int loop_start, int loop_end, int unity_note) {
    if (synth_wav_with_smpl(path, sr, frames, loop_start, loop_end, unity_note) != 0) {
        fprintf(stderr, "[%s] FAIL: could not synth WAV\n", label);
        return 1;
    }

    sample_buffer_t s;
    char err[256] = {0};
    if (load_wav(path, &s, err, sizeof(err)) != 0) {
        fprintf(stderr, "[%s] FAIL: load_wav: %s\n", label, err);
        return 1;
    }

    float exp_lo = (float)loop_start / (float)(frames - 1);
    float exp_hi = (float)loop_end   / (float)(frames - 1);

    printf("[%s] %s (frames=%d, loop=[%d, %d])\n", label, path, frames, loop_start, loop_end);
    printf("    smpl_loop_start = %.8f  (expected %.8f, delta %.2e)\n",
           s.smpl_loop_start, exp_lo, (double)(s.smpl_loop_start - exp_lo));
    printf("    smpl_loop_end   = %.8f  (expected %.8f, delta %.2e)\n",
           s.smpl_loop_end,   exp_hi, (double)(s.smpl_loop_end   - exp_hi));
    printf("    smpl_root_note  = %d  (expected %d)\n", s.smpl_root_note, unity_note);

    int fail = 0;
    if (s.length != frames) { fprintf(stderr, "[%s] FAIL: frames mismatch\n", label); fail = 1; }
    if (s.sample_rate != sr) { fprintf(stderr, "[%s] FAIL: sample_rate mismatch\n", label); fail = 1; }
    if (s.smpl_root_note != unity_note) { fprintf(stderr, "[%s] FAIL: root_note mismatch\n", label); fail = 1; }
    if (fabsf(s.smpl_loop_start - exp_lo) > 1e-5f) { fprintf(stderr, "[%s] FAIL: loop_start mismatch\n", label); fail = 1; }
    if (fabsf(s.smpl_loop_end   - exp_hi) > 1e-5f) { fprintf(stderr, "[%s] FAIL: loop_end mismatch\n", label); fail = 1; }

    free(s.data);
    return fail;
}

int main(void) {
    int sr = 44100;
    int frames = 44100;
    int fail = 0;

    /* Case A: 1% aligned (loop_start = 0.25 exactly) */
    fail |= run_case("aligned", "/tmp/mrsample_smpl_aligned.wav",
                     sr, frames, 11025, 33075, 72);

    /* Case B: sample-accurate, NOT 1% aligned.
     * loop_start = 11129 frames = 11129/44099 = 0.252408...
     * loop_end   = 33321 frames = 33321/44099 = 0.755664...
     */
    fail |= run_case("sub-percent", "/tmp/mrsample_smpl_subpercent.wav",
                     sr, frames, 11129, 33321, 60);

    /* Case C: deeply non-aligned to catch any rounding silliness */
    fail |= run_case("irregular", "/tmp/mrsample_smpl_irregular.wav",
                     sr, frames, 7777, 39999, 48);

    if (fail) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
