

#include "mod.hpp"
#include "net/messages.hpp"
#include "print.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_e_s1.h"
#include "d/actor/d_a_e_warpappear.h"
#include "d/actor/d_a_e_ym.h"
#include "d/actor/d_a_kytag04.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/d_kankyo.h"
#include "d/d_com_inf_game.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"

#include <cstring>

namespace {

struct TrackedBug {
    bool used = false;
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    int8_t room = -1;
    uint8_t swBit = 0xFF;
    cXyz pos;
    uint32_t seenTick = 0;
};
const int kMaxBugs = 32;
TrackedBug s_bugs[kMaxBugs];
uint32_t s_tick = 0;
char s_stage[9] = {};

int s_lastCount = -1;
int s_lastArea = -1;
bool s_tboxWas[64] = {};

bool s_applying = false;

bool s_resync = true;

uint32_t s_fullSince = 0;
bool s_finishLogged = false;

bool live() {
    return coop_net_connected() && daAlink_getAlinkActorClass() != nullptr &&
           dComIfGp_getStartStageName() != nullptr;
}

void reset_stage() {
    s_fullSince = 0;
    s_finishLogged = false;
    for (TrackedBug& b : s_bugs) b = TrackedBug{};
    for (bool& t : s_tboxWas) t = false;
    s_lastCount = -1;
    s_lastArea = -1;
    s_resync = true;
}

struct BugScan {
    daE_YM_c* bugs[kMaxBugs];
    int count = 0;
};

void* scan_bugs(void* proc, void* data) {
    auto* out = static_cast<BugScan*>(data);
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (actor == nullptr || out->count >= kMaxBugs) return nullptr;
    if (fopAcM_GetName(actor) != fpcNm_E_YM_e) return nullptr;
    out->bugs[out->count++] = static_cast<daE_YM_c*>(actor);
    return nullptr;
}

struct DropFind {
    int save = -1;
    uint8_t swBit = 0xFF;
    daObjDrop_c* found = nullptr;
};

void* find_drop(void* proc, void* data) {
    auto* f = static_cast<DropFind*>(data);
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (actor == nullptr || f->found != nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Drop_e) {
        return nullptr;
    }
    auto* drop = static_cast<daObjDrop_c*>(actor);
    if ((f->save >= 0 && drop->getSave() == f->save) ||
        (f->swBit != 0xFF && drop->getYmSwbit() == f->swBit)) {
        f->found = drop;
    }
    return nullptr;
}

void send_kill(const TrackedBug& b) {
    MsgTwilightBug msg{};
    std::memcpy(msg.stage, s_stage, 8);
    msg.room = b.room;
    msg.swBit = b.swBit;
    msg.pos[0] = b.pos.x;
    msg.pos[1] = b.pos.y;
    msg.pos[2] = b.pos.z;
    coop_net_send(kMsgTwilightBug, &msg, sizeof(msg));
    coop_log::info("coop_mod: [TWILIGHT] bug {} room={} killed, sent",
        static_cast<int>(b.swBit), static_cast<int>(b.room));
}

void watch_bugs() {
    BugScan scan;
    fopAcM_Search(scan_bugs, &scan);
    for (int i = 0; i < scan.count; ++i) {
        daE_YM_c* bug = scan.bugs[i];
        const fpc_ProcID id = fopAcM_GetID(bug);
        TrackedBug* slot = nullptr;
        TrackedBug* free = nullptr;
        for (TrackedBug& b : s_bugs) {
            if (b.used && b.id == id) slot = &b;
            if (!b.used && free == nullptr) free = &b;
        }
        if (slot == nullptr) {
            if (free == nullptr || bug->getSwitchBit() == 0xFF) continue;
            slot = free;
            *slot = TrackedBug{};
            slot->used = true;
            slot->id = id;
            slot->room = static_cast<int8_t>(fopAcM_GetRoomNo(bug));
            slot->swBit = bug->getSwitchBit();
        }
        slot->pos = bug->current.pos;
        slot->seenTick = s_tick;
    }

    for (TrackedBug& b : s_bugs) {
        if (!b.used || b.seenTick == s_tick) continue;
        const TrackedBug was = b;
        b = TrackedBug{};
        if (was.seenTick + 1 != s_tick) continue;
        if (fopAcM_SearchByID(was.id) != nullptr) continue;
        if (!dComIfGs_isSwitch(was.swBit, was.room)) continue;
        send_kill(was);
    }
}

void watch_tears() {
    const int area = dComIfGp_getStartStageDarkArea();
    if (area < 0 || area > 3) {

        s_lastArea = area;
        s_resync = true;
        return;
    }
    const int count = dComIfGs_getLightDropNum(static_cast<u8>(area));
    bool flipped[64] = {};
    for (int i = 0; i < 64; ++i) {
        const bool now = dComIfGs_isTbox(i) != 0;
        flipped[i] = now && !s_tboxWas[i] && !s_resync;
        s_tboxWas[i] = now;
    }
    const bool rose = !s_resync && area == s_lastArea && s_lastCount >= 0 && count > s_lastCount;
    s_lastArea = area;
    s_lastCount = count;
    s_resync = false;
    if (!rose) return;

    for (int i = 0; i < 64; ++i) {
        if (!flipped[i]) continue;
        MsgTearGot msg{};
        std::memcpy(msg.stage, s_stage, 8);
        msg.saveNo = static_cast<int8_t>(dStage_stagInfo_GetSaveTbl(dComIfGp_getStageStagInfo()));
        msg.area = static_cast<uint8_t>(area);
        msg.save = static_cast<uint8_t>(i);
        msg.count = static_cast<uint8_t>(count);
        coop_net_send(kMsgTearGot, &msg, sizeof(msg));
        coop_log::info("coop_mod: [TWILIGHT] tear {} picked up ({} in area {})", i, count,
            area);
        return;
    }
}

bool same_stage(const char* stage) {
    return std::strncmp(stage, s_stage, 8) == 0;
}

void on_bug_killed(const MsgTwilightBug& msg) {
    if (msg.swBit == 0xFF || !same_stage(msg.stage)) return;
    s_applying = true;
    const cXyz at(msg.pos[0], msg.pos[1], msg.pos[2]);
    BugScan scan;
    fopAcM_Search(scan_bugs, &scan);
    for (int i = 0; i < scan.count; ++i) {
        daE_YM_c* bug = scan.bugs[i];
        if (bug->getSwitchBit() != msg.swBit || fopAcM_GetRoomNo(bug) != msg.room) continue;

        bug->current.pos = at;
        bug->old.pos = at;
        DropFind find;
        find.swBit = msg.swBit;
        fopAcM_Search(find_drop, &find);
        if (find.found != nullptr) {
            find.found->current.pos = at;
            find.found->old.pos = at;
        }
        fopAcM_createDisappear(bug, &at, 10, 1, 0xFF);

        for (TrackedBug& b : s_bugs) {
            if (b.used && b.id == fopAcM_GetID(bug)) b = TrackedBug{};
        }
        fopAcM_delete(bug);
    }
    if (!dComIfGs_isSwitch(msg.swBit, msg.room)) dComIfGs_onSwitch(msg.swBit, msg.room);
    s_applying = false;
    coop_log::info("coop_mod: [TWILIGHT] bug {} room={} killed remotely",
        static_cast<int>(msg.swBit), static_cast<int>(msg.room));
}

void on_tear_got(const MsgTearGot& msg) {
    if (msg.area > 3 || msg.save >= 64) return;
    s_applying = true;
    int count = dComIfGs_getLightDropNum(msg.area);
    if (same_stage(msg.stage)) {
        if (!dComIfGs_isTbox(msg.save)) {
            dComIfGs_onTbox(msg.save);
            ++count;
        }
        s_tboxWas[msg.save] = true;
        DropFind find;
        find.save = msg.save;
        fopAcM_Search(find_drop, &find);
        if (find.found != nullptr) fopAcM_delete(find.found);
    } else if (msg.saveNo >= 0 && msg.saveNo < dSv_save_c::STAGE_MAX &&
               dComIfGs_getSaveInfo() != nullptr) {

        dSv_memBit_c& bits = dComIfGs_getSaveInfo()->getSavedata().getSave(msg.saveNo).getBit();
        if (!bits.isTbox(msg.save)) {
            bits.onTbox(msg.save);
            ++count;
        }
    }

    if (count > 16) count = 16;
    dComIfGs_setLightDropNum(msg.area, static_cast<u8>(count));

    if (msg.area == 2 && count == 15) dComIfGs_onEventBit(0x0180);

    s_tboxWas[msg.save] = true;
    s_lastArea = dComIfGp_getStartStageDarkArea();
    if (s_lastArea >= 0 && s_lastArea <= 3) {
        s_lastCount = dComIfGs_getLightDropNum(static_cast<u8>(s_lastArea));
    }
    s_applying = false;
    coop_log::info("coop_mod: [TWILIGHT] tear {} picked up remotely ({} in area {})",
        static_cast<int>(msg.save), count, static_cast<int>(msg.area));
}

struct TagFind {
    kytag04_class* found = nullptr;
};

void* find_vessel_tag(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_KYTAG04_e) return nullptr;
    auto* tag = static_cast<kytag04_class*>(actor);
    if (tag->field_0x5b4 != 1 || tag->field_0x5b5 == 0xFF) return nullptr;
    static_cast<TagFind*>(data)->found = tag;
    return actor;
}

