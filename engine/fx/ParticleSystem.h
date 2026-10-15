#pragma once
// ============================================================
// fx/ParticleSystem —— GPU 粒子系统（R6-1）
//
// ---- 要解决的问题 ----
//   烟、火、水花、魔法、碎屑 —— 这类效果的特点是"数量大、个体小、每帧
//   都要变"。CPU 每帧把上万个粒子的位置/速度/颜色算一遍再塞回显存，
//   带宽和提交开销都比真正的渲染还贵。所以整条链路放到 GPU：
//
//     ① 模拟（compute）：出生 / 老化 / 死亡 / 积分（重力 + 阻尼）/
//        曲线求值（尺寸、透明度、颜色）全部在 particle_sim.comp 里做。
//        每个槽位一线程，"该不该在此时出生"由**槽位自己的相位**推出 ——
//        无状态、与帧率无关、给定帧号就能复现（验证脚本依赖这一点）。
//     ② 紧凑化（compute）：活着的粒子用 atomicAdd 挤进 alive[] 表，
//        存活的槽位号连续排在前面。
//     ③ 计数（compute）：一个线程读存活数，写 VkDrawIndirectCommand。
//        —— 这样 CPU 一个粒子都不用数，绘制量完全由 GPU 决定。
//     ④ 绘制（graphics）：一次 vkCmdDrawIndirect 画完全部粒子。
//        实例号 → alive[] → 粒子记录 → 攒一个 billboard（或沿速度拉伸）。
//
// ---- 为什么"每槽位一个相位"而不是"每帧发 n 个" ----
//   常见的写法是"本帧该发 n 颗，就往前 n 个空位写"。那个写法要求 CPU
//   或一个串行 compute 先算出 n、再分配下标，而且槽位的复用顺序与
//   rate 强耦合。这里换个角度：**把每个槽位看成一个独立的周期性发射点**。
//
//     周期 period = capacity / rate
//     槽位 j 的发射时刻 = (k + (j + 0.5) / capacity) * period , k = 0,1,2,…
//
//   于是"本帧该不该发"就是"本帧区间里有没有整数 k"，一次除法取整就能
//   判定，不需要任何跨线程通信；发射在时间上是**均匀**的（相邻槽位差
//   一个 period/capacity），在容量上也是轮转的（每个槽位周期性地被复用）。
//   代价是每个槽位的首次发射时刻不同 → 前 lifetime 秒是"渐入"的，
//   这恰好与真实喷泉的起势一致。
//
// ---- 软粒子为什么单独一个 pass，而不在前向里画 ----
//   软粒子要读**场景深度**：粒子离墙越近越透明，免得硬切出一个方块。
//   前向 pass 里深度还是深度附件（同一 subpass 不能既当附件又当纹理采样），
//   所以粒子被放到前向之后的独立 pass，目标是 HDR 的 resolve 图，
//   深度从 HdrTarget 的 depthSampledView 采（SSAO / 体积雾同一条路子）。
//   没有深度附件 → 遮挡判定在片元里显式做（与软化用的是同一个深度差，
//   本来就是一件事）。
//
// ---- 默认关 = 基线逐像素不变 ----
//   fx.enabled 关着时：不建任何资源、不录任何 pass、一个粒子都不存在 →
//   画面与引入本模块之前逐位一致。资源也是**懒建**的（首次启用才分配）。
//
// ---- 已知取舍（v1，都有明确升级路径）----
//   · 只在**主视口**录（与 DBuffer 延迟贴花同一条约定）：粒子的池子与
//     计数器是按帧在途共享的，离屏目标（预览 / 多视口）复用同一份会
//     把主视口那一份写坏。升级路径 = 每个目标一份池（R7-1 的资源声明）。
//   · 粒子默认不排序：加色混合与顺序无关，Alpha 混合的默认顺序是槽位
//     顺序（近似的环形顺序）。需要正确顺序时把 Renderer > Sort Mode
//     设成 ByDistance —— 那一步由 CPU 在紧凑化之后做一次间接排序，
//     只在 Alpha 混合的发射器上生效（加色排序纯属浪费）。
//   · 不做碰撞：粒子穿过地面就穿过去了。碰撞要回读深度或场景 AABB 表，
//     属于"速度场 / 碰撞"那一档（路线图把它列在同一项里，但作为后续）。
//   · 不做 Sub Emitters / Trails / Texture Sheet Animation / Collision /
//     Triggers —— Unity 里这几个模块需要额外的池与事件通道，属于
//     后续任务；其余模块（Main / Emission / Shape / Velocity /
//     Limit Velocity / Force / Color / Size / Rotation / Noise /
//     Renderer）都已实现并接到面板。
//   · 池耗尽（rate × lifetime > capacity）时槽位会被提前复用 → 粒子
//     提前"跳"到新的出生点。Renderer 检测到这个条件会打一行 WARN。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class ComputePipeline;
class Pipeline;
class Buffer;
class DescriptorAllocator;
} // namespace rhi

