

#include "mod.hpp"
#include "net/messages.hpp"
#include "print.hpp"

#include "mods/svc/hook.hpp"
#include "mods/svc/save.h"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_npc_gwolf.h"
#include "d/actor/d_a_obj_wind_stone.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"

#include <cstring>

extern const SaveService* svc_save;

DEFINE_HOOK_SYMBOL("dComIfGs_onEventBit", void(u16), SkillsOnEventBitHook);
DEFINE_HOOK_SYMBOL("dSv_event_c::onEventBit", void(dSv_event_c*, u16), SkillsSvOnEventBitHook);

namespace {

const int kSkillFirst = 0x152;
const int kSkillCount = 7;
const int kHowlFirst = 0x1D8;
const int kHowlCount = 6;
const int kTracked = kSkillCount + kHowlCount;

const int kWolfTmpFirst = 0x5B;
const int kWolfTmpCount = 7;

const int kWolfGoneFirst = 0x1EC;
const int kWolfGoneCount = 6;

const uint8_t kWolfSaveTbl[kWolfGoneCount] = {0x00, 0x06, 0x06, 0x0A, 0x03, 0x09};
const uint8_t kWolfSwBit[kWolfGoneCount] = {0x41, 0x29, 0x2A, 0x32, 0x79, 0x32};

uint16_t s_earned = 0;
bool s_haveEarned = false;
bool s_applying = false;
uint16_t s_sent = 0;
uint8_t s_sentGone = 0;
uint32_t s_tick = 0;
const uint32_t kResendTicks = 300;

struct SkillsBlob {
    uint32_t version;
    uint16_t earned;
    uint16_t pad;
};
const uint32_t kBlobVersion = 1;

uint16_t label(int k) {
    const int idx = k < kSkillCount ? kSkillFirst + k : kHowlFirst + (k - kSkillCount);
    return static_cast<uint16_t>(dSv_event_flag_c::saveBitLabels[idx]);
}

uint16_t howl_label(int i) {
    return static_cast<uint16_t>(dSv_event_flag_c::saveBitLabels[kHowlFirst + i]);
}

uint16_t gone_label(int i) {
    return static_cast<uint16_t>(dSv_event_flag_c::saveBitLabels[kWolfGoneFirst + i]);
}

int tracked_index(uint16_t flag) {
    for (int k = 0; k < kTracked; ++k) {
        if (label(k) == flag) return k;
    }
    return -1;
}

bool in_game() {
    return daAlink_getAlinkActorClass() != nullptr && dComIfGp_getStartStageName() != nullptr;
}

bool flag_on(int k) {
    return g_dComIfG_gameInfo.info.getSavedata().getEvent().isEventBit(label(k)) != 0;
}

uint16_t have_mask() {
    uint16_t mask = 0;
    for (int k = 0; k < kTracked; ++k) {
        if (flag_on(k)) mask |= static_cast<uint16_t>(1u << k);
    }
    return mask;
}

uint8_t wolf_gone_mask() {
    uint8_t mask = 0;
    for (int i = 0; i < kWolfGoneCount; ++i) {
        if (g_dComIfG_gameInfo.info.getSavedata().getEvent().isEventBit(gone_label(i)) != 0) {
            mask |= static_cast<uint8_t>(1u << i);
        }
    }
    return mask;
}

void save_earned() {
    if (svc_save == nullptr) return;
    SkillsBlob blob{kBlobVersion, s_earned, 0};
    svc_save->set_blob(mod_ctx, "skills", &blob, sizeof(blob));
}

void load_earned() {
    s_haveEarned = true;
    SkillsBlob blob{};
    size_t size = sizeof(blob);
    if (svc_save != nullptr && svc_save->get_blob(mod_ctx, "skills", &blob, &size) == MOD_OK &&
        size == sizeof(blob) && blob.version == kBlobVersion) {
        s_earned = blob.earned;
    } else {
        s_earned = have_mask();
        save_earned();
    }
    s_sent = 0;
    s_sentGone = 0;
    coop_log::info("coop_mod: [SKILLS] earned {:#06x}, have {:#06x}, wolfGone {:#04x}", s_earned,
        have_mask(), wolf_gone_mask());
}

void on_save_loaded(ModContext*, uint32_t, void*) { load_earned(); }

void on_new_save(ModContext*, uint32_t, void*) {
    s_haveEarned = true;
    s_earned = 0;
    s_sent = 0;
    s_sentGone = 0;
    save_earned();
}

void mark_earned(u16 flag) {
    if (s_applying) return;
    for (int i = 0; i < kWolfGoneCount; ++i) {
        if (gone_label(i) == flag) {
            s_sent = 0;
            s_sentGone = 0;
            return;
        }
    }
    const int k = tracked_index(flag);
    if (k < 0) return;
    const uint16_t bit = static_cast<uint16_t>(1u << k);
    if (s_earned & bit) return;
    s_earned |= bit;
    save_earned();
    coop_log::info("coop_mod: [SKILLS] earned {} {} here", k < kSkillCount ? "skill" : "howl",
        k < kSkillCount ? k + 1 : k - kSkillCount + 2);
    s_sent = 0;
    s_sentGone = 0;
}

HookAction on_event_bit_pre(ModContext*, void* args, void*, void*) {
    mark_earned(mods::arg<u16>(args, 0));
    return HOOK_CONTINUE;
}

HookAction on_sv_event_bit_pre(ModContext*, void* args, void*, void*) {
    if (mods::arg<dSv_event_c*>(args, 0) != &g_dComIfG_gameInfo.info.getSavedata().getEvent()) {
        return HOOK_CONTINUE;
    }
    mark_earned(mods::arg<u16>(args, 1));
    return HOOK_CONTINUE;
}

struct GWolfAccess : daNpc_GWolf_c {
    using daNpcF_c::mHide;
};

void* sync_skill_actor_cb(void* proc, void*) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (actor == nullptr) return nullptr;
    const s16 name = fopAcM_GetName(actor);
    if (name == fpcNm_Obj_WindStone_e) {
        auto* stone = static_cast<daWindStone_c*>(actor);
        if (!stone->chkEveOccur()) {
            const u8 sw2 = stone->getSwBit2();
            if (sw2 != 0xFF && !fopAcM_isSwitch(stone, sw2)) {
                fopAcM_onSwitch(stone, sw2);
            }
            const u8 sw1 = stone->getSwBit1();
            if (sw1 != 0xFF && fopAcM_isSwitch(stone, sw1)) {
                fopAcM_offSwitch(stone, sw1);
            }
        }
    } else if (name == fpcNm_NPC_GWOLF_e) {
        auto* wolf = static_cast<GWolfAccess*>(actor);
        if (wolf->mType == 0 && wolf->mParamMode >= 0 && wolf->mParamMode < 7) {
            if (wolf->isDelete()) {
                fopAcM_delete(wolf);
                coop_log::info("coop_mod: [SKILLS] removed retired golden wolf mode={}",
                    static_cast<int>(wolf->mParamMode));
            } else if (wolf->mParamMode >= 1 &&
                       dComIfGs_isEventBit(howl_label(wolf->mParamMode - 1)) && wolf->mHide) {
                wolf->mHide = false;
                coop_log::info("coop_mod: [SKILLS] unhid golden wolf mode={}",
                    static_cast<int>(wolf->mParamMode));
            }
        } else if (wolf->mType == 1 && wolf->mParamMode >= 1 && wolf->mParamMode <= 6) {
            if (dComIfGs_isEventBit(howl_label(wolf->mParamMode - 1))) {
                fopAcM_delete(wolf);
                coop_log::info("coop_mod: [SKILLS] removed howling stone wolf mode={}",
                    static_cast<int>(wolf->mParamMode));
            }
        }
    }
    return nullptr;
}

