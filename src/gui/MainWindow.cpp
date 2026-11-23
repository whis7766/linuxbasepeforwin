#include "MainWindow.h"
#include "Backend.h"
#include <QApplication>
#include <hivex.h>

#include <QTabWidget>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QTreeView>
#include <QFileSystemModel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QFont>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>
#include <QtConcurrent>
#include <QMetaObject>

static QFont titleFont()
{
    QFont f; f.setPointSize(f.pointSize() + 3); f.setBold(true); return f;
}

static QPlainTextEdit *makeLog()
{
    auto *l = new QPlainTextEdit; l->setReadOnly(true); l->setMaximumBlockCount(5000); return l;
}

// Append from any thread safely.
static void post(QPlainTextEdit *log, const QString &s)
{
    QMetaObject::invokeMethod(log, [log, s]() { log->appendPlainText(s); }, Qt::QueuedConnection);
}

// Run a native backend op off the UI thread.
static void async(QPlainTextEdit *log, std::function<QString()> fn)
{
    post(log, QStringLiteral("… 执行中"));
    QtConcurrent::run([log, fn]() { post(log, fn()); });
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("linuxbasepeforwin — Windows 磁盘维护 (Live RAM)"));
    resize(1024, 700);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildWimPage(), QStringLiteral("WIM 备份/还原"));
    m_tabs->addTab(buildFileManagerPage(), QStringLiteral("文件管理器"));
    m_tabs->addTab(buildDiskPage(), QStringLiteral("磁盘管理"));
    m_tabs->addTab(buildBcdPage(), QStringLiteral("BCD 引导"));
    m_tabs->addTab(buildUefiPage(), QStringLiteral("UEFI 启动项"));
    m_tabs->addTab(buildPowerPage(), QStringLiteral("电源"));
    m_tabs->addTab(buildAboutPage(), QStringLiteral("关于"));
    setCentralWidget(m_tabs);
}

