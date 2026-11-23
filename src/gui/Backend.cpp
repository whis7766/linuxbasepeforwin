#include "Backend.h"

#include <wimlib.h>
#include <hivex.h>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <linux/reboot.h>
#include <linux/fs.h>
#include <linux/blkpg.h>
#include <linux/nvme_ioctl.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <cstring>
#include <cstdio>

namespace bk {

static QString err(const char *op) { return QString("%1 failed: %2").arg(op, strerror(errno)); }

// ---------------- WIM (libwim) ----------------
static enum wimlib_progress_status prog(enum wimlib_progress_msg msg,
                                        union wimlib_progress_info *info, void *ctx)
{
    auto *log = static_cast<LogFn *>(ctx);
    if (log && info && msg == WIMLIB_PROGRESS_MSG_WRITE_STREAMS) {
        quint64 total = info->write_streams.total_bytes;
        quint64 done = info->write_streams.completed_bytes;
        if (total) (*log)(QString("progress %1%").arg(done * 100 / total));
    }
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

QString wimCapture(const QString &src, const QString &wim, const QString &excludeFile, LogFn log)
{
    WIMStruct *w = nullptr;
    int r = wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &w);
    if (r) return QString("create_new_wim: %1").arg(wimlib_get_error_string((enum wimlib_error_code)r));
    wimlib_register_progress_function(w, prog, &log);
    wimlib_set_print_errors(true);
    QByteArray s = src.toUtf8(), d = wim.toUtf8(), e = excludeFile.toUtf8();

    // 捕获 wimlib 的 stderr 详情（含具体失败路径与 errno），失败时回传给 GUI
    QString detail;
    FILE *ef = fopen("/tmp/wim.err", "w+");
    int saved = -1;
    if (ef) { fflush(stderr); saved = dup(2); if (saved >= 0) dup2(fileno(ef), 2); }

    r = wimlib_add_image(w, s.constData(), "capture", e.isEmpty() ? nullptr : e.constData(),
                         WIMLIB_ADD_FLAG_NO_ACLS);

    if (ef) {
        fflush(stderr);
        if (saved >= 0) { dup2(saved, 2); close(saved); }
        fseek(ef, 0, SEEK_SET);
        char b[8192]; size_t n = fread(b, 1, sizeof b, ef);
        fclose(ef);
        if (n) detail = QString::fromUtf8(b, int(n)).trimmed();
    }

    if (!r) r = wimlib_write(w, d.constData(), WIMLIB_ALL_IMAGES, 0, 0);
    wimlib_free(w);
    if (r) {
        QString msg = QString("wimlib: %1").arg(wimlib_get_error_string((enum wimlib_error_code)r));
        if (!detail.isEmpty()) msg += "\n" + detail;
        return msg;
    }
    return QString("OK");
}

QString wimApply(const QString &wim, int index, const QString &dest, LogFn log)
{
    WIMStruct *w = nullptr;
    QByteArray p = wim.toUtf8(), t = dest.toUtf8();
    int r = wimlib_open_wim(p.constData(), 0, &w);
    if (r) return QString("open_wim: %1").arg(wimlib_get_error_string((enum wimlib_error_code)r));
    wimlib_register_progress_function(w, prog, &log);
    r = wimlib_extract_image(w, index, t.constData(), WIMLIB_EXTRACT_FLAG_NO_ACLS);
    wimlib_free(w);
    return r ? QString("wimlib: %1").arg(wimlib_get_error_string((enum wimlib_error_code)r)) : QString("OK");
}

// ---------------- Disk (ioctl) ----------------
static int openDev(const QString &dev, int flags) { return open(dev.toUtf8().constData(), flags); }

static bool isPartitionDev(const QString &dev)
{
    QFile f("/sys/class/block/" + QFileInfo(dev).fileName() + "/partition");
    return f.exists();
}

QStringList blockDevices()
{
    QStringList out;
    QFile f("/proc/partitions");
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    QRegularExpression re("^(sd[a-z]+\\d*|nvme\\d+n\\d+(p\\d+)?|vd[a-z]+\\d*|xvd[a-z]+\\d*)$");
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList cols = lines[i].split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (cols.size() < 4) continue;
        const QString name = cols[3];
        if (!re.match(name).hasMatch()) continue;
        out << "/dev/" + name;
    }
    return out;
}

QString diskList()
{
    QString out;
    QDir sys("/sys/block");
    for (const QString &n : sys.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile sz("/sys/block/" + n + "/size");
        quint64 sectors = 0;
        if (sz.open(QIODevice::ReadOnly)) sectors = sz.readAll().trimmed().toULongLong();
        QFile model("/sys/block/" + n + "/device/model");
        QString m = model.open(QIODevice::ReadOnly) ? model.readAll().trimmed() : "";
        QFile rota("/sys/block/" + n + "/queue/rotational");
        QString r = rota.open(QIODevice::ReadOnly) ? rota.readAll().trimmed() : "?";
        out += QString("%1  %2 GiB  rota=%3  %4\n").arg(n).arg(sectors * 512 / 1073741824.0, 0, 'f', 1).arg(r, m);
        QDir parts("/sys/block/" + n);
        for (const QString &p : parts.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!p.startsWith(n)) continue;
            QFile ps("/sys/block/" + n + "/" + p + "/size");
            if (ps.open(QIODevice::ReadOnly))
                out += QString("   %1  %2 MiB\n").arg(p).arg(ps.readAll().trimmed().toULongLong() * 512 / 1048576);
        }
    }
    return out.isEmpty() ? QString("(no block devices)") : out;
}

#pragma pack(push, 1)
struct GptEntry {
    quint8 type[16];
    quint8 guid[16];
    quint64 first;
    quint64 last;
    quint64 attrs;
    quint8 name[72];
};
struct GptHdr {
    char sig[8];
    quint32 rev;
    quint32 size;
    quint32 crc;
    quint32 rsv;
    quint64 cur;
    quint64 backup;
    quint64 firstUse;
    quint64 lastUse;
    quint8 diskGuid[16];
    quint64 entLba;
    quint32 entNum;
    quint32 entSize;
    quint32 entCrc;
};
#pragma pack(pop)

