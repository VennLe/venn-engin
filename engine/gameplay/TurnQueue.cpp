#include "gameplay/TurnQueue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gameplay {

namespace {

// 剩余时间小于这个值就当成"已经满了"。浮点累加 `gauge += speed*dt` 几乎
// 不可能恰好等于 max，用 `>=` 判断会让"还差 1e-7"的人永远轮不到 —— 症状是
// **队列卡死在某一个人身上**（他动完清零，别人差一点点到不了）。
constexpr float kGaugeEps = 1.0e-3f;

} // namespace

void TurnQueue::setGaugeMax(float v) {
    if (!(v > 0.0f)) return;
    // 改上限时按比例缩放已有 gauge，否则"把上限从 1000 调到 100"会让
    // 所有人都变成满槽（瞬间全员插队）。
    const float scale = v / m_gaugeMax;
    for (TurnActor& a : m_actors) a.gauge *= scale;
    m_gaugeMax = v;
}

bool TurnQueue::add(const TurnActor& a) {
    if (a.id == 0) return false;
    if (findMut(a.id)) return false;
    TurnActor copy = a;
    if (!(copy.speed > 0.0f)) copy.speed = 1.0f;   // 速度 0 = 冻结，不当它存在
    copy.order = m_nextOrder++;
    // 进来就带 gauge 的（存档恢复 / 半路加入）保留原值，但夹到 [0, max]。
    if (!(copy.gauge >= 0.0f)) copy.gauge = 0.0f;
    if (copy.gauge > m_gaugeMax) copy.gauge = m_gaugeMax;
    m_actors.push_back(std::move(copy));
    return true;
}

bool TurnQueue::remove(uint32_t id) {
    for (std::size_t i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].id != id) continue;
        m_actors.erase(m_actors.begin() + static_cast<std::ptrdiff_t>(i));
        if (m_acting == id) m_acting = 0;
        return true;
    }
    return false;
}

bool TurnQueue::has(uint32_t id) const { return find(id) != nullptr; }

