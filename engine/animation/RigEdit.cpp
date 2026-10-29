// ============================================================
// animation/RigEdit.cpp
//
// 三件事：导入（几何推断 head/dir/length）、烘焙（没动过的逐位照抄）、
// JSON 往返。**这里没有求值** —— 烘焙出来的东西交给 poseToWorld 去累乘。
//
// 所有批量/编辑操作都经 `RigEdit::setXxx` 一族，UI 与注入脚本共用同一份 ——
// 这样"能不能自动验"不取决于把某颗输入框的坐标算对没有。
// ============================================================

#include "animation/RigEdit.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <fstream>

namespace animation {

namespace {

using json = nlohmann::json;

constexpr float kMinBoneLen = 1.0e-4f;   // 再短的骨没有意义，也除不出方向

glm::vec3 normalizeOr(const glm::vec3& v, const glm::vec3& fallback) {
    const float l = glm::length(v);
    if (!(l > 1.0e-8f)) return fallback;
    return v / l;
}

// a → b 的最短弧旋转。
//
// 为什么手写：`glm::rotation`（gtx/quaternion）在 a、b 反向时不保证给一个
// 稳定的轴，而"骨被翻成 180°"恰好是作者会做出来的姿势（腿抬平）。
// 这里显式挑一条不与 a 平行的参考轴，结果可复现。
glm::quat shortestArc(const glm::vec3& a, const glm::vec3& b) {
    const float d = glm::dot(a, b);
    if (d >= 1.0f - 1.0e-7f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d <= -1.0f + 1.0e-7f) {
        const glm::vec3 ref = std::fabs(a.x) < 0.9f ? glm::vec3(1, 0, 0)
                                                    : glm::vec3(0, 1, 0);
        const glm::vec3 axis = normalizeOr(glm::cross(a, ref), glm::vec3(1, 0, 0));
        return glm::angleAxis(3.14159265358979323846f, axis);
    }
    const glm::vec3 c = glm::cross(a, b);
    glm::quat q(1.0f + d, c.x, c.y, c.z);
    return glm::normalize(q);
}

// 这一根骨（编辑后的）在**父空间**里的 local 矩阵。
//
// 顺序：先把"导入朝向"转到"新朝向"（最短弧），再绕**新朝向**扭 roll。
// 反过来的话，dir 与 restDir 差得越多，roll 的轴就偏得越远 ——
// 作者会说"我 roll 明明拧了 90°，看上去只转了 30°"。
glm::mat4 buildLocal(const EditBone& b, const glm::mat4& parentWorld) {
    const glm::mat4 invP = glm::inverse(parentWorld);
    const glm::vec3 t = glm::vec3(invP * glm::vec4(b.head, 1.0f));
    const glm::vec3 dirP = normalizeOr(glm::vec3(invP * glm::vec4(b.dir, 0.0f)),
                                       glm::vec3(0, 0, 1));
    const glm::vec3 rdP = normalizeOr(
        glm::vec3(invP * glm::vec4(b.restDirOrig, 0.0f)), dirP);

    const glm::quat dq = glm::angleAxis(b.roll, dirP) * shortestArc(rdP, dirP);
    // 只替换旋转，**保留原矩阵的缩放分量**：导入的 restLocal 可能带 S，
    // 直接 mat4_cast 会把它抹成 1，模型局部缩水且不报错。
    const glm::mat3 r = glm::mat3_cast(dq) * glm::mat3(b.restLocalOrig);
    glm::mat4 L(r);
    L[3] = glm::vec4(t, 1.0f);
    return L;
}

// 父先于子的遍历序（骨数组的次序可以随便动，所以每次按当前 parent 现算）。
std::vector<int> boneOrder(const std::vector<EditBone>& bones) {
    const int n = static_cast<int>(bones.size());
    std::vector<int> order;
    order.reserve(static_cast<size_t>(n));
    std::vector<char> seen(static_cast<size_t>(n), 0);

    // 从根往下 DFS；子骨按数组次序访问 → 结果确定（同样的输入同样输出）。
    std::vector<int> stack;
    for (int i = n - 1; i >= 0; --i) {
        const int p = bones[static_cast<size_t>(i)].parent;
        if (p < 0 || p >= n || p == i) stack.push_back(i);
    }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        if (i < 0 || i >= n || seen[static_cast<size_t>(i)]) continue;
        seen[static_cast<size_t>(i)] = 1;
        order.push_back(i);
        for (int c = n - 1; c >= 0; --c)
            if (bones[static_cast<size_t>(c)].parent == i) stack.push_back(c);
    }
    // 防御：环（手改坏的 JSON）里的骨也得进输出，否则它们会被静默丢掉
    for (int i = 0; i < n; ++i)
        if (!seen[static_cast<size_t>(i)]) order.push_back(i);
    return order;
}

void passWorld(const std::vector<EditBone>& bones, const std::vector<int>& order,
               std::vector<glm::mat4>& world, std::vector<glm::mat4>* localOut) {
    const int n = static_cast<int>(bones.size());
    world.assign(static_cast<size_t>(n), glm::mat4(1.0f));
    if (localOut) localOut->assign(static_cast<size_t>(n), glm::mat4(1.0f));
    for (int i : order) {
        const EditBone& b = bones[static_cast<size_t>(i)];
        const glm::mat4 pw = (b.parent >= 0 && b.parent < n)
                                 ? world[static_cast<size_t>(b.parent)]
                                 : b.externalChain;
        const glm::mat4 L = b.touched() ? buildLocal(b, pw) : b.restLocalOrig;
        if (localOut) (*localOut)[static_cast<size_t>(i)] = L;
        world[static_cast<size_t>(i)] = pw * L;
    }
}

// 连着的子骨的头 = 父骨尾。递归（用栈，骨链再长也不爆栈）。
void propagateConnected(std::vector<EditBone>& bones, int i) {
    const int n = static_cast<int>(bones.size());
    std::vector<int> stack{i};
    int guard = 0;
    while (!stack.empty() && guard++ < n * 4) {
        const int p = stack.back();
        stack.pop_back();
        if (p < 0 || p >= n) continue;
        const glm::vec3 tail = bones[static_cast<size_t>(p)].tail();
        for (int c = 0; c < n; ++c) {
            if (bones[static_cast<size_t>(c)].parent != p) continue;
            if (!bones[static_cast<size_t>(c)].connected) continue;
            bones[static_cast<size_t>(c)].head = tail;
            stack.push_back(c);
        }
    }
}

json mat4ToJson(const glm::mat4& m) {
    json a = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) a.push_back(m[c][r]);
    return a;
}

