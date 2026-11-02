#include "CardsPanel.h"

#include "DebugRects.h"
#include "Localization.h"

#include "ecs/Components.h"
#include "project/CardSource.h"
#include "scene/GameplayRuntime.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cstdio>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + tag;
}

// 区的显示顺序与 `CardPile` 的枚举顺序一致（读起来就是"牌怎么流动"）。
const gameplay::CardPile kPiles[] = {
    gameplay::CardPile::Draw, gameplay::CardPile::Hand, gameplay::CardPile::Discard,
    gameplay::CardPile::Exhaust, gameplay::CardPile::Limbo,
};

const char* pileLabel(gameplay::CardPile p) {
    switch (p) {
    case gameplay::CardPile::Draw: return TR("Draw pile");
    case gameplay::CardPile::Hand: return TR("Hand");
    case gameplay::CardPile::Discard: return TR("Discard");
    case gameplay::CardPile::Exhaust: return TR("Exhaust");
    case gameplay::CardPile::Limbo: return TR("Resolving");
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------- 命令入口

std::string CardsPanel::stateLine(ecs::Entity e) const {
    if (!m_rt) return std::string("cards state=nort");
    return m_rt->cardLine(e);
}

std::string CardsPanel::detail(ecs::Entity e) const {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->cardDetail(e);
}

std::string CardsPanel::ensure(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->ensureDeck(e);
}

std::string CardsPanel::loadPresetDefs(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    ecs::CardDeckComponent* d = m_rt->deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent (add one first)");
    // ⚠ 定义表**不是存档的一部分**（见 `CardZones.h`），所以这里只改内存里的
    //   那一份 —— 存盘不会带走它，读档之后要重新载入。P3-1 会把它换成
    //   "进战斗时从配表读"，那时这条手动入口就退化成调试工具。
    d->zones.setDefs(gameplay::presetCardDefs());
    char buf[96];
    // i18n-keep：`key=value` 诊断行
    std::snprintf(buf, sizeof(buf), "defs=%d", static_cast<int>(d->zones.defs().size()));
    return std::string(buf);
}

std::string CardsPanel::loadTableDefs(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    // 先解析（不依赖实体）：这样"配表坏了"能在**任何**实体上被看见。
    std::vector<gameplay::CardDef> defs;
    const project::CardSourceResult r = project::loadCardDefs(defs);
    // ⚠ 灌进**整个场景**的所有牌堆，不只是选中的那个：定义表是内容，
    //   所有牌堆共用同一份。只灌一个的话，另一个单位的牌仍然 `known=0`。
    const int decks = defs.empty() ? 0 : m_rt->setCardDefs(defs);
    char buf[192];
    // i18n-keep：`key=value` 诊断行
    std::snprintf(buf, sizeof(buf),
                  "cards.defstable found=%d defs=%d skipped=%d errors=%d decks=%d "
                  "dir=%s",
                  r.tableFound ? 1 : 0, r.defs, r.skipped, r.errors, decks,
                  r.dir.c_str());
    (void)e;
    return std::string(buf);
}

std::string CardsPanel::cast(ecs::Entity e, int idx) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    // 目标 = **场上另一个**带玩法组件的单位（没有阵营概念 → "别人"就是敌人）。
    //
    // ⚠ 不要用"自己"当目标：`target=enemy` 的牌会因此解析不到目标，
    //   整条效果被算成 missed，症状是"这张牌打出去一点伤害都没有"，
    //   而 `ok=1`、牌也正常进弃牌堆 —— 看起来像"伤害公式坏了"。
    //   场上只有自己一个时，退化成打自己（技能/护盾那类仍然能验）。
    ecs::Entity target = m_rt->firstOtherGameplayEntity(e);
    if (!target.valid()) target = e;
    return m_rt->castCard(e, idx, target);
}

std::string CardsPanel::addCards(ecs::Entity e, const std::string& defId, int n) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    ecs::CardDeckComponent* d = m_rt->deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    if (defId.empty()) return std::string("FAIL: empty card id");
    if (n <= 0) return std::string("FAIL: count must be > 0");
    // 定义表空的时候顺手载入示例表 —— 否则"造出来的牌没有名字/费用/效果"，
    // 而那种状态在面板上看起来像"这张卡的配表丢了"。
    if (d->zones.defs().empty()) d->zones.setDefs(gameplay::presetCardDefs());
    const uint32_t first = d->zones.create(defId, n);
    char buf[128];
    // i18n-keep：`key=value` 诊断行
    std::snprintf(buf, sizeof(buf), "cards.add id=%s n=%d first-uid=%u draw=%d", defId.c_str(),
                  n, first, static_cast<int>(d->zones.count(gameplay::CardPile::Draw)));
    return std::string(buf);
}

