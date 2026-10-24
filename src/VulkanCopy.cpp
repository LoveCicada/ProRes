#include "VulkanCopy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kIgnoredQueue = ~0u;
constexpr int kSlots = 4;

constexpr uint32_t kR16 = 70;
constexpr uint32_t kRgba16Unorm = 91;
constexpr uint32_t kRgba16Float = 97;
constexpr uint32_t kD3d11Texture = 0x8;
constexpr uint32_t kStorageFeature = 0x2;
constexpr uint32_t kBlitSrc = 0x400;
constexpr uint32_t kBlitDst = 0x800;
constexpr uint32_t kUsageStorage = 0x8;
constexpr uint32_t kUsageTransferSrc = 0x1;
constexpr uint32_t kUsageTransferDst = 0x2;
constexpr uint32_t kUsageSampled = 0x4;
constexpr uint32_t kUsageColor = 0x10;
constexpr uint32_t kAspectColor = 0x1;
constexpr uint32_t kAspectPlane0 = 0x10;
constexpr uint32_t kLayoutGeneral = 1;
constexpr uint32_t kLayoutShaderRead = 5;
constexpr uint32_t kLayoutTransferSrc = 6;
constexpr uint32_t kLayoutTransferDst = 7;
constexpr uint32_t kStageTop = 0x1;
constexpr uint32_t kStageCompute = 0x800;
constexpr uint32_t kStageTransfer = 0x1000;
constexpr uint32_t kAccessShaderRead = 0x20;
constexpr uint32_t kAccessShaderWrite = 0x40;
constexpr uint32_t kAccessTransferRead = 0x800;
constexpr uint32_t kAccessTransferWrite = 0x1000;
constexpr uint32_t kStageComputeBit = 0x20;
constexpr uint32_t kCombinedSampler = 1;
constexpr uint32_t kStorageImage = 3;

struct DevHead {
    const void* alloc;
    void* (__stdcall* getProc)(void*, const char*);
    uint64_t instance;
    uint64_t physical;
    uint64_t device;
};

struct FrameView {
    uint64_t img[8];
    int32_t tiling;
    int32_t pad0;
    uint64_t mem[8];
    uint64_t size[8];
    uint32_t flags;
    uint32_t pad1;
    uint64_t access[8];
    int32_t layout[8];
    uint64_t sem[8];
    uint64_t semValue[8];
    int64_t offset[8];
    uint32_t queueFamily[8];
};

static_assert(offsetof(FrameView, layout) == 272, "AVVkFrame layout");
static_assert(offsetof(FrameView, sem) == 304, "AVVkFrame sem");
static_assert(offsetof(FrameView, semValue) == 368, "AVVkFrame semValue");
static_assert(offsetof(FrameView, queueFamily) == 496, "AVVkFrame queue");

struct Push {
    int32_t w;
    int32_t h;
    int32_t chromaX;
    int32_t hasAlpha;
    float scale;
    float black;
    float mid;
    float range;
    float alphaMax;
    int32_t chromaY;
};
static_assert(sizeof(Push) == 40, "push constant size");

struct Extent {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

struct ImageInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t imageType;
    uint32_t format;
    Extent extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    uint32_t samples;
    uint32_t tiling;
    uint32_t usage;
    uint32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    uint32_t initialLayout;
};

struct ExtImageInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t handleTypes;
};

struct ViewInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint64_t image;
    uint32_t viewType;
    uint32_t format;
    uint32_t swizzle[4];
    uint32_t aspect;
    uint32_t baseMip;
    uint32_t levelCount;
    uint32_t baseArray;
    uint32_t layerCount;
};

struct MemReq {
    uint64_t size;
    uint64_t alignment;
    uint32_t bits;
};

struct AllocInfo {
    uint32_t sType;
    const void* pNext;
    uint64_t size;
    uint32_t index;
};

struct DedicatedInfo {
    uint32_t sType;
    const void* pNext;
    uint64_t image;
    uint64_t buffer;
};

struct ImportInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t handleType;
    HANDLE handle;
    const wchar_t* name;
};

struct Win32Props {
    uint32_t sType;
    void* pNext;
    uint32_t memoryTypeBits;
};

struct MemProps {
    uint32_t typeCount;
    struct {
        uint32_t flags;
        uint32_t heap;
    } types[32];
    uint32_t heapCount;
    uint32_t pad;
    struct {
        uint64_t size;
        uint32_t flags;
        uint32_t pad;
    } heaps[16];
};

struct SamplerInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t magFilter;
    uint32_t minFilter;
    uint32_t mipmapMode;
    uint32_t addressU;
    uint32_t addressV;
    uint32_t addressW;
    float mipLodBias;
    uint32_t anisotropyEnable;
    float maxAnisotropy;
    uint32_t compareEnable;
    uint32_t compareOp;
    float minLod;
    float maxLod;
    uint32_t borderColor;
    uint32_t unnormalized;
};

struct Binding {
    uint32_t binding;
    uint32_t type;
    uint32_t count;
    uint32_t stages;
    const void* samplers;
};

struct LayoutInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t bindingCount;
    const Binding* bindings;
};

struct PushRange {
    uint32_t stages;
    uint32_t offset;
    uint32_t size;
};

struct PipeLayoutInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t setCount;
    const uint64_t* sets;
    uint32_t pushCount;
    const PushRange* pushes;
};

struct ShaderInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    size_t codeSize;
    const uint32_t* code;
};

struct StageInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t stage;
    uint64_t module;
    const char* name;
    const void* spec;
};

struct ComputeInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    StageInfo stage;
    uint64_t layout;
    uint64_t base;
    int32_t baseIndex;
};

struct PoolSize {
    uint32_t type;
    uint32_t count;
};

struct PoolInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t maxSets;
    uint32_t sizeCount;
    const PoolSize* sizes;
};

