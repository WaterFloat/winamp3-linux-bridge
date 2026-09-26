# Winamp3 Linux Bridge - Reviving a Classic Player on Modern Linux


> **中文版**：[中文版本请点击查看](README_zh.md)

> **Project Overview**: Using AI-assisted development (Tencent WorkBuddy), this project enables ordinary users to run Winamp3 from 2002 on modern Linux systems without complex audio configuration.

## 🌟 Project Highlights

> **Note**: Winamp3 was released in 2002, over 20 years ago. As very old software, it may not be fully stable — the fact that it can run on modern Linux at all is already impressive. The core value of this project lies in proving that with AI assistance, ordinary users can also make classic software "come back to life" in a new era.

### 1. AI-Powered Revival of Vintage Software

This project demonstrates how **AI assistance** can solve the common problem of "classic software that no longer runs on modern systems":

- **Reverse Engineering**: AI-assisted analysis of decompiled binaries to understand original plugin interfaces
- **Intelligent Bridge Design**: shim + helper architecture to transparently intercept OSS audio calls
- **Automated Debugging**: Root-cause analysis from crash logs with iterative fixes

### 2. Tech Stack

| Component | Description |
|-----------|-------------|
| **Target Software** | Winamp3 Linux alpha1 (2002-12-12), 32-bit x86 ELF |
| **AI Assistant** | Tencent WorkBuddy (local multimodal AI assistant) |
| **Packaging** | AppImage (single-file portable app) |
| **Audio Bridge** | shim_oss.so + oss_helper (AF_UNIX socket communication) |
| **Testing** | 64-bit protocol test client + 32-bit probe |

### 3. Core Value: Green Solution, Zero Additional Dependencies

**Traditional approach**: Install `osspd` (OSS over PulseAudio bridge) — requires root privileges, kernel modules, and extra system packages.

**Our approach**: No dependency on any third-party audio bridge. The AppImage ships its own shim+helper layer, connecting directly to the host system's ALSA/PipeWire.

✅ **No osspd needed** — works even after osspd is uninstalled  
✅ **No PulseAudio configuration** — auto-adapts to ALSA / PipeWire / PulseAudio  
✅ **Green & portable** — single AppImage file, copy and run, no system pollution  
✅ **AI-assisted development** — fully supported by Tencent WorkBuddy  

---

## 🔧 Problem Background

### The Original Challenge

Winamp3 Linux was a classic music player released in 2002. Its audio output plugin (`of_oss.so`) directly calls the Linux kernel's OSS device interface `/dev/dsp`. However:

1. **Modern kernels have removed the OSS subsystem** — `/dev/dsp` no longer exists
2. **Users want to run vintage software without deep system knowledge** — need a simple solution

### The Pain of Traditional Solutions: osspd

Historically, the standard way to make old OSS software work on modern Linux was to install **osspd**:

```bash
# Traditional approach — requires root + system packages + kernel modules
sudo apt install osspd osspd-pulseaudio   # Install userspace bridge service
sudo modprobe cuse                         # Load kernel character device module
# Now /dev/dsp appears and Winamp3 produces sound
```

**Problems**:
- Requires root privileges for installation and configuration
- Introduces extra system dependencies (osspd + cuse kernel module)
- Difficult to deploy in containerized/headless/minimal systems
- Some modern distributions (Fedora/Arch) don't even have osspd packages

### Our Green Solution

**Zero additional installs, zero system modifications**. The AppImage ships a complete audio bridge layer that connects directly to the host's ALSA/PipeWire:

```
Winamp3.exe → oss_shim.so (LD_PRELOAD) → AF_UNIX Socket → oss_helper (64-bit) → ALSA/PipeWire
```

| Comparison | Traditional osspd | This Project's Bridge |
|------------|-------------------|-----------------------|
| Install osspd | ✅ Required | ❌ Not needed |
| Load cuse module | ✅ Required | ❌ Not needed |
| Root privileges | ✅ Required | ❌ Not needed |
| System dependencies | 3 packages (osspd+osspd-pulseaudio+cuse) | 0 extra dependencies |
| Portability | Must configure per machine | Single file, runs anywhere |

