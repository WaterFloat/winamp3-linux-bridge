# Stage 1: 碎片盘点与分类

## 收集时间
2026-09-07 19:15

## 碎片来源

### 来源 1: wasabi.dll (Wasabi 框架核心)
- **文件路径**: `Download/winamp3/wasabi.dll`
- **文件大小**: 未记录
- **符号类型**: 动态库导出符号

### 来源 2: common.dll (公共组件库)
- **文件路径**: `Download/winamp3/common.dll`
- **文件大小**: 未记录
- **符号类型**: 动态库导出符号

### 来源 3: winamp3.exe (主程序)
- **文件路径**: `Download/winamp3/winamp3.exe`
- **文件大小**: 61KB
- **符号类型**: 可执行文件字符串

## 碎片清单

### 类别 1: Wasabi 框架 (wasabi.dll)

#### 关键标识符
```
WinampAbstractionLayer/*
WinampAbstractionLayer
Winamp3.SkinZip
Winamp3.PlayList
Winamp3.File
Winamp_Back
```

#### 脚本对象系统
```
SOM::isNumeric
VCPU::VSP
RootObjectInstance
ScriptObject
ScriptObjectController
ScriptHook
```

#### 类名相关
```
?vcpu_getClassName@ScriptObject@@QAEPBDXZ
?addClassHook@ScriptObjectControllerI@@UAEXPAVScriptHook@@@Z
?getAncestorClassId@ScriptObjectControllerI@@UAEHXZ
?setAncestorClassId@ScriptObjectControllerI@@UAEXH@Z
?getClassId@ScriptObjectControllerI@@UAEHXZ
?setClassId@ScriptObjectControllerI@@UAEXH@Z
?onRegisterClass@ScriptObjectControllerI@@UAEXPAVScriptObjectController@@@Z
?getClassName@RootObjectInstance@@UAEPBDXZ
?getAncestorClassName@ScriptObjectController@@QAEPBDXZ
?getClassName@ScriptObjectController@@QAEPBDXZ
?getAncestorClassId@ScriptObjectController@@QAEHXZ
?getClassId@ScriptObjectController@@QAEHXZ
Class : %s
duplicate script class guid
```

#### 音频相关
```
audio/mpeg
audio/mpg
audio/mp3
audio/x-mpg
audio/x-mp3
audio/x-mpegurl
audio/x-mpegurl
audio/x-ms-wma
```

#### 系统函数
```
OutputDebugStringA
wasabi_getKernel
ComponentBucket
Component
componentbucket
COMPONENTBUCKET
ComponentAPI(%d): %s: %s
winamp.wac
WinampXML/configuration/*
WinampXML/configuration
WinampXML
```

### 类别 2: Common 库 (common.dll)

#### 窗口相关
```
WindowFromPoint
BaseWindow_RootWnd
canvas != NULL
basewnd != NULL
wnddc != NULL
canvas->getHDC() != NULL
skin_iterator != NULL
wnd == NULL
wnd != NULL
```

#### 通用术语
```
Window
BaseWindow
Canvas
Skin
```

### 类别 3: 主程序 (winamp3.exe)

#### 关键功能
```
wasabi_getKernel
wasabi.dll
```

## 聚类结果

### 模块 1: Wasabi 框架
- **职责**: 插件系统、脚本引擎、组件管理
- **置信度**: 高
- **缺口**: 缺少具体类定义、成员函数、接口签名

### 模块 2: Common 库
- **职责**: 窗口管理、画布渲染、皮肤系统
- **置信度**: 高
- **缺口**: 缺少具体类定义、成员函数、接口签名

### 模块 3: 主程序
- **职责**: 启动 Wasabi 框架、加载组件
- **置信度**: 中
- **缺口**: 缺少主函数、组件加载逻辑

## 置信度标注

| 模块 | 置信度 | 说明 |
|------|--------|------|
| Wasabi 框架 | 高 | 有明确的命名空间和类名线索 |
| Common 库 | 高 | 有窗口、画布、皮肤相关标识 |
| 主程序 | 中 | 只有框架初始化线索 |

## 缺口清单

1. ❌ 缺少类定义（头文件）
2. ❌ 缺少成员函数签名
3. ❌ 缺少接口定义（COM/COM+）
4. ❌ 缺少依赖库列表
5. ❌ 缺少构建系统信息
