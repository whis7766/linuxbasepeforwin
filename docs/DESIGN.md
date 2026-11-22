# linuxbasepeforwin 设计文档

> 状态：方向已与项目所有者确认（2026-11-21）。本文件为权威设计基线，后续迭代以此为准。

## 1. 定位

基于 Linux 内核、USB 启动、**整体加载进内存运行**的 Live 维护系统（RAM OS），用于对安装有
Windows 的磁盘做备份 / 还原 / 维护。所有功能集成于**单一 Qt6 图形界面**。

- 架构：x86_64
- 启动：BIOS（legacy）+ UEFI 双启动；UEFI 为必需（否则无法编辑 UEFI 变量）
- 运行方式：rootfs 以 initramfs 形式内嵌进内核，启动后整体解包到 tmpfs，全程不依赖磁盘
- 构建/编译/发布：**一律 GitHub Actions**；本地维护机仅用于测试环境与网络资源

## 2. 已确认的关键决策

| 决策点 | 结论 |
| --- | --- |
| ReFS 支持 | **暂缓/留空**。NTFS 必须完整支持（硬性要求）；ReFS 以后想办法 |
| 系统基底 | **Buildroot**（外部树定制，最小化镜像） |
| GUI 栈 | **Qt6 Widgets / C++** |
| HighDPI | **全局高分屏适配**：按 framebuffer 宽度设 QT_SCALE_FACTOR（≥2560→1.5、≥3840→2），
  Qt6 高分缩放 + PassThrough 舍入 + 点数字体，保证高分显示器文字不缩小 |
| 测试环境/镜像中转 | 由维护者在受控本机管理，细节不入仓库（见 §6） |

### ReFS 暂缓的依据
- 内核主线（6.x/7.x）无 ReFS 驱动；开源 libfsrefs / refs-forensic 仅只读取证级。
- 唯一商业读写驱动 Paragon ReFS for Linux（闭源 urefs）仅支持内核 2.6.36–4.10.x、仅 x86-64，
  与现代硬件/工具链脱节，不适合维护 PE。
- 故 v1 不做 ReFS 读写；NTFS 覆盖绝大多数 Windows 磁盘场景。

## 3. 功能 → 实现映射（**全部原生内核 syscall/ioctl + 底层库 API，禁止 shell/外部 CLI**）

1. **WIM 捕获/应用**：libwim C API（`wimlib_add_image`/`wimlib_extract_image`/`wimlib_overwrite`），
   排除经 config 的 `[ExclusionList]`；进度经 `wimlib_register_progress_function` 回调。
2. **BCD 创建/编辑**：libhivex C API（`hivex_open/node_add_child/node_set_value/commit`）。
3. **文件管理器（ACL/属性）**：`mount(2)` 挂载 NTFS（ntfs3）+ `getxattr/setxattr`
   原生读写 `system.ntfs_attrib`（u32 属性位）与 `system.ntfs_security`
   （SECURITY_DESCRIPTOR_RELATIVE，十六进制编辑）。ntfs3 不提供 `system.ntfs_acl`。
4. **磁盘管理**：
   - 分区：直接构造并写入保护性 MBR / GPT 头与分区表项（扇区精确），再 `BLKRRPART`
   - NVMe 擦除：`ioctl(NVME_IOCTL_ADMIN_CMD)` sanitize(0x84)，轮询 sanitize-log(0x81)
   - HDD 填零：`pwrite` 顺序写零
   - SSD TRIM：`ioctl(BLKDISCARD)`
5. **冷重启/关机**：`reboot(2)`（`LINUX_REBOOT_CMD_RESTART` / `POWER_OFF`）；冷重启写
   `/proc/sysrq-trigger` 的 `b`。
6. **UEFI 启动项/驱动项**：efivarfs 直接 read/write（Boot####/BootOrder/Driver####/DriverOrder），
   解析 EFI_LOAD_OPTION，删除经 unlink。

GUI 为 Qt6 页内进程内调用上述后端（`src/gui/Backend.cpp`），长任务 QtConcurrent 线程，
不启动任何外部进程。

## 4. 技术栈与构建

- Buildroot 外部树 `buildroot-external/`，defconfig `linuxbasepe_x86_64_defconfig`
- 内核 6.12 LTS，x86_64 默认 defconfig；glibc 工具链 + C++
- Qt6：qt6base（gui + widgets + linuxfb 平台插件），GUI 直接跑 framebuffer，无需 X/Wayland
- rootfs：initramfs 内嵌内核（真·内存系统）
- 引导：grub2（i386-pc + x86_64-efi），genimage 生成 BIOS+UEFI 混合 ISO
- 自研 GUI 作为 Buildroot 本地包 `lbpe-gui`（CMake，源码 `src/gui/`）
- 专用库：libwim（WIM）、libhivex（注册表/BCD），均由 Buildroot 包直接链接进 GUI 进程

## 5. 仓库结构

```
buildroot-external/        Buildroot 外部树（defconfig / board / 自定义包）
src/gui/                   Qt6 C++ GUI 源码（CMake）
docs/                      设计文档
.github/workflows/         Actions 构建/发布流水线
```

## 6. 测试环境

测试机、镜像中转与远端访问的地址、账号、凭据、证书指纹及操作脚本，仅保存在维护者直接操控的
本地机器上；不写入 Git 仓库，不写入 GitHub Actions 的 Variables / Secrets，也不进入构建产物。

## 7. 里程碑

- M1：Buildroot 可启动 ISO（BIOS+UEFI）+ Qt6 GUI 骨架经 Actions 产出 ✅ 进行中
- M2：WIM 捕获/应用 + 磁盘管理（分区/擦除）接入 GUI
- M3：文件管理器（NTFS ACL/属性）+ BCD 创建/编辑
- M4：UEFI 启动项/驱动项编辑 + 冷重启/关机
- M5：实机验证 + 发布