static quint32 crc32g(const void *data, size_t len)
{
    const unsigned char *p = static_cast<const unsigned char *>(data);
    quint32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void guidRandom(quint8 *g)
{
    for (int i = 0; i < 16; i += 4) {
        quint32 v = QRandomGenerator::global()->generate();
        memcpy(g + i, &v, 4);
    }
    g[6] = (g[6] & 0x0F) | 0x40;
    g[8] = (g[8] & 0x3F) | 0x80;
}

static bool pwriteAll(int fd, const void *buf, size_t n, off_t off)
{
    const char *p = static_cast<const char *>(buf);
    while (n) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w <= 0) return false;
        p += w; n -= size_t(w); off += w;
    }
    return true;
}

QString diskPartition(const QString &dev, int pno, quint64 startSector, quint64 endSector)
{
    Q_UNUSED(pno);
    if (isPartitionDev(dev)) return err("目标是分区，请选择整块磁盘");
    int fd = openDev(dev, O_RDWR);
    if (fd < 0) return err("open");

    quint64 bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0 || bytes < 1024 * 1024) {
        close(fd);
        return err("BLKGETSIZE64");
    }
    const quint64 total = bytes / 512;
    if (endSector >= total) { close(fd); return QString("end sector beyond device (%1)").arg(total); }

    GptHdr h{};
    if (pread(fd, &h, sizeof h, 512) != ssize_t(sizeof h)) { close(fd); return err("read GPT header"); }
    const bool hasGpt = memcmp(h.sig, "EFI PART", 8) == 0 && h.size >= 92 && h.size <= 512;

    if (hasGpt) {
        // 追加条目到现有 GPT
        const quint32 entBytes = h.entNum * h.entSize;
        if (h.entSize != sizeof(GptEntry) || entBytes == 0 || entBytes > 1024 * 1024) {
            close(fd); return QStringLiteral("unsupported GPT entry layout");
        }
        QByteArray ents(int(entBytes), 0);
        if (pread(fd, ents.data(), entBytes, off_t(h.entLba) * 512) != ssize_t(entBytes)) {
            close(fd); return err("read GPT entries");
        }
        GptEntry *items = reinterpret_cast<GptEntry *>(ents.data());
        int slot = -1;
        for (quint32 i = 0; i < h.entNum; i++) {
            bool zero = true;
            for (int k = 0; k < 16; k++) if (items[i].type[k]) { zero = false; break; }
            if (zero) { slot = int(i); break; }
        }
        if (slot < 0) { close(fd); return QStringLiteral("no free GPT entry"); }

        static const quint8 dataType[16] = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x43,0x33,0x87,0xC3,0x68,0xB6,0xB7,0x26,0x99,0xC7};
        memcpy(items[slot].type, dataType, 16);
        guidRandom(items[slot].guid);
        items[slot].first = startSector;
        items[slot].last = endSector;
        items[slot].attrs = 0;
        memset(items[slot].name, 0, sizeof items[slot].name);

        h.entCrc = crc32g(ents.constData(), size_t(entBytes));
        h.crc = 0;
        h.crc = crc32g(&h, 92);

        bool ok = pwriteAll(fd, ents.constData(), entBytes, off_t(h.entLba) * 512) &&
                  pwriteAll(fd, &h, 92, off_t(h.cur) * 512);
        if (ok && h.backup) {
            GptHdr hb = h;
            hb.cur = h.backup;
            hb.backup = h.cur;
            hb.entLba = h.backup - 32; // 128*128B = 32 扇区
            hb.crc = 0;
            hb.crc = crc32g(&hb, 92);
            ok = pwriteAll(fd, ents.constData(), entBytes, off_t(hb.entLba) * 512) &&
                 pwriteAll(fd, &hb, 92, off_t(hb.cur) * 512);
        }
        if (!ok) { close(fd); return err("write GPT"); }
        ioctl(fd, BLKRRPART, 0);
        close(fd);
        return QString("GPT partition added: slot %1, sectors %2-%3").arg(slot + 1).arg(startSector).arg(endSector);
    }

    // 无 GPT：仅允许在空白盘上新建，避免误毁 MBR 分区表
    unsigned char mbr[512]{};
    if (pread(fd, mbr, sizeof mbr, 0) != ssize_t(sizeof mbr)) { close(fd); return err("read MBR"); }
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        bool hasEntry = false;
        for (int i = 0; i < 4; i++) if (mbr[446 + i * 16 + 4]) { hasEntry = true; break; }
        if (hasEntry) { close(fd); return QStringLiteral("existing MBR partition table; refusing to convert to GPT"); }
    }
    if (total < 4096) { close(fd); return QStringLiteral("device too small for GPT"); }

    QByteArray zero(512 * 34, 0);
    if (!pwriteAll(fd, zero.constData(), size_t(zero.size()), 0)) { close(fd); return err("zero GPT area"); }

    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    quint32 pstart = 1;
    quint32 plen = (total - 1) > 0xFFFFFFFFull ? 0xFFFFFFFFu : quint32(total - 1);
    memcpy(mbr + 446 + 8, &pstart, 4);
    memcpy(mbr + 446 + 12, &plen, 4);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (!pwriteAll(fd, mbr, 512, 0)) { close(fd); return err("write MBR"); }

    QByteArray ents(128 * 128, 0);
    GptEntry *items = reinterpret_cast<GptEntry *>(ents.data());
    static const quint8 dataType[16] = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x43,0x33,0x87,0xC3,0x68,0xB6,0xB7,0x26,0x99,0xC7};
    memcpy(items[0].type, dataType, 16);
    guidRandom(items[0].guid);
    items[0].first = startSector;
    items[0].last = endSector;

    GptHdr ph{};
    memcpy(ph.sig, "EFI PART", 8);
    ph.rev = 0x00010000; ph.size = 92;
    ph.cur = 1; ph.backup = total - 1;
    ph.firstUse = 34; ph.lastUse = total - 34;
    guidRandom(ph.diskGuid);
    ph.entLba = 2; ph.entNum = 128; ph.entSize = 128;
    ph.entCrc = crc32g(ents.constData(), size_t(ents.size()));
    ph.crc = crc32g(&ph, 92);

    const off_t backupEntOff = off_t(total - 33) * 512;
    const off_t backupHdrOff = off_t(total - 1) * 512;
    bool ok = pwriteAll(fd, ents.constData(), size_t(ents.size()), 1024) &&
              pwriteAll(fd, &ph, 92, 512);
    if (ok) {
        GptHdr bh = ph;
        bh.cur = total - 1; bh.backup = 1; bh.entLba = total - 33;
        bh.crc = 0; bh.crc = crc32g(&bh, 92);
        ok = pwriteAll(fd, ents.constData(), size_t(ents.size()), backupEntOff) &&
             pwriteAll(fd, &bh, 92, backupHdrOff);
    }
    if (!ok) { close(fd); return err("write GPT"); }
    ioctl(fd, BLKRRPART, 0);
    close(fd);
    return QString("new GPT created + partition 1 at sectors %1-%2").arg(startSector).arg(endSector);
}

