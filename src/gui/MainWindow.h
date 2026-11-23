#pragma once

#include <QMainWindow>

class QTabWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    QWidget *buildWimPage();
    QWidget *buildFileManagerPage();
    QWidget *buildDiskPage();
    QWidget *buildBcdPage();
    QWidget *buildUefiPage();
    QWidget *buildBitLockerPage();
    QWidget *buildPowerPage();
    QWidget *buildAboutPage();

    QTabWidget *m_tabs = nullptr;
};