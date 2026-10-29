#pragma once
// ============================================================
// animation/ClipEdit —— 可编辑的骨骼动画片段（Pose Mode 的落盘格式）
//
// ------------------------------------------------------------
// 为什么不往 assets::Skeleton 上加字段
// ------------------------------------------------------------
// `engine/assets/**` 在 `tools/run_regression.py` 的 GLOBAL_GLOBS 里 ——
// 动它就是全量回归。而且 `Skeleton` 的定位一直是"导入产物"（glTF/FBX 解析
// 出来的只读结果），让编辑器往它上面写用户数据会把"重新导入"变成一场
// 数据合并。所以这里另立一套结构，`Skeleton` 保持只读。
//
// ------------------------------------------------------------
// 三条设计约定（每条都是踩过之后的结论）
// ------------------------------------------------------------
// ① **关节用名字，不用下标**。
//    下标是导入顺序的产物：模型重导一次、Mixamo 换一版骨架、多了根手指
//    辅助骨，下标就整体平移。用下标存盘的后果是"打开旧文件，右臂的帧跑到
//    了左腿上"——而且不报错。名字是唯一跨导入稳定的键。
//    （解析出下标的动作放在 `resolveEditTracks`，只在骨架变化时做一次。）
//
// ② **只覆盖有轨道的通道，不是整段替换**。
//    编辑资产是叠在"导入 clip + 静止姿势"之上的**一层**：samplePose 先把
//    整具骨架铺好，这里只改自己打过帧的那几个 (joint, path)。这既是
//    "打帧不影响其他骨骼"的实现依据，也意味着一个只动了右臂的资产不会
//    把其他 60 根骨锁死在静止姿势。
//
// ③ **采样只走 `assets::sampleTrack`**，不另写求值。
//    roadmap §5 第 3 条：求值只留一个入口。自己写一份插值，症状是"骨骼
//    轨道和过场相机走出两条不同的曲线"，而且只在 STEP / 端点这些边角
//    上表现出来。
//
// ------------------------------------------------------------
// 逐位一致（roadmap §5 第 2 条）
// ------------------------------------------------------------
// 没有编辑数据时，整条链 `samplePose → overlayEditPose(空) → poseToJoints`
// 必须与老的 `Skeleton::sample` **逐位相同**。`overlayEditPose` 在
// resolved 为空时直接返回 0 且**一个字节都不写** —— 这条由
// `MYVK_ANIM_TEST=1` 的 refactor_identity 组和 `verify_pose_mode.py` 的 A 组
// 一起钉住。
// ============================================================

#include "animation/Curve.h"
#include "animation/Pose.h"

