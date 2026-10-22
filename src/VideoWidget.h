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

protected:
    QPaintEngine* paintEngine() const override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void ensureDevice();
    void applyRound();
    void resizeSwapChain();

    struct Impl;
    std::unique_ptr<Impl> d;
};
