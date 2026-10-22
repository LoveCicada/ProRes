#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <vector>

struct VideoFrame {
    int width = 0;
    int height = 0;
    bool alpha = false;
    double pts = 0.0;
    std::vector<uint8_t> bgra;
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

    bool takeFrameForClock(VideoFrame& out);
    qint64 positionMs() const;
    qint64 durationMs() const;
    QString filePath() const;

signals:
    void statusChanged(const QString& text);
    void timingChanged(qint64 positionMs, qint64 durationMs);

private:
    void decodeLoop(const QString& path);

    struct Engine;
    Engine* m;
};