namespace fx {

// 一个粒子的 GPU 记录（std430，80 字节）。
//
// ⚠ 改这里必须同步改 assets/shaders/particle_common.glsl 的 `struct Particle`
//   —— 下面有一条 static_assert 钉住大小，但**字段顺序与语义**只能靠人同步。
struct ParticleGpu {
    // xyz = 世界坐标（米）；w = 当前尺寸（米，已乘曲线）
    glm::vec4 posSize;
    // xyz = 速度（米/秒）；w = 已存活秒数。**< 0 表示这个槽位是死的** ——
    // 用"负数年龄"而不是单独一个 flag：判定与读取是同一份数据，
    // 不可能出现"标记说活着、年龄是垃圾"的状态。
    glm::vec4 velLife;
    // rgb = 颜色（线性 HDR，已乘自发光强度）；a = 不透明度（已乘曲线）
    glm::vec4 color;
    // x = 总寿命（秒）；y = 随机种子（调试复现用）；z = 拉伸长度（米）；
    // w = 所属发射器下标（诊断用）
    glm::vec4 misc;
    // x = 当前旋转（度，Rotation over Lifetime 积分出来的）；
    // yzw = 保留。
    // ⚠ v1 只有 4 个 vec4，加这一块是因为旋转必须有逐粒子的存储，
    //   而原来 64 字节一个空位都没有。80 B × 4096 槽位 = 320 KB。
    glm::vec4 extra;
};
static_assert(sizeof(ParticleGpu) == 80,
              "ParticleGpu 布局变了，必须同步 particle_common.glsl 的 Particle");

// 一个发射器的 GPU 记录（std430，624 字节）。
// 与 particle_common.glsl 里的 `struct Emitter` 逐字对齐。
//
// ⚠ 全部用 vec4 打包：std430 里 vec4 的步长恒为 16，所以"字段顺序"就是
//   "偏移量"，两边照着同一份清单写就不会错。散装 float/int 会引入
//   4 / 8 字节的对齐与尾填充，是 layout 错位最常见的来源。
//
// 分组与 ecs::ParticleEmitterComponent / InspectorPanel 的模块一一对应。
struct ParticleEmitterGpu {
    // ---- 变换与位置 ----
    glm::mat4 world;         //   0  发射器局部 → 世界（形状采样与方向都在局部做）
    glm::vec4 origin;        //  64  xyz = 世界坐标；w = 形状半径
    // ---- 物理 ----
    glm::vec4 gravityDrag;   //  80  xyz = 重力（米/秒²）；w = 阻尼系数（1/秒）
    glm::vec4 lifeSpeed;     //  96  x = 寿命 y = 寿命方差 z = 初速 w = 速度方差
    glm::vec4 shapeExtents;  // 112  xyz = 盒体半轴（米）；w = 扩散半角 cos 阈值
    // ---- 曲线 ----
    glm::vec4 sizeCurve;     // 128  尺寸曲线 4 个控制点（t = 0, 1/3, 2/3, 1）
    glm::vec4 alphaCurve;    // 144  不透明度曲线 4 个控制点
    // ---- Main / Playback ----
    glm::vec4 play;          // 160  x = duration y = looping z = prewarm w = startDelay
    // ---- Emission ----
    glm::vec4 emit;          // 176  x = rate y = rateOverDistance z = 形状枚举
                             //      w = emitFrom（0 Volume / 1 Edge / 2 Shell）
    glm::vec4 emit2;         // 192  x = 形状旋转(度) y = radiusThickness
                             //      z = arc(度) w = simulationSpace
    // ---- Shape 附加 ----
    // Simulation Space = Local 时，重力必须换到**发射器局部空间**再积分
    //（粒子存的是局部坐标）。CPU 直接算好传过来，省掉每粒子一次
    // mat3 逆矩阵 —— inverse() 在 GPU 上要十几条指令，而它对整个发射器
    // 是同一个值。
    glm::vec4 shapeAux;      // 208  xyz = 发射器局部空间里的重力
                             //      w = speedModifier 曲线是否启用(0/1)
    // ---- Velocity over Lifetime ----
    glm::vec4 velX;          // 224  x / y / z / w = 轴向速度曲线的 4 个控制点
    glm::vec4 velY;          // 240
    glm::vec4 velZ;          // 256
    glm::vec4 speedMod;      // 272  速度倍率曲线 4 个控制点
    glm::vec4 orbital;       // 288  xyz = 轨道角速度（度/秒）w = 轨道偏移（米）
    // ---- Force / Limit Velocity ----
    glm::vec4 force;         // 304  xyz = 力（米/秒²）w = 随机化比例
    glm::vec4 limitVel;      // 320  x = 速度上限 y = dampen z = 开关(0/1)
                             //      w = 径向速度 radial（米/秒）
    // ---- Rotation over Lifetime ----
    glm::vec4 rot;           // 336  x = 初始旋转(度) y = 初始随机幅度(度)
                             //      z = 角速度(度/秒) w = 角速度随机比例
    // ---- Noise ----
    glm::vec4 noiseA;        // 352  x = 强度 y = 频率 z = 滚动速度 w = 阻尼
    glm::vec4 noiseB;        // 368  x = 开关(0/1) y = 倍频层数
                             //      z = velOverLife 用世界空间(0/1) w = 保留
    // ---- Renderer ----
    glm::vec4 miscParams;    // 384  x = 基础尺寸（米）y = 混合（0 加色 1 Alpha）
                             //      z = 拉伸系数 w = 自发光
    glm::vec4 renderMisc;    // 400  x = 渲染模式 y = 排序模式
                             //      z = 渐变停靠点数 w = 有效的 burst 条数
    // ---- Rate over Distance ----
    // 需要"累积走了多远"这个量：发射判据从"时间区间里有没有整数 k"
    // 变成"距离区间里有没有整数 k"，所以 CPU 每帧把累积距离写进来。
    glm::vec4 dist;          // 416  x = 本帧累积距离 y = 上一步的累积距离
                             //      z = 启动延迟结束时记下的基准距离 w = 保留
    // ---- Color over Lifetime：最多 8 个停靠点（xyz = 颜色，w = 位置）----
    glm::vec4 ramp[8];       // 432..544
    // ---- Bursts：最多 4 条（time, count, cycles, interval）----
    glm::vec4 bursts[4];     // 560..608
};
// std430：mat4 步长 64、vec4 步长 16 → 64 + 23×16 + 8×16 + 4×16 = 624。
static_assert(sizeof(ParticleEmitterGpu) == 624,
              "ParticleEmitterGpu 布局变了，必须同步 particle_sim.comp 的 Emitter");

// 两种混合各一张紧凑表 + 一条间接命令。
//   0 = 加色（Additive），1 = Alpha 混合
//
// 为什么不是"都画一遍"：混合模式是管线的固定状态，一个 draw 只能有一种。
// 把两种粒子分开紧凑化之后，两条间接绘制各自只画自己那一类 ——
// 空的那条 instanceCount = 0，驱动直接跳过。
static constexpr uint32_t kBlendModeCount = 2;

// 排序用的深度 / 年龄分档数（Renderer > Sort Mode != None 时生效）。
//
// 为什么是"桶排序"而不是精确排序：GPU 上对 3 万个元素做精确排序要么上
// 基数排序（多趟 + 直方图 + 前缀和 + 散射，代码量与带宽都不小），要么
// 回读到 CPU（那正是本模块要消掉的瓶颈）。64 档桶排序只需要
// 直方图 + 前缀和 + 散射三趟，而且**桶内顺序无关紧要** —— 半透明粒子的
// 排序本来就是"视觉上大致从远到近"，64 档的深度分辨率已经远超肉眼可辨。
static constexpr uint32_t kSortBuckets = 64;

// 粒子系统的全局设置。前六项接到 FeatureRegistry（Engine 面板 + env），
// 后两项是"装机参数"（容量与模拟频率），只走 env 与代码默认值。
struct ParticleSettings {
    bool enabled = false;        // 总开关（默认关 = 基线不变）
    float sizeScale = 1.0f;      // 全局尺寸倍率（乘在每个粒子上）
    float softness = 1.0f;       // 全局软化距离倍率
    float intensity = 1.0f;      // 全局亮度倍率（乘在颜色上）
    // 冻结帧：> 0 时模拟推进到这一帧就停住（不再派发 compute），
    // 画面随之完全确定 —— 自动化验证需要"同一配置两次运行逐像素一致"，
    // 而实时模拟的帧号在抓拍时无法对齐。**0 = 不冻结**（正常使用）。
    // 与 MYVK_TAA_PHASE 冻结抖动相位是同一条取舍。
    int freezeFrame = 0;