struct AllocSetInfo {
    uint32_t sType;
    const void* pNext;
    uint64_t pool;
    uint32_t count;
    const uint64_t* layouts;
};

struct ImageDesc {
    uint64_t sampler;
    uint64_t view;
    uint32_t layout;
};

struct WriteSet {
    uint32_t sType;
    const void* pNext;
    uint64_t dstSet;
    uint32_t dstBinding;
    uint32_t dstElement;
    uint32_t count;
    uint32_t type;
    const ImageDesc* image;
    const void* buffer;
    const void* texel;
};

struct PoolCreate {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t family;
};

struct CmdAlloc {
    uint32_t sType;
    const void* pNext;
    uint64_t pool;
    uint32_t level;
    uint32_t count;
};

struct BeginInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    const void* inherit;
};

struct Barrier {
    uint32_t sType;
    const void* pNext;
    uint32_t srcAccess;
    uint32_t dstAccess;
    uint32_t oldLayout;
    uint32_t newLayout;
    uint32_t srcQueue;
    uint32_t dstQueue;
    uint64_t image;
    uint32_t aspect;
    uint32_t baseMip;
    uint32_t levelCount;
    uint32_t baseLayer;
    uint32_t layerCount;
};

struct Layers {
    uint32_t aspect;
    uint32_t mip;
    uint32_t baseLayer;
    uint32_t layerCount;
};

struct Offset3 {
    int32_t x, y, z;
};

struct Blit {
    Layers src;
    Offset3 srcOff[2];
    Layers dst;
    Offset3 dstOff[2];
};

struct TimelineInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t waitCount;
    const uint64_t* waitValues;
    uint32_t signalCount;
    const uint64_t* signalValues;
};

struct SubmitInfo {
    uint32_t sType;
    const void* pNext;
    uint32_t waitCount;
    const uint64_t* waits;
    const uint32_t* waitStages;
    uint32_t cmdCount;
    const uint64_t* cmds;
    uint32_t signalCount;
    const uint64_t* signals;
};

struct Subres {
    uint32_t aspect;
    uint32_t baseMip;
    uint32_t levelCount;
    uint32_t baseLayer;
    uint32_t layerCount;
};

using PFN_GetDev = void* (__stdcall*)(uint64_t, const char*);
using PFN_Queue = void (__stdcall*)(uint64_t, uint32_t, uint32_t, uint64_t*);
using PFN_CreateImage = int (__stdcall*)(uint64_t, const ImageInfo*, const void*, uint64_t*);
using PFN_DestroyImage = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_MemReq = void (__stdcall*)(uint64_t, uint64_t, MemReq*);
using PFN_Alloc = int (__stdcall*)(uint64_t, const AllocInfo*, const void*, uint64_t*);
using PFN_Free = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_Bind = int (__stdcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
using PFN_CreateView = int (__stdcall*)(uint64_t, const ViewInfo*, const void*, uint64_t*);
using PFN_DestroyView = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_CreateSampler = int (__stdcall*)(uint64_t, const SamplerInfo*, const void*, uint64_t*);
using PFN_DestroySampler = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_CreateShader = int (__stdcall*)(uint64_t, const ShaderInfo*, const void*, uint64_t*);
using PFN_DestroyShader = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_CreateSetLayout = int (__stdcall*)(uint64_t, const LayoutInfo*, const void*, uint64_t*);
using PFN_DestroySetLayout = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_CreatePipeLayout = int (__stdcall*)(uint64_t, const PipeLayoutInfo*, const void*, uint64_t*);
using PFN_DestroyPipeLayout = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_CreateCompute = int (__stdcall*)(uint64_t, uint64_t, uint32_t, const ComputeInfo*, const void*, uint64_t*);
using PFN_DestroyPipe = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_CreatePool = int (__stdcall*)(uint64_t, const PoolInfo*, const void*, uint64_t*);
using PFN_DestroyPool = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_AllocSets = int (__stdcall*)(uint64_t, const AllocSetInfo*, uint64_t*);
using PFN_WriteSets = void (__stdcall*)(uint64_t, uint32_t, const WriteSet*, uint32_t, const void*);
using PFN_CreateCmdPool = int (__stdcall*)(uint64_t, const PoolCreate*, const void*, uint64_t*);
using PFN_DestroyCmdPool = void (__stdcall*)(uint64_t, uint64_t, const void*);
using PFN_AllocCmd = int (__stdcall*)(uint64_t, const CmdAlloc*, uint64_t*);
using PFN_ResetCmd = int (__stdcall*)(uint64_t, uint32_t);
using PFN_Begin = int (__stdcall*)(uint64_t, const BeginInfo*);
using PFN_End = int (__stdcall*)(uint64_t);
using PFN_Barrier = void (__stdcall*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, const void*, uint32_t, const Barrier*);
using PFN_BindPipe = void (__stdcall*)(uint64_t, uint32_t, uint64_t);
using PFN_BindSets = void (__stdcall*)(uint64_t, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t*, uint32_t, const uint32_t*);
using PFN_Push = void (__stdcall*)(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, const void*);
using PFN_Dispatch = void (__stdcall*)(uint64_t, uint32_t, uint32_t, uint32_t);
using PFN_Blit = void (__stdcall*)(uint64_t, uint64_t, uint32_t, uint64_t, uint32_t, uint32_t, const Blit*, uint32_t);
using PFN_Clear = void (__stdcall*)(uint64_t, uint64_t, uint32_t, const float*, uint32_t, const Subres*);
using PFN_Submit = int (__stdcall*)(uint64_t, uint32_t, const SubmitInfo*, uint64_t);
using PFN_Wait = int (__stdcall*)(uint64_t);
using PFN_Win32Props = int (__stdcall*)(uint64_t, uint32_t, HANDLE, Win32Props*);
using PFN_MemProps = void (__stdcall*)(uint64_t, MemProps*);
using PFN_FmtProps = void (__stdcall*)(uint64_t, uint32_t, uint32_t*);

extern "C" const int32_t* av_vkfmt_from_pixfmt(enum AVPixelFormat pixfmt);

const uint32_t kSpirv[] = {
#include "yuv_spirv.inc"
};

int memoryIndex(const MemProps& props, uint32_t bits, bool deviceLocal)
{
    int fallback = -1;
    for (uint32_t i = 0; i < props.typeCount && i < 32; ++i) {
        if ((bits & (1u << i)) == 0)
            continue;
        if (deviceLocal && (props.types[i].flags & 0x1))
            return static_cast<int>(i);
        if (fallback < 0)
            fallback = static_cast<int>(i);
    }
    return fallback;
}

} // namespace