std::string CardsPanel::buildTestDeck(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    if (e.id == 0) return std::string("FAIL: no entity selected");
    m_rt->ensureDeck(e);
    ecs::CardDeckComponent* d = m_rt->deck(e);
    if (!d) return std::string("FAIL: cannot create deck");

    d->zones.clear();
    d->zones.setDefs(gameplay::presetCardDefs());
    d->zones.create("strike", 5);
    d->zones.create("defend", 4);
    d->zones.create("bash", 1);
    d->zones.create("slimed", 1);
    // ⚠ **不洗**：判据要能算准"抽 5 张必然是 5 张打击"。
    //   洗牌是单独的动词（`cards.shuffle` / 按钮），
    //   把两件事合起来会让"抽到了预期之外的一张"既可能是洗牌、也可能是排序。
    char buf[128];
    // i18n-keep：`key=value` 诊断行
    std::snprintf(buf, sizeof(buf), "cards.testdeck draw=%d total=%d",
                  static_cast<int>(d->zones.count(gameplay::CardPile::Draw)),
                  static_cast<int>(d->zones.total()));
    return std::string(buf);
}

std::string CardsPanel::drawCards(ecs::Entity e, int n) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->cardDraw(e, n);
}

std::string CardsPanel::play(ecs::Entity e, int idx) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->cardPlay(e, idx);
}

std::string CardsPanel::resolveLast(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->cardResolveLast(e);
}

std::string CardsPanel::discardHand(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->cardDiscardHand(e);
}

std::string CardsPanel::shuffle(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->cardShuffle(e);
}

std::string CardsPanel::sweep(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    ecs::CardDeckComponent* d = m_rt->deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    const int n = d->zones.sweepLimbo();
    char buf[128];
    // i18n-keep：`key=value` 诊断行
    std::snprintf(buf, sizeof(buf), "cards.sweep n=%d limbo=%d discard=%d exhaust=%d", n,
                  static_cast<int>(d->zones.count(gameplay::CardPile::Limbo)),
                  static_cast<int>(d->zones.count(gameplay::CardPile::Discard)),
                  static_cast<int>(d->zones.count(gameplay::CardPile::Exhaust)));
    return std::string(buf);
}

std::string CardsPanel::roundTrip(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    ecs::CardDeckComponent* d = m_rt->deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");

    const std::string text = d->zones.serialize();
    gameplay::CardZones copy;
    // 定义表不参与往返（它不属于存档），但拷一份能保证比较的是"牌"而不是
    // "牌 + 定义"。
    copy.setDefs(d->zones.defs());
    std::string err;
    const bool ok = copy.deserialize(text, &err);

    // 比较**每个区里每张牌的 uid 顺序**，不是只比张数：
    // 张数相同而顺序不同 = "抽到的下一张变了"，那正是存档最该保证的东西。
    bool same = ok;
    for (int i = 0; i < gameplay::kCardPileCount && same; ++i) {
        const gameplay::CardPile p = static_cast<gameplay::CardPile>(i);
        const std::vector<gameplay::CardInstance>& a = d->zones.pile(p);
        const std::vector<gameplay::CardInstance>& b = copy.pile(p);
        if (a.size() != b.size()) {
            same = false;
            break;
        }
        for (std::size_t k = 0; k < a.size(); ++k) {
            if (a[k].uid != b[k].uid || a[k].defId != b[k].defId) {
                same = false;
                break;
            }
        }
    }
    char buf[160];
    // i18n-keep：`key=value` 诊断行
    std::snprintf(buf, sizeof(buf), "cards.roundtrip ok=%d same=%d bytes=%d cards=%d",
                  ok ? 1 : 0, same ? 1 : 0, static_cast<int>(text.size()),
                  static_cast<int>(d->zones.total()));
    return std::string(buf);
}