QString nvmeSanitize(const QString &dev, int action)
{
    if (isPartitionDev(dev)) return err("目标是分区，不能执行 NVMe sanitize");
    int fd = openDev(dev, O_RDWR);
    if (fd < 0) return err("open");
    struct nvme_admin_cmd cmd{};
    cmd.opcode = 0x84; // sanitize
    cmd.nsid = 0xffffffff;
    cmd.cdw10 = action & 0x7;
    int r = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    close(fd);
    return r ? err("NVME_SANITIZE") : QString("sanitize action %1 issued").arg(action);
}

QString nvmeSanitizeLog(const QString &dev)
{
    int fd = openDev(dev, O_RDWR);
    if (fd < 0) return err("open");
    unsigned char buf[512]{};
    struct nvme_admin_cmd cmd{};
    cmd.opcode = 0x02; // get log page
    cmd.nsid = 0xffffffff;
    cmd.addr = (uintptr_t)buf;
    cmd.data_len = sizeof(buf);
    cmd.cdw10 = 0x81 | (127 << 16); // sanitize status log, 512B
    int r = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    close(fd);
    if (r) return err("NVME_GETLOG");
    QString hex;
    for (int i = 0; i < 16; i++) hex += QString("%1 ").arg(buf[i], 2, 16, QChar('0'));
    return QString("sanitize-log: %1").arg(hex);
}

static QString wholeDevOp(const QString &dev, unsigned long req, const char *name)
{
    if (isPartitionDev(dev)) return err("目标是分区，请选择整块磁盘");
    int fd = openDev(dev, O_RDWR);
    if (fd < 0) return err("open");
    quint64 size = 0;
    ioctl(fd, BLKGETSIZE64, &size);
    quint64 range[2] = {0, size};
    int r = ioctl(fd, req, range);
    close(fd);
    return r ? err(name) : QString("%1 OK (%2 bytes)").arg(name).arg(size);
}

QString diskZero(const QString &dev) { return wholeDevOp(dev, BLKZEROOUT, "BLKZEROOUT"); }
QString diskTrim(const QString &dev) { return wholeDevOp(dev, BLKDISCARD, "BLKDISCARD"); }

// ---------------- BCD (libhivex) ----------------
static QString utf16ToQString(const char *data, size_t len)
{
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(data), len / 2);
}

static QString nodeName(hive_h *h, hive_node_h node)
{
    size_t len = hivex_node_name_len(h, node);
    char *name = hivex_node_name(h, node);
    QString s;
    if (name) {
        s = QString::fromUtf8(name, int(len));
        free(name);
    }
    if (s.isEmpty()) s = QStringLiteral("(unnamed len=%1)").arg(qlonglong(len));
    return s;
}

static void dumpNode(hive_h *h, hive_node_h node, const QString &path, QString &out, int depth)
{
    if (depth > 12) return;
    QString nm = nodeName(h, node);
    QString p = path + "/" + nm;
    out += p + "\n";
    hive_value_h *vals = hivex_node_values(h, node);
    if (vals) {
        for (int i = 0; vals[i]; i++) {
            char *k = hivex_value_key(h, vals[i]);
            out += QString("  %1\n").arg(k ? k : "");
            free(k);
        }
        free(vals);
    }
    hive_node_h *children = hivex_node_children(h, node);
    if (children) {
        for (int i = 0; children[i]; i++) dumpNode(h, children[i], p, out, depth + 1);
        free(children);
    }
}

QString bcdExport(const QString &file)
{
    hive_h *h = hivex_open(file.toUtf8().constData(), 0);
    if (!h) return err("hivex_open");
    QString out;
    dumpNode(h, hivex_root(h), "", out, 0);
    hivex_close(h);
    return out;
}

static QByteArray toUtf16(const QString &s)
{
    QByteArray b;
    for (QChar c : s) { quint16 u = c.unicode(); b.append((char)(u & 0xff)); b.append((char)(u >> 8)); }
    b.append((char)0); b.append((char)0);
    return b;
}

static void bcdBackup(const QString &file)
{
    const QString bak = file + ".bak";
    if (!QFile::exists(bak) && QFile::exists(file))
        QFile::copy(file, bak);
}

static hive_node_h bcdChildCI(hive_h *h, hive_node_h parent, const QString &name)
{
    hive_node_h n = hivex_node_get_child(h, parent, name.toUtf8().constData());
    if (n) return n;
    hive_node_h *kids = hivex_node_children(h, parent);
    if (!kids) return 0;
    hive_node_h found = 0;
    for (int i = 0; kids[i]; i++) {
        QString nm = nodeName(h, kids[i]);
        if (nm.compare(name, Qt::CaseInsensitive) == 0) { found = kids[i]; break; }
    }
    free(kids);
    return found;
}

