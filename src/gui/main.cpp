#include <QApplication>
#include <QFont>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QPainter>
#include <QPolygon>
#include <QSocketNotifier>
#include <QMouseEvent>
#include <QDir>
#include <QFile>
#include <QByteArray>
#include <QVector>
#include <QPointer>
#include <QStringList>
#include "MainWindow.h"

#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// 原生指针输入：不依赖 Qt evdevmouse 通用插件（Buildroot Qt6 默认不编译该
// 插件），直接读 /dev/input/eventN，把 REL/ABS + BTN_LEFT 翻译成 Qt 鼠标事件。
// 这样 PS/2、USB 鼠标、USB 平板(绝对坐标) 全部覆盖，无需 udev。
// ---------------------------------------------------------------------------

static QVector<quint64> readCaps(const QString &file)
{
    QVector<quint64> words;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return words;
    const QStringList parts = QString::fromLatin1(f.readAll()).trimmed()
                                  .split(' ', Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        bool ok = false;
        quint64 v = p.toULongLong(&ok, 16);
        if (ok) words.append(v);
    }
    return words;
}

static bool capBit(const QVector<quint64> &words, int bit)
{
    if (words.isEmpty()) return false;
    const int idx = words.size() - 1 - bit / 64;
    if (idx < 0 || idx >= words.size()) return false;
    return (words[idx] >> (bit % 64)) & 1ULL;
}

struct PointerDev { QString path; bool absolute = false; };

