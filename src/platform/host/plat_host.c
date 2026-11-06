#include "plat_host_internal.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * Monotonic Clock
 * -------------------------------------------------------------------------- */
u64 plat_time_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (u64)ts.tv_sec * 1000000ULL + (u64)(ts.tv_nsec / 1000L);
}

/* --------------------------------------------------------------------------
 * Video Capture & Periodic Frame Dump
 * -------------------------------------------------------------------------- */
static bool g_vid_capture = false;
static char g_vid_periodic_prefix[512];
static u32  g_vid_every_n = 0u;
static u32  g_vid_present_count = 0u;
static u8  *g_vid_rgb24 = NULL;
static u32  g_vid_cap_bytes = 0u;
static u16  g_vid_w = 0u;
static u16  g_vid_h = 0u;

void plat_host_set_video_capture(bool enable, const char *periodic_prefix, u32 every_n) {
    g_vid_capture = enable;
    g_vid_every_n = every_n;
    if (periodic_prefix) {
        snprintf(g_vid_periodic_prefix, sizeof(g_vid_periodic_prefix), "%s", periodic_prefix);
    } else {
        g_vid_periodic_prefix[0] = '\0';
    }
}

bool plat_host_has_presented_frame(void) {
    return (g_vid_rgb24 != NULL) && (g_vid_w > 0u) && (g_vid_h > 0u) && (g_vid_present_count > 0u);
}

PsxResult plat_host_write_last_ppm(const char *path) {
    if (!plat_host_has_presented_frame()) {
        return PSX_ERR_IO;
    }
    return video_ppm_write_rgb24_raw(path, g_vid_rgb24, g_vid_w, g_vid_h, (u32)g_vid_w * 3u);
}

static void format_periodic_ppm_path(char *out, size_t cap, const char *base, u32 idx) {
    const char *dot = strrchr(base, '.');
    if (dot && strcmp(dot, ".ppm") == 0) {
        size_t stem_len = (size_t)(dot - base);
        if (stem_len > cap - 24u) {
            stem_len = cap - 24u;
        }
        memcpy(out, base, stem_len);
        snprintf(out + stem_len, cap - stem_len, "_%06u.ppm", (unsigned)idx);
    } else {
        snprintf(out, cap, "%s_%06u.ppm", base, (unsigned)idx);
    }
}

void plat_video_present(const u16 *rgb555, u16 w, u16 h, u16 pitch_px, bool is24) {
    if (!rgb555 || w == 0u || h == 0u) {
        return;
    }
    g_vid_present_count++;
    if (!g_vid_capture) {
        return;
    }

    u32 need_bytes = (u32)w * (u32)h * 3u;
    if (need_bytes > g_vid_cap_bytes) {
        u8 *nbuf = (u8 *)realloc(g_vid_rgb24, need_bytes);
        if (!nbuf) {
            return;
        }
        g_vid_rgb24 = nbuf;
        g_vid_cap_bytes = need_bytes;
    }
    g_vid_w = w;
    g_vid_h = h;

    size_t stride_bytes = pitch_px != 0u
                              ? ((size_t)pitch_px * 2u)
                              : (is24 ? (((size_t)w * 3u + 1u) & ~1u) : ((size_t)w * 2u));
    const u8 *src_base = (const u8 *)rgb555;

    for (u16 y = 0u; y < h; y++) {
        const u8 *src = src_base + (size_t)y * stride_bytes;
        u8 *dst = g_vid_rgb24 + (size_t)y * (size_t)w * 3u;
        if (is24) {
            memcpy(dst, src, (size_t)w * 3u);
        } else {
            for (u16 x = 0u; x < w; x++) {
                u16 px = (u16)((u16)src[(size_t)x * 2u] |
                               ((u16)src[(size_t)x * 2u + 1u] << 8));
                u8 r5 = (u8)(px & 0x1Fu);
                u8 g5 = (u8)((px >> 5) & 0x1Fu);
                u8 b5 = (u8)((px >> 10) & 0x1Fu);
                dst[(size_t)x * 3u + 0u] = (u8)((r5 << 3) | (r5 >> 2));
                dst[(size_t)x * 3u + 1u] = (u8)((g5 << 3) | (g5 >> 2));
                dst[(size_t)x * 3u + 2u] = (u8)((b5 << 3) | (b5 >> 2));
            }
        }
    }

    if (g_vid_every_n > 0u && g_vid_periodic_prefix[0] != '\0' &&
        (g_vid_present_count % g_vid_every_n) == 0u) {
        char path_buf[544];
        format_periodic_ppm_path(path_buf, sizeof(path_buf),
                                 g_vid_periodic_prefix, g_vid_present_count);
        (void)video_ppm_write_rgb24_raw(path_buf, g_vid_rgb24, w, h, (u32)w * 3u);
    }
}

