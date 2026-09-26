/*
 * bridge_stress.c — 64-bit protocol-level stress client for the OSS bridge
 * ------------------------------------------------------------------------
 * Speaks the shim<->helper protocol directly over the AF_UNIX socket, so the
 * helper's async ring pump can be exercised WITHOUT launching Winamp3 (which
 * needs an X display) and without any 32-bit toolchain.
 *
 * It reproduces exactly what disassembling Plugins/of_oss.so showed:
 *   WOSSFilter::do_open()   -> ioctl SETFMT(0x10) STEREO(1) SPEED(44100)
 *                              ioctl GETBLKSIZE -> block size (4096)
 *   WOSSFilter::ThreadProc()-> busy-spins, then write(fd, buf, 4096) forever
 *
 * i.e. a fixed 4096-byte write every 23.2 ms (1024 frames @ 44.1 kHz stereo
 * S16), paced at real time, plus injected stalls that stand in for the VM
 * scheduling hiccups that were starving the device and causing the popping.
 *
 * Build:  gcc -O2 -o bridge_stress bridge_stress.c -lm
 * Run:    OSS_BRIDGE_LOG=/tmp/oss_bridge_test.log ./oss_helper &
 *         ./bridge_stress [seconds] [stall_ms_every_25_writes]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAGIC 0x4F535332u
#define SOCK_PATH "/tmp/.oss_bridge.sock"

#define T_OPEN_DSP   1
#define T_OPEN_MIXER 2
#define T_IOCTL      3
#define T_WRITE      4
#define T_READ       5
#define T_CLOSE      6
#define T_SETFL      7

#define OSS_DSP_SETFMT     0xC0045005u
#define OSS_DSP_SPEED      0xC0045002u
#define OSS_DSP_STEREO     0xC0045003u
#define OSS_DSP_GETBLKSIZE 0xC0045004u

struct hdr { uint32_t magic; uint32_t type; uint32_t tag; uint32_t len; };

static int fd = -1;

static void send_all(const void *p, size_t n) {
    const char *b = (const char *)p;
    while (n) { ssize_t r = send(fd, b, n, 0); if (r <= 0) exit(2); b += r; n -= (size_t)r; }
}
static int recv_all(void *p, size_t n) {
    char *b = (char *)p;
    while (n) { ssize_t r = recv(fd, b, n, 0); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}
static int32_t req_resp(uint32_t type, const void *body, uint32_t len) {
    struct hdr h; h.magic = MAGIC; h.type = type; h.tag = 0; h.len = len;
    send_all(&h, sizeof h);
    if (len) send_all(body, len);
    int32_t r = -1;
    if (recv_all(&r, 4) < 0) return -1;
    return r;
}
static int32_t do_ioctl(uint32_t req, uint32_t arg) {
    uint32_t body[2]; body[0] = req; body[1] = arg;
    struct hdr h; h.magic = MAGIC; h.type = T_IOCTL; h.tag = 0; h.len = 8;
    send_all(&h, sizeof h);
    send_all(body, 8);
    int32_t ret = -1; uint32_t out_len = 0;
    if (recv_all(&ret, 4) < 0) return -1;
    if (recv_all(&out_len, 4) < 0) return -1;
    char out[64];
    if (out_len) { if (out_len > sizeof out) out_len = sizeof out; if (recv_all(out, out_len) < 0) return -1; }
    if (req == OSS_DSP_GETBLKSIZE && out_len >= 4) {
        int32_t v; memcpy(&v, out, 4);
        printf("  GETBLKSIZE -> %d\n", v);
    }
    return ret;
}

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
static void sleep_ms(double ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1000000.0);
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    double seconds   = (argc > 1) ? atof(argv[1]) : 8.0;
    double stall_ms  = (argc > 2) ? atof(argv[2]) : 250.0;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un un; memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, SOCK_PATH, sizeof un.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&un, sizeof un) < 0) {
        perror("connect (is oss_helper running?)"); return 1;
    }

    printf("oss-bridge stress: %.1fs, 4096-byte blocks, %.0fms stall every 25 writes\n",
           seconds, stall_ms);

    if (req_resp(T_OPEN_DSP, NULL, 0) < 0) { printf("OPEN failed\n"); return 1; }
    printf("  opened /dev/dsp\n");
    do_ioctl(OSS_DSP_SETFMT, 0x10);   /* AFMT_S16_LE */
    do_ioctl(OSS_DSP_STEREO, 1);
    do_ioctl(OSS_DSP_SPEED,  44100);
    printf("  format set: 44100 Hz stereo S16_LE\n");

    /* 4096-byte block = 1024 frames = 23.22 ms of audio */
    const int    BLK = 4096;
    const double FRAME_MS = 1000.0 / 44100.0;
    const double BLK_MS = 1024.0 * FRAME_MS;   /* 23.22 ms */

    unsigned char *buf = (unsigned char *)malloc(BLK);
    double t_start = now_ms(), next_due = t_start;
    unsigned long n = 0;
    double phase = 0.0;

    while ((now_ms() - t_start) < seconds * 1000.0) {
        /* 440 Hz sine, stereo S16_LE */
        short *s = (short *)buf;
        for (int i = 0; i < BLK / 2; i++) {
            short v = (short)(12000.0 * sin(phase));
            phase += 2.0 * M_PI * 440.0 / 44100.0;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            s[i] = v;
        }
        int32_t r = req_resp(T_WRITE, buf, (uint32_t)BLK);
        if (r < 0) { printf("write failed at block %lu\n", n); break; }
        n++;

        /* simulate a decoder/scheduler hiccup the way a loaded VM does */
        if (stall_ms > 0 && (n % 25) == 0) sleep_ms(stall_ms);

        /* hold real-time cadence */
        next_due += BLK_MS;
        double wait = next_due - now_ms();
        if (wait > 0) sleep_ms(wait);
        if (wait < -500) next_due = now_ms();   /* don't accumulate debt */
    }

    double el = now_ms() - t_start;
    printf("  wrote %lu blocks (%lu bytes) in %.0f ms = %.1f blocks/s\n",
           n, n * (unsigned long)BLK, el, (double)n / (el / 1000.0));
    req_resp(T_CLOSE, NULL, 0);
    printf("  closed. check the helper log for underrun counts.\n");
    close(fd);
    free(buf);
    return 0;
}
