#pragma once
// ============================================================
// animation/RigEdit —— **可编辑的骨架**（Blender 的 Armature + Edit Mode）
//
// 为什么要有这一层（2026-10-29，S-A）：
//   `assets::Skeleton` 是**导入产物**：glTF/FBX 解析完就只读，`Joint` 只有
//   `name / parent / restLocal / invBind / externalChain`。于是三件事做不到 ——
//     · **Roll**：扭转信息烧进了 `restLocal` 的旋转分量，4×4 拆不出"当时设的
//       roll 是几度"。手脚本该绕自己的轴转，实际绕的是导入器随手定的轴，
//       症状是"手指/脚背一摆就翻"。这是本轮的核心。
//     · **Connected**：子骨的头是不是钉在父骨尾上，只是矩阵碰巧的结果，
//       不是一个可以锁定的关系。改父骨长度，子骨不跟。
//     · **Deform / Inherit Rotation**：没有标志位，控制器骨做不到"不参与变形"。
//
// 三条设计约定（省了任何一条都会静默出错）：
//
//   ① **绝不重建没动过的骨。** 每根骨存一份 `restLocalOrig`（导入时的原样），
//      烘焙时若这骨"没动过"就**逐位照抄**，只有动过的骨才由
//      `head / dir / length / roll` 重新算。理由是硬的：glTF 的 `restLocal`
//      是 16 个 float 直接落盘，一旦过一遍 `quat_cast → mat4_cast` 就少几位，
//      整具骨架会以 1e-7 的量级漂 —— 单看没有，但"零编辑烘焙 ≡ 导入路径"
//      这条验收就永远通不过，而它正是这道工序唯一的安全网。
//
//   ② **改 rest 必须同时重算 `invBind`，而且只重算"动过的那棵子树"。**
//      `jointMat = invMeshNode · world(pose) · invBind`。rest 变了而 invBind
//      没变 → 静止姿态下 `world · invBind ≠ I`，模型立刻整体缩进/歪掉，
//      **运行时一条日志都不会报**。反过来，没动的子树连 `invBind` 都不许碰，
//      否则老资产的观感会被"顺手重算"改掉。
//
//   ③ **roll 是相对导入朝向的扭转（弧度），不是绝对朝向。**
//      `roll = 0` 的含义是"保持导入时的样子"。这样"我只想调手"不会顺手把
//      整具骨架的初始朝向也改一遍 —— 那是最难查的一类回归。
//      dir 改变时先做"最短弧对齐旧朝向 → 新朝向"，再绕新朝向扭 `roll`。
//
// 坐标系：`head / dir` 都在**骨架空间**（= `poseToWorld` 输出的那个空间，
//   也等于 `restLocal` 累乘出来的空间）。glTF 路径下它是 Y-up、FBX 路径下是
//   Z-up —— 本模块不关心是哪个，只要求"和 restLocal 同一套"。
//   ⚠ 父骨带**非均匀缩放**时，方向换算会被拉斜；这种情况在真实角色骨架上
//     极少见（导入器的 restLocal 通常是 R·S 且 S 均匀），不做特判，但别假设。
// ============================================================

#include "assets/Skeleton.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace animation {

// 一根可编辑的骨。
struct EditBone {
    std::string name;
    int parent = -1;
    // 对应导入骨架里的 joint 下标；**-1 = 这一轮新加的骨**。
    // 烘焙时用它找回 `restLocalOrig / invBind / externalChain`，所以骨数组
    // 的次序可以随便动（删骨、加骨）而不会串味。
    int srcJoint = -1;

    // ---- 当前状态（作者在 Edit Mode 里改的就是这四个）----
    glm::vec3 head{0.0f};               // 骨架空间里的点
    glm::vec3 dir{0.0f, 0.0f, 1.0f};    // 单位向量：head → tail
    float length = 0.1f;                // 骨长（米）
    float roll = 0.0f;                  // 绕 dir 的扭转；0 = 保持导入朝向

    // ---- 标志位（Blender 的骨骼属性面板）----
    bool deform = true;      // 参与网格变形（控制器骨关掉）
    bool connected = false;  // 头钉在父骨尾上（改父骨尾时子骨跟着走）
    bool inheritRot = true;  // 继承父骨旋转（S-C 的约束栈会读它）

    // ---- 导入时的原样（约定 ①：没动过就照抄它）----
    glm::mat4 restLocalOrig{1.0f};
    glm::mat4 externalChain{1.0f};   // 骨架根之外那条祖先链（导入时抄下来，
                                     // 让 restWorld 不必再依赖 src）
    glm::vec3 restHeadOrig{0.0f};
    glm::vec3 restDirOrig{0.0f, 0.0f, 1.0f};

    glm::vec3 tail() const { return head + dir * length; }

    // 有没有偏离导入值。新加的骨（srcJoint < 0）恒为真 —— 它没有原样可抄。
    bool touched() const {
        return srcJoint < 0 || !sameVec3(head, restHeadOrig) ||
               !sameVec3(dir, restDirOrig) || roll != 0.0f;
    }

