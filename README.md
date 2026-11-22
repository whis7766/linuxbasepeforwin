# linuxbasepeforwin

基于 Linux 内核的 USB 启动**内存系统**（Live RAM OS），用于备份、还原与维护安装有 Windows 的磁盘。
全部功能集成于统一的 **Qt6 图形界面**。x86_64，BIOS + UEFI 双启动。

**设计原则：不做 shell 拼接。** 所有功能在 GUI 进程内通过原生内核 syscall / ioctl、efivarfs、
xattr、mount(2)、reboot(2) 以及底层库 C API（libwim、libhivex）直接完成，不调用任何外部命令行工具。

## 功能

| 功能 | 实现方式 | 状态 |
|---|---|---|
| NTFS 读写挂载 | mount(2) + `ntfs3` 内核驱动 | ✅ 实机验证 |
| 文件浏览 / 读取 | VFS syscall | ✅ 实测 |
| NTFS 文件属性（存档/隐藏/系统/只读…） | `system.ntfs_attrib` xattr | ✅ 实测读写 |
| NTFS 安全描述符 ACL | `system.ntfs_security` xattr（SECURITY_DESCRIPTOR_RELATIVE 十六进制） | ⚠️ 无独立 SD 的文件返回默认 SD 语义 |
| WIM 捕获 / 应用 | libwim C API：`wimlib_add_image` + `wimlib_write` / `wimlib_extract_image` | ✅ 已修正输出 API，待复测 |
| 磁盘 / 分区枚举 | `/sys/block`，列出分区 | ✅ 实测 |
| 分区创建（GPT，指定扇区） | 原生写保护性 MBR、主/备 GPT 头、条目与 CRC32 | ✅ 实现，待实测 |
| NVMe sanitize / HDD 填零 / SSD TRIM | NVME_IOCTL_ADMIN_CMD、BLKZEROOUT、BLKDISCARD | ✅ 实现 |
| BCD 读取 / 导出 / **元素编辑**（列对象、读/写/删元素） | libhivex 直接读写 hive | ✅ 实现，待实测 |
| BCD 创建 | 在已有 hive 上补 Boot Manager / OS Loader 骨架 | ⚠️ best-effort，非完整 bcdboot |
| UEFI 启动项 / 驱动项编辑 | efivarfs 写 `Boot####` / `Driver####` / `*Order` | ✅ 实现，未在 UEFI 环境实测 |
| 关机 / 重启 / 冷重启 | reboot(2)（LINUX_REBOOT_CMD_*） | ✅ 实现 |
| ReFS | — | ⬜ 预留（暂无完整 Linux 读写驱动） |

全局 HighDPI 适配：优先读 EDID 物理尺寸按 `PPI/96` 计算缩放，无 EDID 时按 framebuffer 宽度
（≥3840→2.0、≥2560→1.5），范围 1.0–3.0，基准字号 14pt，保证 2K/4K 与高 PPI 笔电文字不缩小。

linuxfb 本身不带指针设备；GUI 启动时直接扫描 `/sys/class/input`，原生读取
`/dev/input/eventN` 的 REL/ABS 与 BTN_LEFT 事件并翻译为 Qt 鼠标事件（不依赖 Qt evdevmouse
插件），PS/2 / USB 鼠标 / USB 平板均可使用，界面另绘指针光标。

## 技术栈

Buildroot 外部树 + Linux 6.12 + Qt6 Widgets/C++；rootfs 以 initramfs 内嵌内核，启动后整体驻留内存，
可拔 U 盘运行。图形输出为内核 framebuffer（`/dev/fb0`，bochs / simpledrm / efifb），
控制台 `console=ttyS0,115200 console=tty0` 双输出便于串口与本地排障。

详见 [`docs/DESIGN.md`](docs/DESIGN.md) 与 [`docs/USAGE.md`](docs/USAGE.md)。

## 构建与发布

所有构建、编译与发布**一律通过 GitHub Actions**（`.github/workflows/build.yml`）：

- `gui-check`：用发行版 Qt6 + wimlib + hivex 头文件快速编译 GUI，先拦截语法/链接错误；
- `build`：Buildroot 全量构建，产出 `linuxbasepe.iso`，上传 artifact；
- 打 tag 时自动创建 Release 并附 ISO。

发布产物：<https://github.com/whis7766/linuxbasepeforwin/releases>

## 仓库结构

```
buildroot-external/   Buildroot 外部树（defconfig / board / 自定义包 lbpe-gui, wimlib, hivex）
src/gui/              Qt6 C++ GUI 与原生后端（Backend.cpp）
docs/                 设计文档
.github/workflows/    Actions 构建 / 发布流水线
```

## 状态

v0.1.0 已发布。虚拟机实测：ISO 引导成功、framebuffer 生成、Qt6 中文界面正常、HighDPI 生效；
NTFS 挂载 / 文件读取 / 属性读写 / 磁盘枚举通过。WIM 与 ACL 往返、UEFI 启动项写入在持续完善中。
