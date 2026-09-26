/*
 * shim_oss.c — 32-bit LD_PRELOAD shim for Winamp3 (RH9 glibc 2.3.2)
 * ----------------------------------------------------------------
 * Winamp3's of_oss.so open()s /dev/dsp (and /dev/mixer) and write()s raw PCM
 * into it via OSS ioctls. Modern kernels (Deepin 6.12) have no OSS layer, so
 * this shim intercepts those calls and forwards OSS operations to a 64-bit
 * helper over an AF_UNIX socket. The helper uses the host libpulse (PipeWire's
 * pulse compatibility socket), so Winamp3 gets sound WITHOUT needing osspd.
 *
 * Design constraints (why it is written this way):
 *   * This .so is loaded INTO the 32-bit WA3 process which runs on the BUNDLED
 *     RH9 glibc 2.3.2. If we linked against a modern 32-bit glibc we would hit
 *     GLIBC_2.x symbol-version clashes at load time. So this shim references
 *     NO libc symbol at all — every syscall goes through raw int 0x80, and
 *     string helpers are hand-rolled. It builds with
 *         gcc -m32 -shared -fPIC -nostdlib -nostartfiles
 *     and loads cleanly into any glibc.
 *   * If the helper is not running (connect fails), open("/dev/dsp") falls back
 *     to the real kernel node, so osspd still works as before (graceful).
 *
 * Build:  gcc -m32 -shared -fPIC -nostdlib -nostartfiles -O2 -o oss_shim.so shim_oss.c
 */

/* ---- no libc headers on purpose ---- */
typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long ulong;
typedef long          slong;
typedef unsigned long size_t;
typedef long          ssize_t;

#define AF_UNIX 1
#define SOCK_STREAM 1
#define O_RDWR  2
#define F_SETFL 4

/* i386 syscall numbers (host kernel 6.12 supports the independent ones) */
#define NR_OPEN      5
#define NR_CLOSE     6
#define NR_READ      3
#define NR_WRITE     4
#define NR_IOCTL     54
#define NR_FCNTL     55
#define NR_SOCKET    359
#define NR_CONNECT   362
#define NR_SENDTO    369
#define NR_RECVFROM  371

#define SOCK_PATH "/tmp/.oss_bridge.sock"
#define MAGIC     0x4F535332u   /* "OSS2" */

/* ---- message protocol (fixed-width, endian-clean across 32/64) ---- */
#define T_OPEN_DSP   1
#define T_OPEN_MIXER 2
#define T_IOCTL      3
#define T_WRITE      4
#define T_READ       5
#define T_CLOSE      6
#define T_SETFL      7

struct hdr { u32 magic; u32 type; u32 tag; u32 len; };

/* ===================== raw syscalls (i386 int 0x80) ===================== */
static inline slong sys(int n, slong a1, slong a2, slong a3, slong a4, slong a5) {
    slong r;
    __asm__ volatile("int $0x80"
        : "=a"(r)
        : "a"((slong)n), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
        : "memory", "cc");
    return r;
}
/* variant carrying a 6th argument in ebp (socket/connect/sendto/recvfrom) */
static inline slong sys6(int n, slong a1, slong a2, slong a3,
                         slong a4, slong a5, slong a6) {
    slong r;
    register slong ebp asm("ebp") = a6;
    __asm__ volatile("int $0x80"
        : "=a"(r)
        : "a"((slong)n), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), "r"(ebp)
        : "memory", "cc");
    return r;
}

