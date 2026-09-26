# Winamp3 Linux Bridge 技术架构

## 架构图

```
┌─────────────────────────────────────────────────────────────┐
│                    用户空间 (User Space)                      │
│                                                             │
│  ┌─────────────────┐                                        │
│  │  Winamp3.exe    │ ← 2002年 32-bit x86 ELF                │
│  │  (老播放器)      │   只认 /dev/dsp (OSS音频)               │
│  └────────┬────────┘                                        │
│           │ open()/write()/ioctl()                          │
│           ▼                                                   │
│  ┌─────────────────┐                                        │
│  │  oss_shim.so   │ ← LD_PRELOAD 拦截                         │
│  │  (32位 shim)   │   拦截 OSS 系统调用                       │
│  └────────┬────────┘                                        │
│           │ 转发到 AF_UNIX Socket                            │
│           ▼                                                   │
│  ┌─────────────────┐                                        │
│  │  oss_helper    │ ← 64位独立进程                           │
│  │  (64位 helper) │   使用 ALSA/PipeWire                     │
│  └────────┬────────┘                                        │
│           │ snd_pcm_writei()                                 │
│           ▼                                                   │
│  ┌─────────────────┐                                        │
│  │  ALSA / PipeWire│ ← 现代音频栈                             │
│  │  (系统音频)     │   播放声音                               │
│  └─────────────────┘                                        │
└─────────────────────────────────────────────────────────────┘
```

---

## 核心组件详解

### 1. shim_oss.c - OSS 拦截层

**作用**: 作为 `LD_PRELOAD` 库注入到 Winamp3 进程中，拦截所有 OSS 相关系统调用。

**关键功能**:
- `open()` - 拦截 `/dev/dsp` 和 `/dev/mixer` 的打开请求
- `write()` - 拦截 PCM 数据写入，转发到 helper 进程
- `ioctl()` - 拦截音频格式、速率、声道等配置参数
- `close()` - 监听关闭请求，清理资源

**设计要点**:
```c
// 使用 raw syscall 避免依赖 libc（兼容 32位 + 64位混合环境）
int open(const char *path, int flags, ...) {
    // 对 /dev/dsp 直接桥接，不要求真实节点存在
    if (my_strcmp(path, "/dev/dsp") == 0) {
        g_dsp_fd = open("/dev/null", O_RDWR);  // 占位 fd
        req_resp_int(T_OPEN_DSP, g_dsp_fd, 0); // 通知 helper
        return g_dsp_fd;
    }
    return real_open(path, flags);  // 其他文件走真实路径
}
```

**为什么用 raw syscall？**
- 避免链接 32位 libc（Winamp3 使用老版 glibc 2.3.2）
- 最小化依赖，保证在多种 Linux 发行版上可用

---

### 2. helper_pulse.c - 音频转发层

**作用**: 64位独立进程，通过 AF_UNIX socket 接收 shim 发来的音频数据，交给系统音频栈播放。

**架构**:
```
┌─────────────────────────────────────────────┐
│               oss_helper 进程                 │
│                                             │
│  ┌─────────────┐    ┌──────────────────┐   │
│  │ Socket 线程  │───→│ 环形缓冲 (Ring)   │   │
│  │ (接收数据)   │    │ 512KB / 256KB    │   │
│  └─────────────┘    └────────┬─────────┘   │
│                              │              │
│                    ┌─────────▼─────────┐   │
│                    │   ALSA 播放线程    │   │
│                    │ (非阻塞 PCM)      │   │
│                    └─────────┬─────────┘   │
│                              │              │
│                    ┌─────────▼─────────┐   │
│                    │  snd_pcm_writei()  │   │
│                    └─────────┬─────────┘   │
└─────────────────────────────────────────────┘
                         │
                         ▼
                   ALSA / PipeWire
```

**三层防护机制**:

| 层 | 问题 | 解决方案 |
|----|------|----------|
| 1 | ALSA 线程卡死 | 非阻塞 PCM (`SND_PCM_NONBLOCK`) |
| 2 | PipeWire 断管崩溃 | 自动重开 PCM + 重生播放线程 |
| 3 | 写入阻塞无限等待 | 250ms 超时后丢弃，保持播放 |

**环形缓冲大小选择**:
- 初始: 512KB (~2.9秒音频缓冲)
- 优化尝试: 256KB (~1.5秒缓冲) → 导致 GUI 卡死 → 回退到 512KB

---

### 3. AppRun - 应用启动器