struct VulkanCopy::Impl {
    uint64_t instance = 0;
    uint64_t physical = 0;
    uint64_t device = 0;
    ComPtr<ID3D11Device> d3d;
    PFN_Queue getQueue = nullptr;
    PFN_CreateImage createImage = nullptr;
    PFN_DestroyImage destroyImage = nullptr;
    PFN_MemReq memReq = nullptr;
    PFN_Alloc allocMem = nullptr;
    PFN_Free freeMem = nullptr;
    PFN_Bind bindMem = nullptr;
    PFN_CreateView createView = nullptr;
    PFN_DestroyView destroyView = nullptr;
    PFN_CreateSampler createSampler = nullptr;
    PFN_DestroySampler destroySampler = nullptr;
    PFN_CreateShader createShader = nullptr;
    PFN_DestroyShader destroyShader = nullptr;
    PFN_CreateSetLayout createSetLayout = nullptr;
    PFN_DestroySetLayout destroySetLayout = nullptr;
    PFN_CreatePipeLayout createPipeLayout = nullptr;
    PFN_DestroyPipeLayout destroyPipeLayout = nullptr;
    PFN_CreateCompute createCompute = nullptr;
    PFN_DestroyPipe destroyPipe = nullptr;
    PFN_CreatePool createDescPool = nullptr;
    PFN_DestroyPool destroyDescPool = nullptr;
    PFN_AllocSets allocSets = nullptr;
    PFN_WriteSets writeSets = nullptr;
    PFN_CreateCmdPool createCmdPool = nullptr;
    PFN_DestroyCmdPool destroyCmdPool = nullptr;
    PFN_AllocCmd allocCmd = nullptr;
    PFN_ResetCmd resetCmd = nullptr;
    PFN_Begin beginCmd = nullptr;
    PFN_End endCmd = nullptr;
    PFN_Barrier barrier = nullptr;
    PFN_BindPipe bindPipe = nullptr;
    PFN_BindSets bindSets = nullptr;
    PFN_Push pushConst = nullptr;
    PFN_Dispatch dispatch = nullptr;
    PFN_Blit blit = nullptr;
    PFN_Clear clearColor = nullptr;
    PFN_Submit submit = nullptr;
    PFN_Wait waitIdle = nullptr;
    PFN_Win32Props win32Props = nullptr;
    PFN_MemProps memProps = nullptr;
    uint64_t sampler = 0;
    uint64_t shader = 0;
    uint64_t setLayout = 0;
    uint64_t pipeLayout = 0;
    uint64_t pipeline = 0;
    uint64_t descPool = 0;
    uint64_t descSet = 0;
    uint64_t cmdPool = 0;
    uint64_t cmd = 0;
    uint64_t queue = 0;
    uint32_t family = kIgnoredQueue;
    uint64_t storageImage = 0;
    uint64_t storageMem = 0;
    uint64_t storageView = 0;
    uint32_t storageLayout = 0;
    uint64_t dummyImage = 0;
    uint64_t dummyMem = 0;
    uint64_t dummyView = 0;
    bool dummyReady = false;
    int width = 0;
    int height = 0;
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        HANDLE handle = nullptr;
        uint64_t image = 0;
        uint64_t memory = 0;
        uint64_t view = 0;
        uint32_t layout = 0;
    } slots[kSlots];
    std::shared_ptr<std::atomic<int>> busy[kSlots];

    ~Impl()
    {
        resetGpu();
    }

    void resetGpu()
    {
        auto drop = [&](uint64_t& image, uint64_t& memory, uint64_t& view) {
            if (view && destroyView)
                destroyView(device, view, nullptr);
            if (image && destroyImage)
                destroyImage(device, image, nullptr);
            if (memory && freeMem)
                freeMem(device, memory, nullptr);
            image = memory = view = 0;
        };
        for (Slot& slot : slots) {
            drop(slot.image, slot.memory, slot.view);
            slot.layout = 0;
            if (slot.handle)
                CloseHandle(slot.handle);
            slot.handle = nullptr;
            slot.texture.Reset();
        }
        drop(storageImage, storageMem, storageView);
        drop(dummyImage, dummyMem, dummyView);
        storageLayout = 0;
        dummyReady = false;
        width = height = 0;
        if (cmdPool && destroyCmdPool)
            destroyCmdPool(device, cmdPool, nullptr);
        cmdPool = cmd = 0;
        queue = 0;
        family = kIgnoredQueue;
        if (descPool && destroyDescPool)
            destroyDescPool(device, descPool, nullptr);
        descPool = descSet = 0;
        if (pipeline && destroyPipe)
            destroyPipe(device, pipeline, nullptr);
        if (pipeLayout && destroyPipeLayout)
            destroyPipeLayout(device, pipeLayout, nullptr);
        if (setLayout && destroySetLayout)
            destroySetLayout(device, setLayout, nullptr);
        if (shader && destroyShader)
            destroyShader(device, shader, nullptr);
        if (sampler && destroySampler)
            destroySampler(device, sampler, nullptr);
        pipeline = pipeLayout = setLayout = shader = sampler = 0;
    }

    template <typename T>
    T load(PFN_GetDev get, const char* name)
    {
        return reinterpret_cast<T>(get(device, name));
    }

    bool loadProcs(PFN_GetDev get)
    {
        getQueue = load<PFN_Queue>(get, "vkGetDeviceQueue");
        createImage = load<PFN_CreateImage>(get, "vkCreateImage");
        destroyImage = load<PFN_DestroyImage>(get, "vkDestroyImage");
        memReq = load<PFN_MemReq>(get, "vkGetImageMemoryRequirements");
        allocMem = load<PFN_Alloc>(get, "vkAllocateMemory");
        freeMem = load<PFN_Free>(get, "vkFreeMemory");
        bindMem = load<PFN_Bind>(get, "vkBindImageMemory");
        createView = load<PFN_CreateView>(get, "vkCreateImageView");
        destroyView = load<PFN_DestroyView>(get, "vkDestroyImageView");
        createSampler = load<PFN_CreateSampler>(get, "vkCreateSampler");
        destroySampler = load<PFN_DestroySampler>(get, "vkDestroySampler");
        createShader = load<PFN_CreateShader>(get, "vkCreateShaderModule");
        destroyShader = load<PFN_DestroyShader>(get, "vkDestroyShaderModule");
        createSetLayout = load<PFN_CreateSetLayout>(get, "vkCreateDescriptorSetLayout");
        destroySetLayout = load<PFN_DestroySetLayout>(get, "vkDestroyDescriptorSetLayout");
        createPipeLayout = load<PFN_CreatePipeLayout>(get, "vkCreatePipelineLayout");
        destroyPipeLayout = load<PFN_DestroyPipeLayout>(get, "vkDestroyPipelineLayout");
        createCompute = load<PFN_CreateCompute>(get, "vkCreateComputePipelines");
        destroyPipe = load<PFN_DestroyPipe>(get, "vkDestroyPipeline");
        createDescPool = load<PFN_CreatePool>(get, "vkCreateDescriptorPool");
        destroyDescPool = load<PFN_DestroyPool>(get, "vkDestroyDescriptorPool");
        allocSets = load<PFN_AllocSets>(get, "vkAllocateDescriptorSets");
        writeSets = load<PFN_WriteSets>(get, "vkUpdateDescriptorSets");
        createCmdPool = load<PFN_CreateCmdPool>(get, "vkCreateCommandPool");
        destroyCmdPool = load<PFN_DestroyCmdPool>(get, "vkDestroyCommandPool");
        allocCmd = load<PFN_AllocCmd>(get, "vkAllocateCommandBuffers");
        resetCmd = load<PFN_ResetCmd>(get, "vkResetCommandBuffer");
        beginCmd = load<PFN_Begin>(get, "vkBeginCommandBuffer");
        endCmd = load<PFN_End>(get, "vkEndCommandBuffer");
        barrier = load<PFN_Barrier>(get, "vkCmdPipelineBarrier");
        bindPipe = load<PFN_BindPipe>(get, "vkCmdBindPipeline");
        bindSets = load<PFN_BindSets>(get, "vkCmdBindDescriptorSets");
        pushConst = load<PFN_Push>(get, "vkCmdPushConstants");
        dispatch = load<PFN_Dispatch>(get, "vkCmdDispatch");
        blit = load<PFN_Blit>(get, "vkCmdBlitImage");
        clearColor = load<PFN_Clear>(get, "vkCmdClearColorImage");
        submit = load<PFN_Submit>(get, "vkQueueSubmit");
        waitIdle = load<PFN_Wait>(get, "vkQueueWaitIdle");
        win32Props = load<PFN_Win32Props>(get, "vkGetMemoryWin32HandlePropertiesKHR");
        return getQueue && createImage && destroyImage && memReq && allocMem && freeMem && bindMem
            && createView && destroyView && createSampler && destroySampler && createShader
            && createSetLayout && createPipeLayout && createCompute && createDescPool && allocSets
            && writeSets && createCmdPool && allocCmd && beginCmd && endCmd && barrier && bindPipe
            && bindSets && pushConst && dispatch && blit && clearColor && submit && waitIdle
            && win32Props;
    }

    int pickMemory(uint32_t bits, bool local) const
    {
        MemProps props = {};
        memProps(physical, &props);
        return memoryIndex(props, bits, local);
    }

    bool makeView(uint64_t image, uint32_t format, uint32_t aspect, uint64_t* view)
    {
        ViewInfo info = {};
        info.sType = 15;
        info.image = image;
        info.viewType = 1;
        info.format = format;
        info.aspect = aspect;
        info.levelCount = 1;
        info.layerCount = 1;
        return createView(device, &info, nullptr, view) == 0;
    }

    bool makeLocal(uint32_t w, uint32_t h, uint32_t format, uint32_t usage, uint64_t* image, uint64_t* memory)
    {
        ImageInfo info = {};
        info.sType = 14;
        info.imageType = 1;
        info.format = format;
        info.extent = { w, h, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = 1;
        info.usage = usage;
        if (createImage(device, &info, nullptr, image) != 0)
            return false;
        MemReq req = {};
        memReq(device, *image, &req);
        const int index = pickMemory(req.bits, true);
        if (index < 0)
            return false;
        AllocInfo alloc = {};
        alloc.sType = 5;
        alloc.size = req.size;
        alloc.index = static_cast<uint32_t>(index);
        if (allocMem(device, &alloc, nullptr, memory) != 0)
            return false;
        return bindMem(device, *image, *memory, 0) == 0;
    }

    bool importHandle(HANDLE handle, uint32_t w, uint32_t h, uint64_t* image, uint64_t* memory, uint64_t* view)
    {
        ExtImageInfo ext = {};
        ext.sType = 1000072001;
        ext.handleTypes = kD3d11Texture;
        ImageInfo info = {};
        info.sType = 14;
        info.pNext = &ext;
        info.imageType = 1;
        info.format = kRgba16Unorm;
        info.extent = { w, h, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = 1;
        info.usage = kUsageTransferDst | kUsageSampled | kUsageColor;
        if (createImage(device, &info, nullptr, image) != 0)
            return false;
        MemReq req = {};
        memReq(device, *image, &req);
        Win32Props props = {};
        props.sType = 1000073002;
        if (win32Props(device, kD3d11Texture, handle, &props) != 0)
            return false;
        const int index = pickMemory(req.bits & props.memoryTypeBits, true);
        if (index < 0)
            return false;
        DedicatedInfo dedicated = {};
        dedicated.sType = 1000127001;
        dedicated.image = *image;
        ImportInfo imported = {};
        imported.sType = 1000073000;
        imported.pNext = nullptr;
        imported.handleType = kD3d11Texture;
        imported.handle = handle;
        dedicated.pNext = &imported;
        AllocInfo alloc = {};
        alloc.sType = 5;
        alloc.pNext = &dedicated;
        alloc.size = req.size;
        alloc.index = static_cast<uint32_t>(index);
        if (allocMem(device, &alloc, nullptr, memory) != 0) {
            dedicated.pNext = nullptr;
            alloc.pNext = &imported;
            if (allocMem(device, &alloc, nullptr, memory) != 0)
                return false;
        }
        if (bindMem(device, *image, *memory, 0) != 0)
            return false;
        return makeView(*image, kRgba16Unorm, kAspectColor, view);
    }

    bool openD3d(int64_t luidHigh, uint32_t luidLow)
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))))
            return false;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIAdapter1> current;
        for (UINT i = 0; factory->EnumAdapters1(i, current.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (FAILED(current->GetDesc1(&desc)))
                continue;
            if (desc.AdapterLuid.HighPart == luidHigh && desc.AdapterLuid.LowPart == luidLow) {
                adapter = current;
                break;
            }
        }
        if (!adapter)
            return false;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
        return SUCCEEDED(D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 1, D3D11_SDK_VERSION, d3d.GetAddressOf(), &chosen, nullptr));
    }

    bool sharedTexture(uint32_t w, uint32_t h, ComPtr<ID3D11Texture2D>* texture, HANDLE* handle)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(d3d->CreateTexture2D(&desc, nullptr, texture->GetAddressOf())))
            return false;
        ComPtr<IDXGIResource1> resource;
        if (FAILED((*texture)->QueryInterface(IID_PPV_ARGS(resource.GetAddressOf()))))
            return false;
        return SUCCEEDED(resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, handle));
    }

    void cmdBarrier(uint64_t image, uint32_t aspect, uint32_t oldLayout, uint32_t newLayout,
                    uint32_t srcAccess, uint32_t dstAccess, uint32_t srcStage, uint32_t dstStage)
    {
        if (oldLayout == newLayout && srcAccess == dstAccess)
            return;
        Barrier item = {};
        item.sType = 45;
        item.srcAccess = srcAccess;
        item.dstAccess = dstAccess;
        item.oldLayout = oldLayout;
        item.newLayout = newLayout;
        item.srcQueue = kIgnoredQueue;
        item.dstQueue = kIgnoredQueue;
        item.image = image;
        item.aspect = aspect;
        item.levelCount = 1;
        item.layerCount = 1;
        barrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &item);
    }

    bool ensureQueue(uint32_t wanted)
    {
        if (wanted == kIgnoredQueue)
            wanted = 0;
        if (queue && family == wanted)
            return true;
        uint64_t found = 0;
        getQueue(device, wanted, 0, &found);
        if (!found)
            return false;
        if (cmdPool && destroyCmdPool)
            destroyCmdPool(device, cmdPool, nullptr);
        cmdPool = cmd = 0;
        PoolCreate pool = {};
        pool.sType = 39;
        pool.flags = 0x2;
        pool.family = wanted;
        if (createCmdPool(device, &pool, nullptr, &cmdPool) != 0)
            return false;
        CmdAlloc alloc = {};
        alloc.sType = 40;
        alloc.pool = cmdPool;
        alloc.level = 0;
        alloc.count = 1;
        if (allocCmd(device, &alloc, &cmd) != 0)
            return false;
        queue = found;
        family = wanted;
        return true;
    }

    bool pipelineReady()
    {
        if (pipeline)
            return true;
        SamplerInfo samplerInfo = {};
        samplerInfo.sType = 31;
        samplerInfo.addressU = samplerInfo.addressV = samplerInfo.addressW = 2;
        samplerInfo.maxLod = 0;
        if (createSampler(device, &samplerInfo, nullptr, &sampler) != 0)
            return false;
        ShaderInfo shaderInfo = {};
        shaderInfo.sType = 16;
        shaderInfo.codeSize = sizeof(kSpirv);
        shaderInfo.code = kSpirv;
        if (createShader(device, &shaderInfo, nullptr, &shader) != 0)
            return false;
        Binding bindings[5] = {};
        for (int i = 0; i < 4; ++i) {
            bindings[i].binding = static_cast<uint32_t>(i);
            bindings[i].type = kCombinedSampler;
            bindings[i].count = 1;
            bindings[i].stages = kStageComputeBit;
        }
        bindings[4].binding = 4;
        bindings[4].type = kStorageImage;
        bindings[4].count = 1;
        bindings[4].stages = kStageComputeBit;
        LayoutInfo layout = {};
        layout.sType = 32;
        layout.bindingCount = 5;
        layout.bindings = bindings;
        if (createSetLayout(device, &layout, nullptr, &setLayout) != 0)
            return false;
        PushRange range = {};
        range.stages = kStageComputeBit;
        range.size = sizeof(Push);
        PipeLayoutInfo pipe = {};
        pipe.sType = 30;
        pipe.setCount = 1;
        pipe.sets = &setLayout;
        pipe.pushCount = 1;
        pipe.pushes = &range;
        if (createPipeLayout(device, &pipe, nullptr, &pipeLayout) != 0)
            return false;
        ComputeInfo compute = {};
        compute.sType = 29;
        compute.stage.sType = 18;
        compute.stage.stage = kStageComputeBit;
        compute.stage.module = shader;
        compute.stage.name = "main";
        compute.layout = pipeLayout;
        compute.baseIndex = -1;
        if (createCompute(device, 0, 1, &compute, nullptr, &pipeline) != 0)
            return false;
        PoolSize sizes[2] = { { kCombinedSampler, 4 }, { kStorageImage, 1 } };
        PoolInfo pool = {};
        pool.sType = 33;
        pool.maxSets = 1;
        pool.sizeCount = 2;
        pool.sizes = sizes;
        if (createDescPool(device, &pool, nullptr, &descPool) != 0)
            return false;
        AllocSetInfo sets = {};
        sets.sType = 34;
        sets.pool = descPool;
        sets.count = 1;
        sets.layouts = &setLayout;
        return allocSets(device, &sets, &descSet) == 0;
    }
};

