#pragma once

#include <cstdint>
#include <functional>
#include <memory>

struct AVFrame;
struct AVBufferRef;

struct VulkanCopyResult {
    bool ok = false;
    bool interrupted = false;
    void* handle = nullptr;
    std::shared_ptr<void> slot;
};

class VulkanCopy {
public:
    VulkanCopy();
    ~VulkanCopy();

    bool init(AVBufferRef* vulkanDevice, int64_t luidHigh, uint32_t luidLow);
    bool canImportSharedTexture();
    VulkanCopyResult copy(AVFrame* frame, const std::function<bool()>& stop);

private:
    struct Impl;
    Impl* impl;
};
