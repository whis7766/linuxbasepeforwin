// 宿主 Linux 自检：不依赖 Buildroot，用发行版 Qt6/wimlib/hivex 直接跑后端功能。
// 用法：
//   lbpe-selftest wim  <workdir>
//   lbpe-selftest bcd  <hive 文件>
//   lbpe-selftest gpt  <块设备>   (需要 root + loop 设备)
#include "Backend.h"
#include "MainWindow.h"

#include <QApplication>
#include <QTabWidget>
#include <QWidget>
#include <QDir>

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

static int runUefi()
{
    // 合成一条 EFI_LOAD_OPTION：attrs=1，描述 "自检启动项"，设备路径 4 字节
    QByteArray opt;
    opt.append("\x01\x00\x00\x00", 4);
    QByteArray desc;
    for (QChar c : QString("自检启动项")) {
        desc.append((char)(c.unicode() & 0xff));
        desc.append((char)(c.unicode() >> 8));
    }
    desc.append("\x00\x00", 2);
    QByteArray dp = QByteArray::fromHex("01041400");
    opt.append((char)(dp.size() & 0xff));
    opt.append((char)((dp.size() >> 8) & 0xff));
    opt.append(desc);
    opt.append(dp);

    QString d, p;
    QString e = bk::uefiParseLoadOption(opt, &d, &p);
    check(e.isEmpty(), "解析 EFI_LOAD_OPTION: " + e);
    check(d == QString("自检启动项"), "描述解析: " + d);
    check(p == "01041400", "设备路径解析: " + p);

    // 截断的变量应被拒绝而不是越界读
    QByteArray cut = opt.left(8);
    QString e2 = bk::uefiParseLoadOption(cut, nullptr, nullptr);
    check(!e2.isEmpty(), "截断变量被拒: " + e2);
    return 0;
}

static int runGui()
{
    static char a0[] = "lbpe-selftest", a1[] = "-platform", a2[] = "offscreen";
    char *args[] = {a0, a1, a2, nullptr};
    int argc = 3;
    QApplication app(argc, args);
    MainWindow w;
    auto *tabs = w.findChild<QTabWidget *>();
    check(tabs != nullptr, "主窗口构建成功");
    if (tabs) {
        check(tabs->count() >= 7, QString("页面数量 %1").arg(tabs->count()));
        QStringList titles;
        for (int i = 0; i < tabs->count(); ++i) titles << tabs->tabText(i);
        fprintf(stdout, "  tabs: %s\n", titles.join(" | ").toUtf8().constData());
    }
    return 0;
}

static int runNtfs(const QString &dev, const QString &mnt)
{
    QString r = bk::fsMount(dev, mnt);
    check(r.contains("mounted"), "挂载 NTFS: " + r);
    if (!r.contains("mounted")) return 0;

    QString file = mnt + "/selftest.txt";
    QFile f(file);
    check(f.open(QIODevice::WriteOnly), "写入测试文件");
    f.write("ntfs selftest");
    f.close();
    QFile rd(file);
    rd.open(QIODevice::ReadOnly);
    check(rd.readAll() == "ntfs selftest", "读回内容一致");
    rd.close();

    QString a0 = bk::fsAttrGet(file);
    fprintf(stdout, "  属性: %s\n", a0.toUtf8().constData());
    QString a1 = bk::fsAttrSet(file, 0x22); // ARCHIVE|HIDDEN
    check(a1 == "attributes set", "设置属性: " + a1);
    QString a2 = bk::fsAttrGet(file);
    check(a2 == "34", "属性读回: " + a2);

    QString acl = bk::fsAclGet(file);
    fprintf(stdout, "  ACL: %s\n", acl.left(200).toUtf8().constData());

    QString u = bk::fsUnmount(mnt);
    check(u.contains("unmounted") || u.isEmpty(), "卸载: " + u);
    return 0;
}

int main(int argc, char **argv)
{
    const QString mode = argc > 1 ? argv[1] : QString();
    if (mode == "gui" || mode == "uefi") {
        int r = mode == "gui" ? runGui() : runUefi();
        fprintf(stdout, "== %s: %d failure(s) ==\n", mode.toUtf8().constData(), fails);
        return fails == 0 ? r : 1;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s wim|bcd|gpt|ntfs <path> [mountdir] | gui | uefi\n", argv[0]);
        return 2;
    }
    const QString arg = argv[2];
    const int argcFull = argc;
    int rc = 2;
    if (mode == "wim") rc = runWim(arg);
    else if (mode == "bcd") rc = runBcd(arg);
    else if (mode == "gpt") rc = runGpt(arg);
    else if (mode == "uefi") rc = runUefi();
    else if (mode == "gui") rc = runGui();
    else if (mode == "ntfs") rc = runNtfs(arg, argcFull > 3 ? argv[3] : QStringLiteral("/tmp/ntfsmnt"));
    fprintf(stdout, "== %s: %d failure(s) ==\n", argv[1], fails);
    return fails == 0 ? (rc == 0 ? 0 : rc) : 1;
}