VulkanCopy::VulkanCopy()
    : impl(new Impl)
{
}

VulkanCopy::~VulkanCopy()
{
    delete impl;
}

bool VulkanCopy::init(AVBufferRef* vulkanDevice, int64_t luidHigh, uint32_t luidLow)
{
    if (!vulkanDevice || !vulkanDevice->data)
        return false;
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(vulkanDevice->data);
    auto* head = reinterpret_cast<DevHead*>(hw->hwctx);
    if (!head || !head->getProc || !head->device)
        return false;
    impl->instance = head->instance;
    impl->physical = head->physical;
    impl->device = head->device;
    auto getDeviceProc = reinterpret_cast<PFN_GetDev>(head->getProc(reinterpret_cast<void*>(head->instance), "vkGetDeviceProcAddr"));
    if (!getDeviceProc || !impl->loadProcs(getDeviceProc))
        return false;
    impl->memProps = reinterpret_cast<PFN_MemProps>(head->getProc(reinterpret_cast<void*>(head->instance), "vkGetPhysicalDeviceMemoryProperties"));
    auto fmt = reinterpret_cast<PFN_FmtProps>(head->getProc(reinterpret_cast<void*>(head->instance), "vkGetPhysicalDeviceFormatProperties"));
    if (!impl->memProps || !fmt)
        return false;
    uint32_t storageProps[3] = {};
    uint32_t blitProps[3] = {};
    fmt(impl->physical, kRgba16Float, storageProps);
    fmt(impl->physical, kRgba16Unorm, blitProps);
    if ((storageProps[1] & kStorageFeature) == 0)
        return false;
    if ((storageProps[1] & kBlitSrc) == 0 || (blitProps[1] & kBlitDst) == 0)
        return false;
    if (!impl->openD3d(luidHigh, luidLow))
        return false;
    return impl->pipelineReady();
}

