#include "MainWindow.h"

#include "GpuAdapter.h"
#include "MediaProbe.h"
#include "Player.h"
#include "VideoWidget.h"

#include <QCloseEvent>
#include <QComboBox>
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
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QDir>
#include <QHash>
#include <QUrl>
#include <QVector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>

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

class SeekSlider : public QSlider {
public:
    explicit SeekSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QSlider::mousePressEvent(event);
            return;
        }
        setSliderDown(true);
        setValue(valueFromPos(event->pos()));
        emit sliderPressed();
        emit sliderMoved(value());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!isSliderDown()) {
            QSlider::mouseMoveEvent(event);
            return;
        }
        setValue(valueFromPos(event->pos()));
        emit sliderMoved(value());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (!isSliderDown() || event->button() != Qt::LeftButton) {
            QSlider::mouseReleaseEvent(event);
            return;
        }
        setValue(valueFromPos(event->pos()));
        setSliderDown(false);
        emit sliderReleased();
        event->accept();
    }

private:
    int valueFromPos(const QPoint& pos) const
    {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        const int span = groove.width() - handle.width();
        if (!groove.isValid() || span <= 0)
            return value();
        const int x = qBound(0, pos.x() - groove.x() - handle.width() / 2, span);
        return QStyle::sliderValueFromPosition(minimum(), maximum(), x, span, opt.upsideDown);
    }
};

QVector<GpuAdapter> listAdapters()
{
    QVector<GpuAdapter> adapters;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return adapters;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            GpuAdapter gpu;
            gpu.luidHigh = desc.AdapterLuid.HighPart;
            gpu.luidLow = desc.AdapterLuid.LowPart;
            gpu.name = QString::fromWCharArray(desc.Description);
            gpu.valid = true;
            adapters.push_back(gpu);
        }
        adapter->Release();
    }
    factory->Release();
    QHash<QString, int> copies;
    for (const GpuAdapter& gpu : adapters)
        copies[gpu.name] = copies.value(gpu.name) + 1;
    QHash<QString, int> seen;
    for (GpuAdapter& gpu : adapters) {
        if (copies.value(gpu.name) < 2)
            continue;
        const int number = seen.value(gpu.name) + 1;
        seen[gpu.name] = number;
        gpu.name += QStringLiteral(" #") + QString::number(number);
    }
    return adapters;
}

