/*
 * helper_pulse.c — 64-bit OSS bridge helper for Winamp3
 * -----------------------------------------------------
 * Runs as a SEPARATE 64-bit process (so it links the host's modern audio
 * library freely, unaffected by Winamp3's bundled RH9 glibc 2.3.2). The
 * 32-bit shim (oss_shim.so, LD_PRELOADed into Winamp.exe) forwards OSS
 * open/ioctl/write/close over an AF_UNIX socket to this helper, which plays
 * the PCM through the host audio stack. This makes Winamp3 produce sound
 * WITHOUT osspd.
 *
 * Backend: ALSA (libasound.so.2), loaded at runtime via dlopen. ALSA is the
 * lowest-common-denominator audio API on Linux — every modern desktop (real
 * ALSA, PulseAudio, and PipeWire alike) ships alsa-lib, and both PulseAudio
 * and PipeWire expose an ALSA compatibility PCM. So a single ALSA backend
 * gives true out-of-the-box audio on essentially any Linux host.
 *
 * ---------------------------------------------------------------------
 * v2 (2026-09-06): ASYNC RING-BUFFER PUMP + DIAGNOSTIC LOGGING
 * ---------------------------------------------------------------------
 * Why this rewrite exists (evidence from disassembling Plugins/of_oss.so):
 *
 *   WOSSFilter::ThreadProc() busy-spins until its internal ring holds one
 *   block, then issues  write(fd, buf, N)  with N fixed at 4096 bytes —
 *   the value our own SNDCTL_DSP_GETBLKSIZE reply hands it (do_open copies
 *   global_0x4c00 -> 0x4bfc -> member+0x10). 4096 bytes = 1024 frames =
 *   23.2 ms of 44.1 kHz stereo S16 — i.e. ~43 writes/second, EVERY one of
 *   them a full synchronous round trip through shim -> socket -> helper ->
 *   snd_pcm_writei -> reply. Any scheduling hiccup on that path (and the
 *   plugin's spin thread is burning a whole core while doing it) starves the
 *   ALSA buffer and PipeWire underruns -> the "tu tu tu" popping.
 *
 *   Fix: decouple the two clocks.
 *     * socket side  : memcpy() into a lock-protected ring, ACK immediately.
 *                      Only blocks (back-pressure) when the ring is full,
 *                      which is exactly correct blocking-OSS semantics.
 *     * ALSA thread  : owns the device, drains the ring continuously, starts
 *                      only after a prefill cushion so it never runs dry.
 *   Winamp3's write() cadence no longer has ANY influence on device timing.
 *
 * Diagnostic logging: set OSS_BRIDGE_LOG=/path/to.log (AppRun does this).
 * Every write, every ioctl, ALSA hw params and each underrun is timestamped
 * so a bad stream can be diagnosed from the log alone.
 *
 * Build (ZERO extra packages on the build host):
 *     gcc -O2 -o oss_helper helper_pulse.c -ldl -lpthread
 * No audio dev headers required — everything is resolved through dlsym.
 *
 * Protocol (see shim_oss.c): every message is { hdr; body }.
 *   hdr = { u32 magic, u32 type, u32 tag, u32 len }
 *   T_OPEN_DSP / T_OPEN_MIXER / T_CLOSE / T_SETFL : no body -> reply i32
 *   T_WRITE  : body = PCM bytes                 -> reply i32 (bytes written)
 *   T_IOCTL  : body = { u32 req, u32 argval }   -> reply { i32 ret, u32 out_len, out[] }
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dlfcn.h>

/* ---- ALSA types/constants we need (copied from upstream, stable ABI) ---- */
typedef struct snd_pcm snd_pcm_t;
typedef long            snd_pcm_sframes_t;
typedef unsigned long   snd_pcm_uframes_t;
typedef enum { SND_PCM_STREAM_PLAYBACK = 0 } snd_pcm_stream_t;
typedef enum { FMT_U8 = 1, FMT_S16_LE = 2, FMT_S32_LE = 10 } snd_pcm_format_t;
typedef enum { ACC_RW_INTERLEAVED = 3 } snd_pcm_access_t;
/* open mode bit: request a NON-BLOCKING PCM so snd_pcm_writei returns
 * -EAGAIN instead of blocking the ALSA thread when the device's buffer is
 * full or the PipeWire stream is momentarily not consuming. (alsa-lib value.) */