glm::mat4 mat4FromJson(const json& j, const glm::mat4& def) {
    if (!j.is_array() || j.size() < 16) return def;
    glm::mat4 m(def);
    int k = 0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            if (j[static_cast<size_t>(k)].is_number())
                m[c][r] = j[static_cast<size_t>(k)].get<float>();
            ++k;
        }
    return m;
}

glm::vec3 vec3FromJson(const json& j, const glm::vec3& def) {
    if (!j.is_array()) return def;
    glm::vec3 v = def;
    for (size_t i = 0; i < 3 && i < j.size(); ++i)
        if (j[i].is_number()) v[static_cast<glm::length_t>(i)] = j[i].get<float>();
    return v;
}

} // namespace

// ============================================================
// 查询
// ============================================================

int RigEdit::indexOf(const std::string& name) const {
    for (int i = 0; i < boneCount(); ++i)
        if (bones[static_cast<size_t>(i)].name == name) return i;
    return -1;
}

void RigEdit::childrenOf(int bone, std::vector<int>& out) const {
    out.clear();
    if (bone < 0 || bone >= boneCount()) return;
    for (int i = 0; i < boneCount(); ++i)
        if (bones[static_cast<size_t>(i)].parent == bone) out.push_back(i);
}

bool RigEdit::isDescendantOf(int bone, int ancestor) const {
    if (bone == ancestor) return true;
    int guard = 0;
    int p = (bone >= 0 && bone < boneCount()) ? bones[static_cast<size_t>(bone)].parent : -1;
    while (p >= 0 && p < boneCount() && guard++ <= boneCount()) {
        if (p == ancestor) return true;
        p = bones[static_cast<size_t>(p)].parent;
    }
    return false;
}

void RigEdit::clear() {
    source.clear();
    bones.clear();
}