static hive_node_h bcdObjects(hive_h *h, bool create);

QString bcdCreate(const QString &file, const QString &label, const QString &winroot)
{
    bcdBackup(file);
    hive_h *h = hivex_open(file.toUtf8().constData(), HIVEX_OPEN_WRITE);
    if (!h) return err("hivex_open(write)");
    hive_node_h root = hivex_root(h);
    hive_node_h objs = bcdObjects(h, true);
    if (!objs) { hivex_close(h); return QStringLiteral("cannot create Objects"); }
    const char *bm = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}";
    const char *os = "{b2721d73-1db4-4c62-bf78-c548a880142d}";
    hive_node_h bmn = hivex_node_get_child(h, objs, bm);
    if (!bmn) bmn = hivex_node_add_child(h, objs, bm);
    hive_node_h osn = hivex_node_get_child(h, objs, os);
    if (!osn) osn = hivex_node_add_child(h, objs, os);
    hive_node_h bme = hivex_node_get_child(h, bmn, "Elements");
    if (!bme) bme = hivex_node_add_child(h, bmn, "Elements");
    hive_node_h ose = hivex_node_get_child(h, osn, "Elements");
    if (!ose) ose = hivex_node_add_child(h, osn, "Elements");
    QByteArray lab = toUtf16(label);
    QByteArray path = toUtf16("\\Windows\\system32\\winload.efi");
    QByteArray wr = toUtf16(winroot);
    auto setSz = [&](hive_node_h parent, const char *sub, const QByteArray &val) {
        hive_node_h n = bcdChildCI(h, parent, QString::fromUtf8(sub));
        if (!n) n = hivex_node_add_child(h, parent, sub);
        struct hive_set_value v{};
        v.key = const_cast<char *>("Element");
        v.t = hive_t_REG_SZ;
        v.len = val.size();
        v.value = const_cast<char *>(val.constData());
        hivex_node_set_value(h, n, &v, 0);
    };
    setSz(bme, "12000004", lab);
    setSz(ose, "12000002", path);
    setSz(ose, "12000004", lab);
    setSz(ose, "22000002", wr);

    auto setType = [&](hive_node_h obj, quint32 type) {
        hive_node_h d = hivex_node_get_child(h, obj, "Description");
        if (!d) d = hivex_node_add_child(h, obj, "Description");
        struct hive_set_value v{};
        v.key = const_cast<char *>("Type");
        v.t = hive_t_REG_DWORD;
        v.len = 4;
        v.value = reinterpret_cast<char *>(&type);
        hivex_node_set_value(h, d, &v, 0);
    };
    setType(bmn, 0x10100002); // boot manager
    setType(osn, 0x10200003); // OS loader

    int r = hivex_commit(h, nullptr, 0);
    hivex_close(h);
    return r ? err("hivex_commit") : QString("BCD created");
}

static hive_node_h bcdObjects(hive_h *h, bool create)
{
    hive_node_h root = hivex_root(h);
    hive_node_h *kids = hivex_node_children(h, root);
    hive_node_h o = 0;
    if (kids) {
        for (int i = 0; kids[i]; i++) {
            QString nm = nodeName(h, kids[i]);
            if (nm.compare(QStringLiteral("Objects"), Qt::CaseInsensitive) == 0) { o = kids[i]; break; }
        }
        free(kids);
    }
    if (!o) o = hivex_node_get_child(h, root, "Objects");
    if (!o && create) o = hivex_node_add_child(h, root, "Objects");
    return o;
}

static hive_node_h bcdElemNode(hive_h *h, const QString &guid, const QString &elem, bool create)
{
    hive_node_h o = bcdObjects(h, create);
    if (!o) return 0;
    QByteArray g = guid.trimmed().toUtf8(), e = elem.trimmed().toUtf8();
    hive_node_h obj = bcdChildCI(h, o, guid.trimmed());
    if (!obj) {
        if (!create) return 0;
        obj = hivex_node_add_child(h, o, g.constData());
    }
    hive_node_h els = bcdChildCI(h, obj, QStringLiteral("Elements"));
    if (!els) {
        if (!create) return 0;
        els = hivex_node_add_child(h, obj, "Elements");
    }
    hive_node_h n = bcdChildCI(h, els, elem.trimmed());
    if (!n && create) n = hivex_node_add_child(h, els, e.constData());
    return n;
}

static QString bcdValueText(hive_h *h, hive_value_h v, hive_type *typeOut = nullptr)
{
    hive_type t; size_t len = 0;
    if (hivex_value_type(h, v, &t, &len) < 0) return QStringLiteral("(unreadable)");
    if (typeOut) *typeOut = t;
    if (t == hive_t_REG_SZ || t == hive_t_REG_EXPAND_SZ) {
        char *s = hivex_value_string(h, v);
        QString r = s ? QString::fromUtf8(s) : QString();
        free(s);
        return r;
    }
    if (t == hive_t_REG_MULTI_SZ) {
        char **ss = hivex_value_multiple_strings(h, v);
        QStringList parts;
        if (ss) { for (int i = 0; ss[i]; i++) parts << QString::fromUtf8(ss[i]); free(ss); }
        return parts.join("\n");
    }
    if (t == hive_t_REG_DWORD) return QString::number(hivex_value_dword(h, v));
    char *raw = hivex_value_value(h, v, &t, &len);
    QString hex;
    if (raw) {
        for (size_t i = 0; i < len && i < 64; i++) hex += QString("%1").arg((unsigned char)raw[i], 2, 16, QChar('0'));
        if (len > 64) hex += "...";
        free(raw);
    }
    return hex;
}

