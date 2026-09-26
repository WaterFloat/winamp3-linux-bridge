# Winamp3 Linux — AppImage 打包工程

把 **Winamp3 Linux alpha 1（2002-12-12，原生 32-bit x86 ELF）** 连同它需要的老库
（GTK 1.2 / glib 1.2 / libpng 1.0 / 旧 libstdc++ / X11）一起打成一个**单文件
`.AppImage`**，在现代 x86_64 Linux 上双击/命令行即可跑。

> 这是 **方案 B**（最小依赖闭包 + AppImage）。它和之前 `docker/` 方案互补：
> docker 适合容器化分享，AppImage 适合单文件分发。

---

## 重要约束（任何打包都突破不了）

AppImage 只是把老库**绑在一起**，改不了二进制本身的 ABI：

1. **宿主必须能跑 32 位** —— x86_64 现代 Linux 内核原生支持（开 multilib 即可）；
   **ARM（含 Apple Silicon）直接不行**。
2. **宿主得有显示服务** —— 它是 X11 客户端，需要 X11 / XWayland。
3. **音频仍需桥接** —— 它只认 `/dev/dsp`（OSS），现代内核没有，靠 `padsp` / `aoss`
   桥到 PulseAudio / ALSA。

---

## 构建（必须在 32 位能跑的 Linux 上）

`build.sh` 不能在 macOS 上跑 —— `appimagetool` 是 Linux 工具，且依赖闭包要靠 `ldd`
解析 32 位库 + 拷 32 位 `ld-linux`/`glibc`。

```bash
# 在一个 32 位可运行的 Linux 里（StartOS 5.1 chroot / RH9 / 开了 multilib 的 x86_64）
# 1) 装 appimagetool
wget -O /usr/local/bin/appimagetool \
  https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x /usr/local/bin/appimagetool

# 2) 构建（默认从 ../docker/app 取 Winamp 树；也可传别的位置）
cd appimage
bash build.sh            # 或 bash build.sh /path/to/Winamp-tree

# 3) 产物
./Winamp3-Linux.AppImage
```

`build.sh` 会做的事：把 Winamp 树 + `extra_libs` 拷进 `Winamp3.AppDir`，用 `ldd`
做**依赖闭包**只打包真正需要的 32 位 `.so`，拷入 32 位 `ld-linux.so.2`，然后调用
`appimagetool` 出包。成品通常只有几十 MB（去掉了 StartOS 里的 LibreOffice 等一堆东西）。

---

## 运行

```bash
# 需要 X 服务（X11 或 XWayland）；声音自动走 padsp/aoss 桥
./Winamp3-Linux.AppImage

# 想用原始 OSS（宿主已装 oss4 并有 /dev/dsp）时：
WINAMP_OSS=1 ./Winamp3-Linux.AppImage
```

- **没声音？** 默认用 `padsp`（PulseAudio）桥；若宿主是 ALSA 则回退 `aoss`；
  两者都没有就静默运行（GUI 正常）。先确认 `padsp`/`aoss` 已装
  （`pulseaudio-utils` / `alsa-oss`）。
- **启动即崩 / X_PutImage BadMatch？** 老 GTK 1.2 对色深敏感，在 Xvfb 下把深度
  调成 16 或 32 再试；X11 转发（XWayland）一般没事。
- **先验证解码管线**：偏好里把输出插件切成 **File Writer (of_file.so)**，放歌
  「播放」会渲染出文件 —— 能出文件就说明 `in_mp3` 解码 + 整条管线正常，只剩设备层。

---

## 目录

```
appimage/
├── build.sh            # 在 Linux 上执行：依赖闭包 + 出 AppImage
├── gen_icon.py         # 生成 256x256 图标（build.sh 会自动调用）
├── README.md
└── Winamp3.AppDir/     # AppDir 骨架
    ├── AppRun          # 启动器（LD_LIBRARY_PATH + padsp/aoss 桥 + $DISPLAY 透传）
    ├── winamp3.desktop
    └── winamp3.png     # 由 gen_icon.py 生成
```

构建时 `build.sh` 会把 `docker/app` 的 `Winamp.exe` + `libs/` + `wacs/` + `Plugins/`
+ `extra_libs/` 等拷进 `Winamp3.AppDir`，再补依赖闭包。