/* --------------------------------------------------------------------------
 * Audio Capture (44.1 kHz Stereo PCM16)
 * -------------------------------------------------------------------------- */
#define HOST_AUDIO_MAX_FRAMES (16u * 1024u * 1024u) /* 64 MiB cap (~6 min @ 44.1kHz) */

static bool g_aud_capture = false;
static s16 *g_aud_samples = NULL;
static u32  g_aud_frames = 0u;
static u32  g_aud_cap_frames = 0u;

void plat_host_set_audio_capture(bool enable) {
    g_aud_capture = enable;
}

u32 plat_host_audio_captured_frames(void) {
    return g_aud_frames;
}

PsxResult plat_host_write_captured_wav(const char *path) {
    return audio_wav_write(path, g_aud_samples, g_aud_frames, 44100u);
}

void plat_audio_push(const s16 *stereo, u32 frames) {
    if (!g_aud_capture || !stereo || frames == 0u) {
        return;
    }
    if (g_aud_frames >= HOST_AUDIO_MAX_FRAMES) {
        return;
    }
    if (frames > HOST_AUDIO_MAX_FRAMES - g_aud_frames) {
        frames = HOST_AUDIO_MAX_FRAMES - g_aud_frames;
    }
    u32 needed = g_aud_frames + frames;
    if (needed > g_aud_cap_frames) {
        u32 new_cap = g_aud_cap_frames ? g_aud_cap_frames : 8192u;
        while (new_cap < needed && new_cap < HOST_AUDIO_MAX_FRAMES) {
            new_cap *= 2u;
        }
        if (new_cap > HOST_AUDIO_MAX_FRAMES) {
            new_cap = HOST_AUDIO_MAX_FRAMES;
        }
        s16 *nbuf = (s16 *)realloc(g_aud_samples, (size_t)new_cap * 2u * sizeof(s16));
        if (!nbuf) {
            return;
        }
        g_aud_samples = nbuf;
        g_aud_cap_frames = new_cap;
    }
    memcpy(g_aud_samples + (size_t)g_aud_frames * 2u, stereo, (size_t)frames * 2u * sizeof(s16));
    g_aud_frames += frames;
}

void plat_host_reset_capture(void) {
    free(g_vid_rgb24);
    g_vid_rgb24 = NULL;
    g_vid_cap_bytes = 0u;
    g_vid_w = 0u;
    g_vid_h = 0u;
    g_vid_present_count = 0u;
    g_vid_capture = false;
    g_vid_every_n = 0u;
    g_vid_periodic_prefix[0] = '\0';

    free(g_aud_samples);
    g_aud_samples = NULL;
    g_aud_frames = 0u;
    g_aud_cap_frames = 0u;
    g_aud_capture = false;
}

/* --------------------------------------------------------------------------
 * Deterministic Scheduled Input (--pad-press & --input)
 * -------------------------------------------------------------------------- */
typedef struct HostInputEvent {
    u32 start_frame;
    u32 end_frame;
    u16 press_mask; /* active-high mask of buttons to press */
} HostInputEvent;

#define HOST_MAX_INPUT_EVENTS 256u

static HostInputEvent g_input_events[HOST_MAX_INPUT_EVENTS];
static u32            g_input_event_count = 0u;
static u32            g_input_cur_frame = 0u;

void plat_host_input_clear(void) {
    g_input_event_count = 0u;
    g_input_cur_frame = 0u;
}

bool plat_host_input_add_event(u32 start_frame, u32 end_frame, u16 press_mask) {
    if (g_input_event_count >= HOST_MAX_INPUT_EVENTS || end_frame < start_frame) {
        return false;
    }
    g_input_events[g_input_event_count].start_frame = start_frame;
    g_input_events[g_input_event_count].end_frame = end_frame;
    g_input_events[g_input_event_count].press_mask = press_mask;
    g_input_event_count++;
    return true;
}

void plat_host_input_set_frame(u32 frame_idx) {
    g_input_cur_frame = frame_idx;
}