**功能**:
1. 设置 `LD_LIBRARY_PATH` (指向 bundled 32位库)
2. 启用 OSS 桥接 (默认开启)
3. 启动 `oss_helper` 进程
4. 设置 `LD_PRELOAD` 加载 `oss_shim.so`
5. 启动 `Winamp.exe`
6. 修复 WM_CLASS 以便任务栏正确显示

**单实例限制**:
```bash
INSTANCE_PID_FILE="${HOME}/.local/share/Winamp3-instance.pid"
# 检查并创建锁文件
```

---

## 通信协议

### Socket 消息格式

```c
struct msg_hdr {
    uint32_t magic;   // 0x4F535332 ('OSS2')
    uint32_t type;    // 消息类型
    uint32_t tag;     // 请求序号
    uint32_t len;     // 数据长度
};

// 消息类型
enum {
    T_OPEN_DSP   = 1,
    T_OPEN_MIXER = 2,
    T_IOCTL      = 3,
    T_WRITE      = 4,
    T_CLOSE      = 6,
    T_SETFL      = 7,
};
```

### 典型工作流程

1. WA3 调用 `open("/dev/dsp")`
2. shim 捕获，发送到 helper: `{magic: 'OSS2', type: T_OPEN_DSP}`
3. helper 回复: fd 占位符
4. WA3 调用 `ioctl(fd, SOUND_PCM_WRITE_BITS, &format)`
5. shim 捕获，转发到 helper
6. helper 配置 ALSA 格式，回复成功
7. WA3 调用 `write(fd, pcm_data, 4096)`
8. shim 捕获，通过 socket 发送 PCM 数据
9. helper 存入环形缓冲，播放线程消费

---

## 技术挑战与解决

### 挑战 1: osspd 卸载后的兼容

**问题**: 用户卸载 osspd 后，`/dev/dsp` 不存在，旧 shim 尝试真实 open 失败导致无声。

**解决**: 修改 `shim_oss.c` 的 `open()` 函数，对 `/dev/dsp` 直接走桥接，不要求真实节点存在。

### 挑战 2: PipeWire 断管导致崩溃

**问题**: PipeWire 流断裂时，`snd_pcm_writei` 返回 `-EPIPE`，`alsa_recover` 失败后旧代码直接 `return NULL` 放弃线程，导致环形缓冲不被排空，最终 WA3 的 `write()` 永远阻塞。

**解决**: 实现三层防护：
1. 非阻塞 PCM 打开
2. 流断裂时自动重开 PCM 而非放弃
3. 背压超时保护，确保 WA3 永远不会无限等待

### 挑战 3: 32位/64位混合运行

**问题**: Winamp3 是 32位程序，helper 需要是 64位（以链接现代 libasound）。

**解决**: 
- shim 用 `-nostdlib` 编译，零依赖
- helper 正常编译为 64位 ELF
- 通过 AF_UNIX socket 跨进程通信

---

## 编译方法

### 构建桥接组件

```bash
cd oss-bridge
make
```

产出:
- `oss_shim.so` - 32位 shim (用于 LD_PRELOAD)
- `oss_helper` - 64位 helper 可执行文件

### 构建测试工具

```bash
# 64位协议测试 (无需 multilib)
gcc -O2 -o test_helper_proto test_helper_proto.c

# 32位探针 (需要 gcc-multilib)
gcc -m32 -O2 -o test_shim_open test_shim_open.c
```

---

## 验证方法

### 1. 协议测试 (64位)

```bash
# 启动 helper
./oss_helper &

# 运行测试客户端
./test_helper_proto

# 检查日志
grep -E "OPEN|W#|underruns" ~/oss_bridge.log
```

预期: `underruns=0`, `W#` 数量与测试块数一致

### 2. 压测

```bash
./bridge_stress
```

模拟大量随机音频块写入，验证稳定性。

### 3. 实播验证

```bash
./Winamp3-i686.AppImage
# 播放音乐，观察是否有爆音、卡顿或崩溃
# 查看日志: tail -f ~/oss_bridge.log
```

---

## 性能指标

| 指标 | 数值 | 说明 |
|------|------|------|
| 音频延迟 | ~2.9s | 512KB 环形缓冲 |
| 交互延迟 | ~3s | 切歌/暂停响应时间 |
| 内存占用 | ~5MB | helper 进程常驻 |
| CPU 占用 | <1% | 高效环形缓冲机制 |
| underruns | 0 | 无爆音 |

---

## 未来优化方向

1. **进一步降低延迟**: 探索更小的环形缓冲 (128KB?)
2. **多音频流支持**: 同时支持多个播放器实例
3. **可视化插件**: 为 Winamp3 的可视化插件提供桥接支持
4. **跨平台扩展**: 移植到 macOS、Windows