// ============================================================
// 导入 / 烘焙
// ============================================================

void inferBoneGeometry(const assets::Skeleton& src, int joint,
                       const std::vector<glm::mat4>& world, glm::vec3& outHead,
                       glm::vec3& outDir, float& outLen) {
    const int n = src.jointCount();
    outHead = glm::vec3(0.0f);
    outDir = glm::vec3(0.0f, 0.0f, 1.0f);
    outLen = 0.05f;
    if (joint < 0 || joint >= n || static_cast<int>(world.size()) != n) return;

    outHead = glm::vec3(world[static_cast<size_t>(joint)][3]);

    // 骨长 = 到**第一根子骨**的距离（按导入次序取第一根）。
    //
    // 为什么不是"最远的那根子骨"：手有 5 根手指、髋有 spine + 两条腿，
    // 最远的那根会把手骨拉长到指尖、把髋骨指向大腿 —— 而"第一根"在
    // glTF / Mixamo 的导入次序里基本就是主链（spine、lowerarm …）。
    // 这条规则**是猜的**，所以写在这里：改它会让所有骨的长度一起变。
    int firstChild = -1;
    for (int c = 0; c < n; ++c) {
        if (src.joints[static_cast<size_t>(c)].parent != joint) continue;
        firstChild = c;
        break;
    }
    if (firstChild >= 0) {
        const glm::vec3 d = glm::vec3(world[static_cast<size_t>(firstChild)][3]) - outHead;
        const float len = glm::length(d);
        if (len > kMinBoneLen) {
            outDir = d / len;
            outLen = len;
            return;
        }
    }

    // 叶骨：沿父骨方向再延一半。glTF 的关节没有"尾巴"，叶骨（head_end /
    // 手指尖 / 武器骨）**只能**这样给一个可视长度，否则视口里它是零点。
    const int p = src.joints[static_cast<size_t>(joint)].parent;
    if (p >= 0 && p < n) {
        const glm::vec3 d = outHead - glm::vec3(world[static_cast<size_t>(p)][3]);
        const float len = glm::length(d);
        if (len > kMinBoneLen) {
            outDir = d / len;
            outLen = len * 0.5f;
            return;
        }
    }
}

RigEdit RigEdit::fromSkeleton(const assets::Skeleton& src,
                              const std::string& sourcePath) {
    RigEdit r;
    r.source = sourcePath;
    const int n = src.jointCount();
    if (n <= 0) return r;

    // 静止世界矩阵：**直接用 raw restLocal 累乘**，不过 decompose/compose。
    // 后者是 `poseToWorld` 的路径（为了姿态才拆 TRS），多一次往返就多一次
    // 舍入，而这里要的是"导入时到底在哪"。
    std::vector<glm::mat4> world(static_cast<size_t>(n), glm::mat4(1.0f));
    std::vector<int> order = src.topoOrder;
    if (static_cast<int>(order.size()) != n) {
        order.clear();
        for (int i = 0; i < n; ++i) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&src](int a, int b) {
            // 父先于子：数祖先个数
            auto depth = [&src](int j) {
                int d = 0;
                int p = src.joints[static_cast<size_t>(j)].parent;
                int guard = 0;
                while (p >= 0 && guard++ < 512) {
                    ++d;
                    p = src.joints[static_cast<size_t>(p)].parent;
                }
                return d;
            };
            return depth(a) < depth(b);
        });
    }
    for (int j : order) {
        if (j < 0 || j >= n) continue;
        const assets::Skeleton::Joint& jt = src.joints[static_cast<size_t>(j)];
        const glm::mat4 base = jt.parent >= 0
                                   ? world[static_cast<size_t>(jt.parent)]
                                   : jt.externalChain;
        world[static_cast<size_t>(j)] = base * jt.restLocal;
    }

    r.bones.resize(static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) {
        const assets::Skeleton::Joint& jt = src.joints[static_cast<size_t>(j)];
        EditBone& b = r.bones[static_cast<size_t>(j)];
        b.name = jt.name;
        b.parent = jt.parent;
        b.srcJoint = j;
        b.restLocalOrig = jt.restLocal;
        b.externalChain = jt.externalChain;
        b.deform = true;
        b.connected = false;
        b.inheritRot = true;
        inferBoneGeometry(src, j, world, b.head, b.dir, b.length);
        b.restHeadOrig = b.head;
        b.restDirOrig = b.dir;
    }
    r.detectMirrorAxis();   // 左右轴从数据里认，别写死 X（见头文件）
    return r;
}