    // 容量与频率（决定显存与显存带宽，属于"装机参数"）
    uint32_t perEmitterCapacity = 512;  // 每个发射器有多少个槽位
    uint32_t maxEmitters = 8;           // 最多同时模拟几个发射器
    // 模拟步长（Hz）。固定步长是**确定性**的前提 —— 用真实帧间隔的话，
    // 同一份场景在两次运行里会得到不同的粒子状态（帧率本来就不一样）。
    // 推进由 Renderer 的**墙钟累加器**驱动：每帧把真实 dt 攒进来，攒够
    // 1/simHz 才走一步，所以模拟速度与渲染帧率无关（Unity / Blender 同）。
    // 冻结帧（MYVK_FX_FREEZE=N）因此表示"第 N 步之后"，跨机器可复现。
    uint32_t simHz = 60;
};

class ParticleSystem {
public:
    ParticleSystem(rhi::Device& device, rhi::CommandPool& cmdPool,
                   const ParticleSettings& settings,
                   uint32_t framesInFlight = 2);
    ~ParticleSystem();

    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // ---- 资源生命周期（内部含 waitIdle；只在"首次启用 / 容量变了"时调）----
    bool ensureResources();
    void destroyResources();
    bool ready() const { return m_ready; }
    void invalidateResources() { m_needRebuild = true; }

