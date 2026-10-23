#include "Player.h"

#include <QAudioFormat>
#include <QAudioOutput>
#include <QIODevice>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kMaxFrames = 8;

struct PcmChunk {
    double pts = 0.0;
    std::vector<uint8_t> bytes;
};

struct FormatChoice {
    bool triedVulkan = false;
};

AVPixelFormat softwareFormat(const AVPixelFormat* formats)
{
    for (const AVPixelFormat* fmt = formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
        if (*fmt != AV_PIX_FMT_VULKAN)
            return *fmt;
    }
    return formats[0];
}

AVPixelFormat getVulkanFormat(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* fmt = formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
        if (*fmt == AV_PIX_FMT_VULKAN)
            return *fmt;
    }
    return softwareFormat(formats);
}

AVPixelFormat probeGetFormat(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    auto* choice = static_cast<FormatChoice*>(ctx->opaque);
    if (choice && !choice->triedVulkan) {
        choice->triedVulkan = true;
        for (const AVPixelFormat* fmt = formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
            if (*fmt == AV_PIX_FMT_VULKAN)
                return *fmt;
        }
    }
    return softwareFormat(formats);
}

double stampSeconds(int64_t ts, AVRational timeBase, double fallback)
{
    if (ts == AV_NOPTS_VALUE)
        return fallback;
    return ts * av_q2d(timeBase);
}

