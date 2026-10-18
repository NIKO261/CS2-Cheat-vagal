# external/ 目录规则

- 除 `xdraw/xdraw.cpp`、`xdraw/xdraw.hpp`、`xdraw/xui/xui.cpp`、`xdraw/xui/xui.hpp`、`config.hpp`、`poly2d.hpp` 之外全是第三方代码（phnt / luajit / zydis / lz4 / nlohmann / stackwalker / inline-syscall / `xdraw/dependencies` / bc7.hpp / xorstr.hpp）——不要 read、不要 grep。
- `xdraw/dependencies/fonts.hpp` 是 8.3 MB 纯数据头。改 UI 请改 `xui/xui.cpp`；中文本地化表在 `xui.cpp:72 translate_ui_text`。
- 只在需要确认某个 NTAPI 签名时，定点读 `phnt/` 下的单个头文件。