std::string CardsPanel::reset(ecs::Entity e) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    ecs::CardDeckComponent* d = m_rt->deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    d->zones.clear();
    return std::string("cards.reset total=0");
}

// ---------------------------------------------------------------- 绘制

void CardsPanel::draw(ecs::Entity selected) {
    if (!ImGui::Begin(TR("Cards"), nullptr)) {
        ImGui::End();
        return;
    }
    tagItem("CP-RECT");

    if (!m_rt) {
        ImGui::TextDisabled("%s", TR("No gameplay runtime."));
        tagItem("CP-NORT");
        ImGui::End();
        return;
    }

    // ---- 选中实体 ----
    if (!selected.valid()) {
        ImGui::TextDisabled("%s", TR("Select an entity in the Hierarchy first."));
        tagItem("CP-NOSEL");
        ImGui::End();
        return;
    }
    {
        const std::string s = m_rt->cardLine(selected);
        ImGui::TextUnformatted(s.c_str());
        tagItem("CP-STATE");
    }

    ImGui::Separator();

    // ---- 第一步：挂牌组 ----
    //
    // ⚠ 分开一个按钮而不是"自动挂"：脚本里"没牌组的单位抽牌"应该是一个
    //   明确的失败，而不是悄悄给他新建一副空牌。面板上给显式动作，
    //   行为就可解释。
    if (ImGui::Button(idLabel(TR("Add deck"), "##CP-ENSURE").c_str())) {
        m_status = ensure(selected);
    }
    tagItem("CP-ENSURE");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Load card defs"), "##CP-LOADDEFS").c_str())) {
        m_status = loadPresetDefs(selected);
    }
    tagItem("CP-LOADDEFS");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Build test deck"), "##CP-TESTDECK").c_str())) {
        m_status = buildTestDeck(selected);
    }
    tagItem("CP-TESTDECK");

    if (const ecs::CardDeckComponent* d = m_rt->deck(selected)) {
        // 定义表是"内容"，不属于存档 —— 面板上必须显示张数，否则
        // "读档之后每张牌都没有名字"会被当成 bug 而不是"忘了载入配表"。
        ImGui::TextDisabled("%s: %d", TR("Card defs loaded"),
                            static_cast<int>(d->zones.defs().size()));
        tagItem("CP-DEFCOUNT");
    }

    ImGui::Separator();

    // ---- 造牌 ----
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText(idLabel(TR("id"), "##CP-ADDID").c_str(), m_addDef, sizeof(m_addDef));
    tagItem("CP-ADDID");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::InputInt(idLabel(TR("n"), "##CP-ADDN").c_str(), &m_addCount);
    if (m_addCount < 1) m_addCount = 1;
    tagItem("CP-ADDN");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Add to draw"), "##CP-ADD").c_str())) {
        m_status = addCards(selected, m_addDef, m_addCount);
    }
    tagItem("CP-ADD");

    // ---- 流程动词 ----
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt(idLabel(TR("draw n"), "##CP-DRAWN").c_str(), &m_drawCount);
    if (m_drawCount < 1) m_drawCount = 1;
    tagItem("CP-DRAWN");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Draw"), "##CP-DRAW").c_str())) {
        m_status = drawCards(selected, m_drawCount);
    }
    tagItem("CP-DRAW");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Discard hand"), "##CP-DISCARDHAND").c_str())) {
        m_status = discardHand(selected);
    }
    tagItem("CP-DISCARDHAND");
    ImGui::SameLine();
    // P3-1：**打出并结算**（与上面两个"搬牌"动词的区别是它会真的改数值）。
    // 手牌下标固定用 0：面板上要验的是"效果落到了谁身上"，
    // 不是"第几张牌"——后者由 `cards.play` 的 idx 参数覆盖。
    if (ImGui::Button(idLabel(TR("Cast #0"), "##CP-CAST").c_str())) {
        m_status = cast(selected, 0);
    }
    tagItem("CP-CAST");

    if (ImGui::Button(idLabel(TR("Shuffle draw pile"), "##CP-SHUFFLE").c_str())) {
        m_status = shuffle(selected);
    }
    tagItem("CP-SHUFFLE");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Resolve played"), "##CP-RESOLVE").c_str())) {
        m_status = resolveLast(selected);
    }
    tagItem("CP-RESOLVE");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Sweep resolving"), "##CP-SWEEP").c_str())) {
        m_status = sweep(selected);
    }
    tagItem("CP-SWEEP");

    if (ImGui::Button(idLabel(TR("Round-trip"), "##CP-ROUNDTRIP").c_str())) {
        m_status = roundTrip(selected);
    }
    tagItem("CP-ROUNDTRIP");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Clear all"), "##CP-RESET").c_str())) {
        m_status = reset(selected);
    }
    tagItem("CP-RESET");
    ImGui::SameLine();
    // P3-1：从**配表**载入定义（出包之后游戏靠的就是这条路径）。
    if (ImGui::Button(idLabel(TR("Defs from table"), "##CP-DEFSTABLE").c_str())) {
        m_status = loadTableDefs(selected);
    }
    tagItem("CP-DEFSTABLE");

    if (!m_status.empty()) {
        ImGui::TextWrapped("%s", m_status.c_str());
        tagItem("CP-STATUS");
    }

    ImGui::Separator();

    // ---- 五个区 ----
    //
    // ⚠ 用 `BeginChild` 限高：一副完整卡组 20+ 张，不限高会把下面的东西
    //   全推出屏幕（而"推出屏幕"的控件 `GetItemRect` 照样返回真实坐标，
    //   于是按 rect 点击的用例会点到虚空）。
    if (ImGui::BeginChild("##CP-LIST", ImVec2(0.0f, 200.0f), true)) {
        tagItem("CP-LIST");
        const ecs::CardDeckComponent* dc = m_rt->deck(selected);
        if (!dc) {
            ImGui::TextDisabled("%s", TR("No deck on this entity."));
        } else {
            for (gameplay::CardPile p : kPiles) {
                const std::vector<gameplay::CardInstance>& v = dc->zones.pile(p);
                ImGui::TextDisabled("%s (%d)", pileLabel(p), static_cast<int>(v.size()));
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const gameplay::CardInstance& c = v[i];
                    const gameplay::CardDef* def = dc->zones.def(c.defId);
                    // `no def` 必须显示：读档之后定义表还没载入时，每张牌都会
                    // 走到这个分支 —— 那是正常的，但它和"这张卡的 id 拼错了"
                    // 长得一样，所以要把 uid 一起印出来。
                    char line[160];
                    // i18n-keep：牌实例的调试读数（id 与数值，不翻译）
                    std::snprintf(line, sizeof(line), "#%zu uid=%u %s%s cost=%d", i, c.uid,
                                  c.defId.c_str(), def ? "" : " (no def)",
                                  def ? dc->zones.costOf(c) : -1);
                    if (p == gameplay::CardPile::Hand) {
                        // 手牌可点 = 打出。这是"界面点得到"的关键：判据要能
                        // 通过点第 N 张牌验"打出的确实是那张"。
                        char btn[200];
                        // i18n-keep：同上一行（隐藏 id 后缀是控件身份，不翻译）
                        std::snprintf(btn, sizeof(btn), "%s##CP-HAND-%zu", line, i);
                        if (ImGui::Button(btn)) {
                            m_status = play(selected, static_cast<int>(i));
                        }
                        if (i == 0) tagItem("CP-HAND0");
                    } else {
                        ImGui::TextUnformatted(line);
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    tagItem("CP-LIST-END");

    ImGui::End();
}

} // namespace editor
