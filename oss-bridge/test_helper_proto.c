/*
 * test_helper_proto.c — 64-bit protocol conformance client for oss_helper.
 *
 * Speaks the AF_UNIX bridge protocol DIRECTLY (no 32-bit shim, no multilib
 * needed) so we can validate the helper's happy path on a 64-bit build host:
 * OPEN -> ioctl(SETFMT/SPEED/STEREO/GETBLKSIZE) -> flood T_WRITE -> CLOSE.
 * Verifies the ring pump absorbs the burst, the ALSA thread drains it, and the
 * helper never stalls or crashes. Run against a manually launched helper:
 *     OSS_BRIDGE_LOG=/tmp/proto.log ./oss_helper &
 *     ./test_helper_proto
 * then inspect /tmp/proto.log for W# count and underruns=0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAGIC 0x4F535332u
#define T_OPEN_DSP 1
#define T_OPEN_MIXER 2
#define T_IOCTL 3
#define T_WRITE 4
#define T_CLOSE 6

#define OSS_DSP_SETFMT     0xC0045005u
#define OSS_DSP_SPEED      0xC0045002u
#define OSS_DSP_STEREO     0xC0045003u
#define OSS_DSP_GETBLKSIZE 0xC0045004u

struct hdr { uint32_t magic, type, tag, len; };

static int g_fd = -1;
static uint32_t g_tag = 1;

static int send_all(const void *buf, int n) {
    const char *p = (const char *)buf;
    while (n > 0) {
        int r = (int)send(g_fd, p, n, 0);
        if (r < 0) return -1;
        p += r; n -= r;
    }
    return 0;
}
static int recv_all(void *buf, int n) {
    char *p = (char *)buf;
    while (n > 0) {
        int r = (int)recv(g_fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

static int msg_no_body(uint32_t type) {
    struct hdr h = { MAGIC, type, g_tag++, 0 };
    if (send_all(&h, sizeof h) < 0) return -1;
    int32_t ret;
    if (recv_all(&ret, 4) < 0) return -1;
    return ret;
}
static int msg_ioctl(uint32_t req, uint32_t arg) {
    struct hdr h = { MAGIC, T_IOCTL, g_tag++, 8 };
    if (send_all(&h, sizeof h) < 0) return -1;
    uint32_t body[2] = { req, arg };
    if (send_all(body, 8) < 0) return -1;
    int32_t ret; uint32_t out_len;
    if (recv_all(&ret, 4) < 0) return -1;
    if (recv_all(&out_len, 4) < 0) return -1;
    if (out_len) { uint8_t b[64]; if (recv_all(b, out_len) < 0) return -1; }
    return ret;
}
static int msg_write(const void *buf, int n) {
    struct hdr h = { MAGIC, T_WRITE, g_tag++, (uint32_t)n };
    if (send_all(&h, sizeof h) < 0) return -1;
    if (send_all(buf, n) < 0) return -1;
    int32_t ret;
    if (recv_all(&ret, 4) < 0) return -1;
    return ret;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *path = getenv("OSS_BRIDGE_SOCK") ?: "/tmp/.oss_bridge.sock";
    g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un un; memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, path, sizeof un.sun_path - 1);
    if (connect(g_fd, (struct sockaddr *)&un, sizeof un) < 0) {
        perror("connect"); return 1;
    }
    printf("[client] connected to %s\n", path);

    if (msg_no_body(T_OPEN_DSP) != 0) { printf("OPEN_DSP failed\n"); return 1; }
    printf("[client] OPEN_DSP ok\n");
    msg_ioctl(OSS_DSP_SETFMT, 0x10);     /* S16_LE */
    msg_ioctl(OSS_DSP_SPEED, 44100);
    msg_ioctl(OSS_DSP_STEREO, 1);
    msg_ioctl(OSS_DSP_GETBLKSIZE, 0);
    printf("[client] ioctls sent (SETFMT/SPEED/STEREO/GETBLKSIZE)\n");

    const int N = 4096, FRAMES = N / 4;
    static int16_t pcm[2048];     /* N bytes = 2*FRAMES 个 S16 样本 */
    int total = 3000;        /* ~12 s of audio at burst rate */
    int ok = 0, dropped = 0;
    for (int i = 0; i < total; i++) {
        /* 440 Hz stereo S16 sine */
        for (int f = 0; f < FRAMES; f++) {
            int k = i*FRAMES + f;
            int16_t s = (int16_t)(25000 * ((k % 200 < 100) ? 1 : -1));  /* 整数方波 */
            pcm[f*2] = pcm[f*2+1] = s;
        }
        int r = msg_write(pcm, N);
        if (r < 0) { printf("[client] write %d failed: %s\n", i, strerror(errno)); break; }
        if (r == N) ok++; else dropped++;
    }
    printf("[client] wrote %d blocks: ack_full=%d nonfull=%d\n", total, ok, dropped);
    msg_no_body(T_CLOSE);
    printf("[client] CLOSE sent\n");
    close(g_fd);
    return 0;
}