int vulkanDeviceIndex(qint64 luidHigh, quint32 luidLow)
{
    const HMODULE library = LoadLibraryW(L"vulkan-1.dll");
    if (!library)
        return -1;

    using FnGetProc = void* (__stdcall*)(void*, const char*);
    using FnCreate = int (__stdcall*)(const void*, const void*, void**);
    using FnDestroy = void (__stdcall*)(void*, const void*);
    using FnEnum = int (__stdcall*)(void*, uint32_t*, void**);
    using FnProps = void (__stdcall*)(void*, void*);

    auto getProc = reinterpret_cast<FnGetProc>(GetProcAddress(library, "vkGetInstanceProcAddr"));
    if (!getProc) {
        FreeLibrary(library);
        return -1;
    }
    auto createInstance = reinterpret_cast<FnCreate>(getProc(nullptr, "vkCreateInstance"));
    if (!createInstance) {
        FreeLibrary(library);
        return -1;
    }

    struct AppInfo {
        uint32_t sType;
        const void* pNext;
        const char* applicationName;
        uint32_t applicationVersion;
        const char* engineName;
        uint32_t engineVersion;
        uint32_t apiVersion;
    };
    struct InstanceInfo {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        const AppInfo* applicationInfo;
        uint32_t enabledLayerCount;
        const char* const* enabledLayerNames;
        uint32_t enabledExtensionCount;
        const char* const* enabledExtensionNames;
    };
    AppInfo app = {};
    app.sType = 0;
    app.apiVersion = (1u << 22) | (3u << 12);
    InstanceInfo instanceInfo = {};
    instanceInfo.sType = 1;
    instanceInfo.applicationInfo = &app;

    void* instance = nullptr;
    if (createInstance(&instanceInfo, nullptr, &instance) != 0 || !instance) {
        FreeLibrary(library);
        return -1;
    }
    auto destroyInstance = reinterpret_cast<FnDestroy>(getProc(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<FnEnum>(getProc(instance, "vkEnumeratePhysicalDevices"));
    auto properties2 = reinterpret_cast<FnProps>(getProc(instance, "vkGetPhysicalDeviceProperties2"));
    int found = -1;
    if (enumerate && properties2) {
        uint32_t count = 0;
        enumerate(instance, &count, nullptr);
        std::vector<void*> devices(count);
        if (count > 0 && enumerate(instance, &count, devices.data()) == 0) {
            struct IdProps {
                uint32_t sType;
                void* pNext;
                uint8_t deviceUUID[16];
                uint8_t driverUUID[16];
                uint8_t deviceLUID[8];
                uint32_t deviceNodeMask;
                uint32_t deviceLUIDValid;
            };
            struct Props2 {
                uint32_t sType;
                void* pNext;
                unsigned char properties[2048];
            };
            uint8_t wanted[8] = {};
            const uint32_t low = luidLow;
            const int32_t high = static_cast<int32_t>(luidHigh);
            memcpy(wanted, &low, 4);
            memcpy(wanted + 4, &high, 4);
            for (uint32_t i = 0; i < count; ++i) {
                IdProps id = {};
                id.sType = 1000071004;
                Props2 props = {};
                props.sType = 1000059001;
                props.pNext = &id;
                properties2(devices[i], &props);
                if (id.deviceLUIDValid && memcmp(id.deviceLUID, wanted, 8) == 0) {
                    found = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    if (destroyInstance)
        destroyInstance(instance, nullptr);
    FreeLibrary(library);
    return found;
}

bool probeHardwareDecode(const char* path, int vulkanIndex)
{
    if (!path || vulkanIndex < 0)
        return false;
    const int previous = av_log_get_level();
    av_log_set_level(AV_LOG_FATAL);
    bool supported = false;

    AVFormatContext* format = nullptr;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hw = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    FormatChoice choice;

    const bool opened = avformat_open_input(&format, path, nullptr, nullptr) >= 0
        && avformat_find_stream_info(format, nullptr) >= 0;
    const int videoIndex = opened
        ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0)
        : -1;
    const bool prores = videoIndex >= 0
        && format->streams[videoIndex]->codecpar->codec_id == AV_CODEC_ID_PRORES;
    const std::string deviceName = std::to_string(vulkanIndex);
    const bool deviceOk = prores
        && av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_VULKAN, deviceName.c_str(), nullptr, 0) >= 0;
    const AVCodec* codec = deviceOk ? avcodec_find_decoder(AV_CODEC_ID_PRORES) : nullptr;
    if (codec)
        ctx = avcodec_alloc_context3(codec);
    const bool paramsOk = ctx && avcodec_parameters_to_context(ctx, format->streams[videoIndex]->codecpar) >= 0;
    if (paramsOk) {
        ctx->hw_device_ctx = av_buffer_ref(hw);
        ctx->opaque = &choice;
        ctx->get_format = probeGetFormat;
    }
    if (paramsOk && avcodec_open2(ctx, codec, nullptr) >= 0) {
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index != videoIndex) {
                av_packet_unref(packet);
                continue;
            }
            const int sent = avcodec_send_packet(ctx, packet);
            av_packet_unref(packet);
            if (sent < 0)
                break;
            const int received = avcodec_receive_frame(ctx, frame);
            if (received == 0) {
                supported = frame->format == AV_PIX_FMT_VULKAN;
                av_frame_unref(frame);
                break;
            }
            if (received != AVERROR(EAGAIN))
                break;
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    av_buffer_unref(&hw);
    avformat_close_input(&format);
    av_log_set_level(previous);
    return supported;
}

} // namespace

struct Player::Engine {
    QString path;
    std::thread thread;
    std::atomic<bool> stop { false };
    std::atomic<bool> pause { false };
    std::atomic<bool> eof { false };
    std::atomic<bool> running { false };
    std::atomic<bool> seekPending { false };
    std::atomic<bool> loop { false };
    std::atomic<qint64> luidHigh { 0 };
    std::atomic<quint32> luidLow { 0 };
    QString gpuName;
    std::mutex waitMutex;
    std::condition_variable waitCv;

    std::mutex seekMutex;
    double seekSec = 0.0;
    int seekId = 0;
    std::atomic<int> seekFinished { 0 };
    std::atomic<double> dropUntil { -1.0 };

    std::mutex videoMutex;
    std::condition_variable videoCv;
    std::deque<VideoFrame> video;
    std::mutex pcmMutex;
    std::condition_variable pcmCv;
    std::deque<PcmChunk> pcm;
    size_t pcmBytes = 0;

    std::atomic<double> duration { 0.0 };
    std::atomic<double> position { 0.0 };
    std::mutex statusMutex;
    QString status;
    QString shownStatus;

    QAudioOutput* audio = nullptr;
    QIODevice* audioDevice = nullptr;
    int sampleRate = 48000;
    int channels = 2;
    bool audioReady = false;
    bool audioBaseValid = false;
    double audioBase = 0.0;
    bool wallRunning = false;
    double wallAccum = 0.0;
    double wallOrigin = 0.0;
    std::chrono::steady_clock::time_point wallStart {};
    int appliedSeek = 0;

    void setStatus(const QString& text)
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        status = text;
    }

    void clearQueues()
    {
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            video.clear();
        }
        videoCv.notify_all();
        {
            std::lock_guard<std::mutex> lock(pcmMutex);
            pcm.clear();
            pcmBytes = 0;
        }
        pcmCv.notify_all();
    }

    void pushVideo(VideoFrame frame)
    {
        std::unique_lock<std::mutex> lock(videoMutex);
        videoCv.wait(lock, [&] {
            return stop.load() || seekPending.load() || pause.load() || video.size() < kMaxFrames;
        });
        if (stop.load() || seekPending.load() || pause.load())
            return;
        video.push_back(std::move(frame));
    }

    void pushPcm(PcmChunk chunk)
    {
        const size_t limit = static_cast<size_t>(sampleRate) * static_cast<size_t>(channels) * 4;
        std::unique_lock<std::mutex> lock(pcmMutex);
        pcmCv.wait(lock, [&] {
            return stop.load() || seekPending.load() || pause.load() || pcmBytes < limit;
        });
        if (stop.load() || seekPending.load() || pause.load())
            return;
        pcmBytes += chunk.bytes.size();
        pcm.push_back(std::move(chunk));
    }
};

Player::Player(QObject* parent)
    : QObject(parent)
    , m(new Engine)
{
}

Player::~Player()
{
    stop();
    delete m;
}

QString Player::filePath() const
{
    return m->path;
}

qint64 Player::positionMs() const
{
    return static_cast<qint64>(m->position.load() * 1000.0);
}

qint64 Player::durationMs() const
{
    return static_cast<qint64>(m->duration.load() * 1000.0);
}

void Player::open(const QString& path)
{
    stop();
    m->path = path;
    m->stop = false;
    m->pause = false;
    m->eof = false;
    m->seekPending = false;
    m->duration = 0.0;
    m->position = 0.0;
    m->appliedSeek = 0;
    m->seekFinished = 0;
    m->audioBaseValid = false;
    m->wallAccum = 0.0;
    m->wallOrigin = 0.0;
    m->wallRunning = true;
    m->wallStart = std::chrono::steady_clock::now();
    m->shownStatus.clear();
    m->setStatus(QString::fromUtf8("\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x89\x93\xE5\xBC\x80"));
    m->running = true;
    m->thread = std::thread([this, path] { decodeLoop(path); });
}

void Player::play()
{
    if (!m->running.load()) {
        if (!m->path.isEmpty())
            open(m->path);
        return;
    }
    if (m->eof.load())
        seekSeconds(0.0);
    m->pause = false;
    if (m->audio)
        m->audio->resume();
    if (!m->audioReady) {
        m->wallStart = std::chrono::steady_clock::now();
        m->wallRunning = true;
    }
    m->waitCv.notify_all();
    m->videoCv.notify_all();
    m->pcmCv.notify_all();
}

void Player::pause()
{
    if (!m->running.load() || m->pause.load())
        return;
    m->pause = true;
    if (m->audio)
        m->audio->suspend();
    if (m->wallRunning) {
        m->wallAccum += std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - m->wallStart)
                            .count();
        m->wallRunning = false;
    }
    m->waitCv.notify_all();
    m->videoCv.notify_all();
    m->pcmCv.notify_all();
}