const TurnActor* TurnQueue::find(uint32_t id) const {
    for (const TurnActor& a : m_actors) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

TurnActor* TurnQueue::findMut(uint32_t id) {
    for (TurnActor& a : m_actors) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

bool TurnQueue::kill(uint32_t id) {
    TurnActor* a = findMut(id);
    if (!a) return false;
    a->alive = false;
    a->gauge = 0.0f;
    if (m_acting == id) m_acting = 0;
    return true;
}

std::size_t TurnQueue::aliveCount() const {
    std::size_t n = 0;
    for (const TurnActor& a : m_actors) {
        if (a.alive) ++n;
    }
    return n;
}

void TurnQueue::clear() {
    m_actors.clear();
    m_acting = 0;
    m_turn = 0;
    m_nextOrder = 1;
    m_nextOrderFront = 0;
}

float TurnQueue::remaining(const TurnActor& a) const {
    if (!a.alive) return 1.0e30f;
    const float need = m_gaugeMax - a.gauge;
    if (need <= kGaugeEps) return 0.0f;
    return need / a.speed;
}

uint32_t TurnQueue::advance(float dt) {
    if (!(dt > 0.0f)) dt = 0.0f;
    for (TurnActor& a : m_actors) {
        if (a.alive) a.gauge += a.speed * dt;
    }

    // ⚠ 挑"order 最小"而不是"gauge 最大"：gauge 经过不同的累加路径后
    //   会差出 1e-4 这种量，`gauge 最大` 的判据会让出手顺序在两次运行
    //   之间翻转；order 是整数，永远不会因为浮点抖动而变。
    uint32_t best = 0;
    int bestOrder = 0;
    for (const TurnActor& a : m_actors) {
        if (!a.alive) continue;
        if (a.gauge + kGaugeEps < m_gaugeMax) continue;
        if (best == 0 || a.order < bestOrder) {
            best = a.id;
            bestOrder = a.order;
        }
    }

    if (best == 0) {
        m_acting = 0;
        return 0;
    }
    TurnActor* a = findMut(best);
    // 只扣一个槽：dt 很大时多出来的累积留给下一次 `advance(0)`，
    // 于是"速度极快"的人真的能连动，而不是被一次吞掉。
    a->gauge -= m_gaugeMax;
    if (a->gauge < 0.0f) a->gauge = 0.0f;
    m_acting = best;
    ++m_turn;
    return best;
}

uint32_t TurnQueue::nextActor() const {
    uint32_t best = 0;
    float bestT = 1.0e30f;
    int bestOrder = 0;
    for (const TurnActor& a : m_actors) {
        if (!a.alive) continue;
        const float t = remaining(a);
        if (t >= 1.0e30f) continue;
        // 先用时间比，时间相等（含都为 0）再比 order —— 与 `advance` 的
        // tie-break 必须一致，否则"预读条显示 A 先动"而实际 B 先动。
        const bool better = (best == 0) || (t < bestT - 1.0e-6f) ||
                            (std::fabs(t - bestT) <= 1.0e-6f && a.order < bestOrder);
        if (better) {
            best = a.id;
            bestT = t;
            bestOrder = a.order;
        }
    }
    return best;
}

float TurnQueue::timeToNext() const {
    float best = 1.0e30f;
    bool any = false;
    for (const TurnActor& a : m_actors) {
        if (!a.alive) continue;
        any = true;
        best = std::min(best, remaining(a));
    }
    if (!any) return -1.0f;
    if (best >= 1.0e30f) return -1.0f;
    return best;
}

std::vector<std::pair<uint32_t, float>> TurnQueue::timeline(int n) const {
    std::vector<std::pair<uint32_t, float>> out;
    if (n <= 0) return out;
    // 拷贝一份模拟。actor 数是个位数到几十，拷一份比"算完再还原"安全得多
    // （还原漏一个字段 = 预读条把队列搞坏）。
    TurnQueue sim = *this;
    float now = 0.0f;   // 从"此刻"起累计了多少秒
    for (int i = 0; i < n; ++i) {
        float bestT = 1.0e30f;
        for (const TurnActor& a : sim.m_actors) {
            if (!a.alive) continue;
            bestT = std::min(bestT, sim.remaining(a));
        }
        if (bestT >= 1.0e30f) break;
        // 已经满槽的人 `remaining == 0` → 不需要等待，直接在同一时刻出手。
        now += bestT;
        const uint32_t id = sim.advance(bestT);
        if (id == 0) break;
        out.push_back(std::make_pair(id, now));
        if (!sim.endTurn()) break;
    }
    return out;
}

std::vector<uint32_t> TurnQueue::preview(int n) const {
    std::vector<uint32_t> out;
    const std::vector<std::pair<uint32_t, float>> rows = timeline(n);
    out.reserve(rows.size());
    for (const std::pair<uint32_t, float>& r : rows) out.push_back(r.first);
    return out;
}

uint32_t TurnQueue::beginTurn(uint32_t id) {
    if (id == 0) return 0;
    const TurnActor* a = find(id);
    if (!a || !a->alive) return 0;
    m_acting = id;
    ++m_turn;
    return id;
}

bool TurnQueue::endTurn() {
    if (m_acting == 0) return false;
    if (TurnActor* a = findMut(m_acting)) {
        a->gauge = 0.0f;
    }
    m_acting = 0;
    return true;
}

bool TurnQueue::insertNow(uint32_t id) {
    TurnActor* a = findMut(id);
    if (!a || !a->alive) return false;
    a->gauge = m_gaugeMax;
    // 负数 order → 排在所有正常加入者之前（含已经满槽的）。
    a->order = --m_nextOrderFront;
    return true;
}

bool TurnQueue::cancelAction(uint32_t id) {
    TurnActor* a = findMut(id);
    if (!a) return false;
    a->gauge = 0.0f;
    if (m_acting == id) m_acting = 0;
    return true;
}

std::string TurnQueue::orderPreview(int n) const {
    // ⚠ 括号里印的是 `timeline` 给的**绝对时刻**，不是 `find(id)->remaining()`。
    //   之前用后者：同一个人在这场预读里出现三次，就印三次同一个"剩余时间"
    //   （`A(5.00) > B(10.00) > A(5.00)`），面板上看起来像时间倒流。
    const std::vector<std::pair<uint32_t, float>> rows = timeline(n);
    std::string s;
    char buf[192];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const TurnActor* a = find(rows[i].first);
        std::snprintf(buf, sizeof(buf), "%s%s(%.2f)",
                      s.empty() ? "" : " > ",
                      (a && !a->name.empty()) ? a->name.c_str() : "?",
                      static_cast<double>(rows[i].second));
        s += buf;
    }
    return s;
}

std::string TurnQueue::dump() const {
    char buf[192];
    const TurnActor* act = find(m_acting);
    std::snprintf(buf, sizeof(buf), "acting=%s turn=%d n=%d alive=%d max=%.0f",
                  (m_acting && act) ? act->name.c_str() : "-", m_turn,
                  static_cast<int>(m_actors.size()),
                  static_cast<int>(aliveCount()), static_cast<double>(m_gaugeMax));
    return std::string(buf);
}

// ---------------------------------------------------------------- 存档
//
// 用文本行而不是 JSON：这一层不引 nlohmann，好让 `--selftest gameplay`
// 能在"只有纯 C++ 标准库"的环境里跑。格式固定、字段名自带，改字段时
// 老存档会被**忽略**而不是解析错位。

std::string TurnQueue::serialize() const {
    std::string s;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "turnqueue v1 max=%.4f acting=%u turn=%d order=%d front=%d\n",
                  static_cast<double>(m_gaugeMax), m_acting, m_turn, m_nextOrder,
                  m_nextOrderFront);
    s += buf;
    for (const TurnActor& a : m_actors) {
        std::snprintf(buf, sizeof(buf),
                      "a id=%u order=%d speed=%.4f gauge=%.4f alive=%d team=%d name=%s\n",
                      a.id, a.order, static_cast<double>(a.speed),
                      static_cast<double>(a.gauge), a.alive ? 1 : 0, a.team,
                      a.name.c_str());
        s += buf;
    }
    return s;
}