QString bcdList(const QString &file)
{
    hive_h *h = hivex_open(file.toUtf8().constData(), 0);
    if (!h) return err("hivex_open");
    QString out;
    hive_node_h root = hivex_root(h);
    QString rootName = nodeName(h, root);
    hive_node_h *rk = hivex_node_children(h, root);
    int rn = 0; while (rk && rk[rn]) rn++;
    if (rk) free(rk);
    hive_node_h objs = bcdObjects(h, false);
    if (!objs) { hivex_close(h); return QString("(no Objects key; root=%1 children=%2)").arg(rootName).arg(rn); }
    hive_node_h *os = hivex_node_children(h, objs);
    if (os) {
        for (int i = 0; os[i]; i++) {
            char *nm = hivex_node_name(h, os[i]);
            QString guid = nm ? QString::fromUtf8(nm) : QString();
            free(nm);
            out += guid + "\n";
            hive_node_h els = hivex_node_get_child(h, os[i], "Elements");
            hive_node_h *es = els ? hivex_node_children(h, els) : nullptr;
            if (es) {
                for (int k = 0; es[k]; k++) {
                    char *en = hivex_node_name(h, es[k]);
                    QString eid = en ? QString::fromUtf8(en) : QString();
                    free(en);
                    hive_value_h v = hivex_node_get_value(h, es[k], "Element");
                    hive_type t = hive_t_REG_NONE;
                    QString text = v ? bcdValueText(h, v, &t) : QStringLiteral("(no Element value)");
                    out += QString("  %1  type=%2  %3\n").arg(eid).arg(int(t)).arg(text);
                }
                free(es);
            }
        }
        free(os);
    }
    hivex_close(h);
    return out;
}

QStringList bcdObjectGuids(const QString &file)
{
    QStringList out;
    hive_h *h = hivex_open(file.toUtf8().constData(), 0);
    if (!h) return out;
    hive_node_h objs = bcdObjects(h, false);
    if (!objs) { hivex_close(h); return out; }
    hive_node_h *os = hivex_node_children(h, objs);
    if (os) {
        for (int i = 0; os[i]; i++) {
            char *nm = hivex_node_name(h, os[i]);
            if (nm && nm[0]) out << QString::fromUtf8(nm);
            free(nm);
        }
        free(os);
    }
    hivex_close(h);
    return out;
}

QStringList bcdElementIds(const QString &file, const QString &guid)
{
    QStringList out;
    hive_h *h = hivex_open(file.toUtf8().constData(), 0);
    if (!h) return out;
    hive_node_h objs = bcdObjects(h, false);
    hive_node_h obj = objs ? bcdChildCI(h, objs, guid.trimmed()) : 0;
    hive_node_h els = obj ? bcdChildCI(h, obj, QStringLiteral("Elements")) : 0;
    hive_node_h *es = els ? hivex_node_children(h, els) : nullptr;
    if (es) {
        for (int i = 0; es[i]; i++) {
            char *nm = hivex_node_name(h, es[i]);
            if (nm && nm[0]) out << QString::fromUtf8(nm);
            free(nm);
        }
        free(es);
    }
    hivex_close(h);
    return out;
}

QString bcdGetElement(const QString &file, const QString &guid, const QString &elem)
{
    hive_h *h = hivex_open(file.toUtf8().constData(), 0);
    if (!h) return err("hivex_open");
    hive_node_h n = bcdElemNode(h, guid, elem, false);
    if (!n) { hivex_close(h); return QStringLiteral("element not found"); }
    hive_value_h v = hivex_node_get_value(h, n, "Element");
    if (!v) { hivex_close(h); return QStringLiteral("element has no Element value"); }
    hive_type t = hive_t_REG_NONE;
    QString text = bcdValueText(h, v, &t);
    hivex_close(h);
    return QString("t=%1\n%2").arg(int(t)).arg(text);
}

QString bcdSetElement(const QString &file, const QString &guid, const QString &elem, int type, const QString &value)
{
    QByteArray val;
    if (type == hive_t_REG_SZ || type == hive_t_REG_EXPAND_SZ) {
        val = toUtf16(value);
    } else if (type == hive_t_REG_MULTI_SZ) {
        for (const QString &s : value.split('\n', Qt::SkipEmptyParts)) val += toUtf16(s);
        val.append((char)0); val.append((char)0);
    } else if (type == hive_t_REG_DWORD) {
        quint32 v = value.trimmed().startsWith("0x", Qt::CaseInsensitive)
                    ? value.trimmed().toUInt(nullptr, 16) : value.trimmed().toUInt();
        val.append((char)(v & 0xff)); val.append((char)((v >> 8) & 0xff));
        val.append((char)((v >> 16) & 0xff)); val.append((char)((v >> 24) & 0xff));
    } else if (type == hive_t_REG_BINARY) {
        QString hex = value;
        hex.remove(QRegularExpression("\\s"));
        if (hex.size() % 2 || hex.size() > 65536) return QStringLiteral("invalid hex length");
        val = QByteArray::fromHex(hex.toLatin1());
        if (val.size() * 2 != hex.size()) return QStringLiteral("invalid hex characters");
    } else {
        return QStringLiteral("unsupported type (use 1/3/4/7)");
    }

    bcdBackup(file);
    hive_h *h = hivex_open(file.toUtf8().constData(), HIVEX_OPEN_WRITE);
    if (!h) return err("hivex_open(write)");
    hive_node_h n = bcdElemNode(h, guid, elem, true);
    if (!n) { hivex_close(h); return QStringLiteral("cannot create element node"); }
    struct hive_set_value sv{};
    sv.key = const_cast<char *>("Element");
    sv.t = (hive_type)type;
    sv.len = val.size();
    sv.value = const_cast<char *>(val.constData());
    int r = hivex_node_set_value(h, n, &sv, 0);
    if (!r) r = hivex_commit(h, nullptr, 0);
    hivex_close(h);
    return r ? err("hivex set/commit") : QString("element %1 set (%2 bytes)").arg(elem).arg(val.size());
}

