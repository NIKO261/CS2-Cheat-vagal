# core/resources/ 目录规则

- 这里全是生成的数据头（字库 / 图片 / 粒子字节数组，合计约 6 MB；`fonts/inter.hpp` 单文件 4.2 MB / 57,250 行）——不要 read、不要 grep。
- 改视觉不要动这里：字库加载在 `core/rendering/impl/fonts.cpp`，材质与图标在 `core/systems/impl/materials.cpp`、`icons.cpp`。
- 只有替换资源本身（新增 PNG/TTF 并重新生成头文件）时才需要碰本目录。
