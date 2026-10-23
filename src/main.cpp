#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ProResPlayer"));
    QApplication::setApplicationName(QString::fromUtf8("\x50\x72\x6F\x52\x65\x73\x20\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8"));
    app.setStyleSheet(QStringLiteral(
        "QWidget { background: #14110e; color: #f4ecdf; "
        "font-family: 'Segoe UI', 'Microsoft YaHei'; font-size: 14px; }"
        "QPushButton { background: #2a241c; color: #f4ecdf; border: none; "
        "border-radius: 10px; padding: 8px 16px; min-height: 28px; }"
        "QPushButton:hover { background: #3a3228; }"
        "QPushButton:pressed { background: #e0a106; color: #1a140e; }"
        "QPushButton#play { background: #e0a106; color: #1a140e; }"
        "QPushButton#loop:checked { background: #e0a106; color: #1a140e; }"
        "QComboBox { background: #2a241c; color: #f4ecdf; border: none; "
        "border-radius: 8px; padding: 6px 10px; min-height: 28px; }"
        "QComboBox QAbstractItemView { background: #1c1914; color: #f4ecdf; "
        "selection-background-color: #e0a106; selection-color: #1a140e; }"
        "QLabel#title { font-size: 20px; }"
        "QLabel#status, QLabel#time { color: #c9bba6; }"
        "QSlider::groove:horizontal { height: 6px; background: #3a3228; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 14px; margin: -5px 0; "
        "background: #e0a106; border-radius: 7px; }"
        "QPlainTextEdit { background: #1c1914; color: #f4ecdf; border: none; "
        "border-radius: 12px; padding: 12px; }"
        "QListWidget { background: #1c1914; color: #f4ecdf; border: none; "
        "border-radius: 12px; padding: 6px; }"
        "QListWidget::item { padding: 6px; border-radius: 8px; }"
        "QListWidget::item:selected { background: #e0a106; color: #1a140e; }"
    ));

    MainWindow window;
    window.resize(1100, 720);
    window.show();
    return app.exec();
}