QString bcdDeleteElement(const QString &file, const QString &guid, const QString &elem)
{
    bcdBackup(file);
    hive_h *h = hivex_open(file.toUtf8().constData(), HIVEX_OPEN_WRITE);
    if (!h) return err("hivex_open(write)");
    hive_node_h n = bcdElemNode(h, guid, elem, false);
    if (!n) { hivex_close(h); return QStringLiteral("element not found"); }
    int r = hivex_node_delete_child(h, n);
    if (!r) r = hivex_commit(h, nullptr, 0);
    hivex_close(h);
    return r ? err("hivex delete/commit") : QString("element %1 deleted; remount/reload BCD in Windows").arg(elem);
}

// ---------------- UEFI (efivarfs) ----------------
static const QString EV = "/sys/firmware/efi/efivars/";
static const QString GUID = "8be4df61-93ca-11d2-aa0d-00e098032b8c";

static QByteArray readVar(const QString &name, quint32 *attr)
{
    QFile f(EV + name);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    QByteArray b = f.readAll();
    if (b.size() < 4) return QByteArray();
    if (attr) *attr = (quint32)(unsigned char)b[0] | ((quint32)(unsigned char)b[1] << 8) |
                      ((quint32)(unsigned char)b[2] << 16) | ((quint32)(unsigned char)b[3] << 24);
    return b.mid(4);
}

static bool writeVar(const QString &name, quint32 attr, const QByteArray &data)
{
    QByteArray b;
    b.append((char)(attr & 0xff)); b.append((char)((attr >> 8) & 0xff));
    b.append((char)((attr >> 16) & 0xff)); b.append((char)((attr >> 24) & 0xff));
    b.append(data);
    QFile f(EV + name);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(b) == b.size();
}

static QString uefiAddEntry(const QString &prefix, const QString &desc, const QString &devicePathHex);
static QString uefiDelEntry(const QString &prefix, const QString &num);
static QString uefiListEntries(const QString &prefix);
static quint16 parseBootNum(const QString &s);

QString uefiList()
{
    QByteArray order = readVar("BootOrder-" + GUID, nullptr);
    QString out;
    QDir d(EV);
    QStringList names = d.entryList(QDir::Files);
    names.sort();
    for (const QString &n : names) {
        if (!n.startsWith("Boot") || !n.contains('-')) continue;
        quint32 attr = 0;
        QByteArray data = readVar(n, &attr);
        QString desc;
        if (data.size() >= 6) {
            int flen = (unsigned char)data[4] | ((unsigned char)data[5] << 8);
            int chars = flen / 2;
            const char16_t *p = reinterpret_cast<const char16_t *>(data.constData() + 6);
            while (chars > 0 && p[chars - 1] == 0) --chars;
            desc = QString::fromUtf16(p, chars);
        }
        out += QString("%1  %2\n").arg(n.left(8), desc);
    }
    out += "BootOrder: ";
    for (int i = 0; i + 1 < order.size(); i += 2)
        out += QString("%1 ").arg((unsigned char)order[i] | ((unsigned char)order[i + 1] << 8), 4, 16, QChar('0'));
    return out;
}

static QString parentDiskDev(const QString &partDev)
{
    const QString name = QFileInfo(partDev).fileName();
    const QString sys = QStringLiteral("/sys/class/block/") + name;
    QFileInfo fi(sys);
    if (fi.isSymLink()) {
        const QString target = fi.symLinkTarget();
        if (!target.isEmpty()) {
            const QFileInfo targetInfo(fi.dir(), target);
            const QString diskName = QFileInfo(targetInfo.absolutePath()).fileName();
            if (!diskName.isEmpty() && QFile::exists(QStringLiteral("/sys/class/block/") + diskName))
                return QStringLiteral("/dev/") + diskName;
        }
    }
    QString disk = name;
    if (name.startsWith(QStringLiteral("nvme"), Qt::CaseInsensitive))
        disk.remove(QRegularExpression("p\\d+$"));
    else
        disk.remove(QRegularExpression("\\d+$"));
    return disk == name ? QString() : QStringLiteral("/dev/") + disk;
}

