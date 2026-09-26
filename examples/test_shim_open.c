/*
 * test_shim_open.c -- 32-bit probe for oss_shim.so
 *
 * Replays exactly what Winamp3's of_oss.so do_open()/ThreadProc do:
 *   open("/dev/dsp") -> SNDCTL_DSP_SETFMT -> SPEED -> STEREO -> GETBLKSIZE
 *   -> write(4096 bytes)
 *
 * Purpose: verify the shim bridges /dev/dsp even when the real node does
 * NOT exist (osspd uninstalled). Before the fix, shim's open() did a real
 * open() first and returned -ENOENT, so WA3 silently disabled audio.
 *
 * Build : gcc -m32 -O2 -o test_shim_open test_shim_open.c
 * Run   : (helper must be running)
 *         LD_PRELOAD=./oss_shim.so ./test_shim_open
 * Expect: fd >= 0 and all ioctls/writes succeed.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* OSS ioctls observed in the of_oss.so disassembly */
#define SNDCTL_DSP_RESET      0x00005000
#define SNDCTL_DSP_SPEED      0xc0045002
#define SNDCTL_DSP_STEREO     0xc0045003
#define SNDCTL_DSP_GETBLKSIZE 0xc0045004
#define SNDCTL_DSP_SETFMT     0xc0045005
#define AFMT_S16_LE           0x00000010

int main(void)
{
    int fd, r, v;
    char buf[4096];

    printf("[test] /dev/dsp present? ");
    fflush(stdout);
    if (access("/dev/dsp", F_OK) == 0)
        printf("YES (real node exists)\n");
    else
        printf("NO  (osspd uninstalled -- this is the case we fix)\n");

    fd = open("/dev/dsp", O_WRONLY);
    printf("[test] open(\"/dev/dsp\") = %d\n", fd);
    if (fd < 0) {
        printf("[test] FAILED: %s  <-- shim did NOT bridge\n", strerror(errno));
        return 1;
    }
    printf("[test] PASS: bridged fd obtained without a real device node\n");

    v = 0;              r = ioctl(fd, SNDCTL_DSP_RESET, &v);
    printf("[test] RESET      -> ret=%d\n", r);

    v = AFMT_S16_LE;    r = ioctl(fd, SNDCTL_DSP_SETFMT, &v);
    printf("[test] SETFMT     -> ret=%d val=0x%x (want 0x10)\n", r, v);

    v = 44100;          r = ioctl(fd, SNDCTL_DSP_SPEED, &v);
    printf("[test] SPEED      -> ret=%d val=%d (want 44100)\n", r, v);

    v = 1;              r = ioctl(fd, SNDCTL_DSP_STEREO, &v);
    printf("[test] STEREO     -> ret=%d val=%d (want 1)\n", r, v);

    v = 0;              r = ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &v);
    printf("[test] GETBLKSIZE -> ret=%d val=%d (want 4096)\n", r, v);

    memset(buf, 0, sizeof buf);
    printf("[test] write(4096) -> %zd\n", write(fd, buf, sizeof buf));

    close(fd);
    printf("[test] done\n");
    return 0;
}