void sync_wolf_world_state(bool sweepActors) {
    if (!in_game()) return;
    stage_stag_info_class* stag = dComIfGp_getStageStagInfo();
    const int curSaveTbl = stag != nullptr ? dStage_stagInfo_GetSaveTbl(stag) : -1;

    int learnedHowlSkills = 0;
    for (int k = 0; k < kSkillCount; ++k) {
        if (k != 1 && flag_on(k)) ++learnedHowlSkills;
    }
    const int maxActiveWolves =
        learnedHowlSkills < kWolfGoneCount ? kWolfGoneCount - learnedHowlSkills : 0;
    int activeWolves = 0;

    for (int i = 0; i < kWolfGoneCount; ++i) {
        bool gone = dComIfGs_isEventBit(gone_label(i)) != 0;
        bool howled = dComIfGs_isEventBit(howl_label(i)) != 0;
        if (learnedHowlSkills >= kWolfGoneCount && (!gone || !howled)) {
            s_applying = true;
            if (!howled) dComIfGs_onEventBit(howl_label(i));
            if (!gone) dComIfGs_onEventBit(gone_label(i));
            s_applying = false;
            howled = true;
            gone = true;
            s_sent = 0;
            s_sentGone = 0;
            coop_log::info(
                "coop_mod: [SKILLS] all skills learned, auto-retired leftover wolf/stone {}",
                i + 2);
        } else if (gone && !howled) {
            s_applying = true;
            dComIfGs_onEventBit(howl_label(i));
            s_applying = false;
            howled = true;
            s_sent = 0;
        } else if (howled && !gone) {
            if (activeWolves >= maxActiveWolves) {
                s_applying = true;
                dComIfGs_onEventBit(gone_label(i));
                s_applying = false;
                gone = true;
                s_sent = 0;
                s_sentGone = 0;
                coop_log::info(
                    "coop_mod: [SKILLS] retired surplus active wolf {} (learnedHowlSkills={})",
                    i + 2, learnedHowlSkills);
            } else {
                ++activeWolves;
            }
        }
        const int tbl = kWolfSaveTbl[i];
        const int sw = kWolfSwBit[i];
        if (gone) {
            if (dComIfGs_isSaveSwitch(tbl, sw)) {
                dComIfGs_offSaveSwitch(tbl, sw);
                coop_log::info("coop_mod: [SKILLS] cleared wolf {} map switch (tbl={} sw={:#x})",
                    i + 2, tbl, sw);
            }
            if (curSaveTbl == tbl && dComIfGs_isSwitch(sw, -1)) {
                dComIfGs_offSwitch(sw, -1);
            }
        } else if (howled) {
            if (!dComIfGs_isSaveSwitch(tbl, sw)) {
                dComIfGs_onSaveSwitch(tbl, sw);
                coop_log::info("coop_mod: [SKILLS] set wolf {} map switch (tbl={} sw={:#x})",
                    i + 2, tbl, sw);
            }
            if (curSaveTbl == tbl && !dComIfGs_isSwitch(sw, -1)) {
                dComIfGs_onSwitch(sw, -1);
            }
        }
    }

    if (sweepActors && !dComIfGp_event_runCheck()) {
        fopAcM_Search(sync_skill_actor_cb, nullptr);
    }
}