GpuAdapter savedAdapter()
{
    const QVector<GpuAdapter> adapters = listAdapters();
    if (adapters.isEmpty())
        return {};
    QSettings settings;
    const bool hasNewKey = settings.contains(QStringLiteral("gpuAuto"));
    const bool automatic = hasNewKey
        ? settings.value(QStringLiteral("gpuAuto"), true).toBool()
        : settings.value(QStringLiteral("gpuIndex"), -1).toInt() < 0;
    if (automatic)
        return adapters.first();
    const qint64 high = settings.value(QStringLiteral("gpuLuidHigh")).toLongLong();
    const quint32 low = settings.value(QStringLiteral("gpuLuidLow")).toUInt();
    for (const GpuAdapter& gpu : adapters) {
        if (gpu.luidHigh == high && gpu.luidLow == low)
            return gpu;
    }
    if (!hasNewKey) {
        const int index = settings.value(QStringLiteral("gpuIndex"), 0).toInt();
        if (index >= 0 && index < adapters.size())
            return adapters.at(index);
    }
    return adapters.first();
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

    m_slider = new SeekSlider(root);
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
    add(QString::fromUtf8("\xE8\xAE\xBE\xE7\xBD\xAE"), &MainWindow::showSettings);
    m_decode = new QComboBox(root);
    m_decode->setObjectName(QStringLiteral("decode"));
    m_decode->setEnabled(false);
    buttons->addWidget(m_decode);
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

    m_stats = new QLabel(root);
    m_stats->setObjectName(QStringLiteral("status"));
    layout->addWidget(m_title);
    layout->addWidget(m_status);
    layout->addWidget(m_stats);
    layout->addWidget(m_video, 1);
    layout->addWidget(m_slider);
    layout->addWidget(m_time);
    layout->addLayout(buttons);
    setCentralWidget(root);

    m_player = new Player(this);
    m_stats->setText(QString::fromUtf8(
                         "\xE8\xA7\xA3\xE7\xA0\x81 0 fps    "
                         "\xE9\x98\x9F\xE5\x88\x97 0/%1    "
                         "\xE6\xB8\xB2\xE6\x9F\x93 0 fps")
                         .arg(m_player->videoQueueMax()));
    const GpuAdapter startupGpu = savedAdapter();
    m_player->setGpu(startupGpu);
    m_video->setAdapter(startupGpu.luidHigh, startupGpu.luidLow);
    connect(m_player, &Player::statusChanged, m_status, &QLabel::setText);
    connect(m_player, &Player::decodeOptionsChanged, this, [this](int mask, int path) {
        m_updatingDecode = true;
        m_decode->clear();
        if (mask & kPathSoftware)
            m_decode->addItem(QString::fromUtf8("\xE8\xBD\xAF\xE8\xA7\xA3"), static_cast<int>(DecodePath::Software));
        if (mask & kPathReadback)
            m_decode->addItem(QString::fromUtf8("Vulkan \xE5\x9B\x9E\xE8\xAF\xBB"), static_cast<int>(DecodePath::VulkanReadback));
        if (mask & kPathZeroCopy)
            m_decode->addItem(QString::fromUtf8("Vulkan \xE9\x9B\xB6\xE6\x8B\xB7\xE8\xB4\x9D"), static_cast<int>(DecodePath::VulkanZeroCopy));
        const int row = m_decode->findData(path);
        if (row >= 0)
            m_decode->setCurrentIndex(row);
        m_decode->setEnabled(m_decode->count() > 1);
        m_updatingDecode = false;
    });
    connect(m_decode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updatingDecode || m_decode->currentIndex() < 0)
            return;
        m_player->setDecodePath(m_decode->currentData().toInt());
    });
    connect(m_player, &Player::timingChanged, this, [this](qint64 pos, qint64 dur) {
        m_time->setText(formatTime(pos) + QStringLiteral(" / ") + formatTime(dur));
        if (!m_scrubbing) {
            const int maximum = static_cast<int>(qMax<qint64>(0, dur));
            m_updatingSlider = true;
            m_slider->setRange(0, maximum);
            m_slider->setPageStep(qMax(1000, maximum / 20));
            m_slider->setValue(static_cast<int>(qMax<qint64>(0, pos)));
            m_updatingSlider = false;
        }
    });
    connect(m_slider, &QSlider::sliderPressed, this, [this] { m_scrubbing = true; });
    connect(m_slider, &QSlider::sliderMoved, this, [this](int value) {
        m_scrubbing = true;
        m_time->setText(formatTime(value) + QStringLiteral(" / ") + formatTime(m_slider->maximum()));
    });
    connect(m_slider, &QSlider::sliderReleased, this, [this] {
        m_scrubbing = false;
        m_player->seekSeconds(m_slider->value() / 1000.0);
    });
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updatingSlider || m_slider->isSliderDown())
            return;
        m_time->setText(formatTime(value) + QStringLiteral(" / ") + formatTime(m_slider->maximum()));
        m_player->seekSeconds(value / 1000.0);
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
    m_video->releaseShared();
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