**Evidence**: We verified on a VM that the bridge version plays audio correctly even after osspd is completely uninstalled, while the non-bridge version is completely silent.

---

## 🛠️ Technical Implementation

### Core Components

```
oss-bridge/
├── shim_oss.c          # 32-bit shim, intercepts OSS syscalls
├── helper_pulse.c      # 64-bit helper, forwards to ALSA/PipeWire
├── bridge_stress.c     # Stress test tool
├── test_helper_proto.c  # 64-bit protocol test
├── test_shim_open.c    # 32-bit probe (requires multilib)
└── Makefile            # Build script
```

### Key Code Explanations

#### 1. Shim Implementation (`shim_oss.c`)

Intercepts Winamp3's audio calls. Key fix:

```c
int open(const char *path, int flags, ...) {
    if (g_sock >= 0 && (my_strcmp(path, "/dev/dsp") == 0 ||
                        my_strcmp(path, "/dev/dsp0") == 0)) {
        // No longer depends on a real /dev/dsp node
        g_dsp_fd = open("/dev/null", O_RDWR);
        req_resp_int(T_OPEN_DSP, (u32)g_dsp_fd, 0, 0);
        return g_dsp_fd;
    }
    return real_open(path, flags);  // Other files handled normally
}
```

**Why this design?**
- After osspd is uninstalled, `/dev/dsp` no longer exists
- Old shim tried real `open()` first, which failed → bridge branch never executed
- Fixed version goes directly to the bridge without depending on the node existing

#### 2. Helper Implementation (`helper_pulse.c`)

Three-layer protection ensures stability:

```c
// Layer 1: Non-blocking PCM open
int r = alsa_open(&p, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);

// Layer 2: Stream self-heal (when PipeWire drops the connection)
if (alsa_recover(g_pcm, (int)w, 1) < 0) {
    alsa_close(g_pcm); 
    g_pcm = NULL;  // Auto-reopens on next write
}

// Layer 3: Backpressure timeout protection
if (pthread_cond_timedwait(&g_cv_space, &g_mu, &ts) == ETIMEDOUT) {
    dropped = 1;  // Discard this block, keep playing
}
```

**Purpose of each layer:**

| Layer | Problem | Solution |
|-------|---------|----------|
| Non-blocking | ALSA thread deadlock | Returns -EAGAIN when full |
| Stream self-heal | PipeWire disconnect crash | Auto-reopens PCM |
| Backpressure timeout | Infinite write blocking | Discards after 250ms |

---

## 🤖 AI-Assisted Development Process

### Problems Solved with WorkBuddy

| Problem | AI Analysis | Solution |
|---------|-------------|----------|
| No sound after osspd uninstall | Log analysis: zero ioctl/W# | shim open() fix |
| Crash after playing | PipeWire log: `Broken pipe` | Three-layer protection fix |
| High interaction latency | Decompilation: WA3 floods 4x speed | 512KB ring buffer |
| Multi-instance conflicts | Process check logic | AppRun single-instance limit |

### Key Debugging Steps

```bash
# 1. Diagnose no-sound issue
tail -20 ~/oss_bridge.log
# Found: only "listening" + "client connected", no ioctl/W#

# 2. Locate crash cause
journalctl -n 60 --no-pager | grep -iE "pipewire|alsa"
# Found: "snd_pcm_avail after recover: Broken pipe"

# 3. Verify fix
./test_helper_proto  # 64-bit protocol test
# Result: underruns=0, helper alive, 3000 blocks written successfully
```

---

## 📦 Deployment Guide

### Build Bridge Components

```bash
cd oss-bridge
make clean && make
```

### Build AppImage

```bash
cd ../appimage
bash build.sh
# Output: Winamp3-i686.AppImage (~15MB)
```

### Run

```bash
chmod +x Winamp3-i686.AppImage
./Winamp3-i686.AppImage
```

### Single-Instance Limit (Implemented)

The AppRun script includes process checking to prevent conflicts from multiple instances:

```bash
INSTANCE_PID_FILE="${HOME}/.local/share/Winamp3-instance.pid"
# Check if PID exists and is running
if [ -f "$INSTANCE_PID_FILE" ] && kill -0 "$(cat $INSTANCE_PID_FILE)" 2>/dev/null; then
    echo "Winamp3 is already running (PID: $(cat $INSTANCE_PID_FILE)). Exiting."
    exit 1
fi
```

---

## 📊 Verification Data

### osspd Uninstall Verification (Proof of Green Solution)

We tested audio playback on a Deepin 25 VM with osspd **completely uninstalled**:

```bash
# 1. Uninstall osspd (symmetric 2-step)
sudo apt remove -y osspd osspd-pulseaudio
sudo modprobe -r cuse

# 2. Confirm dependencies are cleared
ls /dev/dsp          # → No such file or directory ✅
lsmod | grep cuse    # → (empty) ✅
dpkg -l | grep osspd # → (empty) ✅

# 3. Run bridge version AppImage — audio plays normally ✅
#    Run non-bridge version (c0328d88...) — completely silent ❌
```

**Log Evidence**:
- Bridge version log: `OPEN /dev/dsp` → ioctl sequence → 666 `W#` writes → `underruns=0`
- Non-bridge version log: No ioctl/W# records at all (bridge branch never executed)

This proves the bridge solution **works completely without osspd** — a true green solution.

### Fix Comparison

| Metric | Before Fix | After Fix |
|--------|------------|-----------|
| Play after osspd uninstall | ❌ No sound | ✅ Normal |
| PipeWire disconnect crash | ❌ Crashes | ✅ Self-heals |
| Audio glitches | ✅ underruns=0 | ✅ underruns=0 |
| Interaction latency | ~3s | ~1.85s (256KB optimized) |
| Multi-instance conflict | ❌ Possible | ✅ Single-instance limited |

### Actual Test Log

```text
[ 25794.8] playback STARTED (banked 8192 bytes = 46 ms cushion)
[ 25794.9] W#0      len=4096   gap=361.48ms ring=4096    ( 23ms) ur=0
[ 25795.0] W#1      len=4096   gap=  0.00ms ring=8192    ( 46ms) ur=0
...
[ 26098.2] playback STARTED (banked 8192 bytes = 46 ms cushion)
```

**Key Metrics**:
- `underruns=0` — no audio glitches
- `ring=4096` — ring buffer healthy
- Auto-reopen capability — no crashes

---

## 🎯 Project Significance

### Value for Ordinary Users

1. **No technical background needed** — double-click to run, no audio system configuration
2. **Preserve classic experience** — Winamp3's iconic interface and features intact
3. **Cross-version compatibility** — works from Deepin 25 to Ubuntu 22.04+

### Insights for Developers

1. **AI-assisted debugging efficiency** — from problem discovery to fix verification in hours
2. **Reverse engineering value** — decompiling to understand interfaces is the prerequisite for fixes
3. **Bridge design pattern** — shim+helper architecture reusable for similar problems

---

## 📁 Directory Structure

```
github-repo/
├── README.md              # This document
├── README_en.md           # English version
├── AppRun                 # Application launcher
├── oss-bridge/            # Bridge component source
│   ├── shim_oss.c         # 32-bit shim
│   ├── helper_pulse.c     # 64-bit helper
│   ├── bridge_stress.c    # Stress test
│   ├── test_helper_proto.c # Protocol test
│   └── Makefile
├── docs/                  # Technical documentation
│   ├── REVERSE_ENGINEERING_SUMMARY.md
│   ├── STAGE1_FRAGMENTS.md
│   └── STAGE2_INTERFACES.md
└── LICENSE
```

---

## 🙏 Acknowledgments

- **Winamp** — for creating the classic music player
- **Tencent WorkBuddy** — for AI-assisted development support
- **Linux Community** — for ALSA, PipeWire, AppImage and other projects

---

## 📝 License

Project code is licensed under MIT License. Winamp3 is a trademark of WildTech, Inc. This project is for technical research purposes only.

---

**Star ⭐ if this project helps you!**