void RigEdit::restWorld(std::vector<glm::mat4>& out) const {
    passWorld(bones, boneOrder(bones), out, nullptr);
}

void RigEdit::bake(const assets::Skeleton& src, assets::Skeleton& out) const {
    const assets::Skeleton base = src;   // 先拷一份：out 与 src 可能是同一对象
    const int n = boneCount();

    std::vector<glm::mat4> world;
    std::vector<glm::mat4> local;
    const std::vector<int> order = boneOrder(bones);
    passWorld(bones, order, world, &local);

    // "脏"要沿子树传：祖先动了，自己的 restLocal 没变但**世界矩阵变了**，
    // invBind 必须跟着重算（约定 ②）。
    std::vector<char> dirty(static_cast<size_t>(n), 0);
    for (int i : order) {
        if (i < 0 || i >= n) continue;
        const EditBone& b = bones[static_cast<size_t>(i)];
        dirty[static_cast<size_t>(i)] =
            (b.touched() || (b.parent >= 0 && b.parent < n &&
                             dirty[static_cast<size_t>(b.parent)]))
                ? 1 : 0;
    }

    assets::Skeleton r = base;
    r.joints.assign(static_cast<size_t>(n), assets::Skeleton::Joint{});
    int copied = 0;
    int recomputed = 0;
    for (int i = 0; i < n; ++i) {
        const EditBone& b = bones[static_cast<size_t>(i)];
        assets::Skeleton::Joint& jt = r.joints[static_cast<size_t>(i)];
        jt.name = b.name;
        jt.parent = b.parent;
        jt.externalChain = b.externalChain;
        jt.restLocal = local[static_cast<size_t>(i)];
        const bool keep = (b.srcJoint >= 0 && !dirty[static_cast<size_t>(i)] &&
                           b.srcJoint < static_cast<int>(base.joints.size()));
        if (keep) {
            jt.invBind = base.joints[static_cast<size_t>(b.srcJoint)].invBind;
            ++copied;
        } else {
            jt.invBind = glm::inverse(world[static_cast<size_t>(i)]);
            ++recomputed;
        }
    }
    r.topoOrder = order;
    // nodeMap 是**导入期**的产物（glTF node 下标 → joint 下标）。加/删骨之后
    // 下标已经不对应了，留着只会让某个后续按 node 找 joint 的代码静默错位。
    r.nodeMap.clear();
    out = std::move(r);

    VK_LOG_INFO("RIG-BAKE bones=%d copied=%d recomputed=%d", n, copied, recomputed);
}

// ============================================================
// 编辑
// ============================================================

bool RigEdit::setHead(int i, const glm::vec3& p) {
    if (i < 0 || i >= boneCount()) return false;
    bones[static_cast<size_t>(i)].head = p;
    propagateConnected(bones, i);
    return true;
}

bool RigEdit::setTail(int i, const glm::vec3& p) {
    if (i < 0 || i >= boneCount()) return false;
    EditBone& b = bones[static_cast<size_t>(i)];
    const glm::vec3 d = p - b.head;
    const float len = glm::length(d);
    if (!(len > kMinBoneLen)) return false;   // 尾巴拖到头上 = 没有方向
    b.dir = d / len;
    b.length = len;
    propagateConnected(bones, i);
    VK_LOG_INFO("RIG-SET bone='%s' tail=(%.4f,%.4f,%.4f) len=%.4f",
                b.name.c_str(), static_cast<double>(p.x), static_cast<double>(p.y),
                static_cast<double>(p.z), static_cast<double>(len));
    return true;
}

bool RigEdit::setLength(int i, float len) {
    if (i < 0 || i >= boneCount() || !(len > kMinBoneLen)) return false;
    bones[static_cast<size_t>(i)].length = len;
    propagateConnected(bones, i);
    return true;
}

