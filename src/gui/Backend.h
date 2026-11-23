#pragma once
// Native kernel / low-level library backend. No external CLI processes.
#include <QString>
#include <QStringList>
#include <functional>

using LogFn = std::function<void(const QString &)>;

namespace bk {

// WIM via libwim C API
QString wimCapture(const QString &src, const QString &wim, const QString &excludeFile, LogFn log);
QString wimApply(const QString &wim, int index, const QString &dest, LogFn log);

// Disk via ioctl (BLKPG / BLKDISCARD / BLKZEROOUT / NVMe passthrough)
QString diskList();
QStringList blockDevices();
QString diskPartition(const QString &dev, int pno, quint64 startSector, quint64 endSector);
QString nvmeSanitize(const QString &dev, int action);
QString nvmeSanitizeLog(const QString &dev);
QString diskZero(const QString &dev);
QString diskTrim(const QString &dev);

// BCD via libhivex C API
QString bcdExport(const QString &file);
QString bcdCreate(const QString &file, const QString &label, const QString &winroot);
QString bcdList(const QString &file);
QStringList bcdObjectGuids(const QString &file);
QStringList bcdElementIds(const QString &file, const QString &guid);
QString bcdGetElement(const QString &file, const QString &guid, const QString &elem);
QString bcdSetElement(const QString &file, const QString &guid, const QString &elem, int type, const QString &value);
QString bcdDeleteElement(const QString &file, const QString &guid, const QString &elem);

// UEFI via efivarfs direct read/write
QString uefiList();
QString uefiDevicePath(const QString &espDev, const QString &fileRel);
QString uefiAdd(const QString &desc, const QString &devicePathHex);
QString uefiDel(const QString &num);
QString uefiSetOrder(const QString &csv);
QString uefiAddDriver(const QString &desc, const QString &devicePathHex);
QString uefiDelDriver(const QString &num);
QString uefiSetDriverOrder(const QString &csv);
QString uefiDrvList();
QString uefiDrvOrder();
QString windowsBootRepair(const QString &espMount, const QString &winMount);

// NTFS via mount(2) + xattr syscalls
QString fsMount(const QString &dev, const QString &dir);
QString fsUnmount(const QString &dir);
QString fsAttrGet(const QString &path);
QString fsAttrSet(const QString &path, quint32 value);
QString fsAclGet(const QString &path);
QString fsAclSet(const QString &path, const QString &hex);

// Power via reboot(2) / sysrq
QString powerReboot();
QString powerCold();
QString powerOff();

} // namespace bk