QWidget *MainWindow::buildWimPage()
{
    auto *page = new QWidget; auto *lay = new QVBoxLayout(page);
    auto *h = new QLabel(QStringLiteral("WIM 备份 / 还原 (libwim 原生)")); h->setFont(titleFont()); lay->addWidget(h);
    auto *src = new QLineEdit; src->setPlaceholderText(QStringLiteral("源目录/挂载点 (capture)"));
    src->setText(QStringLiteral("/mnt/win"));
    auto *wim = new QLineEdit; wim->setPlaceholderText(QStringLiteral("WIM 文件路径"));
    wim->setText(QStringLiteral("/tmp/backup.wim"));
    auto *ex = new QPlainTextEdit; ex->setPlaceholderText(QStringLiteral("排除列表，每行一个 (可选)")); ex->setMaximumHeight(80);
    ex->setTabChangesFocus(true);
    auto *cap = new QPushButton(QStringLiteral("捕获 Capture"));
    auto *idx = new QSpinBox; idx->setMinimum(1);
    auto *dest = new QLineEdit; dest->setPlaceholderText(QStringLiteral("应用目标目录 (apply)"));
    dest->setText(QStringLiteral("/mnt/win"));
    auto *app = new QPushButton(QStringLiteral("应用 Apply"));
    auto *log = makeLog();
    auto *srcBrowse = new QPushButton(QStringLiteral("浏览…"));
    auto *srcRow = new QHBoxLayout; srcRow->addWidget(new QLabel(QStringLiteral("源目录"))); srcRow->addWidget(src, 1); srcRow->addWidget(srcBrowse);
    auto *wimBrowse = new QPushButton(QStringLiteral("保存到…"));
    auto *wimRow = new QHBoxLayout; wimRow->addWidget(new QLabel(QStringLiteral("WIM 文件"))); wimRow->addWidget(wim, 1); wimRow->addWidget(wimBrowse);
    auto *destBrowse = new QPushButton(QStringLiteral("浏览…"));
    lay->addLayout(srcRow); lay->addLayout(wimRow); lay->addWidget(ex); lay->addWidget(cap);
    auto *row = new QHBoxLayout; row->addWidget(new QLabel(QStringLiteral("索引"))); row->addWidget(idx); row->addWidget(dest, 1); row->addWidget(destBrowse); row->addWidget(app);
    lay->addLayout(row); lay->addWidget(log, 1);
    connect(srcBrowse, &QPushButton::clicked, page, [=]() {
        QString d = QFileDialog::getExistingDirectory(page, QStringLiteral("选择源目录"), src->text());
        if (!d.isEmpty()) src->setText(d);
    });
    connect(wimBrowse, &QPushButton::clicked, page, [=]() {
        QString f = QFileDialog::getSaveFileName(page, QStringLiteral("保存 WIM"), wim->text(), QStringLiteral("WIM (*.wim)"));
        if (!f.isEmpty()) wim->setText(f);
    });
    connect(destBrowse, &QPushButton::clicked, page, [=]() {
        QString d = QFileDialog::getExistingDirectory(page, QStringLiteral("选择还原目标"), dest->text());
        if (!d.isEmpty()) dest->setText(d);
    });

    connect(cap, &QPushButton::clicked, page, [=]() {
        QString cfg;
        if (!ex->toPlainText().trimmed().isEmpty()) {
            cfg = "/tmp/wim-exclude.cfg";
            QFile f(cfg); f.open(QIODevice::WriteOnly | QIODevice::Text);
            QTextStream ts(&f); ts << "[ExclusionList]\n" << ex->toPlainText() << "\n[ExclusionException]\n"; f.close();
        }
        QString s = src->text().trimmed(), w = wim->text().trimmed();
        if (s.isEmpty() || w.isEmpty()) {
            post(log, QStringLiteral("源目录和 WIM 路径不能为空"));
            return;
        }
        post(log, QString("capture src=[%1] (%2 bytes) wim=[%3]").arg(s).arg(s.toUtf8().size()).arg(w));
        async(log, [=]() {
            return bk::wimCapture(s, w, cfg, [log](const QString &m) { post(log, m); });
        });
    });
    connect(app, &QPushButton::clicked, page, [=]() {
        QString w = wim->text().trimmed(), d = dest->text().trimmed(); int i = idx->value();
        if (w.isEmpty() || d.isEmpty()) { post(log, QStringLiteral("WIM 路径和目标目录不能为空")); return; }
        async(log, [=]() { return bk::wimApply(w, i, d, [log](const QString &m) { post(log, m); }); });
    });
    return page;
}