    static bool sameVec3(const glm::vec3& a, const glm::vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

// 统计（日志与 UI 的角标都读它）
struct RigInfo {
    int bones = 0;
    int roots = 0;
    int touched = 0;
    int deformOff = 0;
    int connected = 0;
};

struct RigEdit {
    std::string source;              // 来源模型/骨架路径（只用于显示与日志）
    std::vector<EditBone> bones;

    int boneCount() const { return static_cast<int>(bones.size()); }
    int indexOf(const std::string& name) const;   // -1 = 没有（精确、区分大小写）
    void childrenOf(int bone, std::vector<int>& out) const;
    bool isDescendantOf(int bone, int ancestor) const;
    void clear();

    // ------------------------------------------------------------
    // 导入 / 烘焙
    // ------------------------------------------------------------

    // 从导入骨架读一份可编辑 rig。**不改 src 一个字节**（纯读）。
    // 骨长与朝向从**几何**推断（父 → 子骨头的距离），不是从旋转矩阵 ——
    // glTF 的关节根本没有"骨长"这个概念，旋转矩阵也不告诉你是哪个轴朝外。
    static RigEdit fromSkeleton(const assets::Skeleton& src,
                                const std::string& sourcePath = std::string());

    // 烘焙成可直接喂给渲染/采样的骨架。
    //   · 没动过的骨：`restLocal / invBind` 逐位照抄 `src`；
    //   · 动过的骨及其子孙：`restLocal` 由 head/dir/roll 重算，`invBind` =
    //     `inverse(restWorld)`（约定 ②）；
    //   · 其余字段（`invMeshNode / skeletonToEngine / clips`）原样带走。
    // `out` 与 `src` 可以是同一个对象（内部先拷一份再用）。
    void bake(const assets::Skeleton& src, assets::Skeleton& out) const;

    // 只算静止世界矩阵（画骨、测试、UI 都用它；不分配 invBind）。
    void restWorld(std::vector<glm::mat4>& out) const;

    // ------------------------------------------------------------
    // 编辑（UI 与注入脚本共用这一层 —— 验收因此不依赖鼠标坐标）
    // ------------------------------------------------------------
    bool setHead(int i, const glm::vec3& p);          // 连着的子骨跟着走
    bool setTail(int i, const glm::vec3& p);          // 同时改 dir 与 length
    bool setLength(int i, float len);                 // 只改长度（> 1e-4）
    bool setDir(int i, const glm::vec3& d);           // 只改朝向（会归一化）
    bool setRoll(int i, float rad);
    void setDeform(int i, bool on);
    void setConnected(int i, bool on);                // 置真时立刻把头顶到父骨尾
    void setInheritRot(int i, bool on);

    // 新加一根骨，返回下标；重名 / 父下标非法 / 零长度 → -1。
    int addBone(const std::string& name, int parent, const glm::vec3& head,
                const glm::vec3& dir, float len);
    // 删掉这根骨**连同它的子孙**；返回删掉的骨数（0 = 下标非法）。
    int removeBone(int i);
    // 把所有 `connected` 的骨的头贴到父骨尾（导入后的第一次校正）。
    int connectAll();

    // 把左/右对称的那根骨按"关于镜像平面镜像"写过去（对面不存在 → 返回 -1）。
    // 镜像平面 = 经过原点、法线为 `mirrorAxis` 的那个平面。
    // ⚠ 只镜像**骨**，不镜像权重（那是 S-B 的事）。
    int mirrorToOpposite(int i);
    static std::string flipSide(const std::string& name);

    // ---- 镜像轴（左右对称面）----
    //
    // **不能写死 X。** 实测 `anime_girl.fbx` 的左右是 **Y** 轴
    //（shoulder.L 的 y = +0.117、shoulder.R 的 y = −0.111，x 只有 ±0.03），
    // 写死 X 的表现在画面上是"镜像完人朝后仰"，而日志一行都不报 ——
    // 这正是 §1 第二节那条 Roll 的同一个坑，只是换成左右。
    //
    // 所以导入时**从数据里认**：在每一对 `.L`/`.R` 上量三根轴的分离量，
    // 取累计最大的那根。认不出（没有成对的骨）就退回 0（X）。
    int mirrorAxis = 0;              // 0 = X / 1 = Y / 2 = Z
    void detectMirrorAxis();
    static const char* axisName(int axis);   // "X" / "Y" / "Z"

    RigInfo info() const;
    void logSummary(const char* tag) const;
};

// ---- JSON 往返（`venn.rig` / version 1）----
bool saveRig(const std::string& path, const RigEdit& rig, std::string& err);
bool loadRig(const std::string& path, RigEdit& out, std::string& err);

// 骨长/朝向的推断规则单独暴露，便于自检直接钉住（改了会动所有骨）。
void inferBoneGeometry(const assets::Skeleton& src, int joint,
                       const std::vector<glm::mat4>& world,
                       glm::vec3& outHead, glm::vec3& outDir, float& outLen);

} // namespace animation