void finish_twilight() {
    const int area = dComIfGp_getStartStageDarkArea();
    const int need = dComIfGp_getNeedLightDropNum();
    if (area < 0 || area > 3 || need <= 0 || !dKy_darkworld_check() ||
        dComIfGs_getLightDropNum(static_cast<u8>(area)) < need) {
        s_fullSince = 0;
        return;
    }
    if (s_fullSince == 0) s_fullSince = s_tick;
    if (s_tick - s_fullSince < 60 || dComIfGp_event_runCheck()) return;
    TagFind find;
    fopAcM_Search(find_vessel_tag, &find);
    if (find.found == nullptr) {

        if (!s_finishLogged) {
            s_finishLogged = true;
            coop_log::info("coop_mod: [TWILIGHT] vessel full, no exit here, next twilit stage handles it");
        }
        return;
    }
    const int room = dComIfGp_roomControl_getStayNo();
    if (dComIfGs_isSwitch(find.found->field_0x5b5, room)) return;
    dComIfGs_onSwitch(find.found->field_0x5b5, room);
    coop_log::info("coop_mod: [TWILIGHT] vessel area {} filled remotely, warping to spring (switch {})", area, static_cast<int>(find.found->field_0x5b5));
    features_toast("The Vessel of Light is full", "The twilight is lifting.");
}