bool VulkanCopy::canImportSharedTexture()
{
    ComPtr<ID3D11Texture2D> texture;
    HANDLE handle = nullptr;
    uint64_t image = 0;
    uint64_t memory = 0;
    uint64_t view = 0;
    const bool shared = impl->sharedTexture(16, 16, &texture, &handle);
    const bool imported = shared && impl->importHandle(handle, 16, 16, &image, &memory, &view);
    if (view)
        impl->destroyView(impl->device, view, nullptr);
    if (image)
        impl->destroyImage(impl->device, image, nullptr);
    if (memory)
        impl->freeMem(impl->device, memory, nullptr);
    if (handle)
        CloseHandle(handle);
    return imported;
}

VulkanCopyResult VulkanCopy::copy(AVFrame* frame, const std::function<bool()>& stop)
{
    VulkanCopyResult result;
    if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->data[0] || !frame->hw_frames_ctx)
        return result;
    auto* view = reinterpret_cast<FrameView*>(frame->data[0]);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(frames->sw_format);
    if (!desc || desc->nb_components < 3 || frame->width < 1 || frame->height < 1)
        return result;

    if (impl->width != frame->width || impl->height != frame->height) {
        for (Impl::Slot& slot : impl->slots) {
            if (slot.view)
                impl->destroyView(impl->device, slot.view, nullptr);
            if (slot.image)
                impl->destroyImage(impl->device, slot.image, nullptr);
            if (slot.memory)
                impl->freeMem(impl->device, slot.memory, nullptr);
            slot.view = slot.image = slot.memory = 0;
            slot.layout = 0;
            if (slot.handle)
                CloseHandle(slot.handle);
            slot.handle = nullptr;
            slot.texture.Reset();
        }
        if (impl->storageView)
            impl->destroyView(impl->device, impl->storageView, nullptr);
        if (impl->storageImage)
            impl->destroyImage(impl->device, impl->storageImage, nullptr);
        if (impl->storageMem)
            impl->freeMem(impl->device, impl->storageMem, nullptr);
        impl->storageView = impl->storageImage = impl->storageMem = 0;
        impl->storageLayout = 0;
        if (!impl->makeLocal(static_cast<uint32_t>(frame->width), static_cast<uint32_t>(frame->height),
                             kRgba16Float, kUsageStorage | kUsageTransferSrc,
                             &impl->storageImage, &impl->storageMem)
            || !impl->makeView(impl->storageImage, kRgba16Float, kAspectColor, &impl->storageView))
            return result;
        if (!impl->dummyImage) {
            if (!impl->makeLocal(1, 1, kR16, kUsageSampled | kUsageTransferDst, &impl->dummyImage, &impl->dummyMem)
                || !impl->makeView(impl->dummyImage, kR16, kAspectColor, &impl->dummyView))
                return result;
        }
        for (int i = 0; i < kSlots; ++i) {
            if (!impl->busy[i])
                impl->busy[i] = std::make_shared<std::atomic<int>>(0);
            if (!impl->sharedTexture(static_cast<uint32_t>(frame->width), static_cast<uint32_t>(frame->height),
                                     &impl->slots[i].texture, &impl->slots[i].handle))
                return result;
            if (!impl->importHandle(impl->slots[i].handle, static_cast<uint32_t>(frame->width),
                                    static_cast<uint32_t>(frame->height),
                                    &impl->slots[i].image, &impl->slots[i].memory, &impl->slots[i].view))
                return result;
        }
        impl->width = frame->width;
        impl->height = frame->height;
    }

    int slot = -1;
    for (;;) {
        if (stop && stop()) {
            result.interrupted = true;
            return result;
        }
        for (int i = 0; i < kSlots; ++i) {
            int expected = 0;
            if (impl->busy[i]->compare_exchange_strong(expected, 1)) {
                slot = i;
                break;
            }
        }
        if (slot >= 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const bool multi = view->img[1] == 0;
    const int32_t* fmts = av_vkfmt_from_pixfmt(frames->sw_format);
    uint64_t images[4] = {};
    uint64_t views[4] = {};
    uint32_t aspects[4] = {};
    bool ownView[4] = {};
    const int planes = desc->nb_components < 4 ? desc->nb_components : 4;
    for (int i = 0; i < 4; ++i) {
        if (i >= planes) {
            images[i] = impl->dummyImage;
            views[i] = impl->dummyView;
            aspects[i] = kAspectColor;
            continue;
        }
        images[i] = multi ? view->img[0] : view->img[i];
        if (!images[i]) {
            impl->busy[slot]->store(0);
            return result;
        }
        uint32_t format = kR16;
        if (fmts && fmts[i] > 0 && fmts[i] < 1000)
            format = static_cast<uint32_t>(fmts[i]);
        aspects[i] = multi ? (kAspectPlane0 << i) : kAspectColor;
        if (!impl->makeView(images[i], format, aspects[i], &views[i])) {
            impl->busy[slot]->store(0);
            return result;
        }
        ownView[i] = true;
    }

    uint32_t family = view->queueFamily[0];
    if (!impl->ensureQueue(family)) {
        for (int i = 0; i < 4; ++i)
            if (ownView[i])
                impl->destroyView(impl->device, views[i], nullptr);
        impl->busy[slot]->store(0);
        return result;
    }

    if (impl->resetCmd(impl->cmd, 0) != 0) {
        for (int i = 0; i < 4; ++i)
            if (ownView[i])
                impl->destroyView(impl->device, views[i], nullptr);
        impl->busy[slot]->store(0);
        return result;
    }
    BeginInfo begin = {};
    begin.sType = 42;
    begin.flags = 0x1;
    if (impl->beginCmd(impl->cmd, &begin) != 0) {
        for (int i = 0; i < 4; ++i)
            if (ownView[i])
                impl->destroyView(impl->device, views[i], nullptr);
        impl->busy[slot]->store(0);
        return result;
    }

    if (!impl->dummyReady) {
        impl->cmdBarrier(impl->dummyImage, kAspectColor, 0, kLayoutTransferDst, 0, kAccessTransferWrite, kStageTop, kStageTransfer);
        const float white[4] = { 1.f, 1.f, 1.f, 1.f };
        Subres range = { kAspectColor, 0, 1, 0, 1 };
        impl->clearColor(impl->cmd, impl->dummyImage, kLayoutTransferDst, white, 1, &range);
        impl->cmdBarrier(impl->dummyImage, kAspectColor, kLayoutTransferDst, kLayoutShaderRead,
                         kAccessTransferWrite, kAccessShaderRead, kStageTransfer, kStageCompute);
        impl->dummyReady = true;
    }

    auto planeAspect = [&](int index) {
        return (index < planes && multi) ? (kAspectPlane0 << index) : kAspectColor;
    };
    for (int i = 0; i < planes; ++i) {
        const uint32_t old = static_cast<uint32_t>(std::max(0, view->layout[multi ? 0 : i]));
        impl->cmdBarrier(images[i], planeAspect(i), old, kLayoutShaderRead,
                         0, kAccessShaderRead, kStageTop, kStageCompute);
    }
    impl->cmdBarrier(impl->storageImage, kAspectColor, impl->storageLayout, kLayoutGeneral,
                     impl->storageLayout == 0 ? 0 : kAccessTransferRead, kAccessShaderWrite,
                     impl->storageLayout == 0 ? kStageTop : kStageTransfer, kStageCompute);

    ImageDesc infos[5] = {};
    for (int i = 0; i < 4; ++i) {
        infos[i].sampler = impl->sampler;
        infos[i].view = views[i];
        infos[i].layout = kLayoutShaderRead;
    }
    infos[4].view = impl->storageView;
    infos[4].layout = kLayoutGeneral;
    WriteSet writes[5] = {};
    for (int i = 0; i < 5; ++i) {
        writes[i].sType = 35;
        writes[i].dstSet = impl->descSet;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].count = 1;
        writes[i].type = i == 4 ? kStorageImage : kCombinedSampler;
        writes[i].image = &infos[i];
    }
    impl->writeSets(impl->device, 5, writes, 0, nullptr);
    impl->bindPipe(impl->cmd, 1, impl->pipeline);
    impl->bindSets(impl->cmd, 1, impl->pipeLayout, 0, 1, &impl->descSet, 0, nullptr);

    const int depth = desc->comp[0].depth > 0 ? desc->comp[0].depth : 10;
    const int shift = std::max(0, desc->comp[0].shift);
    const float maxCode = static_cast<float>((1 << depth) - 1);
    const bool full = frame->color_range == AVCOL_RANGE_JPEG;
    const float black = full ? 0.f : (depth >= 12 ? 256.f : depth >= 10 ? 64.f : 16.f);
    const float mid = full ? (maxCode + 1.f) * 0.5f : (depth >= 12 ? 2048.f : depth >= 10 ? 512.f : 128.f);
    const float range = full ? maxCode : (depth >= 12 ? 3504.f : depth >= 10 ? 876.f : 219.f);
    Push push = {};
    push.w = frame->width;
    push.h = frame->height;
    push.chromaX = desc->log2_chroma_w;
    push.chromaY = desc->log2_chroma_h;
    push.hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) ? 1 : 0;
    push.scale = 65535.f / static_cast<float>(1 << shift);
    push.black = black;
    push.mid = mid;
    push.range = range > 0.f ? range : 1.f;
    push.alphaMax = maxCode > 0.f ? maxCode : 1.f;
    impl->pushConst(impl->cmd, impl->pipeLayout, kStageComputeBit, 0, sizeof(push), &push);
    impl->dispatch(impl->cmd, static_cast<uint32_t>((frame->width + 7) / 8), static_cast<uint32_t>((frame->height + 7) / 8), 1);

    for (int i = 0; i < planes; ++i) {
        const uint32_t old = static_cast<uint32_t>(std::max(0, view->layout[multi ? 0 : i]));
        impl->cmdBarrier(images[i], planeAspect(i), kLayoutShaderRead, old,
                         kAccessShaderRead, 0, kStageCompute, kStageTop);
    }
    impl->cmdBarrier(impl->storageImage, kAspectColor, kLayoutGeneral, kLayoutTransferSrc,
                     kAccessShaderWrite, kAccessTransferRead, kStageCompute, kStageTransfer);
    Impl::Slot& target = impl->slots[slot];
    impl->cmdBarrier(target.image, kAspectColor, target.layout, kLayoutTransferDst,
                     target.layout == 0 ? 0 : kAccessShaderRead, kAccessTransferWrite,
                     target.layout == 0 ? kStageTop : kStageCompute, kStageTransfer);
    Blit region = {};
    region.src.aspect = kAspectColor;
    region.src.layerCount = 1;
    region.srcOff[1] = { frame->width, frame->height, 1 };
    region.dst.aspect = kAspectColor;
    region.dst.layerCount = 1;
    region.dstOff[1] = { frame->width, frame->height, 1 };
    impl->blit(impl->cmd, impl->storageImage, kLayoutTransferSrc, target.image, kLayoutTransferDst, 1, &region, 1);
    impl->cmdBarrier(target.image, kAspectColor, kLayoutTransferDst, kLayoutGeneral,
                     kAccessTransferWrite, kAccessShaderRead, kStageTransfer, kStageCompute);
    impl->cmdBarrier(impl->storageImage, kAspectColor, kLayoutTransferSrc, kLayoutGeneral,
                     kAccessTransferRead, kAccessShaderWrite, kStageTransfer, kStageCompute);
    impl->storageLayout = kLayoutGeneral;
    target.layout = kLayoutGeneral;

    uint64_t sems[8] = {};
    uint64_t values[8] = {};
    uint32_t stages[8] = {};
    int semCount = 0;
    const int semPlanes = multi ? 1 : planes;
    for (int i = 0; i < semPlanes; ++i) {
        if (!view->sem[i])
            continue;
        bool seen = false;
        for (int s = 0; s < semCount; ++s)
            seen = seen || sems[s] == view->sem[i];
        if (seen)
            continue;
        sems[semCount] = view->sem[i];
        values[semCount] = view->semValue[i];
        stages[semCount] = kStageCompute;
        ++semCount;
    }
    uint64_t signals[8] = {};
    for (int i = 0; i < semCount; ++i)
        signals[i] = values[i] + 1;

    if (impl->endCmd(impl->cmd) != 0) {
        for (int i = 0; i < 4; ++i)
            if (ownView[i])
                impl->destroyView(impl->device, views[i], nullptr);
        impl->busy[slot]->store(0);
        return result;
    }

    TimelineInfo timeline = {};
    timeline.sType = 1000207003;
    timeline.waitCount = static_cast<uint32_t>(semCount);
    timeline.waitValues = values;
    timeline.signalCount = static_cast<uint32_t>(semCount);
    timeline.signalValues = signals;
    SubmitInfo submit = {};
    submit.sType = 4;
    submit.pNext = semCount > 0 ? &timeline : nullptr;
    submit.waitCount = static_cast<uint32_t>(semCount);
    submit.waits = sems;
    submit.waitStages = stages;
    submit.cmdCount = 1;
    submit.cmds = &impl->cmd;
    submit.signalCount = static_cast<uint32_t>(semCount);
    submit.signals = sems;
    if (impl->submit(impl->queue, 1, &submit, 0) != 0 || impl->waitIdle(impl->queue) != 0) {
        for (int i = 0; i < 4; ++i)
            if (ownView[i])
                impl->destroyView(impl->device, views[i], nullptr);
        impl->busy[slot]->store(0);
        return result;
    }
    for (int i = 0; i < 8; ++i) {
        for (int s = 0; s < semCount; ++s) {
            if (view->sem[i] == sems[s])
                view->semValue[i] = signals[s];
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (ownView[i])
            impl->destroyView(impl->device, views[i], nullptr);
    }

    std::shared_ptr<std::atomic<int>> flag = impl->busy[slot];
    result.ok = true;
    result.handle = target.handle;
    result.slot = std::shared_ptr<void>(nullptr, [flag](void*) { flag->store(0); });
    return result;
}