void plat_input_poll(PsxPadState pads[2], PsxHostKeys *keys) {
    if (pads) {
        memset(pads, 0, sizeof(PsxPadState) * 2u);
        u16 pressed = 0u;
        for (u32 i = 0u; i < g_input_event_count; i++) {
            if (g_input_cur_frame >= g_input_events[i].start_frame &&
                g_input_cur_frame <= g_input_events[i].end_frame) {
                pressed = (u16)(pressed | g_input_events[i].press_mask);
            }
        }
        pads[0].buttons = (u16)(0xFFFFu & ~pressed);
        pads[0].lx = 0x80u;
        pads[0].ly = 0x80u;
        pads[0].rx = 0x80u;
        pads[0].ry = 0x80u;
        pads[0].connected = 1u;

        pads[1].buttons = 0xFFFFu;
        pads[1].lx = 0x80u;
        pads[1].ly = 0x80u;
        pads[1].rx = 0x80u;
        pads[1].ry = 0x80u;
        pads[1].connected = 0u;
    }
    if (keys) {
        memset(keys, 0, sizeof(*keys));
    }
}

/* --------------------------------------------------------------------------
 * POSIX Threads & Timing
 * -------------------------------------------------------------------------- */
struct PlatThread {
    pthread_t th;
    void    (*fn)(void *);
    void     *arg;
};

static void *host_thread_entry(void *raw) {
    PlatThread *t = (PlatThread *)raw;
    if (t && t->fn) {
        t->fn(t->arg);
    }
    return NULL;
}

PlatThread *plat_thread_create(void (*fn)(void *), void *arg, u32 stack, int prio, int core) {
    (void)prio;
    (void)core;
    if (!fn) {
        return NULL;
    }
    PlatThread *t = (PlatThread *)calloc(1u, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;

    pthread_attr_t attr;
    bool has_attr = (pthread_attr_init(&attr) == 0);
    if (has_attr && stack >= 65536u) {
        (void)pthread_attr_setstacksize(&attr, (size_t)stack);
    }
    int rc = pthread_create(&t->th, has_attr ? &attr : NULL, host_thread_entry, t);
    if (has_attr) {
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(PlatThread *th) {
    if (!th) {
        return;
    }
    (void)pthread_join(th->th, NULL);
    free(th);
}

void plat_sleep_us(u32 us) {
    struct timespec req;
    req.tv_sec = (time_t)(us / 1000000u);
    req.tv_nsec = (long)(us % 1000000u) * 1000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
}

void plat_yield(void) {
    (void)sched_yield();
}

/* --------------------------------------------------------------------------
 * File & Directory I/O
 * -------------------------------------------------------------------------- */
struct PlatFile {
    FILE *fp;
};

PlatFile *plat_fopen(const char *path, const char *mode) {
    if (!path || !mode) {
        return NULL;
    }
    FILE *fp = fopen(path, mode);
    if (!fp) {
        return NULL;
    }
    PlatFile *pf = (PlatFile *)malloc(sizeof(*pf));
    if (!pf) {
        fclose(fp);
        return NULL;
    }
    pf->fp = fp;
    return pf;
}

u32 plat_fread(PlatFile *f, void *dst, u32 bytes) {
    if (!f || !f->fp || (!dst && bytes > 0u)) {
        return 0u;
    }
    if (bytes == 0u) {
        return 0u;
    }
    return (u32)fread(dst, 1u, (size_t)bytes, f->fp);
}

u32 plat_fwrite(PlatFile *f, const void *src, u32 bytes) {
    if (!f || !f->fp || (!src && bytes > 0u)) {
        return 0u;
    }
    if (bytes == 0u) {
        return 0u;
    }
    return (u32)fwrite(src, 1u, (size_t)bytes, f->fp);
}

bool plat_fseek(PlatFile *f, u64 off) {
    if (!f || !f->fp) {
        return false;
    }
    return fseeko(f->fp, (off_t)off, SEEK_SET) == 0;
}

u64 plat_fsize(PlatFile *f) {
    if (!f || !f->fp) {
        return 0u;
    }
    off_t cur = ftello(f->fp);
    if (cur < 0) {
        return 0u;
    }
    if (fseeko(f->fp, 0, SEEK_END) != 0) {
        return 0u;
    }
    off_t end = ftello(f->fp);
    (void)fseeko(f->fp, cur, SEEK_SET);
    return end >= 0 ? (u64)end : 0u;
}

void plat_fclose(PlatFile *f) {
    if (!f) {
        return;
    }
    if (f->fp) {
        fclose(f->fp);
    }
    free(f);
}

bool plat_mkdir_p(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    char tmp[512];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, path, len + 1u);

    for (size_t i = 1u; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static int cmp_dir_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

u32 plat_listdir(const char *path, char (*names)[256], u32 max) {
    if (!path || !names || max == 0u) {
        return 0u;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return 0u;
    }
    u32 count = 0u;
    struct dirent *ent = NULL;
    while (count < max && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(names[count], 256u, "%.255s", ent->d_name);
        count++;
    }
    closedir(dir);
    if (count > 1u) {
        qsort(names, count, 256u, cmp_dir_name);
    }
    return count;
}

void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