static PointerDev findPointer()
{
    QDir dir(QStringLiteral("/sys/class/input"));
    PointerDev fallback, mouse;
    for (const QString &name : dir.entryList({QStringLiteral("event*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = QStringLiteral("/sys/class/input/") + name + QStringLiteral("/device/capabilities/");
        const QVector<quint64> rel = readCaps(base + QStringLiteral("rel"));
        const QVector<quint64> abs = readCaps(base + QStringLiteral("abs"));
        const QVector<quint64> key = readCaps(base + QStringLiteral("key"));
        PointerDev d; d.path = QStringLiteral("/dev/input/") + name;
        const bool hasRel = capBit(rel, REL_X) && capBit(rel, REL_Y);
        const bool hasAbs = capBit(abs, ABS_X) && capBit(abs, ABS_Y);
        if (hasRel) { mouse = d; mouse.absolute = false; break; }
        if (hasAbs && !key.isEmpty() && fallback.path.isEmpty()) {
            fallback = d; fallback.absolute = true;
        }
    }
    return mouse.path.isEmpty() ? fallback : mouse;
}

class PointerCursor : public QWidget
{
public:
    explicit PointerCursor(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFixedSize(18, 24);
        setObjectName(QStringLiteral("lbpePointer"));
        raise();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        QPolygon pts;
        pts << QPoint(1, 1) << QPoint(1, 19) << QPoint(6, 15) << QPoint(9, 23)
            << QPoint(12, 22) << QPoint(9, 14) << QPoint(15, 14);
        p.setPen(QPen(Qt::black, 2));
        p.setBrush(Qt::white);
        p.drawPolygon(pts);
    }
};

class EvdevPointer : public QObject
{
public:
    EvdevPointer(QWidget *window, const PointerDev &dev)
        : QObject(window), m_window(window), m_abs(dev.absolute), m_path(dev.path)
    {
        m_fd = ::open(dev.path.toLatin1().constData(), O_RDONLY | O_NONBLOCK);
        if (m_fd < 0) { m_info = QStringLiteral("open failed"); return; }

        if (m_abs) {
            input_absinfo ai{};
            if (::ioctl(m_fd, EVIOCGABS(ABS_X), &ai) == 0) {
                m_minX = ai.minimum; m_maxX = ai.maximum;
            }
            if (::ioctl(m_fd, EVIOCGABS(ABS_Y), &ai) == 0) {
                m_minY = ai.minimum; m_maxY = ai.maximum;
            }
        }
        const QRect g = QGuiApplication::primaryScreen()->geometry();
        m_pos = g.center();

        m_cursor = new PointerCursor(window);
        m_cursor->move(mapToWindow(m_pos));
        m_cursor->show();

        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        QObject::connect(m_notifier, &QSocketNotifier::activated, this, [this]() { drain(); });
        m_info = QStringLiteral("%1 %2 abs=%3").arg(m_path)
                     .arg(m_abs ? QStringLiteral("ABS") : QStringLiteral("REL"))
                     .arg(m_abs ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    QString info() const { return m_info; }

private:
    QPoint mapToWindow(const QPoint &global) const
    {
        return m_window->mapFromGlobal(global);
    }

    void applyRel(int dx, int dy)
    {
        double scale = 1.0;
        const QByteArray sf = qgetenv("QT_SCALE_FACTOR");
        if (!sf.isEmpty()) scale = sf.toDouble();
        if (scale <= 0) scale = 1.0;
        m_pos += QPoint(qRound(dx / scale), qRound(dy / scale));
        clamp();
    }

    void applyAbs(int x, int y)
    {
        const QRect g = QGuiApplication::primaryScreen()->geometry();
        const double fx = m_maxX > m_minX ? double(x - m_minX) / (m_maxX - m_minX) : 0.5;
        const double fy = m_maxY > m_minY ? double(y - m_minY) / (m_maxY - m_minY) : 0.5;
        m_pos = QPoint(g.left() + qBound(0, int(fx * (g.width() - 1)), g.width() - 1),
                       g.top() + qBound(0, int(fy * (g.height() - 1)), g.height() - 1));
    }

    void clamp()
    {
        const QRect g = QGuiApplication::primaryScreen()->geometry();
        m_pos.setX(qBound(g.left(), m_pos.x(), g.right() - 1));
        m_pos.setY(qBound(g.top(), m_pos.y(), g.bottom() - 1));
    }

    void moveCursor()
    {
        m_cursor->move(mapToWindow(m_pos));
        m_cursor->raise();
        QWidget *w = QApplication::widgetAt(m_pos);
        if (w && w != m_lastHover) {
            if (m_lastHover) { QEvent leave(QEvent::Leave); QApplication::sendEvent(m_lastHover, &leave); }
            m_lastHover = w;
            QEvent enter(QEvent::Enter);
            QApplication::sendEvent(w, &enter);
        }
        if (w) {
            QMouseEvent me(QEvent::MouseMove, QPointF(w->mapFromGlobal(m_pos)), QPointF(m_pos),
                           Qt::NoButton, Qt::NoButton, QGuiApplication::keyboardModifiers());
            QApplication::sendEvent(w, &me);
        }
    }

    void sendButton(bool down)
    {
        QWidget *w = down ? QApplication::widgetAt(m_pos) : m_target.data();
        if (!w) return;
        if (down) m_target = w;
        QMouseEvent me(down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                       QPointF(w->mapFromGlobal(m_pos)), QPointF(m_pos), Qt::LeftButton,
                       down ? Qt::LeftButton : Qt::NoButton, QGuiApplication::keyboardModifiers());
        QApplication::sendEvent(w, &me);
        if (down) w->setFocus(Qt::MouseFocusReason);
    }

    void drain()
    {
        input_event ev;
        while (::read(m_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) applyRel(ev.value, 0);
                else if (ev.code == REL_Y) applyRel(0, ev.value);
                else continue;
                moveCursor();
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_X) { m_absX = ev.value; applyAbs(m_absX, m_absY); }
                else if (ev.code == ABS_Y) { m_absY = ev.value; applyAbs(m_absX, m_absY); }
                else continue;
                moveCursor();
            } else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
                sendButton(ev.value != 0);
                moveCursor();
            }
        }
    }

    QWidget *m_window;
    PointerCursor *m_cursor = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    QPointer<QWidget> m_target;
    QPointer<QWidget> m_lastHover;
    int m_fd = -1;
    bool m_abs = false;
    QString m_path, m_info;
    QPoint m_pos;
    int m_absX = 0, m_absY = 0;
    int m_minX = 0, m_maxX = 0, m_minY = 0, m_maxY = 0;
};

int main(int argc, char *argv[])
{
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // HighDPI 兜底：按 EDID 物理尺寸 / 帧缓冲宽度选择缩放，2K/4K 不出现小字。
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        int fbW = 0;
        QFile fb("/sys/class/graphics/fb0/virtual_size");
        if (fb.open(QIODevice::ReadOnly))
            fbW = QString::fromLatin1(fb.readAll()).trimmed().section(',', 0, 0).toInt();
        double scale = 0;
        QDir drm("/sys/class/drm");
        for (const QString &card : drm.entryList({QStringLiteral("card*")}, QDir::Dirs)) {
            QFile f(drm.filePath(card + "/edid"));
            if (!f.open(QIODevice::ReadOnly)) continue;
            QByteArray edid = f.readAll();
            if (edid.size() < 128 || (unsigned char)edid[0] != 0x00 ||
                edid.mid(1, 6) != QByteArray("\xFF\xFF\xFF\xFF\xFF\xFF", 6))
                continue;
            int hcm = (unsigned char)edid[21];
            if (fbW > 0 && hcm >= 10) { scale = (fbW / (hcm / 2.54)) / 96.0; break; }
        }
        if (scale <= 0) {
            if (fbW >= 3840) scale = 2.0;
            else if (fbW >= 2560) scale = 1.5;
            else scale = 1.0;
        }
        scale = qBound(1.0, scale, 3.0);
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scale, 'f', 2));
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("linuxbasepeforwin");
    QApplication::setOrganizationName("whis7766");

    QFont base = app.font();
    base.setFamilies({QStringLiteral("WenQuanYi Zen Hei"),
                      QStringLiteral("Noto Sans CJK SC"),
                      QStringLiteral("DejaVu Sans")});
    base.setPointSize(14);
    app.setFont(base);

    MainWindow w;
    w.showMaximized();

    QStringList devDump;
    QDir idir(QStringLiteral("/sys/class/input"));
    for (const QString &name : idir.entryList({QStringLiteral("event*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = QStringLiteral("/sys/class/input/") + name + QStringLiteral("/device/capabilities/");
        auto hex = [](const QString &f) {
            QFile x(f);
            return x.open(QIODevice::ReadOnly) ? QString::fromLatin1(x.readAll()).trimmed() : QStringLiteral("-");
        };
        devDump << QStringLiteral("%1 rel=%2 abs=%3 key=%4").arg(name, hex(base + QStringLiteral("rel")),
                                                                  hex(base + QStringLiteral("abs")),
                                                                  hex(base + QStringLiteral("key")));
    }

    const PointerDev dev = findPointer();
    if (!dev.path.isEmpty()) {
        EvdevPointer *p = new EvdevPointer(&w, dev);
        app.setProperty("pointerInfo", p->info() + QStringLiteral(" | ") + devDump.join(QStringLiteral(" ; ")));
    } else {
        app.setProperty("pointerInfo", QStringLiteral("未找到指针设备: ") + devDump.join(QStringLiteral(" ; ")));
    }
    return app.exec();
}