#define SND_PCM_NONBLOCK 1

/* ---- resolved ALSA symbols (loaded via dlopen at startup) ---- */
static int   (*alsa_open)(snd_pcm_t **, const char *, int, int) = NULL;
static int   (*alsa_set_params)(snd_pcm_t *, int, int, unsigned int,
                                unsigned int, int, unsigned int) = NULL;
static snd_pcm_sframes_t (*alsa_writei)(snd_pcm_t *, const void *,
                                        snd_pcm_uframes_t) = NULL;
static int   (*alsa_prepare)(snd_pcm_t *) = NULL;
static int   (*alsa_drain)(snd_pcm_t *) = NULL;
static int   (*alsa_close)(snd_pcm_t *) = NULL;
static int   (*alsa_recover)(snd_pcm_t *, int, int) = NULL;
static const char *(*alsa_strerror)(int) = NULL;
static int   (*alsa_get_params)(snd_pcm_t *, snd_pcm_uframes_t *,
                                snd_pcm_uframes_t *) = NULL;

/* ---- protocol constants (must match shim_oss.c) ---- */
#define MAGIC 0x4F535332u
#define SOCK_PATH "/tmp/.oss_bridge.sock"

#define T_OPEN_DSP   1
#define T_OPEN_MIXER 2
#define T_IOCTL      3
#define T_WRITE      4
#define T_READ       5
#define T_CLOSE      6
#define T_SETFL      7

struct hdr { uint32_t magic; uint32_t type; uint32_t tag; uint32_t len; };

/* OSS ioctl request codes (standard <sys/soundcard.h> values) */
#define OSS_DSP_SETFMT      0xC0045005u  /* SOUND_PCM_WRITE_BITS */
#define OSS_DSP_SPEED       0xC0045002u
#define OSS_DSP_STEREO      0xC0045003u
#define OSS_DSP_GETBLKSIZE  0xC0045004u
#define OSS_DSP_SETFRAGMENT 0xC004500Au
#define OSS_DSP_GETFMTS     0x8004500Bu
#define OSS_DSP_GETOSPACE   0xC004500Cu
#define OSS_DSP_GETOPTR     0xC004500Du
#define OSS_DSP_SYNC        0x00005001u
#define OSS_DSP_RESET       0x00005000u
#define OSS_DSP_NONBLOCK    0x0000500Du
#define OSS_MIXER_PCM_WRITE 0xC0044D04u
#define OSS_MIXER_PCM_READ  0x80044D04u

/* ---- tunables ---------------------------------------------------------- */
/* Ring cushion: ~3 s of 44.1k stereo S16 (176400 B/s). Big enough to ride
 * out multi-hundred-ms scheduling stalls, small enough to keep latency sane. */
#define RING_BYTES   (512u * 1024u)
/* Start the device only once this much audio is banked (~185 ms). */
#define PREFILL_BYTES (32u * 1024u)
/* If the ring never reaches PREFILL (small upstream buffer), start anyway
 * after this long so we can never stall silently. */
#define PREFILL_TIMEOUT_MS 300
/* ALSA latency hint handed to snd_pcm_set_params (microseconds). */
#define ALSA_LATENCY_US 400000