void Player::stop()
{
    m->stop = true;
    m->pause = false;
    m->seekPending = false;
    m->waitCv.notify_all();
    m->videoCv.notify_all();
    m->pcmCv.notify_all();
    if (m->thread.joinable())
        m->thread.join();
    m->running = false;
    m->clearQueues();
    if (m->audio) {
        m->audio->stop();
        delete m->audio;
        m->audio = nullptr;
        m->audioDevice = nullptr;
    }
    m->audioReady = false;
    m->audioBaseValid = false;
    m->position = 0.0;
    m->eof = false;
    m->wallRunning = false;
    if (!m->path.isEmpty()) {
        m->setStatus(QString::fromUtf8("\xE5\xB7\xB2\xE5\x81\x9C\xE6\xAD\xA2"));
        m->shownStatus = QString::fromUtf8("\xE5\xB7\xB2\xE5\x81\x9C\xE6\xAD\xA2");
        emit statusChanged(m->shownStatus);
        emit timingChanged(0, durationMs());
    }
}

void Player::seekSeconds(double seconds)
{
    if (!m->running.load())
        return;
    if (seconds < 0.0)
        seconds = 0.0;
    const double dur = m->duration.load();
    if (dur > 0.0 && seconds > dur)
        seconds = dur;
    {
        std::lock_guard<std::mutex> lock(m->seekMutex);
        m->seekSec = seconds;
        ++m->seekId;
    }
    m->seekPending = true;
    m->eof = false;
    m->pause = false;
    m->position = seconds;
    m->dropUntil = seconds;
    m->audioBaseValid = false;
    m->wallOrigin = seconds;
    m->wallAccum = 0.0;
    m->wallStart = std::chrono::steady_clock::now();
    m->wallRunning = true;
    m->clearQueues();
    m->waitCv.notify_all();
    m->videoCv.notify_all();
    m->pcmCv.notify_all();
}

