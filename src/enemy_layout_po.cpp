

#include "enemy_layout.hpp"

#include "m_Do/m_Do_ext.h"
#include "d/d_particle.h"
#include "d/d_kankyo.h"
#include "d/d_com_inf_game.h"

#include "d/actor/d_a_e_po.h"

namespace {

const EnemyActLayoutExt kAct[] = {
    { (int16_t)0x01DA, (uint16_t)offsetof(e_po_class, mActionID), kEnemyNoModeOffset,
      (uint8_t)sizeof(((e_po_class*)nullptr)->mActionID), (uint8_t)0 },
};

}

const EnemyActLayoutExt* coop_enemy_act_po(int& count) {
    count = (int)(sizeof(kAct) / sizeof(kAct[0]));
    return kAct;
}

namespace {
const EnemyAnmLayoutExt kAnm[] = {
    { (int16_t)0x01DA, (uint16_t)offsetof(e_po_class, mpMorf), false, (uint16_t)0xFFFF, "E_PO" },
};
}

const EnemyAnmLayoutExt* coop_enemy_anm_po(int& count) {
    count = (int)(sizeof(kAnm) / sizeof(kAnm[0]));
    return kAnm;
}

bool coop_enemy_po_soul_ripped(const void* actor) {
    if (actor == nullptr) return false;
    const auto* po = static_cast<const e_po_class*>(actor);
    return po->mActionID == ACT_WOLF_BITE || po->mActionID == ACT_DEAD ||
           (po->mType >= 3 && po->mActionID == 7);
}

uint8_t coop_enemy_po_bit_sw(const void* actor) {
    if (actor == nullptr) return 0xFF;
    return static_cast<const e_po_class*>(actor)->BitSW;
}
