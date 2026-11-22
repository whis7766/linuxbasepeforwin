# 使用说明

## 启动

ISO 同时支持 BIOS 与 UEFI。引导后内核把 initramfs 解包到内存，整个系统驻留 RAM，可拔掉 U 盘运行。
默认控制台同时输出到本地屏幕与串口：`console=ttyS0,115200 console=tty0`。

## WIM 备份 / 还原

- 源目录：已挂载的 Windows 分区挂载点，例如 `/mnt/win`
- WIM 文件：输出或输入路径，例如 `/tmp/backup.wim`
- 排除列表：每行一个通配模式，可留空
- 捕获：调用 libwim `wimlib_add_image` + `wimlib_write`，LZX 压缩，进程内进度回调
- 应用：`wimlib_extract_image` 按索引还原到目标目录

## 文件管理器（NTFS）

1. 设备填 `/dev/sdb`（整盘）或 `/dev/sdb1`（分区），挂载点填 `/mnt/win`
2. 点“挂载 NTFS”，走 `mount(2)` + `ntfs3`
3. 选中文件后可“设置属性”或“设置 ACL”

属性值是 NTFS 属性位，例如 `0x20` 为存档、`0x01` 为只读、`0x02` 为隐藏。

ACL 使用 `system.ntfs_security` xattr，内容是 SECURITY_DESCRIPTOR_RELATIVE 的十六进制。
无独立安全描述符的文件读取会提示使用默认 SD；写入时必须是合法十六进制且长度 ≥ 20 字节。

## 磁盘管理

- “刷新磁盘列表”读取 `/sys/block`，同时列出分区
- “创建 GPT 分区”：直接写保护性 MBR、主/备 GPT 头、分区条目与 CRC32，再 `BLKRRPART`
  - 起始/结束扇区均按 512 字节扇区计，结束扇区包含在分区内
  - 空白盘会自动新建 GPT；已有 MBR 分区表的盘会拒绝转换，避免误毁数据
- 擦除：
  - NVMe block-erase / crypto-erase：`NVME_IOCTL_ADMIN_CMD` sanitize
  - HDD 填零：`BLKZEROOUT`
  - SSD TRIM：`BLKDISCARD`

## BCD 引导

BCD 是注册表 hive，路径通常为 `/mnt/win/Boot/BCD`。

- “导出/读取”：递归打印 hive 树
- “列出对象 / 元素”：按 `Objects\{GUID}\Elements\<元素ID>` 列出元素、类型和值
- 编辑：
  1. 填对象 GUID，例如 `{9dea862c-5cdd-4e70-acc1-f32b344d4795}`（Boot Manager）
  2. 填元素 ID，例如 `12000002`
  3. 选类型：REG_SZ / REG_BINARY / REG_DWORD / REG_MULTI_SZ
  4. 填值：字符串直接写；二进制写十六进制；DWORD 写十进制或 `0x...`；MULTI_SZ 每行一个
  5. “读取元素” / “写入元素” / “删除元素”

常用元素 ID：

| 元素 | 含义 |
|---|---|
| 12000002 | 启动程序路径（如 `\Windows\system32\winload.efi`） |
| 12000004 | 显示描述 |
| 21000001 | OS device（二进制设备元素） |
| 22000002 | systemroot（如 `\Windows`） |
| 25000004 | 超时秒数（DWORD） |
| 24000001 | displayorder（二进制 GUID 列表） |

“创建 BCD”用于在已有 hive 上补齐 Boot Manager / OS Loader 对象骨架，不是从零生成完整 BCD；
完整 bcdboot 等效还需要复制启动文件与设备元素。

## UEFI 启动项 / 驱动项

仅 UEFI 引导时 `/sys/firmware/efi/efivars` 存在。可直接读写 `Boot####`、`BootOrder`、
`Driver####`、`DriverOrder`，删除即 unlink 对应变量。设备路径按 EFI_LOAD_OPTION 的十六进制填写。

## 电源

- 重启：`reboot(2)` `LINUX_REBOOT_CMD_RESTART`
- 关机：`reboot(2)` `LINUX_REBOOT_CMD_POWER_OFF`
- 冷重启：写 `/proc/sysrq-trigger` 的 `b`

## 高分屏

GUI 启动时按以下顺序决定缩放：

1. 若环境已有 `QT_SCALE_FACTOR`，直接使用
2. 读 `/sys/class/drm/card*/edid` 的物理尺寸，按 `PPI / 96` 计算缩放（支持 2K/4K/高 PPI 笔电）
3. 无 EDID 时按 framebuffer 宽度：≥3840 → 2.0，≥2560 → 1.5，否则 1.0
4. 缩放限制在 1.0–3.0，基准字号 12pt，保证高分屏文字不缩小

## 鼠标

linuxfb 平台本身不带指针支持。程序启动时扫描 `/sys/class/input/event*` 的 `rel` 能力位
（REL_X|REL_Y），找到鼠标后设置 `QT_QPA_GENERIC_PLUGINS=evdevmouse` 与其设备节点。
内核已内建 `INPUT_EVDEV`、PS/2 与 USB HID，真机 USB 鼠标可直接使用。
