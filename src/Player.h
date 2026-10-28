#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

#include "GpuAdapter.h"

enum class DecodePath {
    Software = 0,
    VulkanReadback = 1,
    VulkanZeroCopy = 2,
};

constexpr int kPathSoftware = 1 << static_cast<int>(DecodePath::Software);
constexpr int kPathReadback = 1 << static_cast<int>(DecodePath::VulkanReadback);
constexpr int kPathZeroCopy = 1 << static_cast<int>(DecodePath::VulkanZeroCopy);

struct VideoFrame {
    int width = 0;
    int height = 0;
    bool alpha = false;
    double pts = 0.0;
    std::vector<uint8_t> bgra;
    bool zeroCopy = false;
    quintptr sharedHandle = 0;
    std::shared_ptr<void> slot;
};

class Player : public QObject {
    Q_OBJECT
public:
    explicit Player(QObject* parent = nullptr);
    ~Player() override;

    void open(const QString& path);
    void play();
    void pause();
    void stop();
    void seekSeconds(double seconds);
    void seekRelative(double deltaSeconds);
    void setLoop(bool on);
    void setGpu(const GpuAdapter& adapter);
    void setDecodePath(int path);
    int decodePath() const;

    bool takeFrameForClock(VideoFrame& out);
    qint64 positionMs() const;
    qint64 durationMs() const;
    QString filePath() const;
    int decodePushed() const;
    int videoQueued() const;
    int videoQueueMax() const;
    QString videoPixelFormat() const;

signals:
    void statusChanged(const QString& text);
    void timingChanged(qint64 positionMs, qint64 durationMs);
    void decodeOptionsChanged(int mask, int path);
    void pixelFormatChanged(const QString& name);

private:
    void decodeLoop(const QString& path);

    struct Engine;
    Engine* m;
};
