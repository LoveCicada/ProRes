#pragma once

#include <QElapsedTimer>
#include <QMainWindow>

class Player;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;
class VideoWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void openPath(const QString& path);
    void openFileDialog();
    void openFolderDialog();
    void showInfo();
    void showSettings();
    void onTick();
    void play();
    void pause();
    void stopPlayback();
    void seekBack();
    void seekForward();
    void toggleLoop(bool on);
    void refreshStatsLine(int decodeFps, int renderFps);

    Player* m_player = nullptr;
    VideoWidget* m_video = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_stats = nullptr;
    QLabel* m_time = nullptr;
    QComboBox* m_decode = nullptr;
    bool m_updatingDecode = false;
    QSlider* m_slider = nullptr;
    QTimer* m_timer = nullptr;
    bool m_scrubbing = false;
    bool m_updatingSlider = false;
    QElapsedTimer m_statsClock;
    int m_renderFrames = 0;
    int m_lastDecodePushed = 0;
    bool m_statsReady = false;
    QString m_pixelFormat;
};
