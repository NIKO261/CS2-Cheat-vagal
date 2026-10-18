# vagal

本项目为vibe coding产物

Windows x64 CS2 内部 DLL。界面层使用项目自带的 `xdraw` / `xui` 渲染器（非 ImGui）。

> 解决方案与工程文件仍沿用旧名 `firgune.slnx` / `firgune.vcxproj`，编译产物为 `vagal.dll` / `vagal-debug.dll`。

## 构建环境

| 项           | 值                                  |
| ----------- | ---------------------------------- |
| 目标平台        | Windows x64                        |
| 配置类型        | Dynamic Library (DLL)              |
| 工具集         | `v145` (Visual Studio 2022+)       |
| Windows SDK | `10.0.22621.0`                     |
| C++ 标准      | `stdcpplatest` / C 标准 `stdclatest` |
| 字符集         | Unicode                            |
| 运行库         | `MultiThreaded`（静态 CRT）            |
| 预编译头        | `pch`                              |

## 构建产物

| 配置                 | 输出                    |
| ------------------ | --------------------- |
| `Development\|x64` | `bin/vagal-debug.dll` |
| `Release\|x64`     | `bin/vagal.dll`       |

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

| 依赖               | 用途                  |
| ---------------- | ------------------- |
| `phnt`           | 原生 API 头文件（NT 内部接口） |
| `xdraw`          | 2D 渲染器 + `xui` 控件层  |
| `zydis`          | x86/x64 指令反汇编       |
| `luajit`         | Lua 脚本运行时           |
| `lz4`            | 压缩                  |
| `nlohmann/json`  | JSON 序列化            |
| `xorstr`         | 编译期字符串加密            |
| `stackwalker`    | 调用栈回溯               |
| `inline-syscall` | 直接/间接系统调用           |
| `poly2d` / `bc7` | 几何与纹理压缩工具           |

## 配置

菜单语言由 `settings::g_misc.language` 持久化，默认 `english`，可选简体中文。

## 免责声明

本项目仅用于逆向工程与 Windows 底层技术学习。使用者需自行承担因使用本代码产生的一切后果。