QString uefiDevicePath(const QString &espDev, const QString &fileRel)
{
    const QString base = QFileInfo(espDev).fileName();
    QRegularExpression pn(QStringLiteral("(\\d+)$"));
    QRegularExpressionMatch pm = pn.match(base);
    if (!pm.hasMatch()) return QStringLiteral("error: ESP 设备需为分区，如 /dev/sda1");
    int pno = pm.captured(1).toInt();
    if (pno < 1) return QStringLiteral("error: 分区号无效");

    const QString diskDev = parentDiskDev(espDev);
    if (diskDev.isEmpty()) return QStringLiteral("error: 无法从分区定位整块磁盘");
    QFile f(diskDev);
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("error: 打不开 ESP 所在整块磁盘");
    int fd = f.handle();
    GptHdr h{};
    if (pread(fd, &h, sizeof h, 512) != ssize_t(sizeof h) || memcmp(h.sig, "EFI PART", 8) != 0)
        return QStringLiteral("error: ESP 所在磁盘不是 GPT（仅支持 GPT/UEFI）");
    if (h.entSize != sizeof(GptEntry) || h.entNum == 0 || h.entNum > 1024)
        return QStringLiteral("error: GPT 条目布局不支持");
    QByteArray ents(int(h.entNum) * int(h.entSize), 0);
    if (pread(fd, ents.data(), ents.size(), off_t(h.entLba) * 512) != ssize_t(ents.size()))
        return QStringLiteral("error: 读 GPT 分区表失败");
    const GptEntry *e = reinterpret_cast<const GptEntry *>(ents.constData());
    if (pno > int(h.entNum)) return QStringLiteral("error: 分区号超出 GPT 表");
    const GptEntry &pe = e[pno - 1];
    bool zero = true;
    for (int i = 0; i < 16; i++) if (pe.type[i]) { zero = false; break; }
    if (zero) return QStringLiteral("error: 该分区在 GPT 中不存在");
    f.close();

    auto node = [](quint8 type, quint8 sub, const QByteArray &payload) {
        QByteArray n;
        n.append(char(type)); n.append(char(sub));
        quint16 len = quint16(4 + payload.size());
        n.append(char(len & 0xff)); n.append(char((len >> 8) & 0xff));
        n.append(payload);
        return n;
    };
    QByteArray hd(38, 0); // Hard Drive device path payload
    auto put32 = [](QByteArray &b, int off, quint32 v) {
        for (int i = 0; i < 4; i++) b[off + i] = char((v >> (8 * i)) & 0xff);
    };
    auto put64 = [](QByteArray &b, int off, quint64 v) {
        for (int i = 0; i < 8; i++) b[off + i] = char((v >> (8 * i)) & 0xff);
    };
    put32(hd, 0, quint32(pno));
    put64(hd, 4, pe.first);
    put64(hd, 12, pe.last - pe.first + 1);
    memcpy(hd.data() + 20, pe.guid, 16);
    hd[36] = char(0x02); // Partition Format: GPT
    hd[37] = char(0x02); // Signature Type: GUID

    QByteArray filePath;
    QString rel = fileRel.trimmed();
    rel.replace('\\', '/');
    if (!rel.startsWith('/')) rel.prepend('/');
    rel.replace('/', '\\');
    for (QChar c : rel) { filePath.append(char(c.unicode() & 0xff)); filePath.append(char((c.unicode() >> 8) & 0xff)); }
    filePath.append(char(0)); filePath.append(char(0));

    QByteArray dp;
    dp += node(0x04, 0x01, hd);
    dp += node(0x04, 0x04, filePath);
    dp += node(0x7f, 0xff, QByteArray());
    return QString::fromLatin1(dp.toHex());
}

QString uefiAdd(const QString &desc, const QString &devicePathHex)
{
    return uefiAddEntry(QStringLiteral("Boot"), desc, devicePathHex);
}

static quint16 parseBootNum(const QString &s)
{
    QString t = s.trimmed();
    if (t.startsWith("Boot", Qt::CaseInsensitive)) t = t.mid(4);
    if (t.startsWith("Driver", Qt::CaseInsensitive)) t = t.mid(6);
    if (t.contains('-')) t = t.section('-', 0, 0);
    return quint16(t.toUShort(nullptr, 16));
}

// Shared EFI_LOAD_OPTION writer for Boot#### and Driver#### variables.
static QString uefiAddEntry(const QString &prefix, const QString &desc, const QString &devicePathHex)
{
    QString hex = devicePathHex;
    hex.remove(' '); hex.remove(','); hex.remove('\n');
    QByteArray dp = QByteArray::fromHex(hex.toLatin1());
    if (dp.size() < 4) return QStringLiteral("invalid device path hex");

    QByteArray opt;
    quint32 attr = 0x1;                  // LOAD_OPTION_ACTIVE
    opt.append((char)(attr & 0xff)); opt.append((char)((attr >> 8) & 0xff));
    opt.append((char)((attr >> 16) & 0xff)); opt.append((char)((attr >> 24) & 0xff));
    // EFI_LOAD_OPTION.FilePathListLength counts only the device path bytes.
    opt.append((char)(dp.size() & 0xff)); opt.append((char)((dp.size() >> 8) & 0xff));
    for (QChar c : desc) { opt.append((char)(c.unicode() & 0xff)); opt.append((char)((c.unicode() >> 8) & 0xff)); }
    opt.append(QByteArray(2, '\0'));     // description NUL
    opt.append(dp);

    quint16 num = 0;
    for (; num < 0xFFFE; ++num) {
        QString n = QString("%1%2-%3").arg(prefix).arg(num, 4, 16, QChar('0')).arg(GUID);
        if (!QFile::exists(EV + n)) break;
    }
    QString name = QString("%1%2-%3").arg(prefix).arg(num, 4, 16, QChar('0')).arg(GUID);
    if (!writeVar(name, 0x7, opt)) return err("write load option");

    QByteArray order = readVar(prefix + "Order-" + GUID, nullptr);
    order.append((char)(num & 0xff)); order.append((char)((num >> 8) & 0xff));
    if (!writeVar(prefix + "Order-" + GUID, 0x7, order))
        return QString("entry %1 written, but %2Order update failed").arg(name, prefix);
    return QString("added %1  %2").arg(name, desc);
}

static QString uefiListEntries(const QString &prefix)
{
    QString out;
    QDir d(EV);
    QStringList names = d.entryList(QStringList(prefix + "*"), QDir::Files);
    names.sort();
    for (const QString &n : names) {
        if (!n.contains('-') || n.startsWith(prefix + "Order")) continue;
        quint32 attr = 0;
        QByteArray data = readVar(n, &attr);
        QString desc, dpath;
        if (data.size() >= 6) {
            int flen = (unsigned char)data[4] | ((unsigned char)data[5] << 8);
            int chars = flen / 2;
            const char16_t *p = reinterpret_cast<const char16_t *>(data.constData() + 6);
            while (chars > 0 && p[chars - 1] == 0) --chars;
            desc = QString::fromUtf16(p, chars);
            int off = 6 + flen;
            if (data.size() > off)
                dpath = QString::fromLatin1(data.mid(off).toHex());
        }
        out += QString("%1  %2\n").arg(n.left(prefix.size() + 4), desc);
        if (!dpath.isEmpty()) out += QString("    dp: %1\n").arg(dpath.left(160));
    }
    QByteArray order = readVar(prefix + "Order-" + GUID, nullptr);
    out += prefix + "Order: ";
    for (int i = 0; i + 1 < order.size(); i += 2)
        out += QString("%1 ").arg((unsigned char)order[i] | ((unsigned char)order[i + 1] << 8), 4, 16, QChar('0'));
    return out.trimmed().isEmpty() ? QString("(none)") : out;
}

