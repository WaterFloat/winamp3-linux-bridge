# Winamp3 Linux Bridge - 让经典播放器在现代 Linux 上重生

> **项目总览**：[English version](README.md)

> **项目简介**: 使用 AI 辅助技术（腾讯 WorkBuddy），帮助普通用户将 2002 年的 Winamp3 播放器在现代 Linux 系统上成功运行，无需复杂的音频系统配置。

## 🌟 项目亮点

> **注**：Winamp3 发布于 2002 年，距今已超过 20 年。作为非常老的软件，它不一定稳定，能实现在现代 Linux 上运行已经非常不错了。本项目的核心价值在于证明：借助 AI 辅助，普通人也能让经典老软件在新时代"复活"。

### 1. AI 赋能的复古软件复活

本项目展示了如何利用 **人工智能辅助** 的方式，解决"经典老软件在现代系统上无法运行"的常见问题：

- **逆向工程分析**：通过 AI 辅助分析反编译的二进制文件，理解原始插件接口
- **智能桥接方案**：设计 shim + helper 架构，透明拦截 OSS 音频调用
- **自动化故障排除**：从崩溃日志定位根因，迭代修复

### 2. 技术栈

| 组件 | 说明 |
|------|------|
| **目标软件** | Winamp3 Linux alpha1 (2002-12-12)，32-bit x86 ELF |
| **AI 辅助工具** | 腾讯 WorkBuddy (本地多模态 AI 助手) |
| **构建系统** | AppImage (单文件便携应用) |
| **音频桥接** | shim_oss.so + oss_helper (AF_UNIX socket 通信) |
| **测试验证** | 64位协议测试客户端 + 32位探针 |

### 3. 核心价值：绿色方案，零额外依赖

**传统做法**：给老软件安装 `osspd`（OSS over PulseAudio 桥接工具）——需要 root 权限、内核模块、额外系统包。

**我们的做法**：完全不依赖任何第三方音频桥接工具。AppImage 内自带 shim+helper，直接走系统 ALSA/PipeWire。

✅ **无需安装 osspd** — 即使系统已卸载 osspd 也能正常播放  
✅ **无需配置 PulseAudio** — 自动适配 ALSA / PipeWire / PulseAudio  
✅ **绿色便携** — 单文件 AppImage，复制即用，不污染系统  
✅ **AI 辅助开发** — 全程由腾讯 WorkBuddy 提供支持  

---

## 🔧 问题背景

### 原始挑战

Winamp3 Linux 是 2002 年发布的经典音乐播放器，它的音频输出插件 (`of_oss.so`) 直接调用 Linux 内核的 OSS 设备接口 `/dev/dsp`。然而：

1. **现代内核已移除 OSS 子系统** - `/dev/dsp` 不存在
2. **用户想运行老软件但不熟悉系统配置** - 需要简单方案

### 传统方案的痛点：osspd

历史上，让老 OSS 软件在现代 Linux 上出声的标准做法是安装 **osspd**：

```bash
# 传统做法 —— 需要 root 权限 + 安装系统包 + 加载内核模块
sudo apt install osspd osspd-pulseaudio   # 安装用户态桥接服务
sudo modprobe cuse                         # 加载内核字符设备模块
# 现在 /dev/dsp 会出现，Winamp3 能出声
```

**问题**：
- 需要 root 权限安装和配置
- 引入额外系统依赖（osspd + cuse 模块）
- 在容器化/无头/精简系统中难以部署
- 部分现代发行版（Fedora/Arch）甚至没有 osspd 包

### 我们的绿色解法

**零额外安装，零系统修改**。AppImage 内自带完整的音频桥接层，直接走宿主系统的 ALSA/PipeWire：

```
Winamp3.exe → oss_shim.so (LD_PRELOAD) → AF_UNIX Socket → oss_helper (64-bit) → ALSA/PipeWire
```

| 对比项 | 传统 osspd 方案 | 本项目的桥接方案 |
|--------|----------------|------------------|
| 安装 osspd | ✅ 必须 | ❌ 不需要 |
| 加载 cuse 模块 | ✅ 必须 | ❌ 不需要 |
| root 权限 | ✅ 需要 | ❌ 不需要 |
| 系统依赖 | 3 个包（osspd+osspd-pulseaudio+cuse） | 0 个额外依赖 |
| 便携性 | 需每台机器配置 | 单文件即跑 |

