#include "MainWindow.h"

#include "MediaProbe.h"
#include "Player.h"
#include "VideoWidget.h"

#include <QCloseEvent>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QDir>
#include <QUrl>

namespace {

bool isVideoFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("mov") || ext == QLatin1String("mxf")
        || ext == QLatin1String("mp4") || ext == QLatin1String("mkv")
        || ext == QLatin1String("m4v");
}

QString formatTime(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(s / 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QString::fromUtf8("\x50\x72\x6F\x52\x65\x73\x20\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8"));
    setAcceptDrops(true);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("root"));
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(12);

    m_title = new QLabel(QString::fromUtf8("\xE6\x8A\x8A\x20\x50\x72\x6F\x52\x65\x73\x20\xE6\x8B\x96\xE5\x88\xB0\xE8\xBF\x99\xE9\x87\x8C"), root);
    m_title->setObjectName(QStringLiteral("title"));
    m_status = new QLabel(QString::fromUtf8("\xE5\xB0\x9A\xE6\x9C\xAA\xE6\x89\x93\xE5\xBC\x80"), root);
    m_status->setObjectName(QStringLiteral("status"));

    m_video = new VideoWidget(root);

    m_slider = new QSlider(Qt::Horizontal, root);
    m_slider->setRange(0, 0);
    m_time = new QLabel(QStringLiteral("00:00 / 00:00"), root);
    m_time->setObjectName(QStringLiteral("time"));

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    auto add = [&](const QString& text, void (MainWindow::*method)(), const char* name = nullptr) {
        auto* button = new QPushButton(text, root);
        if (name)
            button->setObjectName(QLatin1String(name));
        connect(button, &QPushButton::clicked, this, [this, method](bool) { (this->*method)(); });
        buttons->addWidget(button);
    };
    add(QString::fromUtf8("\xE6\x89\x93\xE5\xBC\x80\xE6\x96\x87\xE4\xBB\xB6"), &MainWindow::openFileDialog);
    add(QString::fromUtf8("\xE6\x89\x93\xE5\xBC\x80\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9"), &MainWindow::openFolderDialog);
    add(QString::fromUtf8("\xE6\x96\x87\xE4\xBB\xB6\xE4\xBF\xA1\xE6\x81\xAF"), &MainWindow::showInfo);
    buttons->addStretch(1);
    add(QString::fromUtf8("\xE5\xBF\xAB\xE9\x80\x80"), &MainWindow::seekBack);
    add(QString::fromUtf8("\xE6\x92\xAD\xE6\x94\xBE"), &MainWindow::play, "play");
    add(QString::fromUtf8("\xE6\x9A\x82\xE5\x81\x9C"), &MainWindow::pause);
    add(QString::fromUtf8("\xE5\x81\x9C\xE6\xAD\xA2"), &MainWindow::stopPlayback);
    add(QString::fromUtf8("\xE5\xBF\xAB\xE8\xBF\x9B"), &MainWindow::seekForward);
    auto* loopButton = new QPushButton(QString::fromUtf8("\xE5\xBE\xAA\xE7\x8E\xAF"), root);
    loopButton->setObjectName(QStringLiteral("loop"));
    loopButton->setCheckable(true);
    connect(loopButton, &QPushButton::toggled, this, &MainWindow::toggleLoop);
    buttons->addWidget(loopButton);

    layout->addWidget(m_title);
    layout->addWidget(m_status);
    layout->addWidget(m_video, 1);
    layout->addWidget(m_slider);
    layout->addWidget(m_time);
    layout->addLayout(buttons);
    setCentralWidget(root);

    m_player = new Player(this);
    connect(m_player, &Player::statusChanged, m_status, &QLabel::setText);
    connect(m_player, &Player::timingChanged, this, [this](qint64 pos, qint64 dur) {
        m_time->setText(formatTime(pos) + QStringLiteral(" / ") + formatTime(dur));
        if (!m_scrubbing) {
            m_slider->setRange(0, static_cast<int>(qMax<qint64>(0, dur)));
            m_slider->setValue(static_cast<int>(qMax<qint64>(0, pos)));
        }
    });
    connect(m_slider, &QSlider::sliderPressed, this, [this] { m_scrubbing = true; });
    connect(m_slider, &QSlider::sliderReleased, this, [this] {
        m_scrubbing = false;
        m_player->seekSeconds(m_slider->value() / 1000.0);
    });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::onTick);
    m_timer->start(15);
}

