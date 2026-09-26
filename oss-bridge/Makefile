# Makefile for the Winamp3 OSS bridge (shim + helper)
#
# shim_oss.c  -> oss_shim.so   (32-bit, -nostdlib, LD_PRELOAD into Winamp.exe)
# helper_pulse.c -> oss_helper  (64-bit, host libasound via dlopen, plays via ALSA/PipeWire)
#
# ZERO extra packages required on the build host (Deepin VM):
#   * shim  : gcc -m32 -nostdlib -nostartfiles  (no gcc-multilib needed — verified)
#   * helper: gcc -ldl -lpthread, resolves libasound.so.2 at runtime (already present)
# The prebuilt binaries are committed into Winamp3.AppDir/oss-bridge/ so the
# shipped AppImage works out-of-the-box on any modern Linux with a pulse/ALSA
# sink — no osspd, no build tools on the end user's machine.

CC64 ?= gcc
CC32 ?= gcc

.PHONY: all clean

all: oss_helper oss_shim.so

oss_helper: helper_pulse.c
	$(CC64) -O2 -o oss_helper helper_pulse.c -ldl -lpthread

oss_shim.so: shim_oss.c
	$(CC32) -m32 -shared -fPIC -nostdlib -nostartfiles -Wl,--hash-style=sysv -O2 -o oss_shim.so shim_oss.c

clean:
	rm -f oss_helper oss_shim.so
