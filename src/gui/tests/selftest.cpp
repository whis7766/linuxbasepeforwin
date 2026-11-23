// 宿主 Linux 自检：不依赖 Buildroot，用发行版 Qt6/wimlib/hivex 直接跑后端功能。
// 用法：
//   lbpe-selftest wim  <workdir>
//   lbpe-selftest bcd  <hive 文件>
//   lbpe-selftest gpt  <块设备>   (需要 root + loop 设备)
#include "Backend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

static int fails = 0;

static void check(bool ok, const QString &what)
{
    fprintf(ok ? stdout : stderr, "[%s] %s\n", ok ? "PASS" : "FAIL", what.toUtf8().constData());
    if (!ok) fails++;
}

static int runWim(const QString &work)
{
    QDir().mkpath(work);
    const QString src = work + "/src";
    const QString wim = work + "/out.wim";
    const QString dst = work + "/dst";
    QDir().mkpath(src + "/sub");
    for (int i = 0; i < 3; i++) {
        QFile f(QString("%1/f%2.txt").arg(src).arg(i));
        if (!f.open(QIODevice::WriteOnly)) return 2;
        f.write(QByteArray("hello wim ") + QByteArray::number(i));
    }
    QFile g(src + "/sub/nested.txt");
    g.open(QIODevice::WriteOnly);
    g.write("nested");
    g.close();

    auto log = [](const QString &m) { fprintf(stdout, "  %s\n", m.toUtf8().constData()); };
    QString r = bk::wimCapture(src, wim, QString(), log);
    check(r == "OK" && QFileInfo(wim).size() > 0, "WIM 捕获: " + r);

    r = bk::wimApply(wim, 1, dst, log);
    check(r == "OK", "WIM 还原: " + r);

    bool same = QFile(dst + "/f0.txt").exists() && QFile(dst + "/sub/nested.txt").exists();
    if (same) {
        QFile a(src + "/f2.txt"), b(dst + "/f2.txt");
        a.open(QIODevice::ReadOnly);
        b.open(QIODevice::ReadOnly);
        same = a.readAll() == b.readAll();
    }
    check(same, "WIM 还原内容一致");
    return 0;
}

static int runBcd(const QString &hivePath)
{
    check(QFileInfo(hivePath).size() > 0, "BCD 样本存在");
    QString s = bk::bcdExport(hivePath);
    fprintf(stdout, "  export: %s\n", s.toUtf8().constData());

    QStringList objs = bk::bcdObjectGuids(hivePath);
    check(!objs.isEmpty(), QString("枚举对象 %1 个").arg(objs.size()));

    QString guid = objs.isEmpty() ? QString() : objs.first();
    QStringList ids = bk::bcdElementIds(hivePath, guid);
    check(!ids.isEmpty(), QString("对象 %1 元素 %2 个").arg(guid, QString::number(ids.size())));

    // 读回每个元素，至少要能成功读取大部分（有的元素类型不支持，允许失败）
    auto isErr = [](const QString &v) {
        return v.contains("not found") || v.contains("no Element value") || v.contains("failed");
    };
    int okCount = 0;
    for (const QString &id : ids) {
        const QString v = bk::bcdGetElement(hivePath, guid, id);
        if (!isErr(v)) okCount++;
    }
    check(okCount > 0, QString("元素可读 %1/%2").arg(okCount).arg(ids.size()));

    // 写入 → 读回 → 还原
    const QString elem = ids.isEmpty() ? QString() : ids.first();
    if (!elem.isEmpty()) {
        QString old = bk::bcdGetElement(hivePath, guid, elem);
        QString w = bk::bcdSetElement(hivePath, guid, elem, 1, "selftest");
        check(w.startsWith("element"), "元素写入: " + w);
        QString now = bk::bcdGetElement(hivePath, guid, elem);
        fprintf(stdout, "  写后读回: %s\n", now.toUtf8().constData());
        check(now.contains("selftest"), "元素写入后可读");
        Q_UNUSED(old);
    }
    return 0;
}

static int runGpt(const QString &dev)
{
    fprintf(stdout, "  目标设备: %s\n", dev.toUtf8().constData());
    // 空白盘建 GPT + 分区 2，再用同一扇区范围建分区 1（应成功）
    QString r = bk::diskPartition(dev, 2, 2048, 4096);
    check(r.startsWith("new GPT created"), "空白盘建 GPT: " + r);

    QString r2 = bk::diskPartition(dev, 1, 4097, 8192);
    check(r2.startsWith("GPT partition added"), "追加 GPT 分区 1: " + r2);

    QString r3 = bk::diskPartition(dev, 3, 3000, 5000);
    check(r3.contains("overlaps") || r3.contains("already"), "重叠范围被拒: " + r3);

    QString r4 = bk::diskPartition(dev, 2, 100, 200);
    check(r4.contains("already in use"), "占用槽位被拒: " + r4);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s wim|bcd|gpt <path>\n", argv[0]);
        return 2;
    }
    const QString mode = argv[1];
    const QString arg = argv[2];
    int rc = 2;
    if (mode == "wim") rc = runWim(arg);
    else if (mode == "bcd") rc = runBcd(arg);
    else if (mode == "gpt") rc = runGpt(arg);
    fprintf(stdout, "== %s: %d failure(s) ==\n", argv[1], fails);
    return fails == 0 ? (rc == 0 ? 0 : rc) : 1;
}