**实证**：我们在 VM 上实测卸载 osspd 后，桥接版 AppImage 依然正常出声，而传统无桥接版完全无声。

---

## 🛠️ 技术实现详解

### 核心组件

```
oss-bridge/
├── shim_oss.c      # 32位 shim，拦截 OSS 系统调用
├── helper_pulse.c  # 64位 helper，转发到 ALSA/PipeWire
├── bridge_stress.c # 压力测试工具
├── test_helper_proto.c  # 64位协议测试
├── test_shim_open.c   # 32位探针（需 multilib）
└── Makefile        # 构建脚本
```

### 关键代码说明

#### 1. Shim 实现 (`shim_oss.c`)

拦截 Winamp3 的音频调用，关键修复：

```c
int open(const char *path, int flags, ...) {
    if (g_sock >= 0 && (my_strcmp(path, "/dev/dsp") == 0 ||
                        my_strcmp(path, "/dev/dsp0") == 0)) {
        // 不再依赖真实 /dev/dsp 节点
        g_dsp_fd = open("/dev/null", O_RDWR);
        req_resp_int(T_OPEN_DSP, (u32)g_dsp_fd, 0, 0);
        return g_dsp_fd;
    }
    return real_open(path, flags);  // 其他文件正常处理
}
```

**为什么这样设计？**
- osspd 卸载后 `/dev/dsp` 不存在
- 旧 shim 先尝试真实 open 失败 → 桥接分支不执行
- 修复后直接走桥接，不依赖真实节点存在

#### 2. Helper 实现 (`helper_pulse.c`)

三层防护确保稳定性：

```c
// 第一层：非阻塞 PCM 打开
int r = alsa_open(&p, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);

// 第二层：流断裂自愈（PipeWire 断管时）
if (alsa_recover(g_pcm, (int)w, 1) < 0) {
    alsa_close(g_pcm); 
    g_pcm = NULL;  // 下次写入自动重开
}

// 第三层：背压超时保护
if (pthread_cond_timedwait(&g_cv_space, &g_mu, &ts) == ETIMEDOUT) {
    dropped = 1;  // 丢弃该块，继续播放
}
```

**三层防护的作用：**
| 层级 | 问题 | 解决 |
|------|------|------|
| 非阻塞 | ALSA 线程卡死 | 满时返回 -EAGAIN |
| 流自愈 | PipeWire 断管崩溃 | 自动重开 PCM |
| 背压超时 | 写入阻塞无限等待 | 250ms 后丢弃 |

---

## 🤖 AI 辅助开发过程

### 使用 WorkBuddy 解决的问题

| 问题 | AI 辅助分析 | 解决方案 |
|------|-------------|----------|
| osspd 卸载后无声 | 日志分析：零 ioctl/W# | shim open 修复 |
| 运行后崩溃 | PipeWire 日志：`断开的管道` | 三层防护修复 |
| 交互延迟高 | 反编译分析：WA3 4x 倍速灌流 | 256KB 环形缓冲 |
| 多实例冲突 | 进程检查逻辑 | AppRun 单实例限制 |

### 关键调试步骤

```bash
# 1. 诊断无声问题
tail -20 ~/oss_bridge.log
# 发现：只有 "listening" + "client connected"，无 ioctl/W#

# 2. 定位崩溃原因
journalctl -n 60 --no-pager | grep -iE "pipewire|alsa"
# 发现："snd_pcm_avail after recover: 断开的管道"

# 3. 验证修复
./test_helper_proto  # 64位协议测试
# 结果：underruns=0, helper 存活，3000 块写入成功
```

---

## 📦 部署指南

### 编译桥接组件

```bash
cd oss-bridge
make clean && make
```

### 构建 AppImage

```bash
cd ../appimage
bash build.sh
# 产物：Winamp3-i686.AppImage (~15MB)
```

### 运行

```bash
chmod +x Winamp3-i686.AppImage
./Winamp3-i686.AppImage
```

### 单实例限制（已实现）

AppRun 脚本已包含实例检查，防止多开导致冲突：