namespace {

// `k=value` 里取 value。找不到返回空串。
std::string field(const std::string& line, const std::string& key) {
    const std::string pat = key + "=";
    std::size_t p = 0;
    while ((p = line.find(pat, p)) != std::string::npos) {
        // 必须是"词首"（前一个字符是空格或行首），否则 `order=` 会命中
        // `front=` 里的子串？不会，但 `name=` 会命中 `nickname=`。
        if (p == 0 || line[p - 1] == ' ') {
            const std::size_t b = p + pat.size();
            std::size_t e = b;
            // name 是最后一个字段，允许带空格 → 取到行尾。
            if (key == "name") e = line.size();
            else while (e < line.size() && line[e] != ' ') ++e;
            return line.substr(b, e - b);
        }
        p += pat.size();
    }
    return std::string();
}

} // namespace

bool TurnQueue::deserialize(const std::string& text, std::string* err) {
    const auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        m_actors.clear();
        m_acting = 0;
        m_turn = 0;
        m_nextOrder = 1;
        m_nextOrderFront = 0;
        return false;
    };

    if (text.find("turnqueue v1") != 0) return fail("bad header (want 'turnqueue v1')");

    TurnQueue tmp;
    std::size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (line.empty()) continue;

        if (line.compare(0, 2, "a ") == 0) {
            TurnActor a;
            a.id = static_cast<uint32_t>(std::strtoul(field(line, "id").c_str(), nullptr, 10));
            a.order = std::atoi(field(line, "order").c_str());
            a.speed = static_cast<float>(std::atof(field(line, "speed").c_str()));
            a.gauge = static_cast<float>(std::atof(field(line, "gauge").c_str()));
            a.alive = field(line, "alive") != "0";
            a.team = std::atoi(field(line, "team").c_str());
            a.name = field(line, "name");
            if (a.id == 0) return fail("actor with id=0 at line " + std::to_string(lineNo));
            if (!(a.speed > 0.0f)) a.speed = 1.0f;
            // ⚠ 直接 push，不走 `add()` —— add 会重新分配 order，
            //   存档里的出手顺序就丢了（表现是"读档后所有人的先后被重排"）。
            tmp.m_actors.push_back(std::move(a));
        } else if (line.compare(0, 9, "turnqueue") == 0) {
            tmp.m_gaugeMax = static_cast<float>(std::atof(field(line, "max").c_str()));
            if (!(tmp.m_gaugeMax > 0.0f)) tmp.m_gaugeMax = kDefaultGaugeMax;
            tmp.m_acting = static_cast<uint32_t>(
                std::strtoul(field(line, "acting").c_str(), nullptr, 10));
            tmp.m_turn = std::atoi(field(line, "turn").c_str());
            tmp.m_nextOrder = std::atoi(field(line, "order").c_str());
            tmp.m_nextOrderFront = std::atoi(field(line, "front").c_str());
        }
        // 其它行一律忽略（向前兼容：以后加字段不会被老版本当成错误）
    }

    if (tmp.m_nextOrder <= 0) {
        tmp.m_nextOrder = 1;
        for (const TurnActor& a : tmp.m_actors) {
            tmp.m_nextOrder = std::max(tmp.m_nextOrder, a.order + 1);
        }
    }
    // acting 指向一个不存在的 id = 存档被截断，清掉而不是让 endTurn 空转。
    if (tmp.m_acting != 0 && !tmp.find(tmp.m_acting)) tmp.m_acting = 0;

    *this = std::move(tmp);
    return true;
}

} // namespace gameplay