bool RigEdit::setDir(int i, const glm::vec3& d) {
    if (i < 0 || i >= boneCount()) return false;
    const float l = glm::length(d);
    if (!(l > 1.0e-8f)) return false;
    bones[static_cast<size_t>(i)].dir = d / l;
    propagateConnected(bones, i);
    return true;
}

bool RigEdit::setRoll(int i, float rad) {
    if (i < 0 || i >= boneCount()) return false;
    bones[static_cast<size_t>(i)].roll = rad;
    VK_LOG_INFO("RIG-ROLL bone='%s' roll=%.6f deg=%.3f",
                bones[static_cast<size_t>(i)].name.c_str(),
                static_cast<double>(rad), static_cast<double>(rad * 57.2957795f));
    return true;
}

void RigEdit::setDeform(int i, bool on) {
    if (i < 0 || i >= boneCount()) return;
    bones[static_cast<size_t>(i)].deform = on;
    VK_LOG_INFO("RIG-FLAG bone='%s' deform=%d",
                bones[static_cast<size_t>(i)].name.c_str(), on ? 1 : 0);
}

void RigEdit::setConnected(int i, bool on) {
    if (i < 0 || i >= boneCount()) return;
    EditBone& b = bones[static_cast<size_t>(i)];
    if (on && b.parent < 0) return;   // 根骨没有父骨可连
    b.connected = on;
    if (on) {
        const EditBone& p = bones[static_cast<size_t>(b.parent)];
        b.head = p.tail();
        propagateConnected(bones, i);
    }
    VK_LOG_INFO("RIG-FLAG bone='%s' connected=%d",
                bones[static_cast<size_t>(i)].name.c_str(), on ? 1 : 0);
}

void RigEdit::setInheritRot(int i, bool on) {
    if (i < 0 || i >= boneCount()) return;
    bones[static_cast<size_t>(i)].inheritRot = on;
    VK_LOG_INFO("RIG-FLAG bone='%s' inheritRot=%d",
                bones[static_cast<size_t>(i)].name.c_str(), on ? 1 : 0);
}

int RigEdit::addBone(const std::string& name, int parent, const glm::vec3& head,
                     const glm::vec3& dir, float len) {
    if (name.empty()) return -1;
    if (parent < -1 || parent >= boneCount()) return -1;
    if (!(len > kMinBoneLen)) return -1;
    const float dl = glm::length(dir);
    if (!(dl > 1.0e-8f)) return -1;
    if (indexOf(name) >= 0) return -1;   // 重名会让"按名字解析"静默命中错的那根

    EditBone b;
    b.name = name;
    b.parent = parent;
    b.srcJoint = -1;                     // 新骨：没有导入原样可抄
    b.head = head;
    b.dir = dir / dl;
    b.length = len;
    b.restLocalOrig = glm::mat4(1.0f);
    // 新骨的"骨架根外链"跟父骨一致（父骨是根的话它就是那根骨的外链）
    b.externalChain = (parent >= 0) ? bones[static_cast<size_t>(parent)].externalChain
                                    : glm::mat4(1.0f);
    b.restHeadOrig = head;
    b.restDirOrig = b.dir;
    bones.push_back(b);
    const int idx = boneCount() - 1;
    VK_LOG_INFO("RIG-ADD bone='%s' parent=%d idx=%d len=%.4f", name.c_str(),
                parent, idx, static_cast<double>(len));
    return idx;
}

int RigEdit::removeBone(int i) {
    if (i < 0 || i >= boneCount()) return 0;
    std::vector<int> kill;
    for (int k = 0; k < boneCount(); ++k)
        if (isDescendantOf(k, i)) kill.push_back(k);
    if (kill.empty()) return 0;

    const std::string gone = bones[static_cast<size_t>(i)].name;
    // 先记"谁要被删"，再从后往前删 —— 下标就不会在删除过程中漂移。
    const int oldN = boneCount();
    std::vector<char> killed(static_cast<size_t>(oldN), 0);
    for (int k : kill) killed[static_cast<size_t>(k)] = 1;
    std::sort(kill.begin(), kill.end(), std::greater<int>());
    for (int k : kill) bones.erase(bones.begin() + k);

    // 父下标重映射：指向被删掉的骨 → 变根（不是"接到祖父上" —— 那会让
    // 一根原本挂在体内的骨突然跟着祖父转，画面歪得莫名其妙）
    std::vector<int> remap(static_cast<size_t>(oldN), -1);
    int w = 0;
    for (int old = 0; old < oldN; ++old)
        remap[static_cast<size_t>(old)] = killed[static_cast<size_t>(old)] ? -1 : w++;
    for (auto& b : bones) {
        if (b.parent < 0) continue;
        b.parent = (b.parent < oldN) ? remap[static_cast<size_t>(b.parent)] : -1;
    }
    VK_LOG_INFO("RIG-DEL bone='%s' removed=%d left=%d", gone.c_str(),
                static_cast<int>(kill.size()), boneCount());
    return static_cast<int>(kill.size());
}

int RigEdit::connectAll() {
    int changed = 0;
    for (int i : boneOrder(bones)) {
        EditBone& b = bones[static_cast<size_t>(i)];
        if (!b.connected || b.parent < 0) continue;
        const glm::vec3 t = bones[static_cast<size_t>(b.parent)].tail();
        if (!EditBone::sameVec3(b.head, t)) {
            b.head = t;
            ++changed;
        }
    }
    VK_LOG_INFO("RIG-CONNECT bones=%d changed=%d", boneCount(), changed);
    return changed;
}

std::string RigEdit::flipSide(const std::string& name) {
    // 后缀式（Blender / 多数 DCC 导出）：`upperarm.L` ↔ `upperarm.R`
    static const char* kBare[] = {".L", ".R", "_L", "_R", ".l", ".r", "_l", "_r"};
    for (int k = 0; k < 4; ++k) {
        const std::string a = kBare[k * 2];
        const std::string b = kBare[k * 2 + 1];
        if (name.size() > a.size() &&
            name.compare(name.size() - a.size(), a.size(), a) == 0) {
            std::string r = name;
            r.replace(r.size() - a.size(), a.size(), b);
            return r;
        }
        if (name.size() > b.size() &&
            name.compare(name.size() - b.size(), b.size(), b) == 0) {
            std::string r = name;
            r.replace(r.size() - b.size(), b.size(), a);
            return r;
        }
    }
    // 前缀/内嵌式（Mixamo：`mixamorig:LeftArm`）
    const auto swapWord = [&name](const char* a, const char* b) -> std::string {
        const size_t p = name.find(a);
        if (p == std::string::npos) return std::string();
        std::string r = name;
        r.replace(p, std::string(a).size(), b);
        return r;
    };
    std::string r = swapWord("Left", "Right");
    if (!r.empty()) return r;
    r = swapWord("Right", "Left");
    if (!r.empty()) return r;
    r = swapWord("left", "right");
    if (!r.empty()) return r;
    r = swapWord("right", "left");
    if (!r.empty()) return r;
    return std::string();   // 没有左右标记：镜像不了，别猜
}

int RigEdit::mirrorToOpposite(int i) {
    if (i < 0 || i >= boneCount()) return -1;
    const EditBone& src = bones[static_cast<size_t>(i)];
    const std::string other = flipSide(src.name);
    if (other.empty()) {
        VK_LOG_WARN("RIG-MIRROR bone='%s' 没有 .L/.R（或 Left/Right）标记，跳过",
                    src.name.c_str());
        return -1;
    }
    const int k = indexOf(other);
    if (k < 0 || k == i) {
        VK_LOG_WARN("RIG-MIRROR bone='%s' 对面 '%s' 不存在，跳过",
                    src.name.c_str(), other.c_str());
        return -1;
    }

    // 关于"法线 = mirrorAxis、过原点"的平面镜像：位置与方向取反那一根轴，
    // roll 取反。轴是**从数据里认出来的**（见 detectMirrorAxis），不是写死的 X。
    EditBone& dst = bones[static_cast<size_t>(k)];
    const int a = (mirrorAxis >= 0 && mirrorAxis < 3) ? mirrorAxis : 0;
    dst.head = src.head;
    dst.dir = src.dir;
    dst.head[static_cast<glm::length_t>(a)] = -dst.head[static_cast<glm::length_t>(a)];
    dst.dir[static_cast<glm::length_t>(a)] = -dst.dir[static_cast<glm::length_t>(a)];
    dst.length = src.length;
    dst.roll = -src.roll;
    propagateConnected(bones, k);
    VK_LOG_INFO("RIG-MIRROR from='%s' to='%s' axis=%s head=(%.4f,%.4f,%.4f) "
                "roll=%.6f",
                src.name.c_str(), dst.name.c_str(), axisName(a),
                static_cast<double>(dst.head.x), static_cast<double>(dst.head.y),
                static_cast<double>(dst.head.z), static_cast<double>(dst.roll));
    return k;
}

const char* RigEdit::axisName(int axis) {
    switch (axis) {
        case 1: return "Y";
        case 2: return "Z";
        default: return "X";
    }
}

void RigEdit::detectMirrorAxis() {
    // 在每一对 `.L`/`.R` 上量三根轴的分离量，累计最大的那根就是左右轴。
    // 成对的骨少于 2 对时不判断（1 对可能是巧合），保留原值。
    double sep[3] = {0.0, 0.0, 0.0};
    int pairs = 0;
    for (int i = 0; i < boneCount(); ++i) {
        const std::string other = flipSide(bones[static_cast<size_t>(i)].name);
        if (other.empty()) continue;
        const int k = indexOf(other);
        if (k < 0 || k == i) continue;
        if (k < i) continue;    // 每对只数一次
        const glm::vec3 d = bones[static_cast<size_t>(i)].head -
                            bones[static_cast<size_t>(k)].head;
        for (int a = 0; a < 3; ++a)
            sep[a] += std::fabs(static_cast<double>(d[static_cast<glm::length_t>(a)]));
        ++pairs;
    }
    if (pairs < 2) {
        VK_LOG_INFO("RIG-AXIS pairs=%d（太少，保留 axis=%s）", pairs,
                    axisName(mirrorAxis));
        return;
    }
    int best = 0;
    for (int a = 1; a < 3; ++a)
        if (sep[a] > sep[best]) best = a;
    mirrorAxis = best;
    VK_LOG_INFO("RIG-AXIS axis=%s pairs=%d sep=(%.4f,%.4f,%.4f)", axisName(best),
                pairs, sep[0], sep[1], sep[2]);
}

RigInfo RigEdit::info() const {
    RigInfo s;
    s.bones = boneCount();
    for (const auto& b : bones) {
        if (b.parent < 0) ++s.roots;
        if (b.touched()) ++s.touched;
        if (!b.deform) ++s.deformOff;
        if (b.connected) ++s.connected;
    }
    return s;
}

void RigEdit::logSummary(const char* tag) const {
    const RigInfo s = info();
    VK_LOG_INFO("RIG-SUMMARY tag=%s bones=%d roots=%d touched=%d deformOff=%d "
                "connected=%d mirrorAxis=%s",
                tag ? tag : "-", s.bones, s.roots, s.touched, s.deformOff, s.connected,
                axisName(mirrorAxis));
    for (int i = 0; i < boneCount(); ++i) {
        const EditBone& b = bones[static_cast<size_t>(i)];
        VK_LOG_INFO("RIG-BONE i=%d name='%s' parent=%d src=%d head=(%.4f,%.4f,%.4f) "
                    "dir=(%.4f,%.4f,%.4f) len=%.4f roll=%.6f deform=%d conn=%d inh=%d",
                    i, b.name.c_str(), b.parent, b.srcJoint,
                    static_cast<double>(b.head.x), static_cast<double>(b.head.y),
                    static_cast<double>(b.head.z), static_cast<double>(b.dir.x),
                    static_cast<double>(b.dir.y), static_cast<double>(b.dir.z),
                    static_cast<double>(b.length), static_cast<double>(b.roll),
                    b.deform ? 1 : 0, b.connected ? 1 : 0, b.inheritRot ? 1 : 0);
    }
}

// ============================================================
// JSON（venn.rig / version 1）
// ============================================================

bool saveRig(const std::string& path, const RigEdit& rig, std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "路径为空";
        return false;
    }
    json doc;
    doc["format"] = "venn.rig";
    doc["version"] = 1;
    doc["source"] = rig.source;
    // 镜像轴存进文件：它是由骨名+位置推断出来的，重开一次推断结果理应相同，
    // 但存下来才能支持"作者手动改成别的轴"（以及让断言看得见它）。
    doc["mirrorAxis"] = rig.mirrorAxis;

    json arr = json::array();
    for (const auto& b : rig.bones) {
        json o;
        o["name"] = b.name;
        o["parent"] = b.parent;
        o["src"] = b.srcJoint;
        o["head"] = json::array({b.head.x, b.head.y, b.head.z});
        o["dir"] = json::array({b.dir.x, b.dir.y, b.dir.z});
        o["len"] = b.length;
        o["roll"] = b.roll;
        o["deform"] = b.deform;
        o["connected"] = b.connected;
        o["inherit"] = b.inheritRot;
        // 原样矩阵必须存：约定 ① 靠它逐位照抄。浮点直接落 JSON number
        // （nlohmann 对 float 用 round-trip 精度的最短表示），读回来精确。
        o["rest"] = mat4ToJson(b.restLocalOrig);
        o["ext"] = mat4ToJson(b.externalChain);
        o["rh"] = json::array({b.restHeadOrig.x, b.restHeadOrig.y, b.restHeadOrig.z});
        o["rd"] = json::array({b.restDirOrig.x, b.restDirOrig.y, b.restDirOrig.z});
        arr.push_back(std::move(o));
    }
    doc["bones"] = std::move(arr);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "写不了文件";
        return false;
    }
    f << doc.dump(2);
    if (!f) {
        err = "写入失败";
        return false;
    }
    VK_LOG_INFO("RIG-SAVE path='%s' bones=%d", path.c_str(), rig.boneCount());
    return true;
}

bool loadRig(const std::string& path, RigEdit& out, std::string& err) {
    err.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "打不开文件";
        return false;
    }
    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        err = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        err = "顶层不是对象";
        return false;
    }
    const std::string fmt = doc.value("format", std::string());
    if (fmt != "venn.rig") {
        err = "不是 venn.rig（format='" + fmt + "'）";
        return false;
    }
    const int ver = doc.value("version", 0);
    if (ver < 1 || ver > 1) {
        err = "版本不支持（version=" + std::to_string(ver) + "）";
        return false;
    }

    RigEdit r;
    r.source = doc.value("source", std::string());
    const int ax = doc.value("mirrorAxis", 0);
    r.mirrorAxis = (ax >= 0 && ax <= 2) ? ax : 0;
    const auto it = doc.find("bones");
    if (it != doc.end() && it->is_array()) {
        for (const auto& o : *it) {
            if (!o.is_object()) continue;
            EditBone b;
            b.name = o.value("name", std::string());
            if (b.name.empty()) continue;      // 没名字的骨存进去也没用
            b.parent = o.value("parent", -1);
            b.srcJoint = o.value("src", -1);
            b.head = vec3FromJson(o.contains("head") ? o["head"] : json(), glm::vec3(0.0f));
            b.dir = vec3FromJson(o.contains("dir") ? o["dir"] : json(),
                                 glm::vec3(0.0f, 0.0f, 1.0f));
            b.length = o.value("len", 0.1f);
            b.roll = o.value("roll", 0.0f);
            b.deform = o.value("deform", true);
            b.connected = o.value("connected", false);
            b.inheritRot = o.value("inherit", true);
            b.restLocalOrig = mat4FromJson(o.contains("rest") ? o["rest"] : json(),
                                           glm::mat4(1.0f));
            b.externalChain = mat4FromJson(o.contains("ext") ? o["ext"] : json(),
                                           glm::mat4(1.0f));
            b.restHeadOrig = vec3FromJson(o.contains("rh") ? o["rh"] : json(), b.head);
            b.restDirOrig = vec3FromJson(o.contains("rd") ? o["rd"] : json(), b.dir);
            r.bones.push_back(std::move(b));
        }
    }
    // 父下标越界 / 自指 → 当根，别让后续 DFS 掉进环里空转
    const int n = r.boneCount();
    for (int i = 0; i < n; ++i) {
        int& p = r.bones[static_cast<size_t>(i)].parent;
        if (p < -1 || p >= n || p == i) p = -1;
    }
    out = std::move(r);
    VK_LOG_INFO("RIG-LOAD path='%s' bones=%d", path.c_str(), out.boneCount());
    return true;
}

} // namespace animation