// Shadow Beast portals. The beasts (E_S1) wait invisible in the sky until the player walks up to the
// portal: then E_WAP plays the "beasts appear" cutscene, freezes the player and drops them one by one,
// once per beast it counted. The last beast to fall turns the portal switch on, and the portal tag
// (kytag04 type 4) sees it and plays WITHWARP_OPEN, which also puts the player in front of the portal.
//
// In co-op the other game only gets the switch and the beasts' state, and that went wrong:
//  - its E_WAP never ran, and it counts a beast again every time the sync rewrites its action, so after a
//    fight it believed there were hundreds of beasts to drop;
//  - WITHWARP_OPEN carried that player to the portal, inside E_WAP's range, and E_WAP's cutscene then
//    held them frozen dropping beasts that no longer exist, 20 frames each: a restart was the only way out;
//  - its copies of the beasts stayed behind, up in the sky or lying in a death pose for good.
// So a beast still up in the sky is left out of the sync, a fight started by someone else retires our idle
// E_WAP, a finished portal clears what is left of it here, and a portal that opens while we are far away
// just appears open.

const int kS1ActFail = 10;        // daE_S1_ACTION::ACT_FAIL
const int kS1ActWarpAppear = 20;  // daE_S1_ACTION::ACT_WARP_APPEAR
const int kS1DeathDemoMode = 20;  // ACT_FAIL step that waits for the beast's own death camera
const f32 kQuietPortalDist = 4000.0f;
// The cutscene lets go of the beasts within 2000 of it, where they land at home: this covers its group.
const f32 kBeastGroupRange = 5000.0f;
const uint32_t kStuckPoseTicks = 60;
const int kMaxWarpAppears = 8;
const int kMaxBeasts = 32;
const int kMaxPortals = 8;

