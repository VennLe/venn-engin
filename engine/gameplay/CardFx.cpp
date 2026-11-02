#include "gameplay/CardFx.h"

namespace gameplay {

const CardFxPreset& cardFxPreset(CardFxKind k) {
    // ⚠ 索引就是枚举值，**只能追加**（中间插一条 = 出牌播成消耗的表现，
    //   而这种错在画面上完全"合理"，自查不出来）。
    static const CardFxPreset table[static_cast<int>(CardFxKind::Count)] = {
        // effect                    popup                    atTarget height
        /* Play    */ {"fx/card_play.veffect", PopupKind::Count,  false, 1.55f},
        /* Hit     */ {"fx/card_hit.veffect",  PopupKind::Damage, true,  1.75f},
        /* Crit    */ {"fx/card_hit.veffect",  PopupKind::Crit,   true,  1.85f},
        /* Heal    */ {"fx/heal_glow.veffect", PopupKind::Heal,   true,  1.65f},
        /* Shield  */ {"fx/card_buff.veffect", PopupKind::Shield, true,  1.55f},
        /* Buff    */ {"fx/card_buff.veffect", PopupKind::Buff,   true,  1.70f},
        // 抽 / 弃 / 消耗：**世界里什么都不该发生**。牌堆变化是 UI 的事，
        // 往场上喷火花会让玩家以为"场上出了什么事"。要改就填 effect。
        /* Draw    */ {"", PopupKind::Count, false, 0.0f},
        /* Discard */ {"", PopupKind::Count, false, 0.0f},
        /* Exhaust */ {"", PopupKind::Count, false, 0.0f},
    };
    const int i = static_cast<int>(k);
    if (i < 0 || i >= static_cast<int>(CardFxKind::Count)) return table[0];
    return table[i];
}

} // namespace gameplay
