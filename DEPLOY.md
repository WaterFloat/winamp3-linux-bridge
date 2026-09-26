# Winamp3 Linux Bridge - 部署指南

## 快速开始

### 方式一: 使用预编译 AppImage

1. 下载 `Winamp3-i686.AppImage`
2. 赋予执行权限:
   ```bash
   chmod +x Winamp3-i686.AppImage
   ```
3. 运行:
   ```bash
   ./Winamp3-i686.AppImage
   ```

### 方式二: 从源码构建

#### 1. 安装依赖

```bash
# Debian/Ubuntu
sudo apt install build-essential gcc-multilib libasound2-dev

# CentOS/RHEL/Fedora
sudo dnf install gcc gcc-c++ make alsa-lib-devel
# 注意: Fedora 37+ 需要 libfuse2
```

#### 2. 构建桥接组件

```bash
cd oss-bridge
make clean
make
```

产出:
- `oss_shim.so` - 32位 shim (13KB)
- `oss_helper` - 64位 helper (23KB)

#### 3. 构建 AppImage

```bash
cd ../appimage
bash build.sh
```

产出: `Winamp3-i686.AppImage` (~15MB)

---

## 手动运行 (开发调试)

```bash
# 1. 启动 helper
./oss-bridge/oss_helper &

# 2. 设置环境变量
export LD_LIBRARY_PATH="$PWD/usr/lib:$PWD/libs:$PWD/extra_libs:$LD_LIBRARY_PATH"
export LD_PRELOAD="$PWD/oss-bridge/oss_shim.so"
export OSS_BRIDGE_LOG="$HOME/oss_bridge.log"

# 3. 启动 Winamp
./usr/lib/ld-linux.so.2 --library-path "$LD_LIBRARY_PATH" ./Winamp.exe
```

---

## 故障排除

### 问题: 无声

**检查项**:
1. 确认 helper 正在运行:
   ```bash
   ps aux | grep oss_helper
   ```

2. 检查 socket 是否存在:
   ```bash
   ls -l /tmp/.oss_bridge.sock
   ```

3. 查看日志:
   ```bash
   tail -20 ~/oss_bridge.log
   ```

**预期输出**:
- 应该有 `OPEN /dev/dsp` 和一系列 `W#` 写入记录
- `underruns=0`

### 问题: 爆音/卡顿

**可能原因**:
1. PipeWire/ALSA 配置问题
2. 系统负载过高

**解决**:
```bash
# 重启 PipeWire
systemctl --user restart wireplumber pipewire pipewire-pulse

# 或使用纯 ALSA
export PW_OUTPUT_NODE=""
```

### 问题: GUI 卡死

**可能原因**:
1. 环形缓冲填满导致背压超时
2. 多实例冲突

**解决**:
```bash
# 检查是否有多实例
ps aux | grep Winamp3

# 清理锁文件
rm -f ~/.local/share/Winamp3-instance.pid

# 重新启动
./Winamp3-i686.AppImage
```

---

## 安装桌面集成

### 复制桌面条目

```bash
# 复制到用户应用目录
mkdir -p ~/.local/share/applications
cp appimage/Winamp3.AppDir/winamp3.desktop ~/.local/share/applications/

# 更新桌面数据库
update-desktop-database ~/.local/share/applications
```

### 安装图标

```bash
# 复制图标到系统目录
mkdir -p ~/.local/share/icons/hicolor/48x48/apps
cp appimage/Winamp3.AppDir/winamp3.png ~/.local/share/icons/hicolor/48x48/apps/

# 更新图标缓存
update-icon-caches ~/.local/share/icons/hicolor/
```

---

## 版本历史

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| v1.0 | 2026-09-08 | 初始版本，含 shim+helper 桥接 |
| v1.1 | 2026-09-07 | 修复 PipeWire 断管崩溃 (三层防护) |
| v1.2 | 2026-09-07 | shim open 修复，不依赖真实 /dev/dsp |

---

## 贡献指南

欢迎提交 Issue 和 Pull Request！

### 开发环境

```bash
# 克隆仓库
git clone https://github.com/your-username/winamp3-linux-bridge.git
cd winamp3-linux-bridge

# 构建
cd oss-bridge && make && cd ..
```

### 测试流程

1. 修改代码
2. 运行测试:
   ```bash
   cd oss-bridge && ./test_helper_proto
   ```
3. 构建 AppImage:
   ```bash
   cd ../appimage && bash build.sh
   ```
4. 实播验证

---

## 联系方式

- GitHub Issues: https://github.com/your-username/winamp3-linux-bridge/issues
- 项目主页: (待添加)