QWidget *MainWindow::buildFileManagerPage()
{
    auto *page = new QWidget; auto *lay = new QVBoxLayout(page);
    auto *h = new QLabel(QStringLiteral("文件管理器 (NTFS 原生 mount(2)/xattr)")); h->setFont(titleFont()); lay->addWidget(h);
    auto *mrow = new QHBoxLayout;
    auto *dev = new QComboBox; dev->setEditable(true);
    const QStringList devs = bk::blockDevices();
    for (const QString &d : devs) dev->addItem(d);
    if (!devs.isEmpty()) dev->setCurrentIndex(devs.size() - 1);
    dev->setToolTip(QStringLiteral("设备（自动枚举 /proc/partitions）"));
    auto *dir = new QLineEdit; dir->setText("/mnt/win");
    auto *mnt = new QPushButton(QStringLiteral("挂载 NTFS"));
    auto *umnt = new QPushButton(QStringLiteral("卸载"));
    mrow->addWidget(dev); mrow->addWidget(dir); mrow->addWidget(mnt); mrow->addWidget(umnt); lay->addLayout(mrow);
    auto *model = new QFileSystemModel; model->setRootPath(QStringLiteral("/mnt"));
    auto *tree = new QTreeView; tree->setModel(model); tree->setRootIndex(model->index(QStringLiteral("/mnt")));
    lay->addWidget(tree, 1);
    auto *brow = new QHBoxLayout;
    auto *attr = new QPushButton(QStringLiteral("设置属性"));
    auto *acl = new QPushButton(QStringLiteral("设置 ACL"));
    auto *del = new QPushButton(QStringLiteral("删除"));
    auto *ren = new QPushButton(QStringLiteral("重命名"));
    brow->addWidget(attr); brow->addWidget(acl); brow->addWidget(del); brow->addWidget(ren); lay->addLayout(brow);
    auto *fmlog = makeLog(); fmlog->setMaximumHeight(120); lay->addWidget(fmlog);

    connect(mnt, &QPushButton::clicked, page, [=]() {
        QString d = dev->currentText(), t = dir->text();
        model->setRootPath(t); tree->setRootIndex(model->index(t));
        async(fmlog, [=]() { return bk::fsMount(d, t); });
    });
    connect(umnt, &QPushButton::clicked, page, [=]() {
        QString t = dir->text();
        QString r = bk::fsUnmount(t);
        fmlog->appendPlainText(r);
        if (r.startsWith("unmounted")) { model->setRootPath(QStringLiteral("/mnt")); tree->setRootIndex(model->index(QStringLiteral("/mnt"))); }
    });
    auto selected = [=]() { return model->filePath(tree->currentIndex()); };

    connect(attr, &QPushButton::clicked, page, [=]() {
        QString path = selected();
        if (path.isEmpty()) { QMessageBox::warning(page, "提示", "先选择文件"); return; }
        quint32 v = bk::fsAttrGet(path).toUInt();
        QDialog d(page); d.setWindowTitle(QStringLiteral("DOS 属性"));
        auto *dl = new QVBoxLayout(&d);
        auto *ro = new QCheckBox(QStringLiteral("只读 (0x1)")); ro->setChecked(v & 1);
        auto *hi = new QCheckBox(QStringLiteral("隐藏 (0x2)")); hi->setChecked(v & 2);
        auto *sy = new QCheckBox(QStringLiteral("系统 (0x4)")); sy->setChecked(v & 4);
        auto *ar = new QCheckBox(QStringLiteral("存档 (0x20)")); ar->setChecked(v & 0x20);
        dl->addWidget(ro); dl->addWidget(hi); dl->addWidget(sy); dl->addWidget(ar);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); dl->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() == QDialog::Accepted) {
            quint32 nv = (ro->isChecked()?1:0)|(hi->isChecked()?2:0)|(sy->isChecked()?4:0)|(ar->isChecked()?0x20:0);
            fmlog->appendPlainText(bk::fsAttrSet(path, nv));
        }
    });
    connect(del, &QPushButton::clicked, page, [=]() {
        QString path = selected();
        if (path.isEmpty()) { QMessageBox::warning(page, "提示", "先选择文件"); return; }
        if (QMessageBox::question(page, "确认删除", "删除 " + path + "（目录将递归删除）？") != QMessageBox::Yes) return;
        QFileInfo fi(path);
        bool ok = fi.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        fmlog->appendPlainText(ok ? "deleted: " + path : "delete failed (只读/权限/NTFS 占用)");
    });
    connect(ren, &QPushButton::clicked, page, [=]() {
        QString path = selected();
        if (path.isEmpty()) { QMessageBox::warning(page, "提示", "先选择文件"); return; }
        bool ok = false;
        QString nn = QInputDialog::getText(page, "重命名", "新名称", QLineEdit::Normal, QFileInfo(path).fileName(), &ok);
        if (!ok || nn.trimmed().isEmpty()) return;
        QString target = QFileInfo(path).dir().filePath(nn.trimmed());
        fmlog->appendPlainText(QDir().rename(path, target) ? "renamed: " + target : "rename failed");
    });
    connect(acl, &QPushButton::clicked, page, [=]() {
        QString path = selected();
        if (path.isEmpty()) { QMessageBox::warning(page, "提示", "先选择文件"); return; }
        QString hex = bk::fsAclGet(path);
        QDialog d(page); d.setWindowTitle(QStringLiteral("Windows ACL (安全描述符 hex)"));
        auto *dl = new QVBoxLayout(&d);
        auto *te = new QPlainTextEdit(hex); dl->addWidget(te);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); dl->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() == QDialog::Accepted)
            fmlog->appendPlainText(bk::fsAclSet(path, te->toPlainText().trimmed()));
    });
    return page;
}