    // 改设置。动**显存布局**的字段（容量 / 发射器上限）会标记重建，
    // 其余（开关 / 倍率 / 冻结帧）只影响上传与着色。
    void setSettings(const ParticleSettings& s);

    // 与 HdrTarget 的重建同步：分辨率 / 采样数 / 视图变了就重建
    // framebuffer 与管线（片元 .spv 按 MSAA 选）。**调用方负责
    // waitIdle**（与 DecalBuffer / VolumetricFog 的 recreate 同一约定）。
    void recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                  const std::vector<VkImageView>& colorViews,
                  const std::vector<VkImageView>& depthViews);

    // ---- 每帧 ----
    // 写本帧的发射器表。emitters.size() 会被夹到 maxEmitters。
    //
    // ⚠ burst 已经**完全由时间轴驱动**（Emitter.bursts 数组 + 计数），
    //   不再需要"这个 frame-in-flight 槽的第一次模拟"这种 CPU 侧的标记 ——
    //   那个标记是 v1 的权宜之计（只有 0 号槽能看见爆发）。现在每条 burst
    //   在自己的时刻命中 (t-dt, t] 区间，所有槽天然一致。
    void upload(const std::vector<ParticleEmitterGpu>& emitters,
                uint32_t frameIndex, bool resetCounters = true);
    // 回读上一轮同槽的存活数 / 本帧发射数（fence 之后调，见 Renderer）。
    void pollStats(uint32_t frameIndex);
    // 模拟 + 紧凑化 + 计数（compute ×2）。pushData = SimPush 的地址。
    //
    // ⚠ 调用方**每帧都会调**，即使本帧一步模拟都没推进（渲染帧率高于
    //   simHz 时这是常态）：upload() 每帧清了计数器，而计数器要靠这一趟
    //   compute 重新累加出来，跳过会让计数器停在 0。
    //   所以 `stats().simDispatches` 数的是**派发次数**，不是推进的步数 ——
    //   推进了几步看 `simFrame`（FX-DUMP 里两者并排打出来，
    //   "派发远多于步数"正是模拟与帧率解耦的直接证据）。
    struct SimPush {
        // x = dt（秒）—— **本槽自上次模拟以来经过的时间**，不是单帧 dt！
        //     池按槽分开 → 每个槽每 framesInFlight 帧才被派发一次，所以
        //     这里必须一次补上漏掉的那些时间步（见 Renderer 里 push 的
        //     组装处）。传单帧 dt 会让模拟以 1/framesInFlight 的速度爬行，
        //     而且**看不出来**：粒子只是慢慢地飘。
        // y = 模拟时刻（秒，= 帧号 × 单帧 dt。**整条时间轴**，不是本槽的）
        // z = 模拟帧号（诊断）
        // w = 发射器数
        glm::vec4 timing;
        // x = 每发射器容量 y = 总槽数 z = 保留 w = 保留
        glm::vec4 pool;
    };
    void recordSimulation(VkCommandBuffer cmd, uint32_t frameIndex,
                          const SimPush& push, const glm::vec3& camPos,
                          int sortMode);
    // 画：自己的 render pass，目标是主视口 HDR 的 resolve 图。
    void draw(VkCommandBuffer cmd, uint32_t frameIndex, VkBuffer frameUbo,
              const glm::vec2& screenSize, float nearZ, float farZ);

