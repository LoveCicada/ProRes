#include "Player.h"
#include "VulkanCopy.h"

#include <QAudio>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QIODevice>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
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

    const char* kLuidExtensions[] = {
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
    };
    std::vector<const char*> enabledExtensions;
    using FnEnumExt = int (__stdcall*)(const char*, uint32_t*, void*);
    auto enumerateExtensions = reinterpret_cast<FnEnumExt>(
        getProc(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (enumerateExtensions) {
        uint32_t extensionCount = 0;
        enumerateExtensions(nullptr, &extensionCount, nullptr);
        struct ExtensionProperty {
            char name[256];
            uint32_t version;
        };
        std::vector<ExtensionProperty> available(extensionCount);
        if (extensionCount > 0
            && enumerateExtensions(nullptr, &extensionCount, available.data()) == 0) {
            for (const char* wantedName : kLuidExtensions) {
                for (const ExtensionProperty& property : available) {
                    if (std::strcmp(property.name, wantedName) == 0) {
                        enabledExtensions.push_back(wantedName);
                        break;
                    }
                }
            }
        }
    }
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    instanceInfo.enabledExtensionNames = enabledExtensions.empty() ? nullptr : enabledExtensions.data();

    void* instance = nullptr;
    if (createInstance(&instanceInfo, nullptr, &instance) != 0 || !instance) {
        instanceInfo.enabledExtensionCount = 0;
        instanceInfo.enabledExtensionNames = nullptr;
        if (createInstance(&instanceInfo, nullptr, &instance) != 0 || !instance) {
            FreeLibrary(library);
            return -1;
        }
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

bool proresFamily(AVCodecID id)
{
    return id == AV_CODEC_ID_PRORES || id == AV_CODEC_ID_PRORES_RAW;
}

DecodePath choosePath(int mask)
{
    if (mask & kPathZeroCopy)
        return DecodePath::VulkanZeroCopy;
    if (mask & kPathReadback)
        return DecodePath::VulkanReadback;
    return DecodePath::Software;
}

enum class ProbeNote {
    None,
    NoVulkanDevice,
    VulkanCreateFailed,
    FrameNotVulkan
};

QString modeText(DecodePath path, const QString& gpu, const QString& note)
{
    QString text = QString::fromUtf8("\xE8\xBD\xAF\xE8\xA7\xA3");
    if (path == DecodePath::VulkanReadback)
        text = QString::fromUtf8("Vulkan \xE5\x9B\x9E\xE8\xAF\xBB");
    else if (path == DecodePath::VulkanZeroCopy)
        text = QString::fromUtf8("Vulkan \xE9\x9B\xB6\xE6\x8B\xB7\xE8\xB4\x9D");
    if (path != DecodePath::Software && !gpu.isEmpty())
        text += QStringLiteral(" · ") + gpu;
    if (path == DecodePath::Software && !note.isEmpty())
        text += QStringLiteral(" · ") + note;
    return text;
}

QString pixFmtName(AVPixelFormat fmt)
{
    if (fmt == AV_PIX_FMT_NONE || fmt == AV_PIX_FMT_VULKAN)
        return {};
    const char* name = av_get_pix_fmt_name(fmt);
    return name ? QString::fromUtf8(name) : QString();
}

QString codecIdLabel(AVCodecID id)
{
    if (id == AV_CODEC_ID_PRORES) {
        return QString::fromUtf8(
            "\xE6\xA0\x87\xE5\x87\x86\x20\x50\x72\x6F\x52\x65\x73");
    }
    if (id == AV_CODEC_ID_PRORES_RAW)
        return QString::fromUtf8("ProRes RAW");
    const char* name = avcodec_get_name(id);
    return name ? QString::fromUtf8(name) : QString();
}

AVPixelFormat contentPixelFormat(AVFrame* src, AVPixelFormat softwareFmt)
{
    if (src && src->format == AV_PIX_FMT_VULKAN && src->hw_frames_ctx && src->hw_frames_ctx->data) {
        auto* frames = reinterpret_cast<AVHWFramesContext*>(src->hw_frames_ctx->data);
        return static_cast<AVPixelFormat>(frames->sw_format);
    }
    return softwareFmt;
}

QString probeNoteText(ProbeNote note)
{
    if (note == ProbeNote::NoVulkanDevice) {
        return QString::fromUtf8(
            "\xE9\x80\x89\xE4\xB8\xAD\xE7\x9A\x84\xE6\x98\xBE\xE5\x8D\xA1\xE6\xB2\xA1\xE6\x9C\x89"
            "\xE5\xAF\xB9\xE5\xBA\x94\xE7\x9A\x84\x20\x56\x75\x6C\x6B\x61\x6E\x20"
            "\xE8\xAE\xBE\xE5\xA4\x87");
    }
    if (note == ProbeNote::VulkanCreateFailed) {
        return QString::fromUtf8(
            "\x56\x75\x6C\x6B\x61\x6E\x20\xE8\xAE\xBE\xE5\xA4\x87"
            "\xE5\x88\x9B\xE5\xBB\xBA\xE5\xA4\xB1\xE8\xB4\xA5");
    }
    if (note == ProbeNote::FrameNotVulkan) {
        return QString::fromUtf8(
            "\xE7\xAC\xAC\xE4\xB8\x80\xE5\xB8\xA7\xE4\xB8\x8D\xE6\x98\xAF\x20"
            "\x56\x75\x6C\x6B\x61\x6E");
    }
    return {};
}

int probeDecodeMask(const char* path, int vulkanIndex, qint64 luidHigh, quint32 luidLow, ProbeNote* note)
{
    if (note)
        *note = ProbeNote::None;
    if (!path)
        return 0;
    const int previous = av_log_get_level();
    av_log_set_level(AV_LOG_ERROR);
    int mask = 0;

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
    const AVCodecID codecId = videoIndex >= 0
        ? format->streams[videoIndex]->codecpar->codec_id
        : AV_CODEC_ID_NONE;
    const AVCodec* codec = videoIndex >= 0 ? avcodec_find_decoder(codecId) : nullptr;
    if (codec)
        mask |= kPathSoftware;

    const bool prores = codec && proresFamily(codecId);
    const bool wantVulkan = prores && vulkanIndex >= 0;
    if (note && prores && vulkanIndex < 0)
        *note = ProbeNote::NoVulkanDevice;
    const std::string deviceName = std::to_string(vulkanIndex);
    const bool deviceOk = wantVulkan
        && av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_VULKAN, deviceName.c_str(), nullptr, 0) >= 0;
    if (note && wantVulkan && !deviceOk)
        *note = ProbeNote::VulkanCreateFailed;
    if (deviceOk)
        ctx = avcodec_alloc_context3(codec);
    const bool paramsOk = ctx && avcodec_parameters_to_context(ctx, format->streams[videoIndex]->codecpar) >= 0;
    if (paramsOk) {
        ctx->hw_device_ctx = av_buffer_ref(hw);
        ctx->opaque = &choice;
        ctx->get_format = probeGetFormat;
    }
    bool vulkanFrame = false;
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
                vulkanFrame = frame->format == AV_PIX_FMT_VULKAN;
                av_frame_unref(frame);
                break;
            }
            if (received != AVERROR(EAGAIN))
                break;
        }
    }
    if (vulkanFrame) {
        mask |= kPathReadback;
        if (note)
            *note = ProbeNote::None;
        VulkanCopy copy;
        if (copy.init(hw, luidHigh, luidLow) && copy.canImportSharedTexture())
            mask |= kPathZeroCopy;
    } else if (note && wantVulkan && deviceOk && *note == ProbeNote::None) {
        *note = ProbeNote::FrameNotVulkan;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    av_buffer_unref(&hw);
    avformat_close_input(&format);
    av_log_set_level(previous);
    return mask;
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
    QString probeNote;
    std::atomic<int> requestedPath { static_cast<int>(DecodePath::Software) };
    std::atomic<int> activePath { static_cast<int>(DecodePath::Software) };
    std::atomic<int> pathMask { kPathSoftware };
    std::atomic<bool> reopenDecoder { false };
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
    std::atomic<int> decodePushed { 0 };
    std::atomic<int> videoQueued { 0 };
    std::mutex pcmMutex;
    std::condition_variable pcmCv;
    std::deque<PcmChunk> pcm;
    size_t pcmBytes = 0;

    std::atomic<double> duration { 0.0 };
    std::atomic<double> position { 0.0 };
    std::mutex statusMutex;
    QString status;
    QString shownStatus;
    QString pixelFormat;
    QString videoCodec;

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
    double lastAudioPlayed = -1.0;
    std::chrono::steady_clock::time_point lastAudioMove {};
    bool audioMoveValid = false;
    bool audioHoldRestart = false;
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
            videoQueued.store(0);
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
        decodePushed.fetch_add(1);
        videoQueued.store(static_cast<int>(video.size()));
    }

    void pushPcm(PcmChunk chunk)
    {
        const size_t limit = static_cast<size_t>(sampleRate) * static_cast<size_t>(channels) * 4;
        std::lock_guard<std::mutex> lock(pcmMutex);
        if (stop.load() || seekPending.load() || pause.load())
            return;
        while (!pcm.empty() && pcmBytes + chunk.bytes.size() > limit) {
            pcmBytes -= pcm.front().bytes.size();
            pcm.pop_front();
        }
        if (chunk.bytes.empty() || chunk.bytes.size() > limit)
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

int Player::decodePushed() const
{
    return m->decodePushed.load();
}

int Player::videoQueued() const
{
    return m->videoQueued.load();
}

int Player::videoQueueMax() const
{
    return kMaxFrames;
}

QString Player::videoPixelFormat() const
{
    std::lock_guard<std::mutex> lock(m->statusMutex);
    return m->pixelFormat;
}

QString Player::videoCodecName() const
{
    std::lock_guard<std::mutex> lock(m->statusMutex);
    return m->videoCodec;
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
    m->lastAudioPlayed = -1.0;
    m->audioMoveValid = false;
    m->audioHoldRestart = false;
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
    if (!m->wallRunning) {
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
    m->lastAudioPlayed = -1.0;
    m->audioMoveValid = false;
    m->audioHoldRestart = false;
    m->position = 0.0;
    m->eof = false;
    m->wallRunning = false;
    {
        std::lock_guard<std::mutex> lock(m->statusMutex);
        m->pixelFormat.clear();
        m->videoCodec.clear();
    }
    emit pixelFormatChanged(QString());
    emit videoCodecChanged(QString());
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
    m->lastAudioPlayed = -1.0;
    m->audioMoveValid = false;
    m->audioHoldRestart = false;
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

int Player::decodePath() const
{
    return m->activePath.load();
}

void Player::setDecodePath(int path)
{
    if ((m->pathMask.load() & (1 << path)) == 0)
        return;
    if (path == m->requestedPath.load())
        return;
    m->requestedPath = path;
    if (!m->running.load())
        return;
    const double seconds = std::max(0.0, m->position.load());
    {
        std::lock_guard<std::mutex> lock(m->seekMutex);
        m->seekSec = seconds;
        ++m->seekId;
    }
    m->reopenDecoder = true;
    m->seekPending = true;
    m->eof = false;
    m->pause = false;
    m->position = seconds;
    m->dropUntil = seconds;
    m->audioBaseValid = false;
    m->lastAudioPlayed = -1.0;
    m->audioMoveValid = false;
    m->audioHoldRestart = false;
    m->wallOrigin = seconds;
    m->wallAccum = 0.0;
    m->wallStart = std::chrono::steady_clock::now();
    m->wallRunning = true;
    m->clearQueues();
    m->waitCv.notify_all();
    m->videoCv.notify_all();
    m->pcmCv.notify_all();
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
        m->lastAudioPlayed = -1.0;
        m->audioMoveValid = false;
        m->audioHoldRestart = false;
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

    const auto now = std::chrono::steady_clock::now();
    if (m->audio && !m->pause.load()) {
        const QAudio::State state = m->audio->state();
        const double playedNow = m->audioBaseValid ? m->audio->processedUSecs() / 1000000.0 : 0.0;
        const bool audioFrozen = m->audioMoveValid
            && playedNow <= m->lastAudioPlayed + 0.0005
            && std::chrono::duration<double>(now - m->lastAudioMove).count() > 0.15;
        if (state == QAudio::StoppedState || (state == QAudio::IdleState && audioFrozen)) {
            if (!m->audioHoldRestart) {
                const double nowPts = m->audioBaseValid ? m->audioBase + playedNow : m->position.load();
                m->audioDevice = m->audio->start();
                m->audioReady = m->audioDevice != nullptr;
                m->audioBase = nowPts;
                m->audioBaseValid = m->audioReady;
                m->lastAudioPlayed = 0.0;
                m->lastAudioMove = now;
                m->audioMoveValid = true;
                m->audioHoldRestart = true;
            }
        } else {
            m->audioHoldRestart = false;
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

    double wall = m->wallOrigin + m->wallAccum;
    if (m->wallRunning)
        wall += std::chrono::duration<double>(now - m->wallStart).count();

    const double played = (m->audio && m->audioBaseValid) ? m->audio->processedUSecs() / 1000000.0 : 0.0;
    const bool audioClock = m->audioReady && m->audioBaseValid && m->audio != nullptr;
    double clock = wall;
    bool audioStalled = false;
    if (audioClock) {
        const double audioNow = m->audioBase + played;
        if (m->pause.load()) {
            clock = audioNow;
            m->lastAudioPlayed = played;
            m->lastAudioMove = now;
            m->audioMoveValid = true;
        } else {
            if (!m->audioMoveValid || played > m->lastAudioPlayed + 0.02) {
                m->lastAudioPlayed = played;
                m->lastAudioMove = now;
                m->audioMoveValid = true;
            }
            const bool fresh = m->audioMoveValid
                && std::chrono::duration<double>(now - m->lastAudioMove).count() < 0.15;
            if (fresh && audioNow + 0.2 >= wall && audioNow < wall + 0.5)
                clock = audioNow;
            else {
                audioStalled = true;
                clock = wall;
            }
        }
    }

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
        m->videoQueued.store(static_cast<int>(m->video.size()));
    }
    if (got && stalled && !audioStalled) {
        if (m->audio && m->audioBaseValid)
            m->audioBase = out.pts - played;
        m->wallOrigin = out.pts;
        m->wallAccum = 0.0;
        m->wallStart = now;
    } else if (audioStalled && m->audio && m->audioBaseValid) {
        m->audioBase = (got ? out.pts : clock) - played;
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
        {
            std::lock_guard<std::mutex> lock(m->statusMutex);
            m->pixelFormat.clear();
            m->videoCodec.clear();
        }
        QMetaObject::invokeMethod(this, [this, text] {
            m->shownStatus = text;
            emit statusChanged(text);
            emit pixelFormatChanged(QString());
            emit videoCodecChanged(QString());
        }, Qt::QueuedConnection);
        m->running = false;
    };

    auto publishPixelFormat = [&](AVPixelFormat fmt, bool onlyIfEmpty) {
        const QString name = pixFmtName(fmt);
        if (name.isEmpty())
            return;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m->statusMutex);
            if (onlyIfEmpty && !m->pixelFormat.isEmpty())
                return;
            if (m->pixelFormat == name)
                return;
            m->pixelFormat = name;
            changed = true;
        }
        if (!changed)
            return;
        QMetaObject::invokeMethod(this, [this, name] {
            emit pixelFormatChanged(name);
        }, Qt::QueuedConnection);
    };

    auto publishVideoCodec = [&](AVCodecID id) {
        const QString label = codecIdLabel(id);
        if (label.isEmpty())
            return;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m->statusMutex);
            if (m->videoCodec == label)
                return;
            m->videoCodec = label;
            changed = true;
        }
        if (!changed)
            return;
        QMetaObject::invokeMethod(this, [this, label] {
            emit videoCodecChanged(label);
        }, Qt::QueuedConnection);
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

    const AVCodecID videoCodecId = format->streams[videoIndex]->codecpar->codec_id;
    publishVideoCodec(videoCodecId);
    publishPixelFormat(static_cast<AVPixelFormat>(format->streams[videoIndex]->codecpar->format), false);

    const AVCodecID videoCodec = videoCodecId;
    const int vulkanIndex = proresFamily(videoCodec)
        ? vulkanDeviceIndex(m->luidHigh.load(), m->luidLow.load())
        : -1;
    const QByteArray pathUtf8 = path.toUtf8();
    ProbeNote probeNote = ProbeNote::None;
    const int mask = probeDecodeMask(pathUtf8.constData(), vulkanIndex, m->luidHigh.load(), m->luidLow.load(), &probeNote);
    {
        std::lock_guard<std::mutex> lock(m->statusMutex);
        m->probeNote = probeNoteText(probeNote);
    }
    if (mask == 0) {
        avformat_close_input(&format);
        fail(QString::fromUtf8("\xE8\xA7\x86\xE9\xA2\x91\xE8\xA7\xA3\xE7\xA0\x81\xE5\x99\xA8\xE6\x89\x93\xE5\xBC\x80\xE5\xA4\xB1\xE8\xB4\xA5"));
        return;
    }
    m->pathMask = mask;
    DecodePath chosen = choosePath(mask);
    m->requestedPath = static_cast<int>(chosen);
    m->activePath = static_cast<int>(chosen);

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
    std::unique_ptr<VulkanCopy> zeroCopy;
    auto publish = [&](DecodePath selected) {
        QString gpuName;
        QString note;
        {
            std::lock_guard<std::mutex> lock(m->statusMutex);
            gpuName = m->gpuName;
            note = m->probeNote;
        }
        const QString mode = modeText(selected, gpuName, note);
        m->activePath = static_cast<int>(selected);
        m->setStatus(mode);
        QMetaObject::invokeMethod(this, [this, mode, selected] {
            m->shownStatus = mode;
            emit statusChanged(mode);
            emit decodeOptionsChanged(m->pathMask.load(), static_cast<int>(selected));
        }, Qt::QueuedConnection);
    };
    auto openVideo = [&](DecodePath selected) {
        zeroCopy.reset();
        avcodec_free_context(&videoCtx);
        av_buffer_unref(&hwDevice);
        const bool wantVulkan = selected != DecodePath::Software;
        if (!openDecoder(videoIndex, wantVulkan && vulkanIndex >= 0, &videoCtx, &hwDevice))
            return false;
        if (selected != DecodePath::Software && !hwDevice) {
            selected = DecodePath::Software;
            m->requestedPath = static_cast<int>(selected);
        }
        if (selected == DecodePath::VulkanZeroCopy) {
            zeroCopy = std::make_unique<VulkanCopy>();
            if (!zeroCopy->init(hwDevice, m->luidHigh.load(), m->luidLow.load())) {
                zeroCopy.reset();
                selected = DecodePath::VulkanReadback;
                m->requestedPath = static_cast<int>(selected);
                m->pathMask = m->pathMask.load() & ~kPathZeroCopy;
            }
        }
        chosen = selected;
        publish(selected);
        return true;
    };
    if (!openVideo(chosen)) {
        avformat_close_input(&format);
        fail(QString::fromUtf8("\xE8\xA7\x86\xE9\xA2\x91\xE8\xA7\xA3\xE7\xA0\x81\xE5\x99\xA8\xE6\x89\x93\xE5\xBC\x80\xE5\xA4\xB1\xE8\xB4\xA5"));
        return;
    }

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
                if (m->loop.load() && sec <= 0.001) {
                    int again = av_seek_frame(format, videoIndex, 0, AVSEEK_FLAG_ANY);
                    if (again < 0)
                        again = avformat_seek_file(format, -1, INT64_MIN, 0, 0, AVSEEK_FLAG_BACKWARD);
                    if (again < 0)
                        again = av_seek_frame(format, -1, 0, AVSEEK_FLAG_BYTE);
                    avcodec_flush_buffers(videoCtx);
                    if (audioCtx)
                        avcodec_flush_buffers(audioCtx);
                    m->clearQueues();
                    m->eof = false;
                    m->dropUntil = 0.0;
                    nextVideoPts = 0.0;
                    nextAudioPts = 0.0;
                    m->seekPending = false;
                    m->seekFinished = id;
                    if (again < 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    return;
                }
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
        const auto stopCopy = [&] {
            return m->stop.load() || m->seekPending.load() || m->pause.load();
        };
        publishPixelFormat(contentPixelFormat(src, static_cast<AVPixelFormat>(src->format)), true);
        if (chosen == DecodePath::VulkanZeroCopy && zeroCopy && src->format == AV_PIX_FMT_VULKAN) {
            const VulkanCopyResult copied = zeroCopy->copy(src, stopCopy);
            if (copied.interrupted)
                return;
            if (copied.ok) {
                const AVPixFmtDescriptor* hwDesc = nullptr;
                if (src->hw_frames_ctx && src->hw_frames_ctx->data) {
                    auto* frames = reinterpret_cast<AVHWFramesContext*>(src->hw_frames_ctx->data);
                    hwDesc = av_pix_fmt_desc_get(frames->sw_format);
                }
                VideoFrame out;
                out.width = src->width;
                out.height = src->height;
                out.alpha = hwDesc && (hwDesc->flags & AV_PIX_FMT_FLAG_ALPHA);
                out.zeroCopy = true;
                out.sharedHandle = reinterpret_cast<quintptr>(copied.handle);
                out.slot = copied.slot;
                out.pts = stampSeconds(src->best_effort_timestamp != AV_NOPTS_VALUE
                                            ? src->best_effort_timestamp
                                            : src->pts,
                                        format->streams[videoIndex]->time_base, nextVideoPts);
                nextVideoPts = out.pts + 0.03;
                const double until = m->dropUntil.load();
                if (until >= 0.0 && out.pts + 0.05 < until)
                    return;
                if (until >= 0.0)
                    m->dropUntil = -1.0;
                m->pushVideo(std::move(out));
                return;
            }
            chosen = DecodePath::VulkanReadback;
            m->requestedPath = static_cast<int>(chosen);
            m->pathMask = m->pathMask.load() & ~kPathZeroCopy;
            publish(chosen);
        }
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
            if (m->reopenDecoder.exchange(false)) {
                const DecodePath next = static_cast<DecodePath>(m->requestedPath.load());
                if (!openVideo(next)) {
                    fail(QString::fromUtf8("\xE8\xA7\x86\xE9\xA2\x91\xE8\xA7\xA3\xE7\xA0\x81\xE5\x99\xA8\xE6\x89\x93\xE5\xBC\x80\xE5\xA4\xB1\xE8\xB4\xA5"));
                    break;
                }
            }
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
    zeroCopy.reset();
    avcodec_free_context(&videoCtx);
    avcodec_free_context(&audioCtx);
    av_buffer_unref(&hwDevice);
    avformat_close_input(&format);
    m->running = false;
}