QWidget *MainWindow::buildDiskPage()
{
    auto *page = new QWidget; auto *lay = new QVBoxLayout(page);
    auto *h = new QLabel(QStringLiteral("磁盘管理 (ioctl 原生)")); h->setFont(titleFont()); lay->addWidget(h);
    auto *log = makeLog();
    auto *refresh = new QPushButton(QStringLiteral("刷新磁盘列表")); lay->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, page, [=]() { log->appendPlainText(bk::diskList()); });

    auto *pform = new QFormLayout;
    auto *pdisk = new QComboBox; pdisk->setEditable(true);
    pdisk->addItem(QStringLiteral("— 选择磁盘 —"));
    for (const QString &d : bk::blockDevices()) pdisk->addItem(d);
    auto *pnum = new QSpinBox; pnum->setMinimum(1);
    auto *pstart = new QSpinBox; pstart->setMaximum(2000000000); pstart->setValue(2048);
    auto *pend = new QSpinBox; pend->setMaximum(2000000000); pend->setValue(2097151);
    auto *pbtn = new QPushButton(QStringLiteral("创建 GPT 分区(扇区精确)"));
    pform->addRow(QStringLiteral("磁盘"), pdisk); pform->addRow(QStringLiteral("分区号"), pnum);
    pform->addRow(QStringLiteral("起始扇区"), pstart); pform->addRow(QStringLiteral("结束扇区"), pend);
    pform->addRow(pbtn); lay->addLayout(pform);
    connect(pbtn, &QPushButton::clicked, page, [=]() {
        QString d = pdisk->currentText(); int n = pnum->value(); quint64 s = pstart->value(), e = pend->value();
        if (d.startsWith(QStringLiteral("—")) || d.isEmpty()) { post(log, QStringLiteral("先选择目标磁盘")); return; }
        if (QMessageBox::question(page, QStringLiteral("重建 GPT"),
              QStringLiteral("将在 %1 新建 GPT 并创建分区 %2（扇区 %3-%4），现有分区表会被覆盖，确认？").arg(d).arg(n).arg(s).arg(e)) != QMessageBox::Yes) return;
        async(log, [=]() { return bk::diskPartition(d, n, s, e); });
    });

    auto *erow = new QHBoxLayout;
    auto *edev = new QComboBox; edev->setEditable(true);
    edev->addItem(QStringLiteral("— 选择擦除目标 —"));
    for (const QString &d : bk::blockDevices()) edev->addItem(d);
    auto *emethod = new QComboBox; emethod->addItems({"NVMe block-erase", "NVMe crypto-erase", "HDD 填零(BLKZEROOUT)", "SSD TRIM(BLKDISCARD)"});
    auto *ebtn = new QPushButton(QStringLiteral("执行擦除"));
    erow->addWidget(edev); erow->addWidget(emethod); erow->addWidget(ebtn); lay->addLayout(erow);
    lay->addWidget(log, 1);
    connect(ebtn, &QPushButton::clicked, page, [=]() {
        QString d = edev->currentText();
        if (d.startsWith(QStringLiteral("—")) || d.isEmpty()) { post(log, QStringLiteral("先选择擦除目标设备")); return; }
        if (QMessageBox::question(page, QStringLiteral("危险"),
              QStringLiteral("将擦除 %1，数据不可恢复，确认？").arg(d)) != QMessageBox::Yes) return;
        int m = emethod->currentIndex();
        async(log, [=]() {
            switch (m) {
            case 0: return bk::nvmeSanitize(d, 2);
            case 1: return bk::nvmeSanitize(d, 4);
            case 2: return bk::diskZero(d);
            default: return bk::diskTrim(d);
            }
        });
    });
    return page;
}

