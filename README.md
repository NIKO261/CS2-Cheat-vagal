# vagal

本项目为vibe coding产物

Windows x64 CS2 内部 DLL。界面层使用项目自带的 `xdraw` / `xui` 渲染器（非 ImGui）。

> 解决方案与工程文件仍沿用旧名 `firgune.slnx` / `firgune.vcxproj`，编译产物为 `vagal.dll` / `vagal-debug.dll`。

## 构建环境

| 项 | 值 |
| --- | --- |
| 目标平台 | Windows x64 |
| 配置类型 | Dynamic Library (DLL) |
| 工具集 | MSVC `v145`（本次使用 Visual Studio 2026） |
| Windows SDK | `10.0.22621.0` |
| C++ 标准 | `stdcpplatest` / C 标准 `stdclatest` |
| 字符集 | Unicode |
| 运行库 | `MultiThreaded`（静态 CRT） |
| 预编译头 | `pch` |

## 构建产物

| 配置 | 输出 |
| --- | --- |
| `Development\|x64` | `bin/vagal-debug.dll` |
| `Release\|x64` | `bin/vagal.dll` |

## 下载 Release DLL

到 [Releases](https://github.com/NIKO261/CS2-Cheat-vagal/releases/latest) 下载 `vagal.dll`。同一版本的 `SHA256SUMS.txt` 用来核对文件是否完整；源码位于对应版本的标签中。

## 自己编译

在已安装 `v145` 和上表 SDK 的 Visual Studio 开发者命令提示符中运行：

```bat
msbuild firgune/firgune.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
msbuild firgune/firgune.vcxproj /t:Rebuild /p:Configuration=Development /p:Platform=x64
```

## 2026-10-18 问题修复

- 修复窗口在首次绘制前缩放时的空设备访问，以及缩放失败后界面消失的问题。
- 修复输入框替换选中文字时丢字、达到字数上限后替换失效的问题。
- 修复 Release 手动卸载时遗漏异常处理器和游戏事件回调清理的问题。
- COM 初始化和清理改为同一线程配对，初始化失败时不做错误清理。
- 连跳清理跳跃键后，Jumpbug 仍可读取本条命令原有的跳跃意图；显式松跳仍会取消。

本次 `Release|x64` 和 `Development|x64` 均已重新编译通过；专项检查 48/48，通过后又在独立副本回滚复验。18 个既有检查中 12 个通过，6 个保留原有历史失败，没有新增失败。

检查脚本：`python _ai/test_project_bugscan.py`。其中渲染与输入回归会编译并运行源码片段测试，设备和输入使用测试替身，不启动游戏。

### 已知问题

- 编译和回归检查不代表游戏内验证；窗口切换、卸载和实际落地效果仍需实机确认。
- 手动 `FreeLibrary` 的旧账号线程清理路径仍可能卡住；本次没有重写整个卸载流程。
- 默认关闭的 Rage 诊断仍可能漏记延迟确认的射击，不能用该计数判断真实命中率。

## 目录结构

```
firgune/                     # 源码目录
├── firgune.vcxproj          # MSVC 工程文件
└── project/
    ├── entry.cpp            # DLL 入口（DllMain / 初始化）
    ├── pch/                 # 预编译头
    ├── core/
    │   ├── features/        # 功能模块
    │   ├── hooks/           # 钩子与 create_move 管线
    │   ├── rendering/       # 渲染层封装
    │   ├── systems/         # 运行时系统
    │   ├── resources/       # 内嵌资源（图片等）
    │   └── settings.hpp     # 配置结构体
    ├── protection/          # 模块保护
    ├── utilities/           # 通用工具
    └── external/            # 第三方依赖（见下）
```

## 第三方依赖

| 依赖 | 用途 |
| --- | --- |
| `phnt` | 原生 API 头文件（NT 内部接口） |
| `xdraw` | 2D 渲染器 + `xui` 控件层 |
| `zydis` | x86/x64 指令反汇编 |
| `luajit` | Lua 脚本运行时 |
| `lz4` | 压缩 |
| `nlohmann/json` | JSON 序列化 |
| `xorstr` | 编译期字符串加密 |
| `stackwalker` | 调用栈回溯 |
| `inline-syscall` | 直接/间接系统调用 |
| `poly2d` / `bc7` | 几何与纹理压缩工具 |

## 配置

菜单语言由 `settings::g_misc.language` 持久化，默认 `english`，可选简体中文。

## 免责声明

本项目仅用于逆向工程与 Windows 底层技术学习。使用者需自行承担因使用本代码产生的一切后果。
