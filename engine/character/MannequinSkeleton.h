#pragma once
// ============================================================
// character/MannequinSkeleton —— UE5 Mannequin 的**标准骨架**
//
// 为什么要有这个文件（2026-11-06）：
//   用户要「把角色模型的动作、姿势、骨骼绑定、动画完全按照 UE5 的风格改造」。
//   UE5 那套的核心不是命名好看，而是**一份骨架被所有角色和所有动画共享**
//   （`SK_Mannequin` + `SKM_Manny` / `SKM_Quinn` + 几百条 `MM_*` 序列）。
//   共享的前提是骨名、骨数、骨层级三者都固定 —— 否则动画不能跨角色复用。
//
//   本工程原来的角色（`default_character.vmesh`）是 **Mixamo 命名、20 根骨**：
//       Hips / Spine / Spine1 / Spine2 / Neck / Head /
//       LeftShoulder / LeftArm / LeftForeArm / LeftHand /
//       LeftUpLeg / LeftLeg / LeftFoot / LeftToeBase（+ HeadTop_End）
//   而 UE5 Mannequin 是 **35 根**（多 spine_04/05、neck_02、ball、ik_*、weapon_*）。
//   骨数不同 → 动画不能互换 → 这是"看起来像 UE"和"真的是 UE 结构"的分界。
//
// 骨名与层级**不是凭记忆写的**：从 `SK_Mannequin.uasset` 里取可读串得到
// （`strings -a SK_Mannequin.uasset | grep -E '^(root|pelvis|spine_[0-9]+|...)$'`），
// 35 根一根不多一根不少。
//
// ------------------------------------------------------------
// 重定向的两条硬约束
// ------------------------------------------------------------
// ① **不动一个顶点的权重**。
//    `spine_04` / `spine_05` / `neck_02` 是"插在骨链中间的过渡骨"，它们的
//    权重天然是 0（没有任何顶点绑在它们身上），`ik_*` / `weapon_*` 是 UE 里
//    的**非形变骨**（`bDeform=false`，只做 IK 目标与挂点）。所以重定向之后
//    蒙皮几何逐位不变 —— 这是能安全做这件事的全部理由。
//    想让脊柱多弯一节，那是"重新刷权重"的活（`animation/WeightSet`），
//    不是这个文件的事。
//
// ② **重定向前后，原 20 根骨的世界姿势必须逐位一致**。
//    做法：先算出源骨架的静止世界矩阵，再把每根 UE 骨的**世界矩阵**照抄
//    （或按插值放在骨链直线上），最后 `restLocal = inverse(parentWorld) * world`。
//    这样即使父子关系变了（`Neck` 的父从 `Spine2` 变成 `spine_05`），
//    乘出来的世界矩阵仍然等于原来那个 —— 静止姿势和所有动画都不会平移一格。
//    自检 `mq_world_*` 就是钉这一条的（见 MannequinSelfTest.cpp）。
// ============================================================

#include "assets/Skeleton.h"

#include <string>
#include <vector>

namespace assets {
struct Model;
}

namespace character {

// 骨是怎么来的 —— 决定重定向时用哪条规则算它的静止变换。
enum class MannequinOrigin : unsigned char {
    // 照抄某根源骨的静止世界矩阵（`from` 是源骨名，可写多个候选）。
    Copy,
    // 在两根源骨之间线性插值（新加的过渡骨：spine_04/05、neck_02）。
    Lerp,
    // 与另一根**UE 骨**完全重合（ik_* / weapon_* 与 foot_* / hand_* 重合）。
    Follow,
    // 落在骨架原点（root / ik_foot_root / ik_hand_root）。
    AtOrigin,
};

// 一条 UE 骨的定义。**表必须保持拓扑序**（parent 下标 < 自己的下标）——
// 重定向是单遍正向扫描，靠这个前提一次算完世界矩阵。
struct MannequinBoneDef {
    const char* name;         // UE 骨名
    int parent;               // 表内下标，-1 = 根
    MannequinOrigin origin;
    const char* from;         // Copy/Lerp 的源骨名候选（逗号分隔）
    const char* to;           // Lerp 的终点源骨名
    float t;                  // Lerp 参数（0..1）
    int follow;               // Follow 的目标表内下标
    bool deform;              // UE 的 bDeform
};

// UE5 Mannequin 的完整骨骼表（35 根，拓扑序）。
const MannequinBoneDef* mannequinBones();
int mannequinBoneCount();

// 按名字取骨下标（精确、区分大小写）；-1 = 没有。
int mannequinBoneIndex(const std::string& name);
bool mannequinBoneDeforms(int index);

// ---- 重定向结果 ----
struct MannequinRetargetReport {
    int bones = 0;              // 产出骨数（通常 = 源骨数 + 追加的 UE 骨数）
    int ueBones = 0;            // 35 根 UE 骨里真正出现在结果中的根数
    int copied = 0;             // 从源骨改名而来的
    int synthesized = 0;        // 追加生成的（插值 / 重合 / 原点）
    int keptAux = 0;            // 源里有、UE 没有、原样保留在骨架里的辅助骨
    int remappedTracks = 0;     // 随骨架一起带过来的轨道条数
    int droppedTracks = 0;      // 丢掉的轨道条数（**恒为 0**，见下面的"下标不动"）
    int missing = 0;            // 源里找不到的 UE 骨（> 0 说明源骨架缺料）
    std::vector<std::string> missingSources;   // 缺的源骨名