QString uefiDel(const QString &num)
{
    return uefiDelEntry(QStringLiteral("Boot"), num);
}

static QString uefiDelEntry(const QString &prefix, const QString &num)
{
    quint16 v = parseBootNum(num);
    QString name = QString("%1%2-%3").arg(prefix).arg(v, 4, 16, QChar('0')).arg(GUID);
    if (!QFile::remove(EV + name)) return err("unlink");

    QByteArray order = readVar(prefix + "Order-" + GUID, nullptr);
    QByteArray next;
    for (int i = 0; i + 1 < order.size(); i += 2) {
        quint16 item = (quint16)(unsigned char)order[i] | ((quint16)(unsigned char)order[i + 1] << 8);
        if (item != v) { next.append(order[i]); next.append(order[i + 1]); }
    }
    if (next != order) writeVar(prefix + "Order-" + GUID, 0x7, next);
    return QString("deleted %1 and removed it from %2Order").arg(name, prefix);
}

QString uefiAddDriver(const QString &desc, const QString &devicePathHex)
{
    return uefiAddEntry(QStringLiteral("Driver"), desc, devicePathHex);
}

QString uefiDelDriver(const QString &num)
{
    return uefiDelEntry(QStringLiteral("Driver"), num);
}

QString uefiSetDriverOrder(const QString &csv)
{
    QByteArray data;
    for (const QString &s : csv.split(',', Qt::SkipEmptyParts)) {
        quint16 v = parseBootNum(s);
        data.append((char)(v & 0xff)); data.append((char)(v >> 8));
    }
    return writeVar("DriverOrder-" + GUID, 0x7, data) ? QString("DriverOrder set") : err("write DriverOrder");
}

QString uefiSetOrder(const QString &csv)
{
    QByteArray data;
    for (const QString &s : csv.split(',', Qt::SkipEmptyParts)) {
        quint16 v = parseBootNum(s);
        data.append((char)(v & 0xff)); data.append((char)(v >> 8));
    }
    return writeVar("BootOrder-" + GUID, 0x7, data) ? QString("BootOrder set") : err("write BootOrder");
}

QString uefiDrvList()
{
    return uefiListEntries(QStringLiteral("Driver"));
}

QString uefiDrvOrder()
{
    QByteArray b = readVar("DriverOrder-" + GUID, nullptr);
    QString out;
    for (int i = 0; i + 1 < b.size(); i += 2)
        out += QString("%1 ").arg((unsigned char)b[i] | ((unsigned char)b[i + 1] << 8), 4, 16, QChar('0'));
    return out.isEmpty() ? QString("(empty)") : out;
}

// ---------------- NTFS (mount(2) + xattr) ----------------
QString fsMount(const QString &dev, const QString &dir)
{
    QDir().mkpath(dir);
    QByteArray s = dev.toUtf8(), t = dir.toUtf8();
    if (mount(s.constData(), t.constData(), "ntfs3", 0, nullptr) == 0) return QString("mounted ntfs3");
    if (mount(s.constData(), t.constData(), "ntfs-3g", 0, nullptr) == 0) return QString("mounted ntfs-3g");
    return err("mount");
}

QString fsUnmount(const QString &dir)
{
    sync();
    if (umount2(dir.toUtf8().constData(), 0) == 0) return QString("unmounted");
    if (umount2(dir.toUtf8().constData(), MNT_DETACH) == 0) return QString("unmounted (lazy)");
    return err("umount");
}

QString fsAttrGet(const QString &path)
{
    quint32 v = 0;
    ssize_t n = getxattr(path.toUtf8().constData(), "system.ntfs_attrib", &v, sizeof(v));
    if (n < 0) return err("getxattr attrib");
    return QString::number(v);
}

QString fsAttrSet(const QString &path, quint32 value)
{
    quint32 le = value; // x86 little-endian
    if (setxattr(path.toUtf8().constData(), "system.ntfs_attrib", &le, sizeof(le), 0) < 0)
        return err("setxattr attrib");
    return QString("attributes set");
}

QString fsAclGet(const QString &path)
{
    QByteArray buf(4096, 0);
    ssize_t n = getxattr(path.toUtf8().constData(), "system.ntfs_security", buf.data(), buf.size());
    if (n < 0) {
        if (errno == ENOENT || errno == ENODATA)
            return QStringLiteral("(no per-file security descriptor; default SD)");
        return err("getxattr security");
    }
    QString hex;
    for (int i = 0; i < n; i++) hex += QString("%1").arg((unsigned char)buf[i], 2, 16, QChar('0'));
    return hex;
}

QString fsAclSet(const QString &path, const QString &hex)
{
    QString t = hex;
    t.remove(QRegularExpression("\\s"));
    if (t.size() < 20 || (t.size() % 2))
        return QStringLiteral("invalid hex: need >=20 bytes, even length");
    QByteArray b = QByteArray::fromHex(t.toLatin1());
    if (b.size() * 2 != t.size())
        return QStringLiteral("invalid hex characters");
    if (setxattr(path.toUtf8().constData(), "system.ntfs_security", b.constData(), b.size(), 0) < 0)
        return err("setxattr security");
    return QString("security descriptor set (%1 bytes)").arg(b.size());
}

// ---------------- Power (reboot(2) / sysrq) ----------------
static void writeProc(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { ssize_t n = write(fd, val, strlen(val)); (void)n; close(fd); }
}

QString powerReboot() { sync(); reboot(LINUX_REBOOT_CMD_RESTART); return QString("reboot issued"); }
QString powerOff() { sync(); reboot(LINUX_REBOOT_CMD_POWER_OFF); return QString("poweroff issued"); }
QString powerCold()
{
    sync();
    writeProc("/proc/sys/kernel/sysrq", "1");
    writeProc("/proc/sysrq-trigger", "b");
    return QString("cold reboot issued");
}

} // namespace bk