void apply(uint16_t mask, uint8_t wolfGone) {
    if (!in_game() || !s_haveEarned) return;
    bool changed = false;
    for (int k = 0; k < kTracked; ++k) {
        if (!(mask & (1u << k)) || flag_on(k)) continue;
        s_applying = true;
        dComIfGs_onEventBit(label(k));
        s_applying = false;
        changed = true;
        coop_log::info("coop_mod: [SKILLS] peer finished {} {}, granted",
            k < kSkillCount ? "skill" : "howl", k < kSkillCount ? k + 1 : k - kSkillCount + 2);
    }
    for (int i = 0; i < kWolfGoneCount; ++i) {
        if (!(wolfGone & (1u << i)) || dComIfGs_isEventBit(gone_label(i)) != 0) continue;
        s_applying = true;
        dComIfGs_onEventBit(gone_label(i));
        s_applying = false;
        changed = true;
        coop_log::info("coop_mod: [SKILLS] peer retired golden wolf {}", i + 2);
    }
    if (changed) {
        sync_wolf_world_state(true);
    }
}

}

void skills_init() {
    bool ok = mods::hook::add_pre<SkillsOnEventBitHook>(on_event_bit_pre) == MOD_OK;
    mods::hook::add_pre<SkillsSvOnEventBitHook>(on_sv_event_bit_pre);
    if (svc_save != nullptr) {
        svc_save->observe_saves(mod_ctx, on_new_save, on_save_loaded, nullptr, nullptr, nullptr);
    }
    coop_log::info("coop_mod: [SKILLS] hooks {}", ok ? "attached" : "PARTLY FAILED");
}

void skills_update() {
    ++s_tick;
    if (!in_game()) return;

    if (!s_haveEarned) load_earned();
    if (s_tick % 15 == 0) {
        sync_wolf_world_state(true);
    }
    if (!coop_net_connected() || !joinsync_ready_to_share()) return;
    const uint16_t have = have_mask();
    const uint8_t gone = wolf_gone_mask();
    if (have == 0 && gone == 0) return;
    if (have == s_sent && gone == s_sentGone && s_tick % kResendTicks != 0) return;
    MsgSkills msg{};
    msg.have = have;
    msg.wolfGone = gone;
    coop_net_send(kMsgSkills, &msg, sizeof(msg));
    s_sent = have;
    s_sentGone = gone;
}

void skills_on_message(const uint8_t* payload, size_t size) {
    if (size < sizeof(uint16_t)) return;
    MsgSkills msg{};
    std::memcpy(&msg, payload, size < sizeof(msg) ? size : sizeof(msg));
    apply(msg.have, size >= sizeof(MsgSkills) ? msg.wolfGone : 0);
}

uint8_t skills_tmp_private(int byte) {
    uint8_t mask = 0;
    for (int i = 0; i < kWolfTmpCount; ++i) {
        const uint16_t flag = dSv_event_tmp_flag_c::tempBitLabels[kWolfTmpFirst + i];
        if ((flag >> 8) == byte) mask |= static_cast<uint8_t>(flag & 0xFF);
    }
    return mask;
}

uint8_t skills_event_private(int) {
    return 0;
}