    bool ok() const { return missing == 0 && ueBones == mannequinBoneCount(); }
};

// 把任意源骨架（Mixamo / UE 自己的 / Blender Rigify）重定向成 UE5 Mannequin。
//
// * `out` 与 `src` 可以是同一个对象（内部先拷一份）。
// * **下标不动**：源骨架的每一根骨留在**原下标**上（其中映射到的改成 UE 骨名），
//   UE 表里源里没有的骨**追加到末尾**。因此：
//     - `Vertex::joints`（顶点关节下标）一个字节都不用改 —— 这是"重定向不动
//       权重"能成立的机制（`Mesh` 没有改写顶点的入口，重排就必然要动 GPU 数据）；
//     - `AnimTrack::joint` 也一个都不用改，所有 clip 原样可用；
//     - 源里 UE 没有的辅助骨（如 Mixamo `HeadTop_End`）原样保留，计入 `keptAux`。
// * 世界姿势不变：父子关系按 UE 层级重挂（`neck_01` 的父从 `Spine2` 变
//   `spine_05`）之后，用 `restLocal = inverse(parentWorld) * world` 反解，
//   所以每根原骨的世界矩阵与重定向前**逐位相同**（自检 `mq_world_*` 钉住）。
bool retargetToMannequin(const assets::Skeleton& src, assets::Skeleton& out,
                         MannequinRetargetReport& report);

// ---- 作用在 Model 上的封装（角色装配路径用这两个）----

// 这具骨架是不是已经是 UE5 Mannequin（35 根 UE 骨全在）。
bool isMannequinSkeleton(const assets::Skeleton& skel);

// 把模型的骨架**就地**换成 UE5 Mannequin 结构（幂等）。
// 没有骨架 / 已经是 Mannequin → 返回 false（调用方不用管，不是错误）。
// 失败原因只可能是 `retargetToMannequin` 返回 false（空骨架）。
bool upgradeModelSkeletonToMannequin(assets::Model& model,
                                     MannequinRetargetReport* report = nullptr);

// 装配路径的统一入口：先看 `MYVK_MANNEQUIN`（`0` = 关，其余/未设 = 开），
// 再调上面那个。**两个宿主（编辑器 Play / Game.exe）都只调这一个** ——
// 环境变量的判断散在两处，迟早出现"编辑器里是 UE 骨架、Game.exe 里不是"。
bool upgradeModelSkeletonForPlay(assets::Model& model);

// ---- 导出成独立骨架资产 `.vskel`（`assets/characters/mannequin/Rigs/`）----
//
// 抄的是 `USkeleton` 的**真实语义**：骨架资产本身只有骨名与父子关系
// （`FReferenceSkeleton` = `RefBoneInfo` 列表 + `BoneTree`），静止姿势属于
// SkeletalMesh（`RefBonePose`）。所以在没有源骨架可抄的时候，导出的
// `SK_Mannequin.vskel` 里 `restLocal` / `invBind` 一律是单位阵 ——
// **这不是"数据没填"**，而是这个资产的语义就只到这里：
// 它是"这 35 根骨叫什么、谁是谁的父"的**定义**，不是一具摆好的姿势。
//
// 用途：
//   · `AssetC --info` 能直接看到 UE 骨表（不必开编辑器）；
//   · 挂点（`weapon_r` / `weapon_l` / `eye`）有了归属地；
//   · 自检 `mq_vskel_*` 拿它做写盘→读盘的往返对照。
bool writeMannequinSkeletonAsset(const std::string& absPath,
                                 std::string* err = nullptr);

} // namespace character
