#include "render/GpuTimer.h"
#include "core/Logger.h"

#include <cstring>
#include <vector>

namespace render {

namespace {
// 单帧最多容忍的 GPU 总耗时（毫秒）。超过说明时戳回绕或读到了垃圾，
// 直接丢弃这一帧的读数而不是把 500ms 的"耗时"显示给用户。
constexpr float kSanityMaxMs = 500.0f;
}  // namespace

const char* GpuTimer::sectionName(uint32_t section) {
    static const char* kNames[kSectionCount] = {
        "shadows", "local_sh",   "depth",  "decals",  "ssao", "cluster",
        "forward", "particles",  "framegraph", "offscreen", "post",
    };
    return section < kSectionCount ? kNames[section] : "?";
}

GpuTimer::GpuTimer(rhi::Device& device, uint32_t framesInFlight)
    : m_device(device.get()), m_frames(framesInFlight) {
    // 图形队列族的 timestampValidBits：0 = 该队列不支持时戳（VUID
    // 要求 vkCmdWriteTimestamp 的 stage 必须在该队列支持）。
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &familyCount,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(device.physical(),
                                                 &familyCount, families.data());
    }
    const uint32_t family = device.graphicsFamily();
    if (family >= familyCount ||
        families[family].timestampValidBits == 0) {
        VK_LOG_INFO("GPU-TIMER unsupported (queue family %u has no "
                    "timestamps) -- profiling disabled",
                    family);
        return;
    }
    const uint32_t bits = families[family].timestampValidBits;
    m_validMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
    m_periodNs = device.properties().limits.timestampPeriod;

    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = framesInFlight * kSectionCount * 2;
    if (vkCreateQueryPool(m_device, &ci, nullptr, &m_pool) != VK_SUCCESS) {
        VK_LOG_INFO("GPU-TIMER vkCreateQueryPool failed -- profiling disabled");
        m_pool = VK_NULL_HANDLE;
        return;
    }
    VK_LOG_INFO("GPU-TIMER ready: %u frames x %u sections, period %.3f ns, "
                "valid bits %u",
                framesInFlight, kSectionCount,
                static_cast<double>(m_periodNs), bits);
}

GpuTimer::~GpuTimer() {
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

void GpuTimer::resetFrame(VkCommandBuffer cmd, uint32_t frameIndex) const {
    if (!m_pool || frameIndex >= m_frames) return;
    vkCmdResetQueryPool(cmd, m_pool, queryBase(frameIndex, 0),
                        kSectionCount * 2);
}

void GpuTimer::begin(VkCommandBuffer cmd, uint32_t frameIndex,
                     uint32_t section) const {
    if (!m_pool || frameIndex >= m_frames || section >= kSectionCount) return;
    // TOP_OF_PIPE：段的起点不受前面阶段忙碌与否影响；end 用
    // BOTTOM_OF_PIPE 保证前面所有阶段都完工。两者之差 = 段墙钟耗时。
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool,
                        queryBase(frameIndex, section));
}

void GpuTimer::end(VkCommandBuffer cmd, uint32_t frameIndex,
                   uint32_t section) const {
    if (!m_pool || frameIndex >= m_frames || section >= kSectionCount) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool,
                        queryBase(frameIndex, section) + 1);
}

bool GpuTimer::readFrame(uint32_t frameIndex) {
    m_hasLast = false;
    m_lastTotal = -1.0f;
    if (!m_pool || frameIndex >= m_frames) return false;
    // 只读"提交过至少一次"的 slot：fence 初始是 signaled 的，头几帧
    // 该 slot 的命令缓冲还没提交过，query 处于 uninitialized 状态 ——
    // 此时调 vkGetQueryPoolResults 是违规的（VUID-...-09401）。
    if (!m_submitted[frameIndex]) return false;

    uint64_t stamps[kSectionCount * 2];
    const VkResult r = vkGetQueryPoolResults(
        m_device, m_pool, queryBase(frameIndex, 0), kSectionCount * 2,
        sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return false;  // VK_NOT_READY：首帧 / 未提交

    for (uint32_t s = 0; s < kSectionCount; ++s) {
        const uint64_t t0 = stamps[s * 2] & m_validMask;
        const uint64_t t1 = stamps[s * 2 + 1] & m_validMask;
        // 同帧内回绕几乎不可能；发生了就当 0 处理
        m_last[s] = t1 >= t0
                        ? static_cast<float>(
                              static_cast<double>(t1 - t0) * m_periodNs /
                              1e6)
                        : 0.0f;
    }
    const uint64_t f0 = stamps[0] & m_validMask;
    const uint64_t f1 = stamps[kSectionCount * 2 - 1] & m_validMask;
    m_lastTotal = f1 >= f0
                      ? static_cast<float>(
                            static_cast<double>(f1 - f0) * m_periodNs / 1e6)
                      : -1.0f;
    if (m_lastTotal < 0.0f || m_lastTotal > kSanityMaxMs) return false;
    m_hasLast = true;
    return true;
}

float GpuTimer::lastMs(uint32_t section) const {
    if (!m_hasLast || section >= kSectionCount) return -1.0f;
    return m_last[section];
}

float GpuTimer::lastTotalMs() const { return m_hasLast ? m_lastTotal : -1.0f; }

}  // namespace render