QWidget *MainWindow::buildBcdPage()
{
    auto *page = new QWidget; auto *lay = new QVBoxLayout(page);
    auto *h = new QLabel(QStringLiteral("Windows BCD 引导 (libhivex 原生读写)")); h->setFont(titleFont()); lay->addWidget(h);
    auto *file = new QLineEdit; file->setPlaceholderText(QStringLiteral("BCD 文件路径"));
    file->setText(QStringLiteral("/mnt/win/Windows/System32/config/BCD"));
    auto *findBtn = new QPushButton(QStringLiteral("自动查找"));
    auto *browseBtn = new QPushButton(QStringLiteral("浏览…"));
    auto *frow = new QHBoxLayout; frow->addWidget(file); frow->addWidget(browseBtn); frow->addWidget(findBtn);
    auto *log = makeLog();
    connect(findBtn, &QPushButton::clicked, page, [=]() {
        const QStringList cands = {"/mnt/win/BCD", "/mnt/win/Windows/System32/config/BCD", "/mnt/win/Boot/BCD"};
        for (const QString &c : cands) {
            if (QFile::exists(c)) { file->setText(c); post(log, QString("found: ") + c); return; }
        }
        post(log, QStringLiteral("未找到 BCD（先挂载 Windows 分区）"));
    });
    auto *exp = new QPushButton(QStringLiteral("导出/读取"));
    auto *cre = new QPushButton(QStringLiteral("创建 BCD(已有 hive 基础上补对象)"));
    auto *root = new QLineEdit; root->setText(QStringLiteral("\\Windows"));
    auto *label = new QLineEdit; label->setText("Windows");
    auto *listBtn = new QPushButton(QStringLiteral("列出对象 / 元素"));

    auto *obj = new QComboBox; obj->setEditable(true); obj->setInsertPolicy(QComboBox::NoInsert);
    obj->lineEdit()->setPlaceholderText(QStringLiteral("对象 GUID，如 {9dea862c-5cdd-4e70-acc1-f32b344d4795}"));
    auto *elem = new QComboBox; elem->setEditable(true); elem->setInsertPolicy(QComboBox::NoInsert);
    elem->lineEdit()->setPlaceholderText(QStringLiteral("元素 ID，如 12000002"));
    auto *type = new QComboBox;
    type->addItem("REG_SZ", int(hive_t_REG_SZ));
    type->addItem("REG_BINARY", int(hive_t_REG_BINARY));
    type->addItem("REG_DWORD", int(hive_t_REG_DWORD));
    type->addItem("REG_MULTI_SZ", int(hive_t_REG_MULTI_SZ));
    auto *val = new QPlainTextEdit; val->setMaximumHeight(90);
    val->setPlaceholderText(QStringLiteral("元素值：字符串直接填；二进制填十六进制；DWORD 填十进制或 0x..."));

    auto *row1 = new QHBoxLayout; row1->addWidget(new QLabel("对象")); row1->addWidget(obj, 1);
    row1->addWidget(new QLabel("元素")); row1->addWidget(elem);
    row1->addWidget(type);
    auto *row2 = new QHBoxLayout;
    auto *getBtn = new QPushButton(QStringLiteral("读取元素"));
    auto *setBtn = new QPushButton(QStringLiteral("写入元素"));
    auto *delBtn = new QPushButton(QStringLiteral("删除元素"));
    row2->addWidget(getBtn); row2->addWidget(setBtn); row2->addWidget(delBtn);

    lay->addLayout(frow); lay->addWidget(exp); lay->addWidget(listBtn);
    lay->addWidget(root); lay->addWidget(label); lay->addWidget(cre);
    lay->addLayout(row1); lay->addWidget(val); lay->addLayout(row2);
    lay->addWidget(log, 1);

    auto refreshElements = [=]() {
        QString f = file->text().trimmed(), g = obj->currentText().trimmed();
        elem->clear();
        if (f.isEmpty() || g.isEmpty()) return;
        for (const QString &id : bk::bcdElementIds(f, g)) elem->addItem(id);
        if (elem->count()) elem->setCurrentIndex(0);
    };
    connect(browseBtn, &QPushButton::clicked, page, [=]() {
        QString f = QFileDialog::getOpenFileName(page, QStringLiteral("选择 BCD"), QStringLiteral("/mnt/win"), QStringLiteral("BCD (*.BCD);;所有文件 (*)"));
        if (!f.isEmpty()) file->setText(f);
    });
    connect(exp, &QPushButton::clicked, page, [=]() {
        QString f = file->text().trimmed(); async(log, [=]() { return bk::bcdExport(f); });
    });
    connect(listBtn, &QPushButton::clicked, page, [=]() {
        QString f = file->text().trimmed();
        QString r = bk::bcdList(f);
        post(log, r);
        QStringList gs;
        for (const QString &line : r.split('\n')) {
            QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^\\{[0-9a-fA-F-]+\\}")).match(line);
            if (m.hasMatch()) gs << m.captured(0);
        }
        QString cur = obj->currentText();
        obj->clear();
        obj->addItems(gs);
        if (!cur.isEmpty()) obj->setEditText(cur);
        else if (!gs.isEmpty()) obj->setCurrentIndex(0);
        refreshElements();
    });
    connect(obj, &QComboBox::currentTextChanged, page, [=]() { refreshElements(); });
    connect(cre, &QPushButton::clicked, page, [=]() {
        QString f = file->text().trimmed(), r = root->text(), l = label->text();
        async(log, [=]() { return bk::bcdCreate(f, l, r); });
    });
    connect(getBtn, &QPushButton::clicked, page, [=]() {
        QString f = file->text().trimmed(), g = obj->currentText().trimmed(), e = elem->currentText().trimmed();
        if (f.isEmpty() || g.isEmpty() || e.isEmpty()) { log->appendPlainText("文件 / GUID / 元素 ID 不能为空"); return; }
        QString r = bk::bcdGetElement(f, g, e);
        int t = -1;
        if (r.startsWith("t=")) {
            int nl = r.indexOf('\n');
            QString head = nl > 0 ? r.left(nl) : r;
            t = head.mid(2).toInt();
            if (nl > 0) r = r.mid(nl + 1);
            for (int i = 0; i < type->count(); i++)
                if (type->itemData(i).toInt() == t) { type->setCurrentIndex(i); break; }
        }
        val->setPlainText(r);
        log->appendPlainText(QString("type=%1\n%2").arg(t).arg(r));
    });
    connect(setBtn, &QPushButton::clicked, page, [=]() {
        QString f = file->text().trimmed(), g = obj->currentText().trimmed(), e = elem->currentText().trimmed();
        if (f.isEmpty() || g.isEmpty() || e.isEmpty()) { log->appendPlainText("文件 / GUID / 元素 ID 不能为空"); return; }
        int t = type->currentData().toInt();
        QString v = val->toPlainText();
        async(log, [=]() { return bk::bcdSetElement(f, g, e, t, v); });
    });
    connect(delBtn, &QPushButton::clicked, page, [=]() {
        QString f = file->text().trimmed(), g = obj->currentText().trimmed(), e = elem->currentText().trimmed();
        if (f.isEmpty() || g.isEmpty() || e.isEmpty()) { log->appendPlainText("文件 / GUID / 元素 ID 不能为空"); return; }
        if (QMessageBox::question(page, QStringLiteral("删除元素"), QStringLiteral("确认删除 %1 的 %2 元素？").arg(g, e)) != QMessageBox::Yes) return;
        async(log, [=]() { return bk::bcdDeleteElement(f, g, e); });
    });
    return page;
}