```bash
INSTANCE_PID_FILE="${HOME}/.local/share/Winamp3-instance.pid"
# 检查 PID 是否存在且运行中
if [ -f "$INSTANCE_PID_FILE" ] && kill -0 "$(cat $INSTANCE_PID_FILE)" 2>/dev/null; then
    echo "Winamp3 is already running (PID: $(cat $INSTANCE_PID_FILE)). Exiting."
    exit 1
fi
```

---

## 📊 验证数据

### osspd 卸载验证（绿色方案铁证）

我们在 Deepin 25 VM 上实测了 osspd **完全卸载**后的播放效果：

```bash
# 1. 卸载 osspd（对称 2 步）
sudo apt remove -y osspd osspd-pulseaudio
sudo modprobe -r cuse

# 2. 确认依赖已清除
ls /dev/dsp          # → No such file or directory ✅
lsmod | grep cuse    # → (空) ✅
dpkg -l | grep osspd # → (空) ✅

# 3. 运行桥接版 AppImage —— 正常出声 ✅
#    运行无桥接版（c0328d88...）—— 完全无声 ❌
```

**日志证据**：
- 桥接版日志：`OPEN /dev/dsp` → ioctl 序列 → 666 次 `W#` 写入 → `underruns=0`
- 无桥接版日志：无任何 ioctl/W# 记录（shim 未执行桥接分支）

这证明了桥接方案**完全不需要 osspd**，是真正的绿色方案。

### 修复效果对比

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| osspd 卸载后播放 | ❌ 无声 | ✅ 正常 |
| PipeWire 断管崩溃 | ❌ 崩溃 | ✅ 自愈 |
| 音频爆音 | ✅ underruns=0 | ✅ underruns=0 |
| 交互延迟 | ~3s | ~1.85s (256KB 优化) |
| 多实例冲突 | ❌ 可能冲突 | ✅ 单实例限制 |

### 实际测试日志

```text
[ 25794.8] playback STARTED (banked 8192 bytes = 46 ms cushion)
[ 25794.9] W#0      len=4096   gap=361.48ms ring=4096    ( 23ms) ur=0
[ 25795.0] W#1      len=4096   gap=  0.00ms ring=8192    ( 46ms) ur=0
...
[ 26098.2] playback STARTED (banked 8192 bytes = 46 ms cushion)
```

**关键指标**：
- `underruns=0` - 无爆音
- `ring=4096` - 环形缓冲健康
- 自动重开能力 - 无崩溃

---

## 🎯 项目意义

### 对普通用户的价值

1. **无需技术背景** - 双击运行，无需配置音频系统
2. **保留经典体验** - Winamp3 标志性界面和功能完好
3. **跨版本兼容** - 从 Deepin 25 到 Ubuntu 22.04+ 均可运行

### 对开发者的启示

1. **AI 辅助调试效率** - 从发现问题到修复验证仅需数小时
2. **逆向工程价值** - 反编译理解接口是修复的前提
3. **桥接设计模式** - shim+helper 架构可复用于类似问题

---

## 📁 目录结构

```
github-repo/
├── README.md              # 本文档（英文主版）
├── README_zh.md           # 中文版（本文档）
├── AppRun                 # 应用启动器
├── oss-bridge/            # 桥接组件源码
│   ├── shim_oss.c         # 32位 shim
│   ├── helper_pulse.c     # 64位 helper
│   ├── bridge_stress.c    # 压力测试
│   ├── test_helper_proto.c # 协议测试
│   └── Makefile
├── docs/                  # 技术文档
│   ├── REVERSE_ENGINEERING_SUMMARY.md
│   ├── STAGE1_FRAGMENTS.md
│   └── STAGE2_INTERFACES.md
└── LICENSE
```

---

## 🙏 致谢

- **Winamp** - 创造经典音乐播放器
- **腾讯 WorkBuddy** - 提供 AI 辅助开发支持
- **Linux 社区** - ALSA、PipeWire、AppImage 等项目

---

## 📝 License

本项目代码采用 MIT License。Winamp3 是 WildTech, Inc. 的注册商标，本项目仅用于技术研究。

---

**Star ⭐ 如果这个项目对你有帮助！**