MainWindow::~MainWindow()
{
    m_timer->stop();
    m_player->stop();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isVideoFile(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isVideoFile(url.toLocalFile())) {
            openPath(url.toLocalFile());
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_timer->stop();
    m_player->stop();
    QMainWindow::closeEvent(event);
}

void MainWindow::openPath(const QString& path)
{
    m_title->setText(QFileInfo(path).fileName());
    m_player->open(path);
}

void MainWindow::openFileDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("\xE6\x89\x93\xE5\xBC\x80\xE8\xA7\x86\xE9\xA2\x91"), QString(),
        QString::fromUtf8("\xE8\xA7\x86\xE9\xA2\x91\x20\x28\x2A\x2E\x6D\x6F\x76\x20\x2A\x2E\x6D\x78\x66\x20\x2A\x2E\x6D\x70\x34\x20\x2A\x2E\x6D\x6B\x76\x20\x2A\x2E\x6D\x34\x76\x29\x3B\x3B\xE6\x89\x80\xE6\x9C\x89\xE6\x96\x87\xE4\xBB\xB6\x20\x28\x2A\x2E\x2A\x29"));
    if (!path.isEmpty())
        openPath(path);
}

void MainWindow::openFolderDialog()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QString::fromUtf8("\xE6\x89\x93\xE5\xBC\x80\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9"));
    if (dir.isEmpty())
        return;
    const QFileInfoList files = QDir(dir).entryInfoList(
        QStringList() << QStringLiteral("*.mov") << QStringLiteral("*.mxf")
                      << QStringLiteral("*.mp4") << QStringLiteral("*.mkv")
                      << QStringLiteral("*.m4v"),
        QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        m_status->setText(QString::fromUtf8("\xE8\xBF\x99\xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\xE9\x87\x8C\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8F\xAF\xE6\x92\xAD\xE6\x94\xBE\xE7\x9A\x84\xE8\xA7\x86\xE9\xA2\x91\xE3\x80\x82"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QString::fromUtf8("\xE9\x80\x89\xE6\x8B\xA9\xE7\xB4\xA0\xE6\x9D\x90"));
    dialog.resize(520, 360);
    auto* box = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    for (const QFileInfo& info : files)
        list->addItem(info.fileName());
    box->addWidget(list);
    QObject::connect(list, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem* item) {
        dialog.done(list->row(item) + 1);
    });
    const int chosen = dialog.exec();
    if (chosen > 0)
        openPath(files.at(chosen - 1).absoluteFilePath());
}

void MainWindow::showInfo()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QString::fromUtf8("\xE6\x96\x87\xE4\xBB\xB6\xE4\xBF\xA1\xE6\x81\xAF"));
    dialog.setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                          | Qt::WindowCloseButtonHint | Qt::WindowMinMaxButtonsHint);
    dialog.setSizeGripEnabled(true);
    dialog.resize(1000, 780);
    dialog.setMinimumSize(720, 480);
    auto* box = new QVBoxLayout(&dialog);
    auto* text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    text->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { font-family: 'Microsoft YaHei'; font-size: 16px; }"));
    text->setPlainText(MediaProbe::inform(m_player->filePath()));
    box->addWidget(text);
    dialog.exec();
}

void MainWindow::onTick()
{
    VideoFrame frame;
    if (m_player->takeFrameForClock(frame))
        m_video->present(frame);
}

void MainWindow::play()
{
    m_player->play();
}

void MainWindow::pause()
{
    m_player->pause();
}

void MainWindow::stopPlayback()
{
    m_player->stop();
    m_video->clear();
}

void MainWindow::seekBack()
{
    m_player->seekRelative(-5.0);
}

void MainWindow::seekForward()
{
    m_player->seekRelative(5.0);
}

void MainWindow::toggleLoop(bool on)
{
    m_player->setLoop(on);
}