QWidget *MainWindow::buildUefiPage()
{
    auto *page = new QWidget; auto *lay = new QVBoxLayout(page);
    auto *h = new QLabel(QStringLiteral("UEFI 启动项 / 驱动项 (efivarfs 原生)")); h->setFont(titleFont()); lay->addWidget(h);
    auto *log = makeLog();
    auto *adesc = new QLineEdit; adesc->setPlaceholderText(QStringLiteral("新启动项描述"));
    auto *adev = new QLineEdit; adev->setPlaceholderText(QStringLiteral("EFI 设备路径 hex (如 01041400...)"));
    auto *add = new QPushButton(QStringLiteral("添加为启动项 Boot####"));
    auto *addDrv = new QPushButton(QStringLiteral("添加为驱动项 Driver####"));
    auto *esp = new QComboBox; esp->setEditable(true);
    for (const QString &d : bk::blockDevices()) esp->addItem(d);
    auto *efi = new QLineEdit; efi->setPlaceholderText(QStringLiteral("ESP 内路径，如 \\EFI\\Microsoft\\Boot\\bootmgfw.efi"));
    auto *gen = new QPushButton(QStringLiteral("生成设备路径"));
    auto *grow = new QHBoxLayout; grow->addWidget(new QLabel(QStringLiteral("ESP 分区"))); grow->addWidget(esp);
    grow->addWidget(new QLabel(QStringLiteral("EFI 文件"))); grow->addWidget(efi, 1); grow->addWidget(gen);
    lay->addLayout(grow);
    connect(gen, &QPushButton::clicked, page, [=]() {
        QString hex = bk::uefiDevicePath(esp->currentText(), efi->text());
        if (hex.startsWith(QStringLiteral("error:"))) { log->appendPlainText(hex); return; }
        adev->setText(hex);
        log->appendPlainText(QString("device path: %1 bytes").arg(hex.size() / 2));
    });
    auto *list = new QPushButton(QStringLiteral("刷新 Boot 列表"));
    auto *drv = new QPushButton(QStringLiteral("Driver 列表"));
    auto *dorder = new QPushButton(QStringLiteral("读 DriverOrder"));
    lay->addWidget(list); lay->addWidget(drv); lay->addWidget(dorder);
    connect(list, &QPushButton::clicked, page, [=]() { log->appendPlainText(bk::uefiList()); });
    connect(drv, &QPushButton::clicked, page, [=]() { log->appendPlainText(bk::uefiDrvList()); });
    connect(dorder, &QPushButton::clicked, page, [=]() { log->appendPlainText(bk::uefiDrvOrder()); });
    auto *drow = new QHBoxLayout;
    auto *dnum = new QLineEdit; dnum->setPlaceholderText(QStringLiteral("Boot0001 / Driver0001 或纯编号"));
    auto *del = new QPushButton(QStringLiteral("删除启动项"));
    auto *delDrv = new QPushButton(QStringLiteral("删除驱动项"));
    auto *order = new QLineEdit; order->setPlaceholderText(QStringLiteral("Order 如 0001,0000"));
    auto *setorder = new QPushButton(QStringLiteral("设置 BootOrder"));
    auto *setDrvOrder = new QPushButton(QStringLiteral("设置 DriverOrder"));
    drow->addWidget(dnum); drow->addWidget(del); drow->addWidget(delDrv);
    drow->addWidget(order, 1); drow->addWidget(setorder); drow->addWidget(setDrvOrder);
    lay->addLayout(drow);
    auto *arow = new QHBoxLayout;
    arow->addWidget(adesc); arow->addWidget(adev, 1); arow->addWidget(add); arow->addWidget(addDrv);
    lay->addLayout(arow); lay->addWidget(log, 1);
    connect(add, &QPushButton::clicked, page, [=]() {
        log->appendPlainText(bk::uefiAdd(adesc->text(), adev->text()));
        log->appendPlainText(bk::uefiList());
    });
    connect(addDrv, &QPushButton::clicked, page, [=]() {
        log->appendPlainText(bk::uefiAddDriver(adesc->text(), adev->text()));
        log->appendPlainText(bk::uefiDrvList());
    });
    connect(del, &QPushButton::clicked, page, [=]() {
        if (QMessageBox::question(page, QStringLiteral("删除启动项"), QStringLiteral("确认删除 UEFI 启动项？")) != QMessageBox::Yes) return;
        log->appendPlainText(bk::uefiDel(dnum->text()));
        log->appendPlainText(bk::uefiList());
    });
    connect(delDrv, &QPushButton::clicked, page, [=]() {
        if (QMessageBox::question(page, QStringLiteral("删除驱动项"), QStringLiteral("确认删除 UEFI 驱动项？")) != QMessageBox::Yes) return;
        log->appendPlainText(bk::uefiDelDriver(dnum->text()));
        log->appendPlainText(bk::uefiDrvList());
    });
    connect(setorder, &QPushButton::clicked, page, [=]() {
        log->appendPlainText(bk::uefiSetOrder(order->text()));
        log->appendPlainText(bk::uefiList());
    });
    connect(setDrvOrder, &QPushButton::clicked, page, [=]() {
        log->appendPlainText(bk::uefiSetDriverOrder(order->text()));
        log->appendPlainText(bk::uefiDrvList());
    });
    return page;
}