bool going_away(const fopAc_ac_c* actor) {
    return actor->state.init_state == 3;
}

bool beast_waiting_drop(const e_s1_class* s1) {
    return s1->mAction == kS1ActWarpAppear && s1->mMode <= 1;
}

bool story_warpappear(const e_warpappear_class* wap) {
    return wap->arg0 == 0x35 || wap->arg0 == 0xFF;
}

bool warpappear_idle(const e_warpappear_class* wap) {
    return wap->action == 0 && wap->demo_mode == 0;
}

bool field_stage() {
    stage_stag_info_class* info = dComIfGp_getStageStagInfo();
    return info != nullptr && dStage_stagInfo_GetSTType(info) == ST_FIELD;
}

bool near_xz(const cXyz& a, const cXyz& b, f32 range) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return dx * dx + dz * dz <= range * range;
}

struct PortalScan {
    e_warpappear_class* waps[kMaxWarpAppears];
    int wapCount = 0;
    e_s1_class* beasts[kMaxBeasts];
    int beastCount = 0;
    kytag04_class* portals[kMaxPortals];
    int portalCount = 0;
};

void* scan_portal_actors(void* proc, void* data) {
    auto* out = static_cast<PortalScan*>(data);
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (actor == nullptr || going_away(actor) || fpcM_IsCreating(fopAcM_GetID(actor))) return nullptr;
    const s16 name = fopAcM_GetName(actor);
    if (name == fpcNm_E_WAP_e && out->wapCount < kMaxWarpAppears) {
        out->waps[out->wapCount++] = static_cast<e_warpappear_class*>(actor);
    } else if (name == fpcNm_E_S1_e && out->beastCount < kMaxBeasts) {
        out->beasts[out->beastCount++] = static_cast<e_s1_class*>(actor);
    } else if (name == fpcNm_KYTAG04_e && out->portalCount < kMaxPortals) {
        auto* tag = static_cast<kytag04_class*>(actor);
        if (tag->field_0x5b4 == 4 && tag->field_0x5b5 != 0xFF) out->portals[out->portalCount++] = tag;
    }
    return nullptr;
}

void retire_warpappear(e_warpappear_class* wap, const char* why) {
    coop_log::info("coop_mod: [PORTAL] beast portal at {:.0f},{:.0f} retired ({}), it had counted {} beasts",
        wap->current.pos.x, wap->current.pos.z, why, wap->target_info_count);
    wap->target_info_count = 0;
    wap->action = 1;
    fopAcM_delete(wap);
}

void remove_beast(e_s1_class* s1, const char* why) {
    coop_log::info("coop_mod: [PORTAL] removed a leftover Shadow Beast at {:.0f},{:.0f} ({})",
        s1->current.pos.x, s1->current.pos.z, why);
    if (!beast_waiting_drop(s1)) {
        // Dark vanish puff, and 0xFF for no drops: the partner's game already gave them.
        cXyz at = s1->current.pos;
        fopAcM_createDisappear(s1, &at, 12, 1, 0xFF);
    }
    fopAcM_delete(s1);
}