/* ===================== tiny string helpers ===================== */
static int  my_strlen(const char *s){ int n=0; while(s[n]) n++; return n; }
static int  my_strcmp(const char *a, const char *b){
    while(*a && *a==*b){ a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void my_memcpy(void *d, const void *s, int n){
    char *p=d; const char *q=s; while(n-->0) *p++ = *q++;
}
static void my_memset(void *d, int v, int n){
    char *p=d; while(n-->0) *p++ = (char)v;
}
/* raw write to stderr (fd 2) for load-time diagnostics (no libc needed) */
static void dbg(const char *m){ sys(NR_WRITE, 2, (slong)m, (slong)my_strlen(m), 0, 0); }
/* write a string followed by a decimal long (for errno reporting) */
static void dbg_int(const char *m, long v){
    dbg(m);
    char b[16]; int i = 0; long x = v;
    if (x < 0) { b[i++] = '-'; x = -x; }
    if (x == 0) b[i++] = '0';
    int j = i; while (x > 0) { b[i++] = (char)('0' + (x % 10)); x /= 10; }
    int k; for (k = 0; k < (i - j) / 2; k++) { char t = b[j + k]; b[j + k] = b[i - 1 - k]; b[i - 1 - k] = t; }
    dbg(b);
    dbg("\n");
}

/* ===================== shim state ===================== */
static int  g_sock     = -1;   /* fd to helper, -1 = bridge disabled */
static int  g_dsp_fd   = -1;   /* fake fd we returned for /dev/dsp   */
static int  g_mixer_fd = -1;   /* fake fd we returned for /dev/mixer */

struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };

/* connect to helper once at load; leave g_sock=-1 if unavailable */
__attribute__((constructor))
static void shim_init(void) {
    g_sock = (int)sys6(NR_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
    if (g_sock < 0) return;
    struct sockaddr_un un;
    my_memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    my_memcpy(un.sun_path, SOCK_PATH, my_strlen(SOCK_PATH));
    slong r = sys6(NR_CONNECT, g_sock, (slong)&un, (slong)sizeof un, 0, 0, 0);
    if (r < 0) {
        sys(NR_CLOSE, g_sock, 0, 0, 0, 0);
        g_sock = -1;
        dbg_int("[shim] helper connect FAILED, errno=", (long)(-r));
        dbg("[shim] will retry on open()\n");
    } else {
        dbg("[shim] connected to helper, /dev/dsp bridged\n");
    }
}

/* ===================== socket helpers ===================== */
/* send a message (hdr + body); returns 0 on success */
static int send_msg(u32 type, u32 tag, const void *body, u32 len) {
    if (g_sock < 0) return -1;
    struct hdr h; h.magic = MAGIC; h.type = type; h.tag = tag; h.len = len;
    slong r = sys6(NR_SENDTO, g_sock, (slong)&h, (slong)sizeof h, 0, 0, 0);
    if (r < 0) return -1;
    if (len) {
        r = sys6(NR_SENDTO, g_sock, (slong)body, (slong)len, 0, 0, 0);
        if (r < 0) return -1;
    }
    return 0;
}
/* recv exactly n bytes into buf (blocking); returns n or -1 */
static int recv_exact(void *buf, int n) {
    int got = 0;
    char *p = buf;
    while (got < n) {
        slong r = sys6(NR_RECVFROM, g_sock, (slong)(p+got), (slong)(n-got), 0, 0, 0);
        if (r <= 0) return -1;
        got += (int)r;
    }
    return got;
}
/* request/response for open/close/write: send then read a 4-byte int result */
static i32 req_resp_int(u32 type, u32 tag, const void *body, u32 len) {
    if (send_msg(type, tag, body, len) < 0) return -1;
    i32 resp = -1;
    if (recv_exact(&resp, 4) < 0) return -1;
    return resp;
}

/*
 * ioctl variant: forward request + the int pointed to by arg, then read back
 * both the return code AND any output bytes (for GET-style ioctls such as
 * SNDCTL_DSP_GETBLKSIZE / GETOSPACE whose results must be written into *arg).
 */
static i32 req_resp_ioctl(u32 fd, u32 req, u32 argval, void *argptr) {
    u32 body[2]; body[0] = req; body[1] = argval;
    if (send_msg(T_IOCTL, fd, body, 8) < 0) return -1;
    i32  ret;     u32 out_len;
    if (recv_exact(&ret, 4) < 0) return -1;
    if (recv_exact(&out_len, 4) < 0) return -1;
    if (out_len) {
        static char obuf[64];
        if (out_len > (u32)sizeof obuf) out_len = (u32)sizeof obuf;
        if (recv_exact(obuf, (int)out_len) < 0) return -1;
        if (argptr) my_memcpy(argptr, obuf, (int)out_len);
    }
    return ret;
}

/* ===================== intercepted libc calls ===================== */

/* open — intercept /dev/dsp and /dev/mixer; return a fake fd, bridge traffic */
int open(const char *path, int flags, ...) {
    if (g_sock < 0) shim_init();   /* lazy (re)connect fallback */

    /* Bridge /dev/dsp and /dev/mixer WITHOUT requiring the real node to
     * exist.  Previously we did a real open() first and bailed out on
     * failure -- so with osspd uninstalled (no /dev/dsp) WA3 got -ENOENT
     * and silently gave up on audio (GUI fine, never any ioctl/write).
     * The bridged nodes are virtual anyway (we substitute /dev/null), so
     * existence of the real device file must not be a precondition. */
    if (g_sock >= 0 && (my_strcmp(path, "/dev/dsp") == 0 ||
                        my_strcmp(path, "/dev/dsp0") == 0)) {
        if (g_dsp_fd < 0)
            g_dsp_fd = (int)sys(NR_OPEN, (slong)"/dev/null", O_RDWR, 0, 0, 0);
        req_resp_int(T_OPEN_DSP, (u32)g_dsp_fd, 0, 0);
        return g_dsp_fd;
    }
    if (g_sock >= 0 && my_strcmp(path, "/dev/mixer") == 0) {
        if (g_mixer_fd < 0)
            g_mixer_fd = (int)sys(NR_OPEN, (slong)"/dev/null", O_RDWR, 0, 0, 0);
        req_resp_int(T_OPEN_MIXER, (u32)g_mixer_fd, 0, 0);
        return g_mixer_fd;
    }

    /* every other file: real open, unchanged behaviour */
    return (int)sys(NR_OPEN, (slong)path, (slong)flags, 0, 0, 0);
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == g_dsp_fd || fd == g_mixer_fd) {
        i32 ret = req_resp_int(T_WRITE, (u32)fd, buf, (u32)count);
        return (ret < 0) ? (ssize_t)count : (ssize_t)ret;  /* pretend ok */
    }
    return (ssize_t)sys(NR_WRITE, fd, (slong)buf, (slong)count, 0, 0);
}

/* ioctl — fixed 3-arg signature matches glibc's ABI on i386 (arg on stack) */
int ioctl(int fd, unsigned long req, void *arg) {
    if (fd == g_dsp_fd || fd == g_mixer_fd) {
        u32 argval = (arg) ? *(u32 *)arg : 0u;
        i32 ret = req_resp_ioctl((u32)fd, (u32)req, argval, arg);
        return (ret < 0) ? 0 : ret;
    }
    return (int)sys(NR_IOCTL, fd, (slong)req, (slong)arg, 0, 0);
}

int close(int fd) {
    if (fd == g_dsp_fd || fd == g_mixer_fd) {
        req_resp_int(T_CLOSE, (u32)fd, 0, 0);
        if (fd == g_dsp_fd) g_dsp_fd = -1; else g_mixer_fd = -1;
    }
    return (int)sys(NR_CLOSE, fd, 0, 0, 0, 0);
}

int fcntl(int fd, int cmd, ...) {
    if ((fd == g_dsp_fd || fd == g_mixer_fd) && cmd == F_SETFL) {
        return 0;   /* non-blocking request: helper write is effectively async */
    }
    return (int)sys(NR_FCNTL, fd, (slong)cmd, 0, 0, 0);
}