/* ---- diagnostics -------------------------------------------------------- */
static FILE *g_log = NULL;
static double t0 = 0.0;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
/* LOG  -> log file only (high volume)                                      */
static void LOG(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    fprintf(g_log, "[%8.1f] ", now_ms() - t0);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}
/* LOGE -> log file AND stderr (milestones the user should see)             */
static void LOGE(const char *fmt, ...) {
    va_list ap;
    if (g_log) {
        va_start(ap, fmt);
        fprintf(g_log, "[%8.1f] ", now_ms() - t0);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    va_start(ap, fmt);
    fprintf(stderr, "[helper] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- audio state ---- */
static int   g_alsa_ok = 0;
static snd_pcm_t *g_pcm = NULL;
static int   g_fmt = FMT_S16_LE;     /* OSS default AFMT_S16_LE */
static unsigned int g_rate = 44100;
static unsigned int g_ch   = 2;

/* ---- ring buffer (shared between socket thread and ALSA thread) --------- */
static unsigned char *g_ring = NULL;
static size_t g_head = 0;            /* write position            */
static size_t g_tail = 0;            /* read position             */
static size_t g_fill = 0;            /* bytes currently buffered  */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_cv_space = PTHREAD_COND_INITIALIZER;

static pthread_t g_th;
static int  g_th_running = 0;
static int  g_stop = 0;

/* ---- stream statistics ---- */
static unsigned long g_wr_count = 0;
static unsigned long g_wr_bytes = 0;
static double        g_last_wr_ms = 0.0;
static double        g_max_gap_ms = 0.0;
static double        g_sum_gap_ms = 0.0;
static unsigned long g_underruns = 0;
static unsigned long g_short_writes = 0;

static int bps(void) { return (g_fmt == FMT_U8) ? 1 : (g_fmt == FMT_S32_LE ? 4 : 2); }

/* ---- ring helpers (caller must hold g_mu) ------------------------------- */
static void ring_put(const unsigned char *src, size_t n) {
    while (n) {
        size_t off = g_head % RING_BYTES;
        size_t c = RING_BYTES - off;
        if (c > n) c = n;
        memcpy(g_ring + off, src, c);
        src += c;
        g_head = (g_head + c) % RING_BYTES;
        g_fill += c;
        n -= c;
    }
}
static size_t ring_get(unsigned char *dst, size_t n) {
    size_t done = 0;
    while (n && g_fill) {
        size_t off = g_tail % RING_BYTES;
        size_t c = RING_BYTES - off;
        if (c > n) c = n;
        if (c > g_fill) c = g_fill;
        memcpy(dst + done, g_ring + off, c);
        done += c;
        g_tail = (g_tail + c) % RING_BYTES;
        g_fill -= c;
        n -= c;
    }
    return done;
}

static int alsa_load(void) {
    void *h = dlopen("libasound.so.2", RTLD_NOW);
    if (!h) { LOGE("dlopen libasound.so.2: %s\n", dlerror()); return -1; }
    alsa_open      = dlsym(h, "snd_pcm_open");
    alsa_set_params= dlsym(h, "snd_pcm_set_params");
    alsa_writei    = dlsym(h, "snd_pcm_writei");
    alsa_prepare   = dlsym(h, "snd_pcm_prepare");
    alsa_drain     = dlsym(h, "snd_pcm_drain");
    alsa_close     = dlsym(h, "snd_pcm_close");
    alsa_recover   = dlsym(h, "snd_pcm_recover");
    alsa_strerror  = dlsym(h, "snd_strerror");
    alsa_get_params= dlsym(h, "snd_pcm_get_params");   /* optional */
    if (!alsa_open || !alsa_set_params || !alsa_writei ||
        !alsa_close || !alsa_strerror) {
        LOGE("missing required alsa symbols\n");
        return -1;
    }
    return 0;
}

/* ---- ALSA playback thread ---------------------------------------------- */
/* Owns g_pcm. Drains the ring into the device as fast as the device accepts.
 * Starts only after the prefill cushion is banked (or the safety timeout),
 * so the very first milliseconds can't underrun.                           */
static void *alsa_thread(void *arg) {
    (void)arg;
    unsigned char *tmp = (unsigned char *)malloc(65536);
    size_t tmp_cap = tmp ? 65536 : 0;
    double waited_since = now_ms();
    int started = 0;

    for (;;) {
        pthread_mutex_lock(&g_mu);

        if (g_stop) { pthread_mutex_unlock(&g_mu); break; }

        /* --- wait for enough audio before touching the device --- */
        if (!started) {
            for (;;) {
                if (g_stop) break;
                if (g_fill >= PREFILL_BYTES) break;
                if (g_fill > 0 && (now_ms() - waited_since) > PREFILL_TIMEOUT_MS) {
                    LOG("prefill timeout: starting with only %zu bytes banked\n", g_fill);
                    break;
                }
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 20L * 1000L * 1000L;          /* 20 ms slice */
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            }
            if (g_stop) { pthread_mutex_unlock(&g_mu); break; }
            started = 1;
            LOGE("playback STARTED (banked %zu bytes = %.0f ms cushion)\n",
                 g_fill, (double)g_fill / (double)(g_rate * g_ch * bps()) * 1000.0);
        }

        /* --- wait for data if the ring ran dry --- */
        while (g_fill == 0 && !g_stop)
            pthread_cond_wait(&g_cv, &g_mu);

        if (g_stop) { pthread_mutex_unlock(&g_mu); break; }

        size_t n = g_fill;
        if (n > tmp_cap) n = tmp_cap;
        n = ring_get(tmp, n);
        pthread_cond_broadcast(&g_cv_space);
        pthread_mutex_unlock(&g_mu);

        /* --- push it to the device (outside the lock) --- */
        size_t bytes_per_frame = (size_t)g_ch * (size_t)bps();
        size_t off = 0;
        while (off < n) {
            snd_pcm_uframes_t frames = (n - off) / bytes_per_frame;
            if (!frames) break;
            snd_pcm_sframes_t w = alsa_writei(g_pcm, tmp + off, frames);
            if (w > 0) {
                size_t got = (size_t)w * bytes_per_frame;
                off += got;
                if ((size_t)w < (size_t)frames) {
                    g_short_writes++;
                    LOG("short writei: %ld of %lu frames (total short=%lu)\n",
                        (long)w, (unsigned long)frames, g_short_writes);
                }
                continue;
            }
            if (w == 0) continue;
            if (w == -EAGAIN) {
                struct timespec ts = { 0, 2 * 1000 * 1000 };
                nanosleep(&ts, NULL);
                continue;
            }
            /* -EPIPE (underrun), -ESTRPIPE (suspend), anything else */
            g_underruns++;
            const char *why = alsa_strerror ? alsa_strerror((int)w) : "?";
            LOG("UNDERRUN #%lu: writei=%s (ring=%zu) recovering...\n",
                g_underruns, why, g_fill);
            if (g_underruns <= 5 || (g_underruns % 50) == 0)
                LOGE("underrun #%lu: %s\n", g_underruns, why);
            if (alsa_recover(g_pcm, (int)w, 1) < 0) {
                /* The stream is fundamentally broken (e.g. PipeWire reported
                 * "Broken pipe" after the device/sink was reconfigured or the
                 * daemon restarted). Do NOT abandon forever: tear the PCM down
                 * and let the next T_WRITE's ensure_stream() reopen a fresh one
                 * and respawn this thread. This is what keeps Winamp3 from
                 * freezing — a dead stream just goes silent until it heals. */
                LOGE("recover failed (%s); tearing down stream, will auto-reopen on next write\n", why);
                if (g_pcm) { alsa_close(g_pcm); g_pcm = NULL; }
                g_th_running = 0;
                pthread_mutex_lock(&g_mu);
                pthread_cond_broadcast(&g_cv_space);  /* wake any blocked writer */
                pthread_mutex_unlock(&g_mu);
                free(tmp);
                return NULL;
            }
            /* after recovery we retry the SAME remaining bytes (never dup) */
        }
    }
    free(tmp);
    return NULL;
}

/* stop the playback thread and (optionally) drain the device */
static void stream_stop(int drain) {
    if (g_th_running) {
        pthread_mutex_lock(&g_mu);
        g_stop = 1;
        pthread_cond_broadcast(&g_cv);
        pthread_cond_broadcast(&g_cv_space);
        pthread_mutex_unlock(&g_mu);
        pthread_join(g_th, NULL);
        g_th_running = 0;
        g_stop = 0;
    }
    if (g_pcm) {
        if (drain && alsa_drain) alsa_drain(g_pcm);
        alsa_close(g_pcm);
        g_pcm = NULL;
    }
    pthread_mutex_lock(&g_mu);
    g_head = g_tail = g_fill = 0;
    pthread_cond_broadcast(&g_cv_space);
    pthread_mutex_unlock(&g_mu);
}

/* (re)create the ALSA playback PCM from the cached format/rate/channels */
static int ensure_stream(void) {
    if (!g_alsa_ok) return -1;
    if (g_pcm && g_th_running) return 0;

    snd_pcm_t *p = NULL;
    int r = alsa_open(&p, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (r < 0) {
        LOGE("snd_pcm_open: %s\n", alsa_strerror(r));
        return -1;
    }
    r = alsa_set_params(p, g_fmt, ACC_RW_INTERLEAVED, g_ch, g_rate,
                        1, ALSA_LATENCY_US);
    if (r < 0) {
        LOGE("snd_pcm_set_params(rate=%u,ch=%u): %s\n",
             g_rate, g_ch, alsa_strerror(r));
        alsa_close(p);
        return -1;
    }
    g_pcm = p;

    snd_pcm_uframes_t bufsz = 0, persz = 0;
    if (alsa_get_params) alsa_get_params(p, &bufsz, &persz);
    LOGE("ALSA stream: rate=%u ch=%u fmt=%d hint=%dus buffer=%lu fr (%.0f ms) period=%lu fr (%.1f ms)\n",
         g_rate, g_ch, g_fmt, ALSA_LATENCY_US,
         (unsigned long)bufsz,
         bufsz ? (double)bufsz / (double)g_rate * 1000.0 : 0.0,
         (unsigned long)persz,
         persz ? (double)persz / (double)g_rate * 1000.0 : 0.0);

    g_wr_count = 0; g_wr_bytes = 0;
    g_max_gap_ms = 0.0; g_sum_gap_ms = 0.0;
    g_underruns = 0; g_short_writes = 0;
    g_last_wr_ms = now_ms();

    if (pthread_create(&g_th, NULL, alsa_thread, NULL) != 0) {
        LOGE("pthread_create failed\n");
        alsa_close(g_pcm); g_pcm = NULL;
        return -1;
    }
    g_th_running = 1;
    LOGE("playback thread spawned; prefill=%u bytes\n", PREFILL_BYTES);
    return 0;
}

/* close any existing PCM so the next ensure_stream() rebuilds with new params */
static void drop_pcm(void) { stream_stop(0); }

/* ---- socket read helpers ---- */
static int recv_exact(int fd, void *buf, int n) {
    int got = 0; char *p = (char *)buf;
    while (got < n) {
        int r = (int)recv(fd, p + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}
static int send_i32(int fd, int32_t v) { return (int)send(fd, &v, 4, 0); }

/* ---- ioctl handling: returns ret, fills out buffer (max 64B) ---- */
static int32_t handle_ioctl(uint32_t req, uint32_t argval, uint8_t *out, uint32_t *out_len) {
    *out_len = 0;
    switch (req) {
        case OSS_DSP_SETFMT:
            /* OSS AFMT bits: U8=0x08, S16_LE=0x10, S32_LE=0x20 */
            g_fmt = (argval == 0x08) ? FMT_U8
                  : (argval == 0x20) ? FMT_S32_LE : FMT_S16_LE;
            LOG("ioctl SETFMT arg=0x%x -> alsa fmt=%d\n", argval, g_fmt);
            drop_pcm();   /* rate/channels may still change; rebuild lazily */
            return 0;
        case OSS_DSP_SPEED:
            g_rate = argval ? argval : 44100;
            LOG("ioctl SPEED arg=%u -> %u\n", argval, g_rate);
            drop_pcm();
            return 0;
        case OSS_DSP_STEREO:
            g_ch = argval ? 2 : 1;
            LOG("ioctl STEREO arg=%u -> ch=%u\n", argval, g_ch);
            drop_pcm();
            return 0;
        case OSS_DSP_GETBLKSIZE: {
            /* of_oss.so copies this straight into its per-write block size
             * (do_open: global_0x4c00 -> 0x4bfc -> member+0x10, and
             *  ThreadProc issues write(fd, buf, that) every time).
             * 4096 = the classic OSS fragment size; with the async ring pump
             * the exact value no longer gates device timing. */
            int32_t v = 4096; memcpy(out, &v, 4); *out_len = 4;
            LOG("ioctl GETBLKSIZE -> 4096\n");
            return 0;
        }
        case OSS_DSP_SETFRAGMENT:
            LOG("ioctl SETFRAGMENT arg=0x%x (ignored)\n", argval);
            return 0;  /* accepted, ignored */
        case OSS_DSP_GETFMTS: {
            int32_t v = (1 << 2); /* AFMT_S16_LE */ memcpy(out, &v, 4); *out_len = 4;
            LOG("ioctl GETFMTS -> 0x%x\n", v);
            return 0;
        }
        case OSS_DSP_GETOSPACE: {
            /* audio_buf_info: fragments, fragstotal, fragsize, bytes, speed */
            int32_t info[5] = { 16, 16, 4096, 16 * 4096, (int32_t)g_rate };
            memcpy(out, info, 20); *out_len = 20; return 0;
        }
        case OSS_DSP_GETOPTR: {
            /* count_info: bytes, blocks, ptr, _unused */
            int32_t info[4] = { 0, 0, 0, 0 };
            memcpy(out, info, 16); *out_len = 16; return 0;
        }
        case OSS_DSP_SYNC:
            LOG("ioctl SYNC (drain)\n");
            if (g_pcm && alsa_drain) alsa_drain(g_pcm);
            return 0;
        case OSS_DSP_RESET:
            LOG("ioctl RESET (drop stream, ring cleared)\n");
            drop_pcm(); return 0;
        case OSS_DSP_NONBLOCK:
            LOG("ioctl NONBLOCK (ignored)\n");
            return 0;
        case OSS_MIXER_PCM_WRITE:
            /* MVP: volume left at system default (pavucontrol/alsamixer controls it) */
            LOG("ioctl MIXER PCM_WRITE arg=%u (ignored)\n", argval);
            return 0;
        case OSS_MIXER_PCM_READ: {
            int32_t v = 100; memcpy(out, &v, 4); *out_len = 4; return 0;
        }
        default:
            LOG("ioctl UNKNOWN req=0x%x arg=0x%x (pretend success)\n", req, argval);
            return 0;  /* unknown ioctl: pretend success so WA3 keeps going */
    }
}

/* ---- per-connection service loop ---- */
static void serve(int cfd) {
    for (;;) {
        struct hdr h;
        if (recv_exact(cfd, &h, sizeof h) < 0) break;
        if (h.magic != MAGIC) break;

        uint8_t *body = NULL;
        if (h.len) {
            body = (uint8_t *)malloc(h.len);
            if (!body || recv_exact(cfd, body, h.len) < 0) { free(body); break; }
        }

        int32_t ret = 0;
        uint8_t  out[64];
        uint32_t out_len = 0;

        switch (h.type) {
            case T_OPEN_DSP:
                LOGE("OPEN /dev/dsp\n");
                ret = 0; send_i32(cfd, ret); break;
            case T_OPEN_MIXER:
                LOG("OPEN /dev/mixer\n");
                ret = 0; send_i32(cfd, ret); break;
            case T_CLOSE:
                LOGE("CLOSE (drain + stop)\n");
                stream_stop(1);
                ret = 0; send_i32(cfd, ret); break;
            case T_SETFL:
                ret = 0; send_i32(cfd, ret); break;
            case T_WRITE: {
                double t = now_ms();
                double gap = (g_wr_count == 0) ? 0.0 : (t - g_last_wr_ms);
                g_last_wr_ms = t;
                if (g_wr_count && gap > g_max_gap_ms) g_max_gap_ms = gap;
                g_sum_gap_ms += gap;
                g_wr_count++;
                g_wr_bytes += h.len;

                if (!g_alsa_ok || !h.len) {
                    ret = (int32_t)h.len;   /* no ALSA: silently drop */
                    send_i32(cfd, ret);
                    break;
                }
                if (ensure_stream() != 0) {
                    ret = (int32_t)h.len;   /* device down: don't stall WA3 */
                    send_i32(cfd, ret);
                    break;
                }

                /* Absorb into the ring with back-pressure. The ACK goes out as
                 * soon as the bytes are banked — NOT after they are played —
                 * so Winamp3's ThreadProc is never hostage to device timing.
                 * The wait is BOUNDED: if the ALSA thread ever stops draining
                 * (stalled or dead stream) we drop the chunk and ACK anyway, so
                 * Winamp3's write() can never block indefinitely. */
                size_t off = 0;
                int dropped = 0;
                pthread_mutex_lock(&g_mu);
                while (off < h.len) {
                    size_t space = RING_BYTES - g_fill;
                    if (space == 0) {
                        pthread_cond_broadcast(&g_cv);
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        ts.tv_nsec += 250L * 1000L * 1000L;   /* 250 ms ceiling */
                        if (ts.tv_nsec >= 1000000000L) {
                            ts.tv_sec++; ts.tv_nsec -= 1000000000L;
                        }
                        if (pthread_cond_timedwait(&g_cv_space, &g_mu, &ts)
                                == ETIMEDOUT) {
                            dropped = 1;
                            break;
                        }
                        continue;
                    }
                    size_t c = h.len - off;
                    if (c > space) c = space;
                    ring_put(body + off, c);
                    off += c;
                }
                if (dropped) {
                    pthread_mutex_unlock(&g_mu);
                    ret = (int32_t)h.len;   /* pretend whole write succeeded */
                    send_i32(cfd, ret);
                    LOG("W#%-6lu DROPPED (backpressure timeout, ring stuck) len=%u\n",
                        g_wr_count, h.len);
                    break;
                }
                size_t fill = g_fill;
                pthread_cond_broadcast(&g_cv);
                pthread_mutex_unlock(&g_mu);

                ret = (int32_t)h.len;
                send_i32(cfd, ret);

                /* compact per-write trace: enough to see cadence + cushion */
                LOG("W#%-6lu len=%-6u gap=%6.2fms ring=%-7zu (%3.0fms) ur=%lu\n",
                    g_wr_count, h.len, gap, fill,
                    (double)fill / (double)(g_rate * g_ch * bps()) * 1000.0,
                    g_underruns);

                if ((g_wr_count % 200) == 0) {
                    double avg = (g_wr_count > 1) ? g_sum_gap_ms / (double)(g_wr_count - 1) : 0.0;
                    LOGE("stats: writes=%lu bytes=%lu avg_gap=%.2fms max_gap=%.2fms underruns=%lu short=%lu ring=%zu\n",
                         g_wr_count, g_wr_bytes, avg, g_max_gap_ms,
                         g_underruns, g_short_writes, fill);
                }
                break;
            }
            case T_READ:
                ret = 0; send_i32(cfd, ret); break;
            case T_IOCTL: {
                uint32_t req = (h.len >= 4) ? *(uint32_t *)body : 0;
                uint32_t arg = (h.len >= 8) ? *(uint32_t *)(body + 4) : 0;
                ret = handle_ioctl(req, arg, out, &out_len);
                send_i32(cfd, ret);
                if (send(cfd, &out_len, 4, 0) != 4) { free(body); break; }
                if (out_len && send(cfd, out, out_len, 0) < 0) { free(body); break; }
                break;
            }
            default:
                ret = 0; send_i32(cfd, ret); break;
        }
        free(body);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    t0 = now_ms();

    const char *lp = getenv("OSS_BRIDGE_LOG");
    if (lp && *lp) {
        g_log = fopen(lp, "w");
        if (g_log) setvbuf(g_log, NULL, _IOLBF, 0);
    }

    g_ring = (unsigned char *)malloc(RING_BYTES);
    if (!g_ring) { LOGE("out of memory for %u-byte ring\n", RING_BYTES); return 1; }

    g_alsa_ok = (alsa_load() == 0);
    if (!g_alsa_ok)
        LOGE("WARNING: libasound not loaded, audio will be silent\n");

    unlink(SOCK_PATH);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, SOCK_PATH, sizeof un.sun_path - 1);
    if (bind(lfd, (struct sockaddr *)&un, sizeof un) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 8) < 0) { perror("listen"); return 1; }

    LOGE("listening on %s (ALSA backend%s), ring=%uKB prefill=%uB, log=%s\n",
         SOCK_PATH, g_alsa_ok ? "" : " — SILENT, no libasound",
         RING_BYTES / 1024, PREFILL_BYTES, (g_log && lp) ? lp : "(none)");

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { perror("accept"); continue; }
        LOGE("client connected (shim loaded into WA3)\n");
        serve(cfd);
        close(cfd);
        drop_pcm();   /* reset stream for the next launch */
    }
    return 0;
}