struct StuckPose {
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    uint32_t since = 0;
};
StuckPose s_stuckPoses[kMaxBeasts];

bool stuck_in_death_pose(const e_s1_class* s1) {
    const fpc_ProcID id = fopAcM_GetID(s1);
    StuckPose* slot = nullptr;
    StuckPose* free = nullptr;
    for (StuckPose& p : s_stuckPoses) {
        if (p.id == id) slot = &p;
        if (p.id == fpcM_ERROR_PROCESS_ID_e && free == nullptr) free = &p;
    }
    if (slot == nullptr) {
        if (free == nullptr) return false;
        free->id = id;
        free->since = s_tick;
        return false;
    }
    return s_tick - slot->since >= kStuckPoseTicks;
}

void forget_stuck_pose(fpc_ProcID id) {
    for (StuckPose& p : s_stuckPoses) {
        if (p.id == id) p = StuckPose{};
    }
}

void open_portal_quietly(kytag04_class* tag) {
    tag->field_0x5d1 = 1;
    tag->field_0x5c8 = 999.0f;
    tag->field_0x5cc = 20;
    tag->mBtk.setFrame(tag->mBtk.getEndFrame());
    tag->mBrk.setFrame(tag->mBrk.getEndFrame());
    coop_log::info("coop_mod: [PORTAL] portal at {:.0f},{:.0f} opened elsewhere (switch {}), shown open without its cutscene",
        tag->current.pos.x, tag->current.pos.z, static_cast<int>(tag->field_0x5b5));
    features_toast("Portal open", "Your partner beat the Shadow Beasts here.");
}

void update_portals() {
    PortalScan scan;
    fopAcM_Search(scan_portal_actors, &scan);
    if (scan.wapCount == 0 && scan.beastCount == 0 && scan.portalCount == 0) return;

    // Leftover beasts go first, so a cutscene that just lost them is retired in this same tick.
    const bool field = field_stage();
    for (int i = 0; i < scan.beastCount; ++i) {
        e_s1_class* s1 = scan.beasts[i];
        if (s1->mDemoMode != 0) {
            forget_stuck_pose(fopAcM_GetID(s1));
            continue;
        }
        if (field && s1->mSwBit != 0xFF && dComIfGs_isSwitch(s1->mSwBit, fopAcM_GetRoomNo(s1))) {
            forget_stuck_pose(fopAcM_GetID(s1));
            remove_beast(s1, "its portal is open");
            continue;
        }
        // Only a copied state puts a beast here without its death camera, and nothing moves it on.
        if (s1->mAction == kS1ActFail && s1->mMode == kS1DeathDemoMode) {
            if (stuck_in_death_pose(s1)) {
                forget_stuck_pose(fopAcM_GetID(s1));
                remove_beast(s1, "stuck in its death pose");
            }
        } else {
            forget_stuck_pose(fopAcM_GetID(s1));
        }
    }
    for (StuckPose& p : s_stuckPoses) {
        if (p.id == fpcM_ERROR_PROCESS_ID_e) continue;
        bool alive = false;
        for (int i = 0; i < scan.beastCount && !alive; ++i) alive = fopAcM_GetID(scan.beasts[i]) == p.id;
        if (!alive) p = StuckPose{};
    }

    // The cutscene counts every beast in the stage, so this counts them all too.
    int waiting = 0;
    for (int i = 0; i < scan.beastCount; ++i) {
        if (!going_away(scan.beasts[i]) && beast_waiting_drop(scan.beasts[i])) ++waiting;
    }
    for (int i = 0; i < scan.wapCount; ++i) {
        e_warpappear_class* wap = scan.waps[i];
        if (!story_warpappear(wap) && warpappear_idle(wap)) {
            const u32 doneSw = (fopAcM_GetParam(wap) >> 0x18) & 0xFF;
            if (doneSw != 0xFF && dComIfGs_isSwitch(doneSw, fopAcM_GetRoomNo(wap))) {
                retire_warpappear(wap, "its portal is already open");
            } else if (wap->target_info_count > 0 && waiting == 0) {
                retire_warpappear(wap, "no beast is left up there to drop");
            }
            continue;
        }
        // Dropping beasts: never wait for more than are still up there.
        if ((wap->demo_mode == 13 || wap->demo_mode == 22) && wap->target_info_count > waiting) {
            coop_log::info("coop_mod: [PORTAL] beast drop cut from {} to {} (the rest are not up there)",
                wap->target_info_count, waiting);
            wap->target_info_count = waiting;
        }
    }

    daAlink_c* alink = daAlink_getAlinkActorClass();
    if (alink == nullptr) return;
    const int stay = dComIfGp_roomControl_getStayNo();
    for (int i = 0; i < scan.portalCount; ++i) {
        kytag04_class* tag = scan.portals[i];
        if (tag->field_0x5d1 != 0 || tag->field_0x5d0 != 0) continue;
        if (!dComIfGs_isSwitch(tag->field_0x5b5, stay)) continue;
        const f32 dx = alink->current.pos.x - tag->current.pos.x;
        const f32 dz = alink->current.pos.z - tag->current.pos.z;
        if (dx * dx + dz * dz <= kQuietPortalDist * kQuietPortalDist) continue;
        open_portal_quietly(tag);
    }
}

}

