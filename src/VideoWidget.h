#pragma once

#include <QWidget>
#include <memory>

struct VideoFrame;

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    void present(const VideoFrame& frame);
    void clear();
    void releaseShared();
    void setAdapter(qint64 luidHigh, quint32 luidLow);

protected:
    QPaintEngine* paintEngine() const override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void ensureDevice();
    void releaseDevice();
    void applyRound();
    void resizeSwapChain();
    void drawFrame(const VideoFrame& frame, bool blockPresent);
    bool clientPixels(unsigned& width, unsigned& height) const;

    struct Impl;
    std::unique_ptr<Impl> d;
};