void Player::seekRelative(double deltaSeconds)
{
    seekSeconds(m->position.load() + deltaSeconds);
}

void Player::setLoop(bool on)
{
    m->loop = on;
    m->waitCv.notify_all();
    m->videoCv.notify_all();
    m->pcmCv.notify_all();
}

void Player::setGpu(const GpuAdapter& adapter)
{
    m->luidHigh = adapter.luidHigh;
    m->luidLow = adapter.luidLow;
    std::lock_guard<std::mutex> lock(m->statusMutex);
    m->gpuName = adapter.name;
}

bool Player::takeFrameForClock(VideoFrame& out)
{
    if (!m->running.load())
        return false;
    if (m->seekPending.load())
        return false;

    const int finished = m->seekFinished.load();
    if (finished != m->appliedSeek) {
        m->appliedSeek = finished;
        if (m->audio) {
            m->audio->reset();
            m->audioDevice = m->audio->start();
        }
        m->audioBaseValid = false;
        m->wallAccum = 0.0;
        m->wallOrigin = m->position.load();
        m->wallStart = std::chrono::steady_clock::now();
        m->wallRunning = !m->pause.load();
    }

    if (!m->audio) {
        int rate = 0;
        int channels = 0;
        {
            std::lock_guard<std::mutex> lock(m->pcmMutex);
            if (!m->pcm.empty()) {
                rate = m->sampleRate;
                channels = m->channels;
            }
        }
        if (rate > 0 && channels > 0) {
            QAudioFormat audioFormat;
            audioFormat.setSampleRate(rate);
            audioFormat.setChannelCount(channels);
            audioFormat.setSampleSize(16);
            audioFormat.setCodec(QStringLiteral("audio/pcm"));
            audioFormat.setByteOrder(QAudioFormat::LittleEndian);
            audioFormat.setSampleType(QAudioFormat::SignedInt);
            m->audio = new QAudioOutput(audioFormat, this);
            m->audioDevice = m->audio->start();
            m->audioReady = m->audioDevice != nullptr;
            if (m->pause.load() && m->audio)
                m->audio->suspend();
        }
    }

    if (m->audioDevice && m->audio) {
        const int freeBytes = m->audio->bytesFree();
        std::vector<char> pending;
        if (freeBytes > 0) {
            std::lock_guard<std::mutex> lock(m->pcmMutex);
            while (!m->pcm.empty() && static_cast<int>(pending.size()) < freeBytes) {
                PcmChunk& chunk = m->pcm.front();
                if (!m->audioBaseValid && pending.empty()) {
                    m->audioBase = chunk.pts;
                    m->audioBaseValid = true;
                }
                const int room = freeBytes - static_cast<int>(pending.size());
                const int n = std::min(room, static_cast<int>(chunk.bytes.size()));
                if (n <= 0)
                    break;
                pending.insert(pending.end(),
                               reinterpret_cast<const char*>(chunk.bytes.data()),
                               reinterpret_cast<const char*>(chunk.bytes.data()) + n);
                chunk.bytes.erase(chunk.bytes.begin(), chunk.bytes.begin() + n);
                m->pcmBytes -= static_cast<size_t>(n);
                if (chunk.bytes.empty())
                    m->pcm.pop_front();
            }
        }
        if (!pending.empty())
            m->audioDevice->write(pending.data(), static_cast<qint64>(pending.size()));
        m->pcmCv.notify_all();
    }

    double clock = m->wallOrigin + m->wallAccum;
    if (m->audioReady && m->audioBaseValid && m->audio)
        clock = m->audioBase + m->audio->processedUSecs() / 1000000.0;
    else if (m->wallRunning)
        clock += std::chrono::duration<double>(std::chrono::steady_clock::now() - m->wallStart).count();

    const double played = (m->audio && m->audioBaseValid) ? m->audio->processedUSecs() / 1000000.0 : 0.0;
    bool got = false;
    bool stalled = false;
    {
        std::lock_guard<std::mutex> lock(m->videoMutex);
        if (!m->video.empty()) {
            const double oldest = m->video.front().pts;
            stalled = oldest > clock + 0.3;
            if (stalled) {
                out = std::move(m->video.front());
                m->video.pop_front();
                got = true;
            } else {
                while (!m->video.empty() && m->video.front().pts <= clock + 0.05) {
                    out = std::move(m->video.front());
                    m->video.pop_front();
                    got = true;
                }
            }
        }
    }
    if (got && stalled) {
        if (m->audio && m->audioBaseValid)
            m->audioBase = out.pts - played;
        else {
            m->wallOrigin = out.pts;
            m->wallAccum = 0.0;
            m->wallStart = std::chrono::steady_clock::now();
        }
    }
    m->videoCv.notify_all();
    m->position = got ? out.pts : clock;
    emit timingChanged(positionMs(), durationMs());

    QString status;
    {
        std::lock_guard<std::mutex> lock(m->statusMutex);
        status = m->status;
    }
    if (status != m->shownStatus) {
        m->shownStatus = status;
        emit statusChanged(status);
    }
    return got;
}