bool twilight_beast_waiting_drop(fopAc_ac_c* actor) {
    return actor != nullptr && fopAcM_GetName(actor) == fpcNm_E_S1_e &&
           beast_waiting_drop(static_cast<e_s1_class*>(actor));
}

bool twilight_peer_started_beasts(fopAc_ac_c* beast) {
    if (beast == nullptr) return false;
    PortalScan scan;
    fopAcM_Search(scan_portal_actors, &scan);
    for (int i = 0; i < scan.wapCount; ++i) {
        const e_warpappear_class* wap = scan.waps[i];
        if (!near_xz(wap->home.pos, beast->home.pos, kBeastGroupRange)) continue;
        // Our own cutscene drops them, or a story one owns them: leave them to it.
        if (story_warpappear(wap) || !warpappear_idle(wap)) return false;
    }
    for (int i = 0; i < scan.wapCount; ++i) {
        if (!near_xz(scan.waps[i]->home.pos, beast->home.pos, kBeastGroupRange)) continue;
        retire_warpappear(scan.waps[i], "a partner is already fighting its beasts");
    }
    return true;
}

void twilight_wake_beast(fopAc_ac_c* actor) {
    if (!twilight_beast_waiting_drop(actor)) return;
    // ACT_WARP_APPEAR step 20 is how the cutscene lets a dropped beast go: next frame it waits on the ground.
    static_cast<e_s1_class*>(actor)->mMode = 20;
}

void twilight_update() {
    ++s_tick;
    if (!live()) {
        reset_stage();
        s_stage[0] = '\0';
        return;
    }
    const char* stage = dComIfGp_getStartStageName();
    if (std::strncmp(stage, s_stage, 8) != 0) {
        reset_stage();
        std::strncpy(s_stage, stage, 8);
        s_stage[8] = '\0';
    }
    if (s_applying) return;
    watch_bugs();
    watch_tears();
    finish_twilight();
    update_portals();
}

void twilight_on_message(uint8_t type, const uint8_t* payload, size_t size) {
    if (!live()) return;
    if (type == kMsgTwilightBug && size >= sizeof(MsgTwilightBug)) {
        MsgTwilightBug msg;
        std::memcpy(&msg, payload, sizeof(msg));
        on_bug_killed(msg);
    } else if (type == kMsgTearGot && size >= sizeof(MsgTearGot)) {
        MsgTearGot msg;
        std::memcpy(&msg, payload, sizeof(msg));
        on_tear_got(msg);
    }
}