    // 总槽数（= perEmitterCapacity × maxEmitters）
    uint32_t capacity() const {
        return m_settings.perEmitterCapacity * m_settings.maxEmitters;
    }

    // 本帧的最大软化距离（米）。Renderer 从各发射器组件的 softDistance
    // 里取最大值传进来 —— 逐粒子携带软化距离要多花 4 B × 上万粒子，
    // 而粒子之间的软化差异远小于"有 / 无软化"的差异。
    void setSoftDistance(float meters) { m_softBase = meters; }

    // ---- 诊断（FX-DUMP 日志的数据源）----
    struct Stats {
        uint32_t emitters = 0;        // 本帧实际模拟的发射器数
        uint32_t capacity = 0;        // 总槽数
        uint32_t alive = 0;           // 上一轮回读的存活粒子数（GPU 计数）
        uint32_t spawned = 0;         // 上一轮回读的本帧发射数（GPU 计数）
        uint32_t maxAlive = 0;        // 进程内峰值存活数
        uint32_t draws = 0;           // 本帧间接绘制的实例数（= CPU 侧镜像）
        uint32_t dropped = 0;         // 超出 maxEmitters 被丢弃的发射器数
        uint64_t simDispatches = 0;    // 累计派发的模拟次数（不是推进的步数）
        bool readbackValid = false;
        // GPU 真正写进间接命令的实例数（回读 indirect buffer 的
        // instanceCount 字段）。与 alive 分开看是有意义的：alive 来自
        // 计数器，这一项来自"绘制真的会读的那个地方"—— 两者不一致就
        // 说明计数 pass 或屏障有问题（画面会是"一颗粒子都没有"）。
        uint32_t instAdditive = 0;
        uint32_t instAlpha = 0;
    };
    const Stats& stats() const { return m_stats; }

private:
    struct Frame {
        VkFramebuffer fb = VK_NULL_HANDLE;
        std::unique_ptr<rhi::Buffer> emitters;   // 常驻映射
        void* emittersMapped = nullptr;
        std::unique_ptr<rhi::Buffer> particles;  // DEVICE_LOCAL（只被 GPU 碰）
        // 两张紧凑表：[0] 加色 [1] Alpha，各 capacity() 个 uint
        std::unique_ptr<rhi::Buffer> alive;
        // 排序后的紧凑表（Sort Mode != None 时由计数那一趟写）。
        // 顶点着色器按 push 常量的开关在它和 alive 之间选一张读 ——
        // 所以**不需要**为排序重录任何绘制命令。
        std::unique_ptr<rhi::Buffer> aliveSorted;
        // 两条间接绘制命令（[0] 加色 [1] Alpha），每条 16 字节
        std::unique_ptr<rhi::Buffer> indirect;
        // [0]=加色存活数 [1]=Alpha 存活数 [2]=本帧发射数 [3]=保留 ——
        // HOST_VISIBLE，CPU 每帧清零、fence 之后回读
        //（配一个 COMPUTE→HOST 屏障）。
        std::unique_ptr<rhi::Buffer> counters;
        void* countersMapped = nullptr;
        VkDescriptorSet simSet = VK_NULL_HANDLE;
        VkDescriptorSet countSet = VK_NULL_HANDLE;
        VkDescriptorSet drawSet = VK_NULL_HANDLE;
    };