void Player::decodeLoop(const QString& path)
{
    auto fail = [&](const QString& text) {
        m->setStatus(text);
        QMetaObject::invokeMethod(this, [this, text] {
            m->shownStatus = text;
            emit statusChanged(text);
        }, Qt::QueuedConnection);
        m->running = false;
    };

    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        fail(QString::fromUtf8("\xE6\x97\xA0\xE6\xB3\x95\xE6\x89\x93\xE5\xBC\x80\xE6\x96\x87\xE4\xBB\xB6"));
        return;
    }
    if (avformat_find_stream_info(format, nullptr) < 0) {
        avformat_close_input(&format);
        fail(QString::fromUtf8("\xE6\x97\xA0\xE6\xB3\x95\xE8\xAF\xBB\xE5\x8F\x96\xE6\xB5\x81\xE4\xBF\xA1\xE6\x81\xAF"));
        return;
    }

    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
    if (videoIndex < 0) {
        avformat_close_input(&format);
        fail(QString::fromUtf8("\xE6\xB2\xA1\xE6\x9C\x89\xE8\xA7\x86\xE9\xA2\x91\xE6\xB5\x81"));
        return;
    }

    const bool prores = format->streams[videoIndex]->codecpar->codec_id == AV_CODEC_ID_PRORES;
    const int vulkanIndex = prores ? vulkanDeviceIndex(m->luidHigh.load(), m->luidLow.load()) : -1;
    const QByteArray pathUtf8 = path.toUtf8();
    const bool useHardware = vulkanIndex >= 0 && probeHardwareDecode(pathUtf8.constData(), vulkanIndex);

    auto openDecoder = [&](int index, bool tryVulkan, AVCodecContext** out, AVBufferRef** hw) {
        *out = nullptr;
        *hw = nullptr;
        AVStream* stream = format->streams[index];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec)
            return false;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx || avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
        bool hwOn = false;
        if (tryVulkan) {
            const std::string deviceName = std::to_string(vulkanIndex);
            if (av_hwdevice_ctx_create(hw, AV_HWDEVICE_TYPE_VULKAN, deviceName.c_str(), nullptr, 0) == 0) {
                ctx->hw_device_ctx = av_buffer_ref(*hw);
                ctx->get_format = getVulkanFormat;
                hwOn = true;
            }
        }
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            if (!hwOn)
                return false;
            hwOn = false;
            ctx = avcodec_alloc_context3(codec);
            if (!ctx || avcodec_parameters_to_context(ctx, stream->codecpar) < 0
                || avcodec_open2(ctx, codec, nullptr) < 0) {
                avcodec_free_context(&ctx);
                av_buffer_unref(hw);
                return false;
            }
        }
        *out = ctx;
        if (!hwOn)
            av_buffer_unref(hw);
        return true;
    };

    AVCodecContext* videoCtx = nullptr;
    AVBufferRef* hwDevice = nullptr;
    if (!openDecoder(videoIndex, useHardware, &videoCtx, &hwDevice)) {
        avformat_close_input(&format);
        fail(QString::fromUtf8("\xE8\xA7\x86\xE9\xA2\x91\xE8\xA7\xA3\xE7\xA0\x81\xE5\x99\xA8\xE6\x89\x93\xE5\xBC\x80\xE5\xA4\xB1\xE8\xB4\xA5"));
        return;
    }
    QString mode = hwDevice
        ? QString::fromUtf8("\xE7\xA1\xAC\xE8\xA7\xA3\x20\x56\x75\x6C\x6B\x61\x6E")
        : QString::fromUtf8("\xE8\xBD\xAF\xE8\xA7\xA3");
    if (hwDevice) {
        QString gpuName;
        {
            std::lock_guard<std::mutex> lock(m->statusMutex);
            gpuName = m->gpuName;
        }
        if (!gpuName.isEmpty())
            mode += QStringLiteral(" · ") + gpuName;
    }
    m->setStatus(mode);
    QMetaObject::invokeMethod(this, [this, mode] {
        m->shownStatus = mode;
        emit statusChanged(mode);
    }, Qt::QueuedConnection);

    AVCodecContext* audioCtx = nullptr;
    AVBufferRef* unusedHw = nullptr;
    if (audioIndex >= 0)
        openDecoder(audioIndex, false, &audioCtx, &unusedHw);

    if (format->duration != AV_NOPTS_VALUE)
        m->duration = format->duration / static_cast<double>(AV_TIME_BASE);
    else if (format->streams[videoIndex]->duration != AV_NOPTS_VALUE)
        m->duration = format->streams[videoIndex]->duration * av_q2d(format->streams[videoIndex]->time_base);

    SwsContext* scaler = nullptr;
    int scaleW = 0;
    int scaleH = 0;
    AVPixelFormat scaleFmt = AV_PIX_FMT_NONE;
    SwrContext* resampler = nullptr;
    double nextVideoPts = 0.0;
    double nextAudioPts = 0.0;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* swFrame = av_frame_alloc();

    auto performSeek = [&] {
        for (;;) {
            if (m->stop.load())
                return;
            double sec = 0.0;
            int id = 0;
            {
                std::lock_guard<std::mutex> lock(m->seekMutex);
                sec = m->seekSec;
                id = m->seekId;
            }
            AVStream* stream = format->streams[videoIndex];
            const int64_t target = av_rescale_q(static_cast<int64_t>(sec * AV_TIME_BASE),
                                                AV_TIME_BASE_Q, stream->time_base);
            const int ret = avformat_seek_file(format, videoIndex, INT64_MIN, target, target,
                                               AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                int latest = 0;
                {
                    std::lock_guard<std::mutex> lock(m->seekMutex);
                    latest = m->seekId;
                }
                if (latest != id)
                    continue;
                const QString text = QString::fromUtf8("\xE8\xB7\xB3\xE8\xBD\xAC\xE5\xA4\xB1\xE8\xB4\xA5");
                m->setStatus(text);
                QMetaObject::invokeMethod(this, [this, text] {
                    m->shownStatus = text;
                    emit statusChanged(text);
                }, Qt::QueuedConnection);
                std::unique_lock<std::mutex> lock(m->waitMutex);
                m->waitCv.wait(lock, [&] {
                    if (m->stop.load())
                        return true;
                    std::lock_guard<std::mutex> seekLock(m->seekMutex);
                    return m->seekId != id;
                });
                if (m->stop.load())
                    return;
                continue;
            }
            avcodec_flush_buffers(videoCtx);
            if (audioCtx)
                avcodec_flush_buffers(audioCtx);
            m->clearQueues();
            m->eof = false;
            m->dropUntil = sec;
            nextVideoPts = sec;
            nextAudioPts = sec;
            int latest = 0;
            {
                std::lock_guard<std::mutex> lock(m->seekMutex);
                latest = m->seekId;
            }
            if (latest != id)
                continue;
            m->seekPending = false;
            m->seekFinished = id;
            return;
        }
    };

    auto handleVideo = [&](AVFrame* src) {
        AVFrame* used = src;
        if (src->format == AV_PIX_FMT_VULKAN) {
            if (av_hwframe_transfer_data(swFrame, src, 0) < 0)
                return;
            used = swFrame;
        }
        const auto fmt = static_cast<AVPixelFormat>(used->format);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
        const bool alpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
        if (!scaler || scaleW != used->width || scaleH != used->height || scaleFmt != fmt) {
            sws_freeContext(scaler);
            scaler = sws_getContext(used->width, used->height, fmt,
                                    used->width, used->height, AV_PIX_FMT_BGRA,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            scaleW = used->width;
            scaleH = used->height;
            scaleFmt = fmt;
        }
        if (!scaler)
            return;
        VideoFrame out;
        out.width = used->width;
        out.height = used->height;
        out.alpha = alpha;
        out.pts = stampSeconds(src->best_effort_timestamp != AV_NOPTS_VALUE
                                    ? src->best_effort_timestamp
                                    : src->pts,
                                format->streams[videoIndex]->time_base, nextVideoPts);
        nextVideoPts = out.pts + 0.03;
        const double until = m->dropUntil.load();
        if (until >= 0.0 && out.pts + 0.05 < until) {
            av_frame_unref(swFrame);
            return;
        }
        if (until >= 0.0)
            m->dropUntil = -1.0;
        out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
        uint8_t* dstData[1] = { out.bgra.data() };
        int dstStride[1] = { out.width * 4 };
        sws_scale(scaler, used->data, used->linesize, 0, used->height, dstData, dstStride);
        m->pushVideo(std::move(out));
        av_frame_unref(swFrame);
    };

    auto handleAudio = [&](AVFrame* src) {
        if (!audioCtx)
            return;
        const int inChannels = audioCtx->ch_layout.nb_channels > 0 ? audioCtx->ch_layout.nb_channels : 2;
        m->channels = inChannels > 2 ? 2 : inChannels;
        m->sampleRate = audioCtx->sample_rate > 0 ? audioCtx->sample_rate : 48000;
        if (!resampler) {
            AVChannelLayout inLayout;
            av_channel_layout_default(&inLayout, 0);
            if (av_channel_layout_copy(&inLayout, &audioCtx->ch_layout) < 0)
                av_channel_layout_default(&inLayout, m->channels);
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, m->channels);
            const int swrOk = swr_alloc_set_opts2(&resampler, &outLayout, AV_SAMPLE_FMT_S16, m->sampleRate,
                                                  &inLayout, static_cast<AVSampleFormat>(src->format),
                                                  src->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&inLayout);
            av_channel_layout_uninit(&outLayout);
            if (swrOk < 0 || swr_init(resampler) < 0) {
                swr_free(&resampler);
                return;
            }
        }
        const int outCount = swr_get_out_samples(resampler, src->nb_samples);
        PcmChunk chunk;
        chunk.pts = stampSeconds(src->pts, format->streams[audioIndex]->time_base, nextAudioPts);
        chunk.bytes.resize(static_cast<size_t>(outCount) * m->channels * 2);
        uint8_t* outPlane = chunk.bytes.data();
        const int got = swr_convert(resampler, &outPlane, outCount,
                                    const_cast<const uint8_t**>(src->extended_data), src->nb_samples);
        if (got <= 0)
            return;
        chunk.bytes.resize(static_cast<size_t>(got) * m->channels * 2);
        nextAudioPts = chunk.pts + static_cast<double>(got) / m->sampleRate;
        m->pushPcm(std::move(chunk));
    };

    auto drain = [&](AVCodecContext* ctx, bool video) {
        while (!m->stop.load() && !m->seekPending.load()) {
            const int ret = avcodec_receive_frame(ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            if (video)
                handleVideo(frame);
            else
                handleAudio(frame);
            av_frame_unref(frame);
        }
    };

    while (!m->stop.load()) {
        if (m->pause.load() && !m->seekPending.load()) {
            std::unique_lock<std::mutex> lock(m->waitMutex);
            m->waitCv.wait(lock, [&] { return m->stop.load() || !m->pause.load() || m->seekPending.load(); });
            continue;
        }
        if (m->seekPending.load()) {
            performSeek();
            continue;
        }
        if (m->eof.load()) {
            if (m->loop.load() && !m->pause.load()) {
                {
                    std::lock_guard<std::mutex> lock(m->seekMutex);
                    m->seekSec = 0.0;
                    ++m->seekId;
                }
                m->seekPending = true;
                m->eof = false;
                m->position = 0.0;
                continue;
            }
            std::unique_lock<std::mutex> lock(m->waitMutex);
            m->waitCv.wait(lock, [&] {
                return m->stop.load() || m->seekPending.load() || m->loop.load();
            });
            continue;
        }

        const int read = av_read_frame(format, packet);
        if (read == AVERROR_EOF) {
            avcodec_send_packet(videoCtx, nullptr);
            drain(videoCtx, true);
            if (audioCtx) {
                avcodec_send_packet(audioCtx, nullptr);
                drain(audioCtx, false);
            }
            m->eof = true;
            continue;
        }
        if (read < 0)
            break;

        if (packet->stream_index == videoIndex) {
            if (avcodec_send_packet(videoCtx, packet) == 0)
                drain(videoCtx, true);
        } else if (audioCtx && packet->stream_index == audioIndex) {
            if (avcodec_send_packet(audioCtx, packet) == 0)
                drain(audioCtx, false);
        }
        av_packet_unref(packet);
    }

    av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(scaler);
    swr_free(&resampler);
    avcodec_free_context(&videoCtx);
    avcodec_free_context(&audioCtx);
    av_buffer_unref(&hwDevice);
    avformat_close_input(&format);
    m->running = false;
}