#include "assets/Skeleton.h"   // 复用 assets::AnimTrack / Skeleton

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace animation {

// ------------------------------------------------------------
// 关键帧类型（R8，Dope Sheet 的"读节奏"标记）
// ------------------------------------------------------------
// ⚠⚠ **完全不参与求值**。它不改变 `sampleTrack` 的任何输出、不改变画面
//     一个像素 —— 它的唯一用途是让人在摄影表上一眼看出"这一下是转折点
//     还是补间"。`verify_dope_sheet.py` 的 D 组就是拿"改类型前后姿势摘要
//     逐位相同"钉住这条：一旦哪天有人把类型塞进插值分支，那条立刻红。
//
// 为什么需要 `Key` 这个"默认"态，而不是只有三种标记：新打出来的帧本来就
// 没有语义，硬塞进三态里会逼作者给每个帧分类。Blender 同样有一个普通
// "Keyframe" 态（它的列表更长：Breakdown / Moving Hold / Extreme / Jitter /
// Generator；这里只取任务书要的三样，其余随时能加，枚举值向后兼容）。
enum class KeyType : unsigned char {
    Key = 0,        // 普通帧（默认，也是老文件里缺字段时的值）
    Extreme = 1,    // 极端：动作的转折点
    Breakdown = 2,  // 间断：两个极端之间的过渡帧
    Inbetween = 3,  // 中间帧
};

const char* keyTypeName(KeyType t);                    // "key"/"extreme"/...
bool parseKeyType(const std::string& s, KeyType& out);  // 大小写不敏感

// ------------------------------------------------------------
// 每键缓动（S-D，Graph Editor 接到骨骼上）
// ------------------------------------------------------------
// `assets::AnimTrack` 只有一个 `step` 布尔 —— 常量 / 线性两态，没有贝塞尔、
// 没有缓动、没有手柄。那张表是**导入产物**（`engine/assets/**` 在
// GLOBAL_GLOBS 里，动它就是全量回归），所以"动作有生命"的表达只能放在
// **编辑层**：每个键一张 `KeyEase`，与 `times` 平行。
//
// ⚠⚠ 默认值必须**逐位等于** `assets::sampleTrack` 的行为，否则"打开曲线
//     编辑器再关掉"会让所有人的动画悄悄变一遍：
//       · 平移/缩放：`easeFn(Linear, None, u)` 恒等于 u（见 Curve.h 的
//         承诺），而 `v0 + (v1-v0)*u` 与 `sampleTrack` 的 `a + (b-a)*u`
//         **是同一个式子** —— 逐位相同，这一条由
//         `curvebake_default_bitwise` 钉住；
//       · 旋转：`sampleTrack` 走 slerp，而四个分量各自插值再归一（nlerp）
//         与 slerp **不是**同一条路（同一条弧、不同速度）—— 所以
//         "全 Linear 的旋转轨"必须仍走 slerp，见 CurveBake.cpp 里的
//         `sampleRotationEased`。这条差别只在贝塞尔段上才允许出现。
//
// ⚠ 表**可以比 `times` 短**（同 `types`）：老文件没有这个字段，越界一律
//   取默认值。全部是默认时整张表被 `pruneDefaultEase()` 抹平 → 回到
//   "根本没有缓动"的路径（那条路径就是 `sampleTrack` 本身），于是
//   "改一圈再清回去"能逐位还原，不是"看起来差不多"。
struct KeyEase {
    Interp interp = Interp::Linear;
    EaseMode ease = EaseMode::None;
    EaseShape shape = EaseShape::Linear;
    HandleType handle = HandleType::AutoClamped;
    // 相对本帧的偏移（`CurveKey` 同款：out.x >= 0、in.x <= 0）。
    // ⚠ **绝对量**，不是相机那边的"段内归一化"—— 骨骼轨道一个分量一条
    //   独立的 F-Curve，手柄没有"跨通道共用"的问题（见 CurveEditorPanel.h ②）。
    glm::vec2 outHandle{1.0f / 3.0f, 0.0f};
    glm::vec2 inHandle{-1.0f / 3.0f, 0.0f};

    // 采样上与"没有缓动"完全等价 → 可以整条轨道退回 `sampleTrack`。
    // `handle` 也算在内：Linear 段上它不参与求值，但它是作者按 V 存下的
    // 意图，抹掉它等于"下次切到贝塞尔时手柄凭空复位"。
    bool isDefault() const;
};


// 一条可编辑轨道。字段与 `assets::AnimTrack` 几乎同构，只有一处不同：
// 目标不是 `int joint` 而是 `std::string joint`（见约定 ①）。
//
// 为什么 path 直接复用 `assets::AnimTrack::Path` 而不是另立一个 enum：
// 两个各一份的后果是"UI 里选的是 Rotation、实际写进 Translation"这类
// 只在画面歪掉时才发现的错误，而且要多写一份 enum→enum 的换算表。
struct EditTrack {
    std::string joint;                    // 关节**名字**
    assets::AnimTrack::Path path = assets::AnimTrack::Rotation;
    std::vector<float> times;             // 秒，单调递增
    std::vector<glm::vec4> values;        // T/S 用 xyz；R 是 (x,y,z,w)
    bool step = false;                    // 阶跃（不插值）
    // 与 `times` 平行的类型表（R8）。**允许比 times 短**：
    // 老文件里没有这个字段、手改坏的 JSON 也可能只给了一半。读的时候
    // 越界一律当 `KeyType::Key`，这样"少写几项"不会让整条轨道错位 ——
    // 三份平行数组已经够容易错了，再加一条"长度必须相等"的硬约束
    // 只会把坏数据变成打不开的文件。
    std::vector<unsigned char> types;
    // 与 `times` 平行的缓动表（S-D）。同样允许更短，同样全默认即抹平。
    // 空 = "这条轨道没进过曲线编辑器"，采样走 `sampleTrack`（逐位等于
    // R7 之前的行为）。
    std::vector<KeyEase> ease;

    int keyCount() const { return static_cast<int>(times.size()); }
    // 时间 → 关键帧下标；没命中返回 -1（容差 kKeyTimeEpsilon）
    int findKey(float time) const;
    // 越界 / 缺项 → KeyType::Key
    KeyType typeAt(int i) const;
    // 越界忽略；i 在范围内时按需把 types 撑到与 times 等长
    void setTypeAt(int i, KeyType t);
    // 越界 / 缺项 → 默认 `KeyEase`（逐位等于 `sampleTrack`）
    const KeyEase& easeAt(int i) const;
    void setEaseAt(int i, const KeyEase& e);
    // 有任何一键不是默认 → true（否则整条轨道不该付曲线的代价）
    bool hasEasing() const;
    // 全默认 → 清空表。返回是否动了。
    bool pruneDefaultEase();
};

// 同帧判定容差（秒）。
//
// 为什么不是 0：编辑器的时间来自"播放头 + 一帧的 delta"，两次点 `I` 之间
// 至少差几毫秒；用 0 判定会得到"每隔 16ms 就多一个关键帧"的轨道，
// 两秒下来几百帧，Dope Sheet 一屏全是菱形。1e-4 = 0.1ms，比一帧小两个
// 数量级，又不会把两次真的想分开的打帧合并。
constexpr float kKeyTimeEpsilon = 1.0e-4f;

// 姿态库里的一条：一具完整的局部姿态（不是增量）。
//
// 为什么存全量而不是"相对静止姿势的偏移"：用户改过的静止姿势（Pose Mode
// 里把某根骨拖歪之后存库）与原 rest 已经不是一回事，存偏移就要额外记
// "基准是哪一版 rest"，而那份 rest 没有任何地方存。全量存虽然 JSON 大一倍，
// 但**读出来就是那个姿势**，不依赖任何隐式上下文。
struct EditPose {
    struct Entry {
        std::string joint;
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
    };
    std::string name;
    std::vector<Entry> bones;
};

// 一份可编辑片段 + 它的姿态库。
//
// 一个文件 = 一段片段。不做"一个文件装多段"：片段之间要独立存盘/独立
// diff，塞在一个文件里之后"我只想给右臂这段重命名"会变成整文件改写。
struct EditClip {
    std::string name;          // 片段名（也是默认文件名）
    // 叠在哪段**导入** clip 上。空 = 纯自定义（未打帧的骨保持静止姿势）。
    //
    // 语义是"叠加"不是"替换"：base 负责大部分骨，本资产的轨道覆盖少数骨。
    // 想完全自己写一段，就把 base 留空。
    std::string baseClip;
    // 片段总长。打帧到更晚的时刻会**自动变长**；不会自动变短 ——
    // 缩短会把尾巴上的帧丢出可见范围，而那是不可撤销的静默数据损失。
    float duration = 1.0f;
    bool loop = true;
    std::vector<EditTrack> tracks;
    std::vector<EditPose> poses;

    bool empty() const { return tracks.empty(); }
    int  totalKeys() const;
    int  findTrack(const std::string& joint,
                   assets::AnimTrack::Path p) const;   // -1 = 没有
    EditTrack*       track(const std::string& joint, assets::AnimTrack::Path p);
    const EditTrack* track(const std::string& joint,
                           assets::AnimTrack::Path p) const;
    // 出现过轨道的关节名，**去重且保持首次出现顺序**（UI 列表要稳定）
    std::vector<std::string> keyedJoints() const;

    // 打帧：写 (joint, path, time) 处的值。
    //   已有同一时刻的帧 → 覆盖（返回 false 表示"是覆盖不是新增"）
    //   否则按时间序插入       → 返回 true
    // 顺带把 duration 撑到不早于 time。
    //
    // `type` 只在**新增**时生效：覆盖一个已有帧不该顺手把它的类型冲掉
    // （作者标好的"这是转折点"被一次重打帧抹掉，是那种当场看不出、
    //  回头怎么也想不起来的丢失）。要改类型走 `setTypeAt` / `setKeyTypes`。
    bool setKey(const std::string& joint, assets::AnimTrack::Path p, float time,
                const glm::vec4& value, KeyType type = KeyType::Key);
    // 删第 index 个帧；index 越界返回 false。轨道空了就整条移除。
    bool removeKey(const std::string& joint, assets::AnimTrack::Path p,
                   int index);
    // 删掉某个关节的所有轨道（"这根骨的帧全清掉"）
    int  removeJoint(const std::string& joint);

    // 姿态库
    const EditPose* findPose(const std::string& name) const;
    // 同名覆盖（返回 false = 覆盖了已有的）
    bool storePose(const EditPose& p);
    bool removePose(const std::string& name);
};

// ------------------------------------------------------------
// 批量编辑（R8，Dope Sheet）
// ------------------------------------------------------------
// 一组被选中的关键帧 = (轨道下标, 帧下标)。
//
// ⚠ 下标是**易失**的：任何重排（挪到别的帧旁边、镜像、对齐）之后帧序全变。
//   所以下面每个操作的签名都是 `std::vector<KeyRef>& sel` —— 传进传出，
//   函数负责把它改写成"操作完之后这批帧的新下标"。调用方（UI / 注入脚本）
//   不许自己算下标，这正是"框选 10 帧一起挪，松手之后还选着那 10 帧"的依据。
struct KeyRef {
    int track = -1;   // EditClip::tracks 下标
    int key = -1;     // EditTrack::times 下标
};
inline bool operator==(const KeyRef& a, const KeyRef& b) {
    return a.track == b.track && a.key == b.key;
}

// 一把操作的读数。日志里就是这几项 —— 断言"合并了 0 帧"比"看起来对"可靠。
struct BatchStats {
    int moved = 0;        // 成功改位置的帧数
    int merged = 0;       // 落到同一时刻被合并掉的帧数（>0 就是丢帧！）
    int dropped = 0;      // 越界的 KeyRef
    float scale = 1.0f;   // alignCycle 用：时间轴缩放的倍率
    float lo = 0.0f;      // 操作前选中帧的时间范围
    float hi = 0.0f;
};

// 选中帧的时间范围；sel 为空或全部越界 → false
bool selectedTimeRange(const EditClip& clip, const std::vector<KeyRef>& sel,
                       float& lo, float& hi);
// 全部帧（"Select all"）
std::vector<KeyRef> allKeys(const EditClip& clip);

// 整体平移（Dope Sheet 的 `G`）。
// **相对间距逐帧不变**：整批加同一个 delta，不是逐个往下排 —— 逐个排会
// 把作者好不容易调出来的节奏压平，而且看起来"只是动了动"。
BatchStats moveKeys(EditClip& clip, std::vector<KeyRef>& sel, float delta);

// 时间镜像：t' = 2·pivot − t（只在选中的帧上做）。
//
// 为什么是**时间**镜像而不是左右骨互换：后者（Blender 的 Paste Flipped）
// 需要知道骨架的左右轴 —— Mixamo 是 X、Blender 导出的是 −X、有的裙子
// 骨架干脆没有对称轴。做错了不报错，只是"镜像完人朝后仰"。时间镜像
// 没有这个问题：它问的是"这段动作在第 3 帧和第 5 帧之间是否对称"，
// 与骨架无关。左右互换留给有骨架契约的独立功能。
BatchStats mirrorKeys(EditClip& clip, std::vector<KeyRef>& sel, float pivot);

// 循环长度对齐：把选中帧的**时间轴**等比缩放，让最后一帧正好落在 `target`。
//
// 相位锚点是**第一个选中帧**（它不动）：`t' = lo + (t − lo)·s`。
// 从 0 起手的循环于是 `t' = t·s`，末帧精确等于 target —— 这一条在
// `verify_dope_sheet.py` C 组按**逐位**比（末帧特意特殊处理，不走乘法，
// 免得 `lo + (hi−lo)·(target−lo)/(hi−lo)` 的舍入把断言变成"差 1 ulp"）。
BatchStats alignCycle(EditClip& clip, std::vector<KeyRef>& sel, float target);

// 批量设类型（不影响求值，见 KeyType 的注释）。返回改了多少帧。
int setKeyTypes(EditClip& clip, const std::vector<KeyRef>& sel, KeyType type);

// 选中的帧里各类类型各有多少（侧栏读数 / 日志）
struct TypeCount { int key = 0, extreme = 0, breakdown = 0, inbetween = 0; };
TypeCount countTypes(const EditClip& clip, const std::vector<KeyRef>& sel);

// ------------------------------------------------------------
// 名字 → 下标
// ------------------------------------------------------------
// 输出可以直接喂给 `overlayEditPose` 的运行时轨道（`assets::AnimTrack`）。
// 找不到的关节**跳过并记录**，不中断：一个从别的角色拷来的姿态库里
// 总有几根骨在这个模型上不存在，为了那几根整份资产打不开是不可接受的。
struct ResolveStats {
    int resolvedChannels = 0;             // 成功映射的轨道数
    int resolvedJoints = 0;               // 涉及的不同关节数
    std::vector<std::string> missing;     // 找不到的关节名（去重）
    int droppedKeys = 0;                  // 因名字缺失被丢掉的帧总数
};

// 一条"已解析"的编辑轨道：运行时轨道 + 它的缓动表 + **预先烘好的曲线**。
//
// 为什么要烘在解析结果里，而不是每次采样现算：`CurveEditorPanel` 一帧要
// 采样几十个通道 × 60 次，`evaluateCurve` 需要 `Curve`（手柄解析要邻居），
// 现造一个容器就是每帧几百次堆分配。解析只在骨架/资产变化时做一次。
//
// `curves[comp]` 与 `track.values[i][comp]` 一一对应；`comps` 是这条路径
// 的分量数（T/S = 3，R = 4）。`hasEase == false` 时 `curves` 为空，
// 采样直接转发 `sampleTrack` —— 这就是"没进过曲线编辑器 = 逐位不变"。
struct ResolvedTrack {
    assets::AnimTrack track;
    std::vector<KeyEase> ease;
    std::vector<Curve> curves;
    int comps = 3;
    bool hasEase = false;
};

ResolveStats resolveEditTracks(const assets::Skeleton& skel,
                               const EditClip& clip,
                               std::vector<ResolvedTrack>& out);

// 叠加：把已解析的轨道在 time 处采到 pose 上。
//
// **只写有轨道的通道**（约定 ②）。`resolved` 为空时立刻返回 0，
// pose 一个字节都不动 —— 这是"无编辑数据时逐位一致"的那条保证。
// 返回写过的通道数。
//
// ⚠ 约定 ③（"采样只走 assets::sampleTrack"）在 S-D 之后有了一个**受控的
//   例外**：轨道带缓动时走 `sampleResolvedTrack`（内部是
//   `animation::Curve`，全仓唯一的曲线求值器）。仍然不是"第三份插值"：
//   没有缓动就转发 `sampleTrack`，有缓动就转发 `evaluateCurve`。
int overlayEditPose(const std::vector<ResolvedTrack>& resolved, float time,
                    Pose& pose);

// 姿态库应用：把一条 EditPose 写到 pose 上（同样是**只写它含的骨**）。
// 返回写过的骨数。
int applyEditPose(const EditPose& p, const assets::Skeleton& skel, Pose& pose);

// 从当前 pose 抓一条姿态库条目（全量：jointCount() 根骨都记）。
EditPose capturePose(const Pose& pose, const assets::Skeleton& skel,
                     const std::string& name);

// ------------------------------------------------------------
// 落盘
// ------------------------------------------------------------
// 格式：`.anim.json`。浮点以 JSON number 存 —— float → double 是精确提升，
// nlohmann 的 dump 走"最短可回读"表示，所以 load 回来再降回 float 是**逐位**
// 相同的（A 组验收就是按 bit 比对，不靠容差）。
bool saveEditClip(const std::string& path, const EditClip& clip,
                  std::string& err);
bool loadEditClip(const std::string& path, EditClip& out, std::string& err);

// 路径/名字小工具：`foo.anim.json` 里取 `foo`
std::string clipNameFromPath(const std::string& path);

} // namespace animation
