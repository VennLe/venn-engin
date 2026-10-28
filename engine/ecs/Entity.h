#pragma once
// ============================================================
// ecs/Entity —— 实体句柄
//
// EntityId 是"槽位索引"，generation 是"代际号"。
// 实体销毁后槽位被回收复用，同时 generation 自增 —— 于是旧的
// 句柄再过 alive() 检查时 generation 不匹配，会被判为失效。
// 这是 ECS 中防止悬空句柄（use-after-free）的标准手法。
// ============================================================

#include <cstdint>
#include <string>

namespace ecs {

using EntityId = uint32_t;
constexpr EntityId kInvalidEntity = 0xFFFFFFFFu;

// ---------------- 稳定身份（G16） ----------------
// `EntityId` 是槽位索引：销毁后会被复用，重启 / 撤销 / Play 往返都会变 ——
// 所以它**不能**当存档键或跨文件引用的键（今天 `HierarchyComponent::parent`
// 存的就是 id，只在"同一次加载内"有效）。
//
// guid 是与之无关的 64 位随机身份：
//   * 0 = 没有身份（`kInvalidGuid`），**不参与存档**；
//   * `create()` 时分配，`setGuid()` 可覆盖（读档时用文件里的值）；
//   * `destroy()` **不回收**、不复用 —— 否则同一个 guid 会"死而复生"，
//     旧存档里的引用就会静默指向另一个物体。
//
// ⚠ 存盘用 **16 位小写 hex 字符串**，不用 JSON number：nlohmann 能存 uint64，
//   但任何中间工具（Python json → float、JS → 精度截断、Excel → 四舍五入）
//   都会把它改成 `8.917e+18`。存档是要被别的工具读的，字符串是唯一安全的表示。
using EntityGuid = uint64_t;
constexpr EntityGuid kInvalidGuid = 0;

struct Entity {
    EntityId id = kInvalidEntity;
    uint32_t generation = 0;

    bool valid() const { return id != kInvalidEntity; }

    bool operator==(const Entity& o) const {
        return id == o.id && generation == o.generation;
    }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

// ---------------- guid 的文本表示 ----------------
// 存档 / Prefab / 流送清单里**只**用 16 位小写 hex，理由见文件上方。

// ⚠ 必须**补满 16 位前导零**（`0x0F` → `"000000000000000f"`）。
//   不补零的话 `parseGuidHex` 的长度检查会把它判成非法 → 一个低位 guid 的实体
//   在"存一次再读"之后变成"无身份"，接着被随机补发一个新 guid，从此成为另一个物体。
inline std::string guidToHex(EntityGuid g) {
    static const char kHex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; --i) {
        buf[i] = kHex[g & 0xFu];
        g >>= 4;
    }
    buf[16] = '\0';
    return std::string(buf, 16);
}

// ⚠ 这**不是** `std::stoull(s, nullptr, 16)`：后者会接受 `"0x..."`、接受尾随垃圾、
//   且输入非法时抛异常。存档是外部可编辑的，解析必须"全串严格"且不抛。
// 返回值 0（全零串）等同 `kInvalidGuid` —— "写进去的是无身份"这件事要能原样读回。
inline EntityGuid parseGuidHex(const std::string& s) {
    if (s.size() != 16) return kInvalidGuid;
    EntityGuid v = 0;
    for (const char ch : s) {
        unsigned d;
        if (ch >= '0' && ch <= '9') d = static_cast<unsigned>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<unsigned>(ch - 'a') + 10u;
        else if (ch >= 'A' && ch <= 'F') d = static_cast<unsigned>(ch - 'A') + 10u;
        else return kInvalidGuid;  // 任一字符不是 hex → 整串作废，不做"尽力而为"
        v = (v << 4) | d;
    }
    return v;
}

} // namespace ecs