    void createFrame(uint32_t i);
    // 把粒子池与紧凑表清零（DEVICE_LOCAL 内存的内容是未定义的，
    // 不清零第一帧就会画出垃圾粒子）。建完资源后调一次。
    void zeroPools();
    void createRenderPass();
    void createPipelines();
    void createDescriptors();
    void destroyFrames();

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;
    ParticleSettings m_settings;
    uint32_t m_framesInFlight = 2;

    bool m_ready = false;
    bool m_needRebuild = true;
    VkExtent2D m_extent{0, 0};
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t m_emitterCount = 0;
    uint32_t m_dropped = 0;
    // 本帧各发射器的软化距离里的最大值（× 全局倍率后给着色器）。
    // 逐粒子携带软化距离要多花 4 B × 上万粒子，而粒子之间的软化差异
    // 远小于"有 / 无软化"的差异 —— v1 用一个全局值。
    float m_softBase = 0.5f;
    // 本帧的排序模式（ecs::ParticleSortMode 的整数值）。recordSimulation
    // 每帧写入，draw() 用它决定顶点着色器读哪张紧凑表。
    int m_sortMode = 0;

    std::vector<Frame> m_frames;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    // 深度只有采样这一种用法（pass 里没有深度附件），一个采样器够。
    VkSampler m_depthSampler = VK_NULL_HANDLE;

    // ① 模拟（particle_sim.comp）
    std::unique_ptr<rhi::ComputePipeline> m_simPipe;
    VkDescriptorSetLayout m_simSetLayout = VK_NULL_HANDLE;
    // ② 计数（particle_count.comp）
    std::unique_ptr<rhi::ComputePipeline> m_countPipe;
    VkDescriptorSetLayout m_countSetLayout = VK_NULL_HANDLE;
    // ③ 绘制（particle.vert + particle.frag，两种混合各一条）
    std::unique_ptr<rhi::Pipeline> m_drawAdditive;
    std::unique_ptr<rhi::Pipeline> m_drawAlpha;
    VkDescriptorSetLayout m_drawSetLayout = VK_NULL_HANDLE;

    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;

    Stats m_stats;
};

} // namespace fx