QWidget *MainWindow::buildPowerPage()
{
    auto *page = new QWidget; auto *lay = new QVBoxLayout(page);
    auto *h = new QLabel(QStringLiteral("电源操作 (reboot(2)/sysrq 原生)")); h->setFont(titleFont()); lay->addWidget(h);
    auto *reboot = new QPushButton(QStringLiteral("重启 (warm reboot)"));
    auto *cold = new QPushButton(QStringLiteral("冷重启 (cold reboot)"));
    auto *off = new QPushButton(QStringLiteral("关机 (poweroff)"));
    connect(reboot, &QPushButton::clicked, page, [=]() {
        if (QMessageBox::question(page, QStringLiteral("重启"), QStringLiteral("确认重启？未保存的更改会丢失。")) == QMessageBox::Yes) bk::powerReboot();
    });
    connect(cold, &QPushButton::clicked, page, [=]() {
        if (QMessageBox::question(page, QStringLiteral("冷重启"), QStringLiteral("确认冷重启？")) == QMessageBox::Yes) bk::powerCold();
    });
    connect(off, &QPushButton::clicked, page, [=]() {
        if (QMessageBox::question(page, QStringLiteral("关机"), QStringLiteral("确认关机？")) == QMessageBox::Yes) bk::powerOff();
    });
    lay->addWidget(reboot); lay->addWidget(cold); lay->addWidget(off); lay->addStretch();
    return page;
}

QWidget *MainWindow::buildAboutPage()
{
    auto *page = new QWidget; auto *lay = new QVBoxLayout(page);
    auto *info = new QPlainTextEdit; info->setReadOnly(true);
    info->setPlainText(QStringLiteral(
        "linuxbasepeforwin\n"
        "基于 Linux 内核的 USB 启动内存维护系统 (x86_64, BIOS+UEFI)\n"
        "Buildroot + Qt6 Widgets/C++，全局 HighDPI 适配\n\n"
        "全部功能采用原生内核 syscall/ioctl 与底层库 API：\n"
        "libwim / libhivex / BLKPG / BLKDISCARD / BLKZEROOUT /\n"
        "NVMe passthrough / efivarfs / mount(2) / xattr / reboot(2)\n\n"
        "NTFS 完整支持；ReFS 留空后续。构建/发布：GitHub Actions。"));
    const QVariant pi = qApp->property("pointerInfo");
    if (pi.isValid())
        info->appendPlainText(QStringLiteral("\n指针设备（原生 evdev）: ") + pi.toString());
    lay->addWidget(info);
    return page;
}