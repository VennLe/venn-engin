// ============================================================
// sequence/SequenceAsset.cpp
// ============================================================

#include "sequence/SequenceAsset.h"

#include "animation/Curve.h"
#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Entity.h"   // guidToHex / parseGuidHex（多机位 marker 绑实体）

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace sequence {

namespace {

using json = nlohmann::json;

constexpr float kPi = 3.14159265358979323846f;

// 时间比较的物理阈值。0.1 ms —— 比任何一帧都小三个数量级，
// 但足够吸收 JSON 里 `0.1 + 0.2` 这类浮点噪声（0.30000000000000004）。
// ⚠ 不用"逐位相等"判浮点时间：那会让"t 正好等于关键帧"永远不成立，
// 插值段就永远走 `u = 0` 分支，画面看起来像"怎么都不动"。
constexpr float kTimeEps = 1.0e-4f;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// smoothstep 现在住在 `animation/easeFn`（`EaseShape::Smooth` + `InOut`），
// 这里是同一套公式的本地副本 —— **只给自检用**，让"统一之后逐位不变"
// 这件事有一个**独立**的比对对象（拿新实现和新实现比是循环论证）。
// ⚠ 两份会漂移（有人改了 Curve 忘改这里），那正是 `sample_legacy_bitwise`
//   这条断言要抓的；`[[maybe_unused]]` 是因为生产路径已经不再调用它。
// ⚠ 它与 `CameraSystem::blendTo(smoothStep=true)` 也是同一条曲线。
[[maybe_unused]] float smoothstep01(float u) {
    return u * u * (3.0f - 2.0f * u);
}

float wrapPi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

} // namespace

// ---------------------------------------------------------------- 枚举

const char* easeName(Ease e) {
    switch (e) {
    case Ease::Linear: return "linear";
    case Ease::Smooth: return "smooth";
    case Ease::Step: return "step";
    case Ease::Bezier: return "bezier";
    }
    return "smooth";
}

bool parseEase(const std::string& s, Ease& out) {
    // 与 `AnimController` 的 parseOp 同一态度：**不做大小写兼容**。
    // 资产里的拼写错误要在加载期报出来，而不是被"智能匹配"吃掉。
    if (s == "linear") { out = Ease::Linear; return true; }
    if (s == "smooth") { out = Ease::Smooth; return true; }
    if (s == "step") { out = Ease::Step; return true; }
    if (s == "bezier") { out = Ease::Bezier; return true; }
    return false;
}

// ---------------------------------------------------------------- 查询

float SequenceAsset::lastCameraTime() const {
    return cameraKeys.empty() ? 0.0f : cameraKeys.back().t;
}

float SequenceAsset::lastEventTime() const {
    return events.empty() ? 0.0f : events.back().t;
}

float SequenceAsset::lastShotTime() const {
    return shots.empty() ? 0.0f : shots.back().t;
}

// ---------------------------------------------------------------- 解析

bool parseSequence(const std::string& text, const std::string& sourcePath,
                   SequenceAsset& out) {
    out = SequenceAsset{};
    out.sourcePath = sourcePath;

    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        out.error = std::string("JSON parse failed: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        out.error = "top level is not an object";
        return false;
    }

    out.name = j.value("name", std::string());
    if (out.name.empty()) {
        // 文件名兜底（`sequences/opening.seq.json` → `opening`）
        std::string s = sourcePath;
        const std::size_t slash = s.find_last_of("/\\");
        if (slash != std::string::npos) s = s.substr(slash + 1);
        const std::string suf = ".seq.json";
        if (s.size() > suf.size() &&
            s.compare(s.size() - suf.size(), suf.size(), suf) == 0)
            s = s.substr(0, s.size() - suf.size());
        out.name = s.empty() ? "<unnamed>" : s;
    }
    out.loop = j.value("loop", false);
    // `"//"` 是作者写的说明（JSON 没有注释语法）。**必须读进来**：不读的话
    // 编辑器存一次盘就把它抹掉了，而作者不会想到"点 Save 会删注释"。
    out.comment = j.value("//", std::string());

    // ---- 相机关键帧 ----
    if (j.contains("camera") && j["camera"].is_array()) {
        int idx = 0;
        for (const auto& k : j["camera"]) {
            SeqCameraKey ck;
            ck.t = k.value("t", 0.0f);
            if (k.contains("pos") && k["pos"].is_array() && k["pos"].size() == 3) {
                ck.position = glm::vec3(k["pos"][0].get<float>(),
                                        k["pos"][1].get<float>(),
                                        k["pos"][2].get<float>());
            } else if (k.contains("position") && k["position"].is_array() &&
                       k["position"].size() == 3) {
                // 接受长名 —— 手写资产时 `pos` / `position` 都会有人写。
                ck.position = glm::vec3(k["position"][0].get<float>(),
                                        k["position"][1].get<float>(),
                                        k["position"][2].get<float>());
            }
            ck.yaw = k.value("yaw", 0.0f);
            ck.pitch = k.value("pitch", -0.25f);
            ck.fov = k.value("fov", 45.0f);

            const std::string es = k.value("ease", std::string("smooth"));
            if (!parseEase(es, ck.ease)) {
                out.error = "camera key #" + std::to_string(idx) + ": unknown ease '" +
                            es + "' (linear / smooth / step / bezier)";
                return false;
            }

            // ---- 贝塞尔手柄（可选；`ease` 不是 bezier 时读也白读，但读了
            //      才能"切回 bezier 时手柄还在"，作者不会因为切一次就丢设置）----
            auto readHandle = [&](const char* key, glm::vec2& h, bool isOut) {
                if (!k.contains(key) || !k[key].is_array() || k[key].size() != 2)
                    return;
                h.x = k[key][0].get<float>();
                h.y = k[key][1].get<float>();
                // ⚠ x 的符号约束（见 .h）：倒了就是曲线倒流。钳回并留痕，
                //   不拒收 —— 一个手柄写反不该让整个过场打不开。
                if (isOut) {
                    if (h.x < 0.0f) h.x = 0.0f;
                } else {
                    if (h.x > 0.0f) h.x = 0.0f;
                }
                if (h.x > 1.0f) h.x = 1.0f;
                if (h.x < -1.0f) h.x = -1.0f;
            };
            readHandle("outHandle", ck.outHandle, true);
            readHandle("inHandle", ck.inHandle, false);

            out.cameraKeys.push_back(ck);
            ++idx;
        }
    }

    // ---- 事件 ----
    if (j.contains("events") && j["events"].is_array()) {
        int idx = 0;
        for (const auto& e : j["events"]) {
            SeqEvent ev;
            ev.t = e.value("t", 0.0f);
            ev.kind = e.value("kind", std::string());
            ev.payload = e.value("payload", std::string());
            if (ev.kind.empty()) {
                out.error = "event #" + std::to_string(idx) + ": 'kind' is required";
                return false;
            }
            out.events.push_back(ev);
            ++idx;
        }
    }

    // ---- 多机位 marker ----
    //
    // ⚠ 解析**不校验 guid 是否存在于场景**：资产不认识场景（可能先写资产再
    //   摆相机，也可能换工程复用）。指向不存在的实体时由 `CameraSystem`
    //   在运行时回落到"关键帧轨"并打一次 WARN —— 那里才有场景。
    if (j.contains("shots") && j["shots"].is_array()) {
        int idx = 0;
        for (const auto& s : j["shots"]) {
            SeqShot sh;
            sh.t = s.value("t", 0.0f);
            // `camera` 是十六进制字符串（与场景 JSON 的 `guid` 同一写法）。
            // 缺省 / 空串 / 全零 → 0 = "回落到关键帧轨"（合法值，见 .h ②）。
            const std::string cam = s.value("camera", std::string());
            if (!cam.empty()) {
                sh.cameraGuid = ecs::parseGuidHex(cam);
                // ⚠⚠ `parseGuidHex` 要求**恰好 16 位**十六进制（低位要补零），
                //   不满足时返回 0 = "回落关键帧轨"。于是"手写一个短 guid"
                //   会静默退化成老行为 —— 画面照常动，只是不切机位，极难查。
                //   这里把它显式记一条警告（非致命：整份资产仍可播）。
                if (sh.cameraGuid == 0) {
                    out.warnings.push_back(
                        "shot #" + std::to_string(idx) + ": camera '" + cam +
                        "' is not a 16-digit hex guid — falling back to the "
                        "camera-key track");
                }
            }
            sh.blendSeconds = std::max(0.0f, s.value("blend", 0.0f));
            sh.label = s.value("label", std::string());
            out.shots.push_back(std::move(sh));
            ++idx;
        }
        (void)idx;
    }

    // ---- 排序（见文件头 ②）----
    // ⚠ 用 stable_sort：同一 t 上的多个事件保持作者的书写顺序。
    //   普通 sort 会把它们换个位置，日志读起来跟资产对不上。
    std::stable_sort(out.cameraKeys.begin(), out.cameraKeys.end(),
                     [](const SeqCameraKey& a, const SeqCameraKey& b) {
                         return a.t < b.t;
                     });
    std::stable_sort(out.events.begin(), out.events.end(),
                     [](const SeqEvent& a, const SeqEvent& b) { return a.t < b.t; });
    std::stable_sort(out.shots.begin(), out.shots.end(),
                     [](const SeqShot& a, const SeqShot& b) { return a.t < b.t; });

    // 重复关键帧：不致命（退化成硬切），但要让人知道。
    for (std::size_t i = 1; i < out.cameraKeys.size(); ++i) {
        if (std::fabs(out.cameraKeys[i].t - out.cameraKeys[i - 1].t) < kTimeEps) {
            out.warnings.push_back(
                "duplicate camera key time t=" +
                std::to_string(out.cameraKeys[i].t) + "s (behaves as a hard cut)");
        }
    }

    // 负时间：作者手滑写成 −1，那个关键帧/事件永远轮不到 → 必须说。
    for (const auto& k : out.cameraKeys)
        if (k.t < -kTimeEps)
            out.warnings.push_back("camera key with negative t=" + std::to_string(k.t));
    for (const auto& e : out.events)
        if (e.t < -kTimeEps)
            out.warnings.push_back("event '" + e.kind + "' with negative t=" +
                                   std::to_string(e.t));
    for (std::size_t i = 0; i < out.shots.size(); ++i) {
        if (out.shots[i].t < -kTimeEps)
            out.warnings.push_back("shot marker #" + std::to_string(i) +
                                   " with negative t=" + std::to_string(out.shots[i].t));
        // 两个 marker 落在同一时刻：后写的赢（stable_sort 保序），前面那个
        // 区间长度为 0 —— 它永远不会被 `shotIndexAt` 选中。不致命，但作者
        // 多半是拖重叠了，要说。
        if (i > 0 && std::fabs(out.shots[i].t - out.shots[i - 1].t) < kTimeEps) {
            out.warnings.push_back(
                "duplicate shot marker time t=" + std::to_string(out.shots[i].t) +
                "s (the earlier one never becomes active)");
        }
    }

    // ---- duration ----
    const float explicitDur = j.value("duration", 0.0f);
    const float derived =
        std::max({out.lastCameraTime(), out.lastEventTime(), out.lastShotTime(), 0.0f});
    if (explicitDur > kTimeEps) {
        out.duration = explicitDur;
        // 显式 duration 比内容短 → 后半段被切掉。这在"我加了台词却不播"里
        // 是头号原因，必须报（否则就是静默丢内容）。
        if (derived > explicitDur + kTimeEps) {
            out.warnings.push_back(
                "duration=" + std::to_string(explicitDur) +
                "s is shorter than content (" + std::to_string(derived) +
                "s) — the tail will never play");
        }
    } else {
        out.duration = derived;
    }

    // ---- `t == duration` 的事件（见文件头 ③）----
    for (const auto& e : out.events) {
        if (e.t >= out.duration - kTimeEps) {
            out.warnings.push_back(
                "event '" + e.kind + "' at t=" + std::to_string(e.t) +
                "s never fires (interval is [from,to); t must be < duration=" +
                std::to_string(out.duration) + "s)");
        }
    }

    if (out.cameraKeys.empty() && out.events.empty() && out.shots.empty()) {
        out.error = "no 'camera' keys, no 'events' and no 'shots' — nothing to play";
        return false;
    }

    out.valid = true;

    for (const auto& w : out.warnings)
        VK_LOG_WARN("SEQ-ASSET-WARN name=%s %s", out.name.c_str(), w.c_str());

    VK_LOG_INFO("SEQ-ASSET name=%s dur=%.3f loop=%d keys=%d events=%d shots=%d path=%s",
                out.name.c_str(), static_cast<double>(out.duration),
                out.loop ? 1 : 0, static_cast<int>(out.cameraKeys.size()),
                static_cast<int>(out.events.size()),
                static_cast<int>(out.shots.size()), sourcePath.c_str());
    return true;
}

bool loadSequence(const std::string& path, SequenceAsset& out) {
    out = SequenceAsset{};
    out.sourcePath = path;

    if (path.empty()) {
        out.error = "empty path";
        return false;
    }

    const std::string abs = assets::resolveAssetPath(path);
    std::ifstream f(abs, std::ios::binary);
    if (!f) {
        out.error = "cannot open '" + abs + "'";
        VK_LOG_WARN("SEQ-ASSET-MISS path=%s resolved=%s", path.c_str(), abs.c_str());
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    if (!parseSequence(ss.str(), path, out)) {
        VK_LOG_WARN("SEQ-ASSET-BAD path=%s err=%s", path.c_str(),
                    out.error.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 写回

std::string toJson(const SequenceAsset& a) {
    // `ordered_json`：键序保持插入顺序。用普通 `json` 的话键会按字典序排
    // （camera / duration / events / loop / name / version），作者打开文件
    // 最先看到的是 `camera`，而 `version` 被甩到最后 —— 与 docs 里的示例
    // 对不上，diff 也难读。
    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    doc["version"] = 1;
    doc["name"] = a.name;
    // 作者写的说明原样带回去（见 .h 里那段 ⚠⚠）。
    if (!a.comment.empty()) doc["//"] = a.comment;
    doc["loop"] = a.loop;
    // ① 永远写显式 duration（见 .h）
    doc["duration"] = a.duration;

    nlohmann::ordered_json keys = nlohmann::ordered_json::array();
    for (const auto& k : a.cameraKeys) {
        nlohmann::ordered_json jk = nlohmann::ordered_json::object();
        jk["t"] = k.t;
        // 位置写短名 `pos`（`parseSequence` 两种都收，但 `pos` 是这个格式
        // 一直以来的写法，老资产全是它）
        jk["pos"] = nlohmann::ordered_json::array({k.position.x, k.position.y,
                                                   k.position.z});
        // ⚠ yaw / pitch 是**弧度**，与场景 JSON 的 `camera.yaw` 同一口径。
        //   界面上的度 → 弧度转换在编辑器里做，资产层只管弧度。
        jk["yaw"] = k.yaw;
        jk["pitch"] = k.pitch;
        jk["fov"] = k.fov;
        jk["ease"] = easeName(k.ease);   // ② 总是写
        // 手柄只在 bezier 上写：其余模式的段根本不看这两个数，写出来会让
        // diff 里多两行噪声，还会让人以为"改这个有用"。
        if (k.ease == Ease::Bezier) {
            jk["outHandle"] = nlohmann::ordered_json::array({k.outHandle.x,
                                                             k.outHandle.y});
            jk["inHandle"] = nlohmann::ordered_json::array({k.inHandle.x,
                                                            k.inHandle.y});
        }
        keys.push_back(std::move(jk));
    }
    doc["camera"] = std::move(keys);

    // 多机位 marker。**只在非空时写出**：老资产（没有多机位）存一次盘不该
    // 冒出一行 `"shots": []` —— 那会让所有既有 `.seq.json` 的 diff 变脏，
    // 也会让 `rt_*` 那组"写回再读回"的字节对比失去意义。
    if (!a.shots.empty()) {
        nlohmann::ordered_json shots = nlohmann::ordered_json::array();
        for (const auto& s : a.shots) {
            nlohmann::ordered_json js = nlohmann::ordered_json::object();
            js["t"] = s.t;
            // `camera` 缺省 = 0 = "回落到关键帧轨"，此时**不写这个键**
            // （省略与写 "" 是同一件事，但 diff 里少一行噪声）。
            if (s.cameraGuid != 0) js["camera"] = ecs::guidToHex(s.cameraGuid);
            // blend 只在非 0（非硬切）时写：硬切是默认语义，写 `"blend": 0.0`
            // 会让"这是一次硬切"看起来像"作者特意调成了 0"。
            if (s.blendSeconds > 0.0f) js["blend"] = s.blendSeconds;
            if (!s.label.empty()) js["label"] = s.label;
            shots.push_back(std::move(js));
        }
        doc["shots"] = std::move(shots);
    }

    nlohmann::ordered_json evs = nlohmann::ordered_json::array();
    for (const auto& e : a.events) {
        nlohmann::ordered_json je = nlohmann::ordered_json::object();
        je["t"] = e.t;
        je["kind"] = e.kind;
        if (!e.payload.empty()) je["payload"] = e.payload;
        evs.push_back(std::move(je));
    }
    doc["events"] = std::move(evs);

    // indent=2 / 空格 / ensure_ascii=false：中文 payload 不转义（转义之后
    // diff 是一屏 \uXXXX，作者没法用 git 看自己改了什么）。
    return doc.dump(2, ' ', false) + "\n";
}

bool saveSequence(const std::string& path, const SequenceAsset& a,
                  std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    const std::string text = toJson(a);
    const std::string full = assets::resolveAssetPath(path);
    const std::string tmp = full + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open " + tmp + " for writing";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f.good()) {
            err = "write failed: " + tmp;
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, full, ec);
    if (ec) {
        // 改名失败（目标被占用 / 只读）时把临时文件清掉，不要留一地 .tmp
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        err = "rename to " + full + " failed: " + ec.message();
        return false;
    }

    VK_LOG_INFO("SEQ-SAVE path=%s full=%s keys=%d events=%d shots=%d dur=%.3f "
                "bytes=%zu",
                path.c_str(), full.c_str(),
                static_cast<int>(a.cameraKeys.size()),
                static_cast<int>(a.events.size()),
                static_cast<int>(a.shots.size()),
                static_cast<double>(a.duration), text.size());
    return true;
}

// ---------------------------------------------------------------- 采样

float shortestAngle(float from, float to) {
    // 先 wrap 到 (−π, π]，再取差 —— 直接 `to - from` 在跨越 ±π 时
    // 会给出 −340° 这种"绕远路"的值（见文件头 ①）。
    return wrapPi(wrapPi(to) - wrapPi(from));
}

int cameraKeyIndexAt(const SequenceAsset& seq, float t) {
    const int n = static_cast<int>(seq.cameraKeys.size());
    if (n <= 0) return -1;
    if (t <= seq.cameraKeys[0].t) return 0;

    // 逆序扫到第一个 `t <= key.t` 的前一帧。关键帧数量是个位到几十，
    // 线性扫足够；用二分反而要处理"重复 t"的边界。
    for (int i = n - 1; i >= 0; --i) {
        if (seq.cameraKeys[static_cast<std::size_t>(i)].t <= t + kTimeEps) return i;
    }
    return 0;
}

int shotIndexAt(const SequenceAsset& seq, float t) {
    const int n = static_cast<int>(seq.shots.size());
    if (n <= 0) return -1;
    // ⚠⚠ 与 `cameraKeyIndexAt` **故意不同**：早于第一个 marker 返回 −1，
    //   而不是 0（见 `SeqShot` 的文件头 ③）。照抄上面那个函数是这里最容易
    //   犯的错，症状是"黑场那 1 秒突然变成第一个机位的画面"。
    if (t < seq.shots[0].t - kTimeEps) return -1;

    for (int i = n - 1; i >= 0; --i) {
        if (seq.shots[static_cast<std::size_t>(i)].t <= t + kTimeEps) return i;
    }
    return -1;
}

bool sampleCamera(const SequenceAsset& seq, float t, SeqCameraKey& out) {
    const int n = static_cast<int>(seq.cameraKeys.size());
    if (n <= 0) return false;

    if (n == 1) {
        out = seq.cameraKeys[0];
        return true;
    }

    const int i = cameraKeyIndexAt(seq, t);
    if (i >= n - 1) {
        out = seq.cameraKeys[static_cast<std::size_t>(n - 1)];
        return true;
    }

    const SeqCameraKey& a = seq.cameraKeys[static_cast<std::size_t>(i)];
    const SeqCameraKey& b = seq.cameraKeys[static_cast<std::size_t>(i + 1)];
    const float span = b.t - a.t;

    // 重复 t / 零长度区间 → 直接取后半帧（等价硬切）。
    if (span <= kTimeEps) {
        out = b;
        return true;
    }

    const float u = clamp01((t - a.t) / span);

    // ⚠ `Ease::Step` 的作用是"**这一段**保持前帧，在 b.t 那一刻跳过去"。
    //   所以 u 严格 < 1 时全部取 a，只有真的到达 b 才切。这不是"插值",
    //   是"选择"—— 写成 lerp(a,b, u>0.5?1:0) 会在段中间就跳，位置错了。
    if (a.ease == Ease::Step) {
        out = a;
        return true;
    }

    // ---- 四个 ease 模式 → 一个标量 w（2026-10-28 / R5）----
    //
    // 这里是"三份插值实现"收敛的第一处：w 不再由 `if (linear) ... else
    // smoothstep01(u)` 现场决定，而是交给 `animation::evaluateSegmentEase`
    // —— 和骨骼轨道、实体关键帧将来共用同一份缓动/贝塞尔求根。
    //
    // ⚠⚠ 老三分支必须**逐位不变**：
    //     · linear → Interp::Linear + EaseMode::None → 恒等映射，w == u；
    //     · smooth → Linear + InOut + Smooth → 特判成 Hermite smoothstep，
    //       逐位等于原来的 `smoothstep01(u)`；
    //     · step   → 上面已经早退（它连 `out.t` 都要保持 a.t）。
    //   证据在 `SequenceSelfTest` 的 `sample_legacy_bitwise_*` 三条。
    animation::Interp ip = animation::Interp::Linear;
    animation::EaseMode em = animation::EaseMode::None;
    animation::EaseShape sh = animation::EaseShape::Linear;
    switch (a.ease) {
    case Ease::Linear:
        break;   // 全默认 = 恒等
    case Ease::Smooth:
        em = animation::EaseMode::InOut;
        sh = animation::EaseShape::Smooth;
        break;
    case Ease::Bezier:
        ip = animation::Interp::Bezier;
        break;
    case Ease::Step:
        break;   // 不可达（上面已早退），留着让 switch 完整
    }
    const float w = animation::evaluateSegmentEase(ip, em, sh, a.outHandle,
                                                   a.inHandle, u);

    out = a;
    out.t = t;
    out.position = glm::mix(a.position, b.position, w);
    // yaw 走最短弧；pitch 不会绕圈（±π/2 以内），直接线性。
    out.yaw = wrapPi(a.yaw + shortestAngle(a.yaw, b.yaw) * w);
    out.pitch = a.pitch + (b.pitch - a.pitch) * w;
    out.fov = a.fov + (b.fov - a.fov) * w;
    out.ease = a.ease;   // 采样结果没有"下一段"，保留来源只为诊断
    return true;
}

int collectEvents(const SequenceAsset& seq, float from, float to,
                  std::vector<int>* out) {
    if (to <= from) return 0;   // 空区间 / 反向：什么都不发（调用方负责拆跨圈）

    int added = 0;
    for (std::size_t i = 0; i < seq.events.size(); ++i) {
        const float et = seq.events[i].t;
        // 半开 [from, to)。用 kTimeEps 是为了让 `from` 正好等于事件时间时
        // 算"已经越过"而不是"又要发一次"。
        if (et >= from - kTimeEps && et < to - kTimeEps) {
            if (out) out->push_back(static_cast<int>(i));
            ++added;
        }
    }
    return added;
}

} // namespace sequence