void MainWindow::showSettings()
{
    QSettings settings;
    const bool savedAuto = settings.contains(QStringLiteral("gpuAuto"))
        ? settings.value(QStringLiteral("gpuAuto"), true).toBool()
        : settings.value(QStringLiteral("gpuIndex"), -1).toInt() < 0;
    const qint64 savedHigh = settings.value(QStringLiteral("gpuLuidHigh")).toLongLong();
    const quint32 savedLow = settings.value(QStringLiteral("gpuLuidLow")).toUInt();

    QDialog dialog(this);
    dialog.setWindowTitle(QString::fromUtf8("\xE8\xAE\xBE\xE7\xBD\xAE"));
    dialog.setMinimumWidth(460);
    auto* box = new QVBoxLayout(&dialog);
    auto* label = new QLabel(QString::fromUtf8("\xE6\x98\xBE\xE5\x8D\xA1"), &dialog);
    auto* combo = new QComboBox(&dialog);
    combo->addItem(QString::fromUtf8("\xE8\x87\xAA\xE5\x8A\xA8"), QStringLiteral("auto"));

    const QVector<GpuAdapter> adapters = listAdapters();
    for (const GpuAdapter& gpu : adapters) {
        const QString key = QString::number(gpu.luidHigh) + QLatin1Char(':') + QString::number(gpu.luidLow);
        combo->addItem(gpu.name, key);
    }

    int row = 0;
    if (!savedAuto) {
        const QString key = QString::number(savedHigh) + QLatin1Char(':') + QString::number(savedLow);
        const int found = combo->findData(key);
        if (found >= 0)
            row = found;
    }
    combo->setCurrentIndex(row);
    auto* ok = new QPushButton(QString::fromUtf8("\xE7\xA1\xAE\xE5\xAE\x9A"), &dialog);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    box->addWidget(label);
    box->addWidget(combo);
    box->addWidget(ok, 0, Qt::AlignRight);
    if (dialog.exec() != QDialog::Accepted || adapters.isEmpty())
        return;

    const bool automatic = combo->currentData().toString() == QLatin1String("auto");
    const GpuAdapter chosen = automatic ? adapters.first() : [&] {
        const QString key = combo->currentData().toString();
        const int split = key.indexOf(QLatin1Char(':'));
        const qint64 high = key.left(split).toLongLong();
        const quint32 low = key.mid(split + 1).toUInt();
        for (const GpuAdapter& gpu : adapters) {
            if (gpu.luidHigh == high && gpu.luidLow == low)
                return gpu;
        }
        return adapters.first();
    }();

    settings.setValue(QStringLiteral("gpuAuto"), automatic);
    settings.setValue(QStringLiteral("gpuLuidHigh"), chosen.luidHigh);
    settings.setValue(QStringLiteral("gpuLuidLow"), chosen.luidLow);
    settings.setValue(QStringLiteral("gpuName"), chosen.name);
    m_player->setGpu(chosen);
    m_video->setAdapter(chosen.luidHigh, chosen.luidLow);
    if (!m_player->filePath().isEmpty())
        openPath(m_player->filePath());
}

void MainWindow::onTick()
{
    VideoFrame frame;
    if (m_player->takeFrameForClock(frame)) {
        m_video->present(frame);
        ++m_renderFrames;
    }
    if (!m_statsReady) {
        m_statsClock.start();
        m_lastDecodePushed = m_player->decodePushed();
        m_statsReady = true;
        return;
    }
    const qint64 elapsed = m_statsClock.elapsed();
    if (elapsed < 500)
        return;
    const double seconds = elapsed / 1000.0;
    const int decoded = m_player->decodePushed();
    const int decodeFps = seconds > 0.0 ? qRound((decoded - m_lastDecodePushed) / seconds) : 0;
    const int renderFps = seconds > 0.0 ? qRound(m_renderFrames / seconds) : 0;
    m_lastDecodePushed = decoded;
    m_renderFrames = 0;
    m_statsClock.restart();
    m_stats->setText(QString::fromUtf8(
                         "\xE8\xA7\xA3\xE7\xA0\x81 %1 fps    "
                         "\xE9\x98\x9F\xE5\x88\x97 %2/%3    "
                         "\xE6\xB8\xB2\xE6\x9F\x93 %4 fps")
                         .arg(decodeFps)
                         .arg(m_player->videoQueued())
                         .arg(m_player->videoQueueMax())
                         .arg(renderFps));
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
    m_video->releaseShared();
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
