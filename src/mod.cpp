// std headers before any game header: aurora defines nullptr as NULL when msvc reports old __cplusplus
#include <cstring>
#include <string>
#include <vector>

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/flow.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/item.h"
#include "mods/svc/log.h"
#include "mods/svc/log.hpp"
#include "mods/svc/resource.h"
#include "mods/svc/save.h"
#include "mods/svc/texture.h"
#include "mods/svc/ui.h"

#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/JMessage/control.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "JSystem/JUtility/JUTFont.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_bg.h"
#include "d/actor/d_a_bg_obj.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_kytag08.h"
#include "d/actor/d_a_obj_iceleaf.h"
#include "d/actor/d_a_tag_shop_item.h"
#include "d/d_a_shop_item_static.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_kankyo.h"
#include "d/d_map_path_dmap.h"
#include "d/d_menu_fmap_map.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_class.h"
#include "d/d_msg_flow.h"
#include "d/d_msg_object.h"
#include "d/d_msg_string_base.h"
#include "d/d_save.h"
#include "d/d_shop_system.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_msg_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(TextureService, svc_texture);
IMPORT_SERVICE(SaveService, svc_save);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(FlowService, svc_flow);
IMPORT_SERVICE(MessageService, svc_message);
IMPORT_SERVICE(ItemService, svc_item);

// ======================== settings ========================

// Less Restrictive Maps
static const char* eponaStages[] = {"F_SP116", "F_SP127", "F_SP124"};
static const char* eponaKeys[] = {"castleTown", "fishingHole", "gerudoDesert"};
static const char* eponaLabels[] = {"Epona in Castle Town", "Epona at the Fishing Hole", "Epona in Gerudo Desert"};
static ConfigVarHandle eponaToggle[3];

enum MistMode : int { MIST_OFF, MIST_SEMI, MIST_ON };
static const char* const mistNames[] = {"Off", "Semi-Default", "On"};
static ConfigVarHandle mistOption;

enum ShopMode : int { SHOP_OFF, SHOP_SEMI, SHOP_PARTIAL, SHOP_FULL };
static const char* const shopNames[] = {"Off", "Semi-Default", "Partial-Default", "On"};
static ConfigVarHandle shopOption;

// Shop Expansion. 0 = register_var failed, boost stays off
static ConfigVarHandle vBoost, vAlways, vGoron, vChud, vLeaf;

// ======================== shared ========================

static constexpr u16 kMaloOpen = 0x2210;  // F_0281, Malo Mart open in Castle Town
static constexpr u16 kDonation = 0x0F10;  // M_091, Malo Mart donation paid

// set while our own code wants the real flags past the Chudley's hiding
static bool peekRealFlags;

static const char* stage_now() {
    return dComIfGp_getStartStageName();
}

static bool stage_is(const char* s) {
    const char* cur = stage_now();
    return cur && !strcmp(cur, s);
}

static bool cfgOn(ConfigVarHandle v) {
    bool on = true;
    if (v) svc_config->get_bool(mod_ctx, v, &on);
    return on;
}

static int cfgInt(ConfigVarHandle v, int def) {
    int64_t val = def;
    if (v) svc_config->get_int(mod_ctx, v, &val);
    return (int)val;
}

static bool real_flag(u16 bit) {
    peekRealFlags = true;
    bool on = dComIfGs_isEventBit(bit);
    peekRealFlags = false;
    return on;
}

template <class H, class F>
static bool pre(F f) {
    if (mods::hook::add_pre<H>(f) == MOD_OK) return true;
    mods::log::error("pre hook failed");
    return false;
}

template <class H, class F>
static bool post(F f) {
    if (mods::hook::add_post<H>(f) == MOD_OK) return true;
    mods::log::error("post hook failed");
    return false;
}

// ======================== Less Restrictive Maps ========================

static fpc_ProcID lastLink = fpcM_ERROR_PROCESS_ID_e;
static fpc_ProcID myEpona = fpcM_ERROR_PROCESS_ID_e;
static cXyz eponaSpot;
static s16 eponaFacing;
static bool rodeIn;
static char oldStage[9];

static int faronSaveTable = 2;
static int drawingRoom = -1;

static int horse_area(const char* stage) {
    if (stage == nullptr) return -1;
    for (int i = 0; i < 3; i++)
        if (strcmp(stage, eponaStages[i]) == 0) return i;
    return -1;
}

static bool horse_allowed(int area) {
    return area >= 0 && cfgOn(eponaToggle[area]);
}

// --- Epona ---

DEFINE_HOOK(&daHorse_c::savePos, HorseSavePos);
DEFINE_HOOK(&daAlink_c::checkCastleTownUseItem, TownUseItem);
DEFINE_HOOK(&dComIfG_play_c::setStartStage, SetStartStage);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_horse.cpp#daHorse_searchEnemy", void*(fopAc_ac_c*, void*), HorseSearchEnemy);

static HookAction keep_field_spot(ModContext*, void* args, void* ret, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    if (myEpona == fpcM_ERROR_PROCESS_ID_e || fopAcM_GetID(horse) != myEpona) return HOOK_CONTINUE;
    return HOOK_SKIP_ORIGINAL;
}

// fish and birds near our Epona don't count as enemies
static HookAction calm_epona(ModContext*, void* args, void* ret, void*) {
    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || myEpona == fpcM_ERROR_PROCESS_ID_e || fopAcM_GetID(horse) != myEpona)
        return HOOK_CONTINUE;

    switch (fopAcM_GetName(mods::arg<fopAc_ac_c*>(args, 0))) {
    case fpcNm_NPC_LF_e:
    case fpcNm_NPC_TR_e:
    case fpcNm_BD_e:
        *(void**)ret = nullptr;
        return HOOK_SKIP_ORIGINAL;
    default:
        return HOOK_CONTINUE;
    }
}

static HookAction remember_stage(ModContext*, void* args, void* ret, void*) {
    const char* from = stage_now();
    strncpy(oldStage, from ? from : "", sizeof(oldStage) - 1);
    return HOOK_CONTINUE;
}

// same area = stay mounted, anything else hangs so drop Link off
static void check_horse_arrival(ModContext*, void* args, void* ret, void*) {
    rodeIn = false;
    int dest = horse_area(stage_now());
    int src = horse_area(oldStage);
    if (dest < 0 && src < 0) return;

    dSv_restart_c& restart = g_dComIfG_gameInfo.info.getRestart();
    u32 lastMode = restart.getLastMode();
    u32 kind = lastMode & 0xF;
    if (kind != 1 && kind != 8) return;

    if (dest == src && horse_allowed(dest))
        rodeIn = true;
    else
        restart.setLastSceneInfo(restart.getLastSpeedF(), lastMode & ~0xFu, restart.getLastAngleY());
}

static void allow_horse_call(ModContext*, void* args, void* ret, void*) {
    u16 item = mods::arg<u16>(args, 0);  // static func, no this
    if (item == dItemNo_HORSE_FLUTE_e && horse_allowed(horse_area(stage_now())))
        *(bool*)ret = true;
}

static bool pick_epona_spot(daAlink_c* link, cXyz* out) {
    static const s16 turns[] = {0, 0x2000, -0x2000, 0x4000, -0x4000, 0x6000, -0x6000, -0x8000};
    const cXyz& feet = link->current.pos;
    cXyz eye(feet.x, feet.y + 80.0f, feet.z);

    for (int reach = 130; reach <= 230; reach += 100) {
        for (s16 turn : turns) {
            s16 dir = link->shape_angle.y + turn;
            cXyz spot(feet.x + cM_ssin(dir) * reach, eye.y, feet.z + cM_scos(dir) * reach);
            if (fopAcM_lc_c::lineCheck(&eye, &spot, link)) continue;

            spot.y = feet.y + 150.0f;
            if (!fopAcM_gc_c::gndCheck(&spot) || fopAcM_gc_c::getHorseNoEntry()) continue;
            f32 ground = fopAcM_gc_c::getGroundY();
            f32 step = ground - feet.y;
            if (step > 120.0f || step < -120.0f) continue;

            spot.y = ground;
            bool wet = fopAcM_wt_c::waterCheck(&spot) && fopAcM_wt_c::getWaterY() > ground + 5.0f;
            if (wet) continue;

            *out = spot;
            return true;
        }
    }
    return false;
}

static void bring_epona(daAlink_c* link, bool riding) {
    if (!pick_epona_spot(link, &eponaSpot)) {
        if (!riding) return;
        eponaSpot = link->current.pos;
    }
    eponaFacing = cLib_targetAngleY(&eponaSpot, &link->current.pos) + 0x4000;

    csXyz rot(0, eponaFacing, 0);
    myEpona = fopAcM_create(fpcNm_HORSE_e, 0xFF, &eponaSpot, -1, &rot, nullptr, -1);
    if (myEpona == fpcM_ERROR_PROCESS_ID_e) mods::log::warn("couldn't spawn Epona in {}", stage_now());
}

static bool whistling(daAlink_c* link) {
    if (link->mProcID != daAlink_c::PROC_GRASS_WHISTLE_WAIT || link->mProcVar2.field_0x300c != 3) return false;
    int left = link->mProcVar0.field_0x3008;  // counts 140 down to 0
    return left > 0 && left <= 60;
}

static void epona_tick() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        lastLink = fpcM_ERROR_PROCESS_ID_e;
        return;
    }
    if (fopAcM_GetID(link) != lastLink) {  // new scene
        lastLink = fopAcM_GetID(link);
        myEpona = fpcM_ERROR_PROCESS_ID_e;
    }

    int area = horse_area(stage_now());
    bool allowed = horse_allowed(area);

    // toggled off while she's here
    if (area >= 0 && !allowed && myEpona != fpcM_ERROR_PROCESS_ID_e) {
        fopAc_ac_c* mine = fopAcM_SearchByID(myEpona);
        if (mine && !link->checkHorseRide()) fopAcM_delete(mine);
        myEpona = fpcM_ERROR_PROCESS_ID_e;
        return;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse != nullptr) {
        if (fopAcM_GetID(horse) == myEpona && horse->checkHorseCallWait()) {
            horse->setHorsePosAndAngle(&eponaSpot, eponaFacing);
            horse->initHorseMtx();
            horse->offNoDrawWait();
        }
        return;
    }

    if (myEpona != fpcM_ERROR_PROCESS_ID_e) {
        if (!fpcM_IsCreating(myEpona) && !fopAcM_IsExecuting(myEpona)) myEpona = fpcM_ERROR_PROCESS_ID_e;
        return;
    }

    if (rodeIn) {
        rodeIn = false;
        if (allowed && link->checkHorseStart()) bring_epona(link, true);
    } else if (allowed && whistling(link) && dComIfGs_isEventBit(0x0601) && !dKy_darkworld_check()) {
        bring_epona(link, false);
    }
}

// --- Faron mist ---

DEFINE_HOOK_SYMBOL("src/d/actor/d_a_bg.cpp#daBg_Draw", int(daBg_c*), BgDraw);
DEFINE_HOOK(dKy_bg_MAxx_proc, BgMaterialProc);
DEFINE_HOOK(&renderingFmap_c::isSwitch, FieldMapGroupSwitch);
DEFINE_HOOK(&renderingDAmap_c::isSwitch, MiniMapGroupSwitch);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_kytag08.cpp#daKytag08_Execute", int(kytag08_class*), FogTagExecute);

static bool mist_room(int room) {
    return room == 5 || room == 11;
}

static bool mist_gone() {
    switch (cfgInt(mistOption, MIST_OFF)) {
    case MIST_ON:
        return !dComIfGs_isEventBit(0x4510) || dComIfGs_isDarkClearLV(0);  // not mid-twilight
    case MIST_SEMI:
        return dComIfGs_isEventBit(0x0602) && dComIfGs_isEventBit(0x1B08);  // temple + lantern monkey
    default:
        return false;
    }
}

static bool in_clear_faron() {
    return stage_is("F_SP108") && !dKy_darkworld_check() && mist_gone();
}

static HookAction note_bg_room(ModContext*, void* args, void* ret, void*) {
    drawingRoom = (int)fopAcM_GetParam(mods::arg<daBg_c*>(args, 0));
    return HOOK_CONTINUE;
}

static void clear_bg_room(ModContext*, void* args, void* ret, void*) {
    drawingRoom = -1;
}

// MA11 = mist sheet
static void hide_mist_sheet(ModContext*, void* args, void* ret, void*) {
    auto* model = static_cast<J3DModel*>(mods::arg<void*>(args, 0));
    if (model == nullptr || !mist_room(drawingRoom) || !in_clear_faron()) return;

    J3DModelData* data = model->getModelData();
    u16 count = data->getMaterialNum();
    for (u16 m = 0; m < count; m++) {
        const char* mat = data->getMaterialName()->getName(m);
        if (mat == nullptr || strlen(mat) < 7 || memcmp(mat + 3, "MA11", 4) != 0) continue;
        J3DMaterial* material = data->getMaterialNodePointer(m);
        J3DGXColorS10 tint = *material->getTevColor(1);
        tint.a = 0;
        material->setTevColor(1, &tint);
    }
}

static void unpurple_group(void* args, void* ret, int room, bool active) {
    const dDrawPath_c::group_class* group = mods::arg<const dDrawPath_c::group_class*>(args, 1);
    if (!active || group == nullptr || group->mSwbit == 0xFF || !mist_room(room)) return;
    *(bool*)ret = group->field_0x1 == 0;
}

static void pause_map_switch(ModContext*, void* args, void* ret, void*) {
    auto& fmap = *mods::arg<renderingFmap_c*>(args, 0);
    bool faron = fmap.mSaveTableNo == faronSaveTable;
    unpurple_group(args, ret, fmap.mRoomNo, faron && mist_gone());
}

static void minimap_switch(ModContext*, void* args, void* ret, void*) {
    int room = mods::arg<renderingDAmap_c*>(args, 0)->mRoomNo;
    unpurple_group(args, ret, room, in_clear_faron());
}

static HookAction kill_mist_tag(ModContext*, void* args, void* ret, void*) {
    kytag08_class* tag = mods::arg<kytag08_class*>(args, 0);
    bool clear = in_clear_faron();

    if (tag->mpEmitter1) clear ? tag->mpEmitter1->stopDrawParticle() : tag->mpEmitter1->playDrawParticle();
    if (tag->mpEmitter2) clear ? tag->mpEmitter2->stopDrawParticle() : tag->mpEmitter2->playDrawParticle();

    if (!clear) return HOOK_CONTINUE;
    if (ret) *(int*)ret = 1;
    return HOOK_SKIP_ORIGINAL;
}

// lighting for rooms 5/11

typedef struct {
    const stage_envr_info_class* envr;
    const stage_pselect_info_class* psel;
    const stage_palette_info_class* pal;
    const stage_vrboxcol_info_class* sky;
    int envrNum, pselNum, palNum, skyNum;
} env_tables;

static stage_envr_info_class envRows[256];
static stage_pselect_info_class pickRows[250];
static stage_palette_info_class palRows[255];
static stage_vrboxcol_info_class skyRows[255];
static int pickCount;
static int pickKeys[250];

static env_tables liveEnv;
static bool lightingOn;
static fpc_ProcID lightingLink = fpcM_ERROR_PROCESS_ID_e;

static bool read_env(int layer, env_tables& out) {
    out = env_tables{};
    auto* dzs = static_cast<dStage_fileHeader*>(dComIfG_getStageRes("stage.dzs"));
    if (dzs == nullptr) return false;

    char suffix = layer < 10 ? '0' + layer : 'W' + layer;
    for (int i = 0; i < dzs->m_chunkCount; i++) {
        dStage_nodeHeader* chunk = &dzs->m_nodes[i];
        char tag[4];
        memcpy(tag, &chunk->m_tag, 4);
        if (layer >= 0 && tag[3] != suffix) continue;

        void* data = (void*)chunk->m_offset;
        int n = chunk->m_entryNum;
        bool take = layer >= 0;
        if (!memcmp(tag, "Env", 3) && (take || data == g_env_light.stage_envr_info)) {
            out.envr = (const stage_envr_info_class*)data;
            out.envrNum = n;
        } else if (!memcmp(tag, "Col", 3) && (take || data == g_env_light.stage_pselect_info)) {
            out.psel = (const stage_pselect_info_class*)data;
            out.pselNum = n;
        } else if (!memcmp(tag, "PAL", 3) && (take || data == g_env_light.stage_palette_info)) {
            out.pal = (const stage_palette_info_class*)data;
            out.palNum = n;
        } else if (!memcmp(tag, "VRB", 3) && (take || data == g_env_light.stage_vrboxcol_info)) {
            out.sky = (const stage_vrboxcol_info_class*)data;
            out.skyNum = n;
        }
    }
    return out.envr && out.psel && out.pal && out.envrNum > 0 && out.pselNum > 0 && out.palNum > 0;
}

// old Faron is daytime-only, so borrow Coro's room for the other times of day
static int add_pick(const env_tables& live, const env_tables& old, int src, int coro) {
    int key = (src << 8) | (coro + 1);
    for (int i = live.pselNum; i < pickCount; i++)
        if (pickKeys[i - live.pselNum] == key) return i;
    if (pickCount >= 250) return -1;

    auto pick = old.psel[src];
    bool sameAllDay = true;
    for (int s = 1; s < 6; s++)
        if (pick.palette_id[s] != pick.palette_id[0]) sameAllDay = false;

    for (int s = 0; s < 8; s++) {
        if (sameAllDay && coro >= 0 && s != 2 && s < 6)
            pick.palette_id[s] = live.psel[coro].palette_id[s];
        else if (pick.palette_id[s] < old.palNum)
            pick.palette_id[s] += live.palNum;
    }
    pickKeys[pickCount - live.pselNum] = key;
    pickRows[pickCount] = pick;
    return pickCount++;
}

static bool merge_old_faron(const env_tables& live, const env_tables& old) {
    bool oldSky = old.sky && old.skyNum > 0;
    int skyTotal = live.skyNum + (oldSky ? old.skyNum : 0);
    if (live.envrNum > 256 || live.pselNum > 250) return false;
    if (live.palNum + old.palNum > 254 || skyTotal > 255) return false;

    if (live.sky) memcpy(skyRows, live.sky, sizeof(skyRows[0]) * live.skyNum);
    if (oldSky) memcpy(skyRows + live.skyNum, old.sky, sizeof(skyRows[0]) * old.skyNum);

    memcpy(palRows, live.pal, sizeof(palRows[0]) * live.palNum);
    for (int i = 0; i < old.palNum; i++) {
        stage_palette_info_class& p = palRows[live.palNum + i];
        p = old.pal[i];
        if (oldSky && p.vrboxcol_id < old.skyNum)
            p.vrboxcol_id += live.skyNum;
        else if (p.vrboxcol_id >= live.skyNum)
            p.vrboxcol_id = 0;
    }

    memcpy(pickRows, live.psel, sizeof(pickRows[0]) * live.pselNum);
    memcpy(envRows, live.envr, sizeof(envRows[0]) * live.envrNum);
    pickCount = live.pselNum;
    const stage_envr_info_class* coroRoom = live.envrNum > 4 ? &live.envr[4] : nullptr;

    for (int room : {5, 11}) {
        if (room >= live.envrNum || room >= old.envrNum) continue;

        auto row = old.envr[room];
        for (int t = 0; t < 65; t++) {
            int src = row.pselect_id[t];
            if (src >= old.pselNum) continue;  // out of range = selection 0, leave it
            int coro = -1;
            if (coroRoom && coroRoom->pselect_id[t] < live.pselNum) coro = coroRoom->pselect_id[t];
            int idx = add_pick(live, old, src, coro);
            if (idx < 0) return false;
            row.pselect_id[t] = (u8)idx;
        }
        envRows[room] = row;
    }
    return true;
}

static void use_env(const env_tables& e) {
    g_env_light.stage_envr_info = (stage_envr_info_class*)e.envr;
    g_env_light.stage_pselect_info = (stage_pselect_info_class*)e.psel;
    g_env_light.stage_palette_info = (stage_palette_info_class*)e.pal;
    if (e.sky) g_env_light.stage_vrboxcol_info = (stage_vrboxcol_info_class*)e.sky;
}

static void put_lighting_back() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (lightingOn && link && fopAcM_GetID(link) == lightingLink) use_env(liveEnv);
    lightingOn = false;
}

static void faron_lighting_tick() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    bool inFaron = link && stage_is("F_SP108");

    stage_stag_info_class* stag = dComIfGp_getStage()->getStagInfo();
    if (inFaron && stag) faronSaveTable = dStage_stagInfo_GetSaveTbl(stag);

    // tables from the last scene are gone already
    if (lightingOn && (!link || fopAcM_GetID(link) != lightingLink)) lightingOn = false;

    if (!inFaron || !in_clear_faron()) {
        put_lighting_back();
        return;
    }
    if (lightingOn) return;

    dStage_Elst_c* elst = dComIfGp_getStage()->getElst();
    if (!elst || elst->m_entryNum <= 5) return;
    int oldLayer = elst->m_entries[5].m_layerTable[1];
    if (oldLayer >= 15) return;

    env_tables live, old;
    if (!read_env(-1, live) || !read_env(oldLayer, old) || !merge_old_faron(live, old)) return;

    liveEnv = live;
    env_tables merged = {envRows, pickRows, palRows, (live.sky || old.sky) ? skyRows : nullptr, 0, 0, 0, 0};
    use_env(merged);
    lightingOn = true;
    lightingLink = fopAcM_GetID(link);
}

// ======================== Ice Leaf ========================

DEFINE_HOOK(&daAlink_c::execute, Exec);
DEFINE_HOOK(&daAlink_c::checkNewItemChange, NewItem);
DEFINE_HOOK(&daAlink_c::checkSnowCode, SnowCode);
DEFINE_HOOK(&daAlink_c::setStickData, Stick);
DEFINE_HOOK(&dSv_player_item_c::setLineUpItem, LineUp);
DEFINE_HOOK(&dMeter2Info_c::readItemTexture, ReadTex);
DEFINE_HOOK(&dMsgObject_c::setMessageIndexDemo, MsgBegin);
// these two are by name (the second is private); sigs are from the decomp, unchecked against this port
DEFINE_HOOK_SYMBOL("dMsgStringBase_c::getStringLocal",
    f32(dMsgStringBase_c*, u32, J2DTextBox*, J2DTextBox*, JUTFont*, COutFont_c*, u8), MsgStr);
DEFINE_HOOK_SYMBOL("JMessage::TControl::setMessageCode_inSequence_",
    bool(JMessage::TControl*, const JMessage::TProcessor*, u16, u16), MsgSeq);

// 0x13 has no vanilla item, slot 7 is skipped by the stock wheel
static constexpr u8 leafItem = dItemNo_NOENTRY_19_e;
static constexpr int leafSlot = SLOT_7;
static constexpr u32 nameMsg = 0x165 + leafItem;
// the item screen reads its description from 0x265 + item no
static constexpr u32 descMsg = 0x265 + leafItem;
static constexpr int iconSz = 96;
// snowpeak ruins cleared + its mirror shard (collect bit 1)
static constexpr u16 spDone = 0x2008;
static constexpr u8 spShard = 1;
// 0x65 + item no is the item-get message id
static constexpr u32 pickMsg = 0x65 + leafItem;
static const char* getText =
    "An iced over leaf from Snowpeak.\n"
    "Rumored to be a part of what was\n"
    "once known as a \"Korok Leaf\"\n"
    "long ago.\n\n"
    "Equip it to sled across Hyrule\n"
    "on downhill slopes.";

// control tags are 1A, total size, group, code (u16 be); the zeros inside are why this is an array and
// not run through strlen. group 6 is the bullet, group 0 has the pad buttons (B 11, X 15, Y 16)
static const char descText[] =
    "A frozen leaf from Snowpeak that\n"
    "lets the rider glide across\n"
    "sloped lands.\n"
    "\x1A\x05\x06\x00\x0A" "Ride it with "
    "\x1A\x05\x00\x00\x10" " or "
    "\x1A\x05\x00\x00\x0F" " to ride\n"
    "\x1A\x05\x06\x00\x0B" "the leaf anywhere.\n"
    "\x1A\x05\x06\x00\x0A" "Press "
    "\x1A\x05\x00\x00\x0B" " to attack with your sword\n"
    "\x1A\x05\x06\x00\x0B" "and hold to deliver a spin attack.";

struct Icon { void* buf; TextureReplacementHandle h; };

static fpc_ProcID leafId = fpcM_ERROR_PROCESS_ID_e;
static fpc_ProcID rideId = fpcM_ERROR_PROCESS_ID_e;
static bool mounting, diveSpawn;
static int mountTimer;
static ResourceBuffer iconRes = RESOURCE_BUFFER_INIT;
static void* iconBuf;
static Icon icons[32];
static int nIcons;

static bool got, fell, customMsg, customDesc, wasAlways;
static fpc_ProcID msgPid = fpcM_ERROR_PROCESS_ID_e;
static int gotPoll;
// spinner stand-in: set while asking the game whether the spinner could come out here
static bool simSpin, wantLeaf;

static bool out() { return leafId != fpcM_ERROR_PROCESS_ID_e; }

static daObjIceLeaf_c* findLeaf() {
    return out() ? static_cast<daObjIceLeaf_c*>(fopAcM_SearchByID(leafId)) : nullptr;
}

// 120 is a guess at head height, shallows don't count as wet
static bool wet(daAlink_c* lnk) {
    return lnk->checkModeFlg(daAlink_c::MODE_SWIMMING) || lnk->mWaterY - lnk->current.pos.y > 120.0f;
}

static void stow(daAlink_c* lnk) {
    // 0x103 = sword, still allowed
    if (lnk->mEquipItem != dItemNo_NONE_e && lnk->mEquipItem != 0x103) lnk->allUnequip(TRUE);
}

static void spawn(daAlink_c* lnk) {
    stow(lnk);
    cXyz pos = lnk->current.pos;
    csXyz ang(0, lnk->shape_angle.y, 0);
    // ride status 1 skips the drop anim
    leafId = fopAcM_create(fpcNm_Obj_IceLeaf_e, 0x0001FFFF, &pos, fopAcM_GetRoomNo(lnk), &ang, nullptr, -1);
    if (!out()) {
        mods::log::warn("couldn't create the ice leaf");
        return;
    }
    mounting = true;
    mountTimer = 0;
    diveSpawn = wet(lnk);
}

static void drop(daAlink_c* lnk) {
    mounting = diveSpawn = false;
    daObjIceLeaf_c* leaf = findLeaf();
    if (leaf) {
        if (lnk && lnk->checkBoardRide()) {
            lnk->setJumpMode();
            lnk->procBackJumpInit(0);
        }
        // same break fx the leaf plays on its own delete, done here so it never gets skipped
        dComIfGp_particle_set(0x8C12, &leaf->eyePos, NULL, NULL);
        dComIfGp_particle_set(0x8C13, &leaf->eyePos, NULL, NULL);
        if (lnk) lnk->seStartOnlyReverb(Z2SE_AL_SNOBO_ICEBREAK);
        fopAcM_delete(leaf);
        if (lnk) lnk->mRideAcKeep.clearData();
    } else if (out())
        fopAcM_delete(leafId);
    leafId = fpcM_ERROR_PROCESS_ID_e;
}

static bool rdBlob(const char* key) {
    uint8_t b = 0;
    size_t n = 1;
    return svc_save->get_blob(mod_ctx, key, &b, &n) == MOD_OK && n == 1 && b;
}

static void wrBlob(const char* key, uint8_t v) { svc_save->set_blob(mod_ctx, key, &v, 1); }

static void setLeafSlot(u8 item) {
    dComIfGs_setItem(leafSlot, item);
    dComIfGp_setItem(leafSlot, item);
    dComIfGs_setLineUpItem();
}

static bool owned() {
    for (int i = 0; i < 24; i++) if (dComIfGs_getItem(i, true) == leafItem) return true;
    return false;
}

static void forgetIcon(void* buf) {
    for (int i = 0; i < nIcons; i++) {
        if (icons[i].buf != buf) continue;
        svc_texture->unregister(mod_ctx, icons[i].h);
        icons[i] = icons[--nIcons];
        return;
    }
}

static HookAction exec_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);

    // leaf switched off: put it away and take it off the wheel, "hidden" (per save) brings it back later
    if (!cfgOn(vLeaf)) {
        if (out()) drop(lnk);
        if (dComIfGs_getItem(leafSlot, true) == leafItem && !lnk->checkEventRun()) {
            setLeafSlot(dItemNo_NONE_e);
            wrBlob("hidden", 1);
        }
        fell = wantLeaf = false;
        rideId = fpcM_ERROR_PROCESS_ID_e;
        return HOOK_CONTINUE;
    }

    if (gotPoll++ % 30 == 0) {
        got = rdBlob("got");
        if (rdBlob("hidden")) {
            wrBlob("hidden", 0);
            if (!owned() && dComIfGs_getItem(leafSlot, true) == dItemNo_NONE_e) setLeafSlot(leafItem);
        }
    }

    bool always = false;
    if (vAlways) svc_config->get_bool(mod_ctx, vAlways, &always);

    // snowpeak slide ends -> the game's own leaf is deleted while we were on it
    fopAc_ac_c* ra = lnk->mRideAcKeep.getActor();
    if (lnk->checkBoardRide() && ra && fopAcM_GetName(ra) == fpcNm_Obj_IceLeaf_e && fopAcM_GetID(ra) != leafId)
        rideId = fopAcM_GetID(ra);
    else if (rideId != fpcM_ERROR_PROCESS_ID_e && !fopAcM_SearchByID(rideId)) {
        rideId = fpcM_ERROR_PROCESS_ID_e;
        fell = true;
    }

    // the snowpeak leaf broke: remember it for this save, prompt every time unless the toggle's on
    if (fell && !lnk->checkEventRun()) {
        fell = false;
        wrBlob("got", 1);
        got = true;
        bool fresh = !owned();
        if (fresh && dComIfGs_getItem(leafSlot, true) == dItemNo_NONE_e) setLeafSlot(leafItem);
        if (fresh && !always && msgPid == fpcM_ERROR_PROCESS_ID_e) {
            msgPid = fopMsgM_messageSetDemo(pickMsg);
            mDoAud_subBgmStart(Z2BGM_ITEM_GET);
            dComIfGp_setMesgBgmOn();
        }
    }

    // save already past snowpeak (mod added late): hand it over quietly
    if (dComIfGs_isEventBit(spDone) && dComIfGs_isCollectMirror(spShard)) {
        if (!got) {
            wrBlob("got", 1);
            got = true;
        }
        if (!owned() && dComIfGs_getItem(leafSlot, true) == dItemNo_NONE_e) setLeafSlot(leafItem);
    }

    // ownership lives in the save's own inventory, the blob only says whether the tree leaf was ever earned.
    // grants happen on the break above or from the toggle, never from the blob alone: it can still be
    // the last save's value for a moment after switching files
    if (always && dComIfGs_getItem(leafSlot, true) == dItemNo_NONE_e) setLeafSlot(leafItem);
    else if (wasAlways && !always && !got && gotPoll > 90 && dComIfGs_getItem(leafSlot, true) == leafItem) {
        if (out()) drop(lnk);
        setLeafSlot(dItemNo_NONE_e);
    }
    wasAlways = always;
    if (msgPid != fpcM_ERROR_PROCESS_ID_e && !fopMsgM_SearchByID(msgPid)) msgPid = fpcM_ERROR_PROCESS_ID_e;

    // leaf can vanish on its own (room change etc)
    if (out() && !mounting && !findLeaf()) leafId = fpcM_ERROR_PROCESS_ID_e;

    bool heavy = lnk->checkHeavyStateOn(TRUE, TRUE);
    if (out() && dComIfGp_getSelectItem(0) != leafItem && dComIfGp_getSelectItem(1) != leafItem) drop(lnk);

    // a dunk only keeps it if it was summoned underwater in iron boots
    if (out() && !mounting && wet(lnk) && (!diveSpawn || !heavy)) drop(lnk);

    // summoning is decided in newitem_pre (same rules as the spinner), this only acts on it
    bool want = wantLeaf;
    wantLeaf = false;
    for (int btn = 0; btn < 2; btn++) {
        if (dComIfGp_getSelectItem(btn) != leafItem) continue;
        if (out()) {
            if (!lnk->itemTriggerCheck(1 << btn)) continue;
            drop(lnk);
        } else if (want && !lnk->checkEventRun() && !lnk->checkRide()) {
            spawn(lnk);
        }
        break;
    }

    if (out()) stow(lnk);

    if (mounting) {
        daObjIceLeaf_c* leaf = findLeaf();
        if (!out() || ++mountTimer > 300) drop(lnk);
        else if (leaf && fopAcM_IsExecuting(leafId)) {
            mounting = false;
            lnk->procBoardWaitInit(leaf);
        }
    }

    return HOOK_CONTINUE;
}

static HookAction newitem_pre(ModContext*, void* args, void* retval, void*) {
    if (simSpin) return HOOK_CONTINUE;
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    u8 idx = mods::arg<u8>(args, 1);
    bool leaf = dComIfGp_getSelectItem(idx) == leafItem;
    if (!out() && !leaf) return HOOK_CONTINUE;
    if (!out() && idx < 2) {
        // ask the full hook chain (no-restriction mods included) what the spinner would do on this button
        simSpin = true;
        g_dComIfG_gameInfo.play.setSelectItem(idx, dItemNo_SPINNER_e);
        int r = lnk->checkNewItemChange(idx);
        g_dComIfG_gameInfo.play.setSelectItem(idx, leafItem);
        simSpin = false;
        // 4 = spinner ready; the trigger check also marks the button usable so the icon isn't dimmed
        if (r == 4 && lnk->itemTriggerCheck(1 << idx)) wantLeaf = true;
    }
    if (retval) *(int*)retval = 0;
    return HOOK_SKIP_ORIGINAL;
}

// the pickup prompt is a plain demo message, nothing stops link once its second box is up
static void stick_post(ModContext*, void* args, void*, void*) {
    if (msgPid == fpcM_ERROR_PROCESS_ID_e || !fopMsgM_SearchByID(msgPid)) return;
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    lnk->mStickValue = lnk->mMoveValue = 0.0f;
    lnk->mItemTrigger = lnk->mItemButton = 0;
}

static void snow_post(ModContext*, void* args, void* retval, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    if (out() && retval && lnk->checkBoardRide()) *(bool*)retval = true;
}

static void lineup_post(ModContext*, void* args, void*, void*) {
    auto* items = mods::arg<dSv_player_item_c*>(args, 0);
    if (items->getItem(leafSlot, true) != leafItem) return;

    // private, sits right after the 24 item bytes
    u8* order = (u8*)items + 0x18;
    for (int i = 0; i < MAX_ITEM_SLOTS; i++) {
        if (order[i] == leafSlot) return;
        if (order[i] == dItemNo_NONE_e) { order[i] = leafSlot; return; }
    }
}

static HookAction msg_pre(ModContext*, void* args, void* retval, void*) {
    u32 id = mods::arg<u32>(args, 1);
    customDesc = id == descMsg;
    if (id != nameMsg) return HOOK_CONTINUE;
    for (int i = 2; i < 4; i++) {
        auto* box = mods::arg<J2DTextBox*>(args, i);
        if (!box) continue;
        // setString() shrinks the buffer, then the next name overflows it
        char* txt = box->getStringPtr();
        u16 room = box->getStringAllocByte();
        if (!txt || !room) continue;
        strncpy(txt, "Ice Leaf", room - 1);
        txt[room - 1] = 0;
    }
    if (retval) *(f32*)retval = 0.0f;
    return HOOK_SKIP_ORIGINAL;
}

static void exec_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    // riding: keep the leaf's button lit so it reads as "press to get off"
    if (out())
        for (int btn = 0; btn < 2; btn++)
            if (dComIfGp_getSelectItem(btn) == leafItem) lnk->field_0x2faf |= (u8)(1 << btn);
    if (!out() || mounting || !lnk->checkBoardRide() || !mDoCPd_c::getHoldR(PAD_1)) return;

    bool on = false;
    if (vBoost) svc_config->get_bool(mod_ctx, vBoost, &on);
    f32 sp = lnk->mNormalSpeed + 2.5f;
    if (on) lnk->mNormalSpeed = sp > 75.0f ? 75.0f : sp;
}

static HookAction msgbegin_pre(ModContext*, void* args, void*, void*) {
    customMsg = mods::arg<u32>(args, 1) == pickMsg;
    return HOOK_CONTINUE;
}

// swap the text pointers on the control; the pickup prompt also gets a speaker
static void msgseq_post(ModContext*, void* args, void* retval, void*) {
    bool desc = customDesc, pick = customMsg;
    customDesc = customMsg = false;
    if ((!desc && !pick) || !retval || !*(bool*)retval) return;
    auto* ctl = mods::arg<JMessage::TControl*>(args, 0);
    const char* txt = desc ? descText : getText;
    ctl->pMessageText_begin_ = txt;
    ctl->pMessageText_current_ = txt;
    // stock item text has no speaker so it's silent; 0x71 falls to the default voice and gives the text blips
    if (pick && ctl->pEntry_) ((JMSMesgEntry_c*)ctl->pEntry_)->speaker = 0x71;
}

static HookAction tex_pre(ModContext*, void* args, void*, void*) {
    void* buf = mods::arg<void*>(args, 2);
    forgetIcon(buf);
    u8& no = mods::arg_ref<u8>(args, 1);
    iconBuf = nullptr;
    if (no == leafItem) {
        no = dItemNo_BOOMERANG_e;
        iconBuf = buf;
    }
    return HOOK_CONTINUE;
}

static void tex_post(ModContext*, void*, void*, void*) {
    void* buf = iconBuf;
    iconBuf = nullptr;
    if (!buf || !iconRes.data || iconRes.size != (size_t)iconSz * iconSz * 4) return;

    uint32_t off = ((const ResTIMG*)buf)->imageOffset;
    if (off > 0xC00) off = ((off >> 24) & 0xFF) | ((off >> 8) & 0xFF00) | ((off << 8) & 0xFF0000) | (off << 24);
    if (off == 0 || off >= 0xC00) off = 0x20;

    if (nIcons >= 32) {
        svc_texture->unregister(mod_ctx, icons[0].h);
        icons[0] = icons[--nIcons];
    }

    TextureKey key = TEXTURE_KEY_INIT;
    key.kind = TEXTURE_KEY_POINTER;
    key.pointer = (uint8_t*)buf + off;

    TextureData d = TEXTURE_DATA_INIT;
    d.data = iconRes.data;
    d.size = iconRes.size;
    d.width = iconSz;
    d.height = iconSz;
    d.gx_format = GX_TF_RGBA8_PC;

    TextureReplacementHandle h = 0;
    if (svc_texture->register_data(mod_ctx, &key, &d, &h) == MOD_OK) icons[nIcons++] = {buf, h};
}

// ======================== Castle Town shops ========================
// Revert to Chudley's (Map Checks) and the shop swaps (Shop Expansion) work on the same shop

// --- Revert to Chudley's ---

DEFINE_HOOK(&dSv_event_c::isEventBit, EventBit);

static int shop_mode(bool inside) {
    if (!stage_is(inside ? "R_SP160" : "F_SP116")) return SHOP_OFF;
    return cfgInt(shopOption, SHOP_SEMI);
}

static void hide_malo_flags(ModContext*, void* args, void* ret, void*) {
    u16 bit = mods::arg<u16>(args, 1);
    if (peekRealFlags || *(int*)ret == 0) return;

    if (bit == kMaloOpen) {
        if (shop_mode(true) >= SHOP_SEMI || shop_mode(false) >= SHOP_PARTIAL) *(int*)ret = 0;
    } else if (bit == kDonation) {
        if (shop_mode(true) == SHOP_FULL || shop_mode(false) >= SHOP_PARTIAL) *(int*)ret = 0;
    }
}

// @bg0061 = poster boards
DEFINE_HOOK(&daBgObj_c::ExecuteType0, BgObjExec);

static HookAction posters_down(ModContext*, void* args, void* ret, void*) {
    daBgObj_c* boards = mods::arg<daBgObj_c*>(args, 0);
    if (fopAcM_GetParam(boards) != 0x1FE00061 || shop_mode(false) < SHOP_PARTIAL) return HOOK_CONTINUE;

    if (boards->field_0xcc8 == 0) {
        boards->release(boards->mpBgW);
        boards->field_0xcc8 = 1;
    }
    *(int*)ret = 1;
    return HOOK_SKIP_ORIGINAL;
}

// file-relative until the loader fixes it up
static void* chunk_data(void* file, OffsetPtr& off) {
    u32 raw = (u32)(s32)off.value;
    if ((raw & 0x80000000u) == 0) return (u8*)file + raw;
    s32 rel = (raw & 0x40000000u) ? (s32)raw : (s32)(raw & 0x7FFFFFFFu);
    return (u8*)&off + rel;
}

// the tag's own create wrapper, fires before the shopkeeper looks at the shelf
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_tag_shop_item.cpp#daTag_ShopItem_Create", int(void*), ShelfSpawn);

// use the Malo Mart (ACT1) prompt for the same item
static void malo_shelf_prompt(ModContext*, void* args, void* ret, void*) {
    auto* shelf = static_cast<daTag_ShopItem_c*>(mods::arg<void*>(args, 0));
    int mode = shop_mode(true);
    if (mode != SHOP_SEMI && mode != SHOP_PARTIAL) return;

    if (!real_flag(kMaloOpen)) return;

    void* room = dComIfG_getStageRes(dComIfG_getRoomArcName(fopAcM_GetRoomNo(shelf)), "room.dzr");
    if (room == nullptr) return;

    auto* hdr = static_cast<dStage_fileHeader*>(room);
    u8 want = fopAcM_GetParam(shelf) & 0xFF;
    int c = 0;
    while (c < hdr->m_chunkCount && memcmp(&hdr->m_nodes[c].m_tag, "ACT1", 4) != 0) c++;
    if (c == hdr->m_chunkCount) return;

    dStage_nodeHeader* act1 = &hdr->m_nodes[c];
    auto* a = static_cast<stage_actor_data_class*>(chunk_data(room, act1->m_offset));
    for (int n = act1->m_entryNum; n > 0; n--, a++) {
        u32 prm = a->base.parameters;
        if (strncmp(a->name, "TGSPITM", 8) == 0 && (prm & 0xFF) == want) {
            csXyz ang = a->base.angle;
            shelf->home.angle.x = ang.x;
            break;
        }
    }
}

// what the shop really is for pricing: On fakes Chudley's all the way, Semi/Partial keep Malo Mart's
static bool malo_pricing() {
    return shop_mode(true) != SHOP_FULL && real_flag(kMaloOpen);
}

// --- shop swaps ---

namespace flw = mods::flow;

DEFINE_HOOK(&dShopSystem_c::shop_process, ShopProc);
DEFINE_HOOK(&dShopSystem_c::searchItemActor, ShopSearch);
DEFINE_HOOK(&daShopItem_c::getShopArcname, ShopArc);
DEFINE_HOOK(&dMsgFlow_c::branchNodeProc, BranchProc);
DEFINE_HOOK(&dMsgFlow_c::query004, QRupee);
DEFINE_HOOK(&dMsgFlow_c::query022, QOwned);
DEFINE_HOOK(&dMsgFlow_c::query024, QArrows);
DEFINE_HOOK(&dMsgFlow_c::event003, EPay);
DEFINE_HOOK(&dMsgFlow_c::event030, EFillArrows);
DEFINE_HOOK(&dMsgFlow_c::event008, EStart);

enum Who { NOBODY, GORON, CHUDLEY, MALO };

static constexpr u16 kGrp = 4;
static constexpr u16 kGoronPrice = 110;
static constexpr const char* kGoronCheck = "shop:F_SP116:0:16";

static dShopSystem_c* g_inShop;
static dShopSystem_c* g_lastShop;
static fpc_ProcID g_lastShopId = fpcM_ERROR_PROCESS_ID_e;
static bool g_reused;
static u16 g_jump, g_nodeShop, g_nodeGoron;
static bool g_net, g_netGot;
static int g_netWait;
static ItemGiveHandle g_obs;

static flw::Graph g_graph;
static std::vector<flw::RegisteredMessage> g_msgs;
static std::vector<flw::MessageOverride> g_ovr;
static std::vector<uint8_t> g_out;

static bool in_shop_room() {
    return stage_is("R_SP160") && dComIfGp_roomControl_getStayNo() == 0;
}

static bool is_arrow(int no) {
    return no == dItemNo_ARROW_10_e || no == dItemNo_ARROW_20_e || no == dItemNo_ARROW_30_e;
}

static bool is_pot(int no) {
    return no == dItemNo_RED_BOTTLE_e || no == dItemNo_BLUE_BOTTLE_e;
}

static int slot_item(dShopSystem_c* s, int i) {
    fopAc_ac_c* it = fopAcM_SearchByID((fpc_ProcID)s->mItemCtrl.getItemIndex(i));
    return it ? (int)(fopAcM_GetParam(it) & 0xFF) : -1;
}

static int shop_item(dShopSystem_c* s) {
    u8 c = s->mCursorPos;
    if (c == 0 || c > dShopSystem_c::ITEM_MAX_e) return -1;
    return slot_item(s, c - 1);
}

static Who shop_who(dShopSystem_c* s) {
    if (!s || !cfgOn(vChud) || !in_shop_room() || !is_arrow(shop_item(s))) return NOBODY;
    return malo_pricing() ? MALO : CHUDLEY;
}

static dShopSystem_c* live_shop() {
    if (g_lastShop && fopAcM_SearchByID(g_lastShopId) == (fopAc_ac_c*)g_lastShop) return g_lastShop;
    return nullptr;
}

// another mod (the randomizer, with shop items on) can claim the goron's check; then he's left alone
static bool goron_ours() {
    if (!cfgOn(vGoron)) return false;
    ItemCheckResolution r{};
    if (svc_item->resolve_check_full(mod_ctx, kGoronCheck, dItemNo_ARROW_30_e, &r) != MOD_OK) return true;
    // claimed = swapped for a real item; NONE is just a sync mod marking the check as taken
    return r.item == dItemNo_ARROW_30_e || r.item == dItemNo_NONE_e;
}

static Who who(fopAc_ac_c* sp) {
    if (!sp) return NOBODY;
    if (stage_is("F_SP116") && fopAcM_GetName(sp) == fpcNm_NPC_GRA_e) return goron_ours() ? GORON : NOBODY;
    return sp == (fopAc_ac_c*)g_inShop ? shop_who(g_inShop) : NOBODY;
}

static u32 price(Who w) {
    return w == GORON ? kGoronPrice : w == MALO ? 15 : w == CHUDLEY ? 12000 : 0;
}

struct Tk {
    bool tag;
    std::string s;
};

static std::vector<Tk> lex(const uint8_t* p, size_t n) {
    std::vector<Tk> v;
    size_t i = 0;
    while (i < n && p[i]) {
        if (p[i] == 0x1A && i + 1 < n && p[i + 1] >= 5) {
            size_t l = p[i + 1];
            v.push_back({true, std::string((const char*)p + i, l)});
            i += l;
        } else {
            if (v.empty() || v.back().tag) v.push_back({false, {}});
            v.back().s += (char)p[i++];
        }
    }
    return v;
}

static void unlex(const std::vector<Tk>& v) {
    g_out.clear();
    for (auto& t : v) g_out.insert(g_out.end(), t.s.begin(), t.s.end());
    g_out.push_back(0);
}

static std::string col(u8 c) {
    std::string s("\x1A\x06\xFF\x00\x00", 5);
    s += (char)c;
    return s;
}

static bool is_col(const Tk& t) {
    return t.tag && t.s.size() == 6 && (u8)t.s[2] == 0xFF && t.s[3] == 0 && t.s[4] == 0;
}

static std::string low(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool has(const std::vector<Tk>& v, const char* w) {
    for (auto& t : v)
        if (!t.tag && low(t.s).find(w) != std::string::npos) return true;
    return false;
}

static void sub(std::string& s, const std::string& a, const std::string& b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size()))
        s.replace(p, a.size(), b);
}

static std::string commas(u32 v) {
    std::string n = std::to_string(v);
    for (int i = (int)n.size() - 3; i > 0; i -= 3) n.insert(i, ",");
    return n;
}

// rewrites the "<number> Rupee" amounts in s; numbers may carry thousands commas ("3,000"),
// the new one copies the old style, five digits and up always get one. fn maps old -> new, 0 = leave it
template <class F>
static void reprice(std::string& s, F fn) {
    for (size_t p = s.find("Rupee"); p != std::string::npos; p = s.find("Rupee", p + 5)) {
        size_t e = p;
        while (e > 0 && s[e - 1] == ' ') e--;
        size_t b = e;
        while (b > 0 && (isdigit((unsigned char)s[b - 1]) || (s[b - 1] == ',' && b > 1 && isdigit((unsigned char)s[b - 2])))) b--;
        if (b == e) continue;
        std::string old = s.substr(b, e - b);
        bool comma = old.find(',') != std::string::npos;
        std::string digits;
        for (char c : old) if (c != ',') digits += c;
        u32 nv = fn((u32)strtoul(digits.c_str(), nullptr, 10));
        if (!nv) continue;
        std::string n = (comma || nv >= 10000) ? commas(nv) : std::to_string(nv);
        s.replace(b, e - b, n);
        p = b + n.size() + (p - e);
    }
}

// whole-message version: the number is often colored on its own ("[purple]3,000[white] Rupees"), so when
// "Rupee" has nothing but spaces before it, the amount is the tail of the previous text run
template <class F>
static void reprice_v(std::vector<Tk>& v, F fn) {
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].tag) continue;
        reprice(v[i].s, fn);
        size_t p = v[i].s.find("Rupee");
        if (p == std::string::npos || v[i].s.find_first_not_of(' ') != p) continue;
        size_t j = i;
        while (j > 0 && v[j - 1].tag) j--;
        if (j == 0) continue;
        std::string& prev = v[j - 1].s;
        size_t e = prev.size();
        while (e > 0 && prev[e - 1] == ' ') e--;
        size_t b = e;
        while (b > 0 && (isdigit((unsigned char)prev[b - 1]) || (prev[b - 1] == ',' && b > 1 && isdigit((unsigned char)prev[b - 2])))) b--;
        if (b == e) continue;
        std::string tail = prev.substr(b) + " Rupee";
        reprice(tail, fn);
        prev.replace(b, e - b, tail.substr(0, tail.find(' ')));
    }
}

static void repr_v(std::vector<Tk>& v, u32 amt) {
    reprice_v(v, [amt](u32) { return amt; });
}

// chudley's tags in hd money (x4). the oil sits on the red potion's flow, so its 3,000 lands here too
static u32 chud_hd(u32 v) {
    switch (v) {
    case 2000:
    case 3000: return 12000;
    case 6000: return 24000;
    case 9000: return 36000;
    case 10000: return 40000;
    case 100000: return 400000;
    default: return 0;
    }
}

static void add_a(std::vector<Tk>& v, size_t at) {
    if (at > 0 && !v[at - 1].tag) {
        std::string& s = v[at - 1].s;
        while (!s.empty() && (s.back() == ' ' || isdigit((unsigned char)s.back()))) s.pop_back();
        s += (s.empty() || s.back() == '\n') ? "a " : " a ";
    } else {
        v.insert(v.begin() + at, Tk{false, "a "});
    }
}

// same rule the randomizer uses for its own text: break at the last space once a line passes
// 17 font cells. tags take no room
static void rewrap(std::vector<Tk>& v) {
    JUTFont* f = mDoExt_getMesgFont();
    f32 cell = f ? (f32)f->getCellWidth() : 0.0f;
    f32 w = 0.0f;
    std::string* sp = nullptr;
    size_t spAt = 0;
    for (auto& t : v) {
        if (t.tag) continue;
        for (size_t i = 0; i < t.s.size(); i++) {
            char c = t.s[i];
            if (c == '\n') {
                w = 0.0f;
                sp = nullptr;
                continue;
            }
            // never strand the "a" from the item name
            bool lone_a = i > 0 && t.s[i - 1] == 'a' && (i == 1 || t.s[i - 2] == ' ' || t.s[i - 2] == '\n');
            if (c == ' ' && !lone_a) {
                sp = &t.s;
                spAt = i;
            }
            w += cell > 0.0f ? (f32)f->getWidth((u8)c) / cell : 0.55f;
            if (w > 17.0f && sp) {
                (*sp)[spAt] = '\n';
                w = 0.0f;
                if (sp == &t.s)
                    for (size_t k = spAt + 1; k <= i; k++)
                        w += cell > 0.0f ? (f32)f->getWidth((u8)t.s[k]) / cell : 0.55f;
                sp = nullptr;
            }
        }
    }
}

static bool goron_text(std::vector<Tk>& v) {
    if (!has(v, "arrow")) return false;
    for (size_t i = 0; i < v.size(); i++) {
        if (!is_col(v[i]) || v[i].s[5] == 0) continue;
        size_t j = i + 1;
        std::string seg;
        while (j < v.size() && !is_col(v[j])) {
            if (!v[j].tag) seg += v[j].s;
            j++;
        }
        if (low(seg).find("arrow") == std::string::npos) continue;
        size_t ws = seg.find_last_not_of(' ');
        std::string tail = ws == std::string::npos ? "" : seg.substr(ws + 1);
        v[i].s[5] = (char)MESSAGE_COLOR_BLUE;
        v.erase(v.begin() + i + 1, v.begin() + j);
        v.insert(v.begin() + i + 1, Tk{false, "Blue Potion" + tail});
        if (tail.empty() && i + 2 < v.size() && is_col(v[i + 2]) && i + 3 < v.size() && !v[i + 3].tag &&
            !v[i + 3].s.empty() && isalnum((unsigned char)v[i + 3].s[0]))
            v[i + 3].s.insert(0, " ");
        add_a(v, i);
        i += 2;
    }
    for (size_t i = 1; i < v.size(); i++) {
        if (v[i].tag || !v[i - 1].tag || is_col(v[i - 1])) continue;
        std::string l = low(v[i].s);
        size_t k = l.find_first_not_of(' ');
        if (k == std::string::npos || l.compare(k, 5, "arrow") != 0) continue;
        size_t e = l.find("arrows", k) == k ? k + 6 : k + 5;
        v[i].s.replace(0, e, col(MESSAGE_COLOR_BLUE) + "Blue Potion" + col(0));
        v.erase(v.begin() + i - 1);
        add_a(v, i - 1);
    }
    for (auto& t : v) {
        if (t.tag) continue;
        std::string l = low(t.s);
        size_t p = l.find("30 arrows");
        if (p != std::string::npos)
            t.s.replace(p, 9, "a " + col(MESSAGE_COLOR_BLUE) + "Blue Potion" + col(0));
        sub(t.s, "ARROWS", "POTIONS");
        sub(t.s, "Arrows", "Potions");
        sub(t.s, "arrows", "potions");
        sub(t.s, "Arrow", "Potion");
        sub(t.s, "arrow", "potion");
    }
    repr_v(v, kGoronPrice);
    rewrap(v);
    return true;
}

static bool shop_text(std::vector<Tk>& v, dShopSystem_c* s, Who w) {
    if (s->chkExplainItem() && has(v, "rupee")) {
        std::vector<Tk> o;
        for (auto& t : v) {
            if (!t.tag) break;
            if (!is_col(t)) o.push_back(t);
        }
        std::string b = col(MESSAGE_COLOR_YELLOW) + "Lantern Oil" + col(0) + " ";
        if (w == MALO)
            b += col(MESSAGE_COLOR_RED) + "!Nice Deal!" + col(0) + " " + col(MESSAGE_COLOR_PURPLE) +
                 "15 Rupees" + col(0) + "\nYou can't live without it!\n(Requires an empty bottle.)";
        else
            b += col(MESSAGE_COLOR_PURPLE) + "12,000 Rupees" + col(0) +
                 "\nThis liquid sears its way into\nhouseholds worldwide.";
        o.push_back({false, b});
        v = o;
        return true;
    }
    if (!has(v, "arrow") && !has(v, "potion") && !has(v, "rupee")) return false;
    for (auto& t : v) {
        if (t.tag) continue;
        for (const char* n : {"10 ", "20 ", "30 "}) {
            sub(t.s, std::string(n) + "Arrows", "Lantern Oil");
            sub(t.s, std::string(n) + "arrows", "lantern oil");
        }
        sub(t.s, " (10)", "");
        sub(t.s, " (20)", "");
        sub(t.s, " (30)", "");
        for (const char* a : {"Red Potion", "Blue Potion", "Arrows", "Arrow", "Potion"})
            sub(t.s, a, "Lantern Oil");
        for (const char* a : {"red potion", "blue potion", "arrows", "arrow", "potion"})
            sub(t.s, a, "lantern oil");
    }
    repr_v(v, price(w));
    return true;
}

static bool on_text(ModContext*, const MessageOverrideContext* m, MessageTextData* out, void*) {
    if (!m->original_text || !out) return false;
    bool goron = m->group == kGrp && stage_is("F_SP116");
    dShopSystem_c* s = goron ? nullptr : live_shop();
    bool chud = s && in_shop_room() && cfgOn(vChud) && !malo_pricing();
    if (!goron && !s) return false;
    auto v = lex(m->original_text, m->original_text_size);
    bool ok = false;
    if (goron) {
        ok = goron_ours() && goron_text(v);
    } else {
        Who w = shop_who(s);
        if (w != NOBODY) ok = shop_text(v, s, w);
        if (chud && has(v, "rupee")) {
            reprice_v(v, chud_hd);
            ok = true;
        }
    }
    if (!ok) return false;
    unlex(v);
    out->text = g_out.data();
    out->text_size = g_out.size();
    return true;
}

static HookAction shop_pre(ModContext*, void* a, void*, void*) {
    g_inShop = mods::arg<dShopSystem_c*>(a, 0);
    g_lastShop = g_inShop;
    g_lastShopId = fopAcM_GetID(g_inShop);
    return HOOK_CONTINUE;
}

static void shop_post(ModContext*, void*, void*, void*) {
    g_inShop = nullptr;
}

static void search_post(ModContext*, void* a, void* r, void*) {
    auto* s = mods::arg<dShopSystem_c*>(a, 0);
    if (!r || !*static_cast<bool*>(r) || !in_shop_room() || !cfgOn(vChud)) return;
    int pot = -1;
    for (int i = 0; i < dShopSystem_c::ITEM_MAX_e; i++) {
        int no = slot_item(s, i);
        if (no == dItemNo_RED_BOTTLE_e || (pot < 0 && is_pot(no))) pot = i;
    }
    g_reused = pot >= 0;
    if (!g_reused) return;
    for (int i = 0; i < dShopSystem_c::ITEM_MAX_e; i++)
        if (is_arrow(slot_item(s, i))) s->mItemCtrl.setMessageIndex(i, s->mItemCtrl.getMessageIndex(pot));
}

static HookAction arc_pre(ModContext*, void* a, void*, void*) {
    auto* it = mods::arg<daShopItem_c*>(a, 0);
    if (is_arrow(it->m_itemNo) && in_shop_room() && cfgOn(vChud)) it->m_itemNo = dItemNo_OIL_BOTTLE_e;
    return HOOK_CONTINUE;
}

static HookAction rupee_pre(ModContext*, void* a, void*, void*) {
    auto* nd = mods::arg<mesg_flow_node_branch*>(a, 1);
    Who w = who(mods::arg<fopAc_ac_c*>(a, 2));
    if (w == NOBODY || !nd || nd->param == 0) return HOOK_CONTINUE;
    nd->param = (u16)price(w);
    if ((w == GORON || !g_reused) && mods::arg<int>(a, 3) != 0 && dComIfGs_checkEmptyBottle() == 0)
        g_jump = w == GORON ? g_nodeGoron : g_nodeShop;
    return HOOK_CONTINUE;
}

static void branch_post(ModContext*, void* a, void*, void*) {
    if (!g_jump) return;
    u16 n = g_jump;
    g_jump = 0;
    mods::arg<dMsgFlow_c*>(a, 0)->setNodeIndex(n, mods::arg<fopAc_ac_c**>(a, 2));
}

static HookAction owned_pre(ModContext*, void* a, void* r, void*) {
    auto* nd = mods::arg<mesg_flow_node_branch*>(a, 1);
    if (!nd || who(mods::arg<fopAc_ac_c*>(a, 2)) != GORON) return HOOK_CONTINUE;
    if ((nd->param & 0xFF) != dItemNo_BOW_e) return HOOK_CONTINUE;
    if (r) *static_cast<u16*>(r) = 0;
    return HOOK_SKIP_ORIGINAL;
}

static HookAction arrows_pre(ModContext*, void* a, void* r, void*) {
    if (who(mods::arg<fopAc_ac_c*>(a, 2)) != GORON) return HOOK_CONTINUE;
    if (r) *static_cast<u16*>(r) = 1;
    return HOOK_SKIP_ORIGINAL;
}

static HookAction fill_pre(ModContext*, void* a, void* r, void*) {
    if (who(mods::arg<fopAc_ac_c*>(a, 2)) != GORON) return HOOK_CONTINUE;
    g_net = true;
    g_netGot = false;
    g_netWait = 0;
    if (r) *static_cast<int*>(r) = 1;
    return HOOK_SKIP_ORIGINAL;
}

static void on_give(ModContext*, const ItemGiveInfo* gi, void*) {
    if (!g_net || !gi) return;
    if (gi->item == dItemNo_BLUE_BOTTLE_e || (gi->check_name && !strcmp(gi->check_name, kGoronCheck)))
        g_netGot = true;
}

// swap the item inside his "give" event instead of through the shop check: the check is a one-time
// collectible to randomizer/online sync mods ("Already collected"), the potion has to stay repeatable.
// a blue potion isn't his check item, so the game hands it over without touching the check at all
// same for the lantern oil: the shop's give event hands out oil directly, so no check gets resolved
static HookAction start_pre(ModContext*, void* a, void*, void*) {
    auto* nd = mods::arg<mesg_flow_node_event*>(a, 1);
    Who w = who(mods::arg<fopAc_ac_c*>(a, 2));
    if (!nd || w == NOBODY || nd->params[0] != 0 || nd->params[1] != 1 || nd->params[2] != 0) return HOOK_CONTINUE;
    u8& item = nd->params[3];
    if (w == GORON && item == dItemNo_ARROW_30_e)
        item = dItemNo_BLUE_BOTTLE_e;
    else if (w != GORON && (is_arrow(item) || is_pot(item)))
        item = dItemNo_OIL_BOTTLE_e;
    return HOOK_CONTINUE;
}

static HookAction pay_pre(ModContext*, void* a, void*, void*) {
    auto* nd = mods::arg<mesg_flow_node_event*>(a, 1);
    Who w = who(mods::arg<fopAc_ac_c*>(a, 2));
    if (w == NOBODY || !nd || !(nd->params[0] | nd->params[1] | nd->params[2] | nd->params[3]))
        return HOOK_CONTINUE;
    u32 p = price(w);
    nd->params[0] = (u8)(p >> 24);
    nd->params[1] = (u8)(p >> 16);
    nd->params[2] = (u8)(p >> 8);
    nd->params[3] = (u8)p;
    return HOOK_CONTINUE;
}

static ModResult reg_msg(const char* txt, u16& id) {
    auto mb = flw::MessageBuilder{flw::MessageStyle{}.trailing_data(0x0400)}.text(txt);
    std::vector<flw::MessageVariant> vs;
    for (auto l : {MESSAGE_LANGUAGE_ENGLISH, MESSAGE_LANGUAGE_GERMAN, MESSAGE_LANGUAGE_FRENCH,
             MESSAGE_LANGUAGE_SPANISH, MESSAGE_LANGUAGE_ITALIAN, MESSAGE_LANGUAGE_JAPANESE})
        vs.push_back(mb.build(l));
    auto m = flw::register_message(kGrp, vs);
    if (!m) return m.result();
    id = m.id();
    g_msgs.push_back(std::move(m));
    return MOD_OK;
}

// shop side never fails the mod, the leaf has to keep working on its own
static void shop_init() {
    g_inShop = g_lastShop = nullptr;
    g_jump = 0;
    g_reused = g_net = false;
    u16 shopMsg, goronMsg;
    if (reg_msg("Sorry, I can only sell this to\ncustomers with empty bottles.\nNext time, maybe.", shopMsg) != MOD_OK ||
        reg_msg("You'll need an empty bottle for\nthis, Brother!", goronMsg) != MOD_OK) {
        mods::log::warn("shop swap: couldn't register messages, shops stay vanilla");
        return;
    }
    {
        flw::GraphBuilder gb{kGrp};
        g_nodeShop = gb.add_message(shopMsg).next(flw::kEnd).id();
        g_nodeGoron = gb.add_message(goronMsg).next(flw::kEnd).id();
        g_graph = gb.commit();
        if (!g_graph) {
            mods::log::warn("shop swap: couldn't commit flow nodes, shops stay vanilla");
            return;
        }
    }
    // castle town text, plus the common group where generic shop prompts may live
    for (u16 id = 4000; id < 9000; id++) g_ovr.push_back(flw::override_message_fn(kGrp, id, MESSAGE_LANGUAGE_ENGLISH, on_text));
    for (u16 id = 0; id < 2500; id++) g_ovr.push_back(flw::override_message_fn(0, id, MESSAGE_LANGUAGE_ENGLISH, on_text));
    svc_item->observe_gives(mod_ctx, on_give, nullptr, &g_obs);
    bool ok = pre<ShopProc>(shop_pre) && post<ShopProc>(shop_post) && post<ShopSearch>(search_post) &&
              pre<ShopArc>(arc_pre) && post<BranchProc>(branch_post) && pre<QRupee>(rupee_pre) &&
              pre<EPay>(pay_pre) && pre<QOwned>(owned_pre) && pre<QArrows>(arrows_pre) &&
              pre<EFillArrows>(fill_pre) && pre<EStart>(start_pre);
    if (!ok) mods::log::warn("shop swap: some hooks failed, shops may act vanilla");
}

static void shop_update() {
    if (!g_net) return;
    if (!stage_is("F_SP116") || g_netGot) {
        g_net = false;
        return;
    }
    if (dComIfGp_event_runCheck()) {
        g_netWait = 0;
        return;
    }
    if (++g_netWait > 30) {
        dComIfGs_setEmptyBottleItemIn(dItemNo_BLUE_BOTTLE_e);
        g_net = false;
    }
}

static void shop_shutdown() {
    g_ovr.clear();
    g_graph.reset();
    g_msgs.clear();
    g_inShop = g_lastShop = nullptr;
}

// ======================== mod page ========================

static bool leaf_off(ModContext*, void*);

static ModResult shop_panel(UiElementHandle panel) {
    svc_ui->pane_add_text(mod_ctx, panel, "Regular Shop Changes", nullptr);

    UiControlDesc gor = UI_CONTROL_DESC_INIT;
    gor.kind = UI_CONTROL_TOGGLE;
    gor.label = "Goron Outpost Changes";
    gor.help_rml = "The Castle Town arrow Goron sells a Blue Potion (needs an empty bottle) for 110 Rupees instead of arrows.";
    gor.binding = UI_BINDING_CONFIG_VAR;
    gor.config_var = vGoron;
    if (vGoron) {
        ModResult r = svc_ui->pane_add_control(mod_ctx, panel, &gor, nullptr);
        if (r != MOD_OK) return r;
    }

    UiControlDesc chud = UI_CONTROL_DESC_INIT;
    chud.kind = UI_CONTROL_TOGGLE;
    chud.label = "Chudleys/Malomart Changes";
    chud.help_rml = "Chudley's and the Castle Town Malo Mart sell Lantern Oil instead of arrows. Shelf changes show after leaving and re-entering the shop.";
    chud.binding = UI_BINDING_CONFIG_VAR;
    chud.config_var = vChud;
    if (vChud) {
        ModResult r = svc_ui->pane_add_control(mod_ctx, panel, &chud, nullptr);
        if (r != MOD_OK) return r;
    }

    svc_ui->pane_add_text(mod_ctx, panel, "Ice Leaf", nullptr);

    if (vLeaf) {
        UiControlDesc en = UI_CONTROL_DESC_INIT;
        en.kind = UI_CONTROL_TOGGLE;
        en.label = "Enable Ice Leaf";
        en.help_rml = "Turns the whole Ice Leaf part of the mod on or off. Off takes it off your item wheel until it's turned back on.";
        en.binding = UI_BINDING_CONFIG_VAR;
        en.config_var = vLeaf;
        ModResult r = svc_ui->pane_add_control(mod_ctx, panel, &en, nullptr);
        if (r != MOD_OK) return r;
    }

    UiControlDesc always = UI_CONTROL_DESC_INIT;
    always.kind = UI_CONTROL_TOGGLE;
    always.label = "Always Have Ice Sled";
    always.help_rml = "Gives you the Ice Leaf without knocking it off the tree in Snowpeak. Turning it off removes it again unless you already did.";
    always.binding = UI_BINDING_CONFIG_VAR;
    always.config_var = vAlways;
    always.is_disabled = leaf_off;
    if (vAlways) {
        ModResult res = svc_ui->pane_add_control(mod_ctx, panel, &always, nullptr);
        if (res != MOD_OK) return res;
    }

    UiControlDesc boost = UI_CONTROL_DESC_INIT;
    boost.kind = UI_CONTROL_TOGGLE;
    boost.label = "RT Speed Boost Cheat";
    boost.help_rml = "While riding the Ice Leaf, hold RT to build up extra speed.";
    boost.binding = UI_BINDING_CONFIG_VAR;
    boost.config_var = vBoost;
    boost.is_disabled = leaf_off;
    if (!vBoost) return MOD_OK;
    return svc_ui->pane_add_control(mod_ctx, panel, &boost, nullptr);
}

static ModResult panel_update(ModContext*, void*, ModError*) { return MOD_OK; }

static bool leaf_off(ModContext*, void*) { return !cfgOn(vLeaf); }

static ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    auto* ui = svc_ui;
    ui->pane_add_section(mod_ctx, panel, "Less Restrictive Maps");

    ui->pane_add_text(mod_ctx, panel, "Epona Locations", nullptr);
    UiControlDesc t = UI_CONTROL_DESC_INIT;
    t.kind = UI_CONTROL_TOGGLE;
    t.binding = UI_BINDING_CONFIG_VAR;
    for (int i = 0; i < 3; i++) {
        t.label = eponaLabels[i];
        t.config_var = eponaToggle[i];
        ui->pane_add_control(mod_ctx, panel, &t, nullptr);
    }

    UiControlDesc dd = UI_CONTROL_DESC_INIT;
    dd.kind = UI_CONTROL_DROPDOWN;
    dd.binding = UI_BINDING_CONFIG_VAR;
    ui->pane_add_text(mod_ctx, panel, "Map Checks", nullptr);

    dd.label = "Remove faron woods mist";
    dd.options = mistNames;
    dd.option_count = 3;
    dd.config_var = mistOption;
    ui->pane_add_control(mod_ctx, panel, &dd, nullptr);

    // same thing, the shop one has an extra choice
    dd.label = "Revert to Chudley's";
    dd.options = shopNames;
    dd.option_count = 4;
    dd.config_var = shopOption;
    ui->pane_add_control(mod_ctx, panel, &dd, nullptr);

    ui->pane_add_section(mod_ctx, panel, "Shop Expansion");
    return shop_panel(panel);
}

static bool add_int_option(const char* key, int64_t def, ConfigVarHandle* out) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = key;
    d.type = CONFIG_VAR_INT;
    d.default_int = def;
    return svc_config->register_var(mod_ctx, &d, out) == MOD_OK;
}

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    // --- Less Restrictive Maps ---
    for (int i = 0; i < 3; i++) {
        ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
        d.name = eponaKeys[i];
        d.type = CONFIG_VAR_BOOL;
        d.default_bool = true;
        if (svc_config->register_var(mod_ctx, &d, &eponaToggle[i]) != MOD_OK)
            return mods::set_error(error, MOD_ERROR, "can't register settings");
    }
    if (!add_int_option("removeFaronMist", MIST_SEMI, &mistOption) ||
        !add_int_option("chudleysMode", SHOP_SEMI, &shopOption))
        return mods::set_error(error, MOD_ERROR, "can't register settings");

    if (!pre<HorseSavePos>(keep_field_spot) || !pre<SetStartStage>(remember_stage) ||
        !post<SetStartStage>(check_horse_arrival))
        return mods::set_error(error, MOD_ERROR, "horse hooks failed");

    if (!post<TownUseItem>(allow_horse_call)) mods::log::warn("Horse Call stays blocked in Castle Town");
    if (!pre<HorseSearchEnemy>(calm_epona)) mods::log::warn("Epona may still spook at fish");

    int faronFails = 0;
    faronFails += !pre<BgDraw>(note_bg_room);
    faronFails += !post<BgDraw>(clear_bg_room);
    faronFails += !post<BgMaterialProc>(hide_mist_sheet);
    faronFails += !post<FieldMapGroupSwitch>(pause_map_switch);
    faronFails += !post<MiniMapGroupSwitch>(minimap_switch);
    faronFails += !pre<FogTagExecute>(kill_mist_tag);
    if (faronFails) mods::log::warn("Faron mist option won't fully work");

    if (!post<EventBit>(hide_malo_flags)) mods::log::warn("Chudley's option off");
    if (!pre<BgObjExec>(posters_down)) mods::log::warn("posters will stay up");
    if (!post<ShelfSpawn>(malo_shelf_prompt)) mods::log::warn("shelves keep Chudley's prices");

    // --- Shop Expansion ---
    ModResult res = mods::hook::add_pre<Exec>(exec_pre);
    if (res == MOD_OK) res = mods::hook::add_post<Exec>(exec_post);
    if (res != MOD_OK)
        return mods::set_error(error, res, "couldn't hook daAlink_c::execute, the leaf won't work");

    ModResult a = mods::hook::add_pre<NewItem>(newitem_pre);
    ModResult b = mods::hook::add_post<SnowCode>(snow_post);
    if (mods::hook::add_post<Stick>(stick_post) != MOD_OK)
        mods::log::warn("stick hook failed, link can walk during the pickup prompt");
    if (a != MOD_OK || b != MOD_OK)
        mods::log::warn("riding hooks failed (items {}, snow {}), items might work on the leaf", (int)a, (int)b);

    res = mods::hook::add_post<LineUp>(lineup_post);
    if (res == MOD_OK) res = mods::hook::add_pre<MsgStr>(msg_pre);
    if (res == MOD_OK) res = mods::hook::add_pre<ReadTex>(tex_pre);
    if (res == MOD_OK) res = mods::hook::add_post<ReadTex>(tex_post);
    if (res != MOD_OK)
        mods::log::warn("wheel hooks failed ({}), item may be missing its name or icon", (int)res);

    if (svc_resource->load(mod_ctx, "iceleaf_icon.rgba", &iconRes) != MOD_OK)
        mods::log::warn("iceleaf_icon.rgba missing, wheel will show the boomerang icon");

    res = mods::hook::add_pre<MsgBegin>(msgbegin_pre);
    if (res == MOD_OK) res = mods::hook::add_post<MsgSeq>(msgseq_post);
    if (res != MOD_OK)
        mods::log::warn("pickup text hooks failed ({}), prompt will show the vanilla text", (int)res);

    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.type = CONFIG_VAR_BOOL;
    d.name = "rtBoost";
    d.default_bool = false;
    ModResult c = svc_config->register_var(mod_ctx, &d, &vBoost);
    d.name = "alwaysHave";
    ModResult e = svc_config->register_var(mod_ctx, &d, &vAlways);
    d.default_bool = true;
    d.name = "goronShop";
    if (svc_config->register_var(mod_ctx, &d, &vGoron) != MOD_OK) vGoron = 0;
    d.name = "chudleyShop";
    if (svc_config->register_var(mod_ctx, &d, &vChud) != MOD_OK) vChud = 0;
    d.name = "iceLeaf";
    if (svc_config->register_var(mod_ctx, &d, &vLeaf) != MOD_OK) vLeaf = 0;
    if (c != MOD_OK || e != MOD_OK) {
        mods::log::warn("config register failed (boost {}, always {}), toggles off", (int)c, (int)e);
        vBoost = vAlways = 0;
    }

    shop_init();

    // one panel for both halves
    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = panel_update;
    res = svc_ui->register_mods_panel(mod_ctx, &panel);
    if (res != MOD_OK) mods::log::warn("mods panel failed ({}), toggles keep their saved values", (int)res);

    svc_log->info(mod_ctx, "Tools and Expansion loaded");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    epona_tick();
    faron_lighting_tick();
    shop_update();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    put_lighting_back();
    if (out()) drop((daAlink_c*)dComIfGp_getPlayer(0));
    if (dComIfGs_getItem(leafSlot, true) == leafItem) setLeafSlot(dItemNo_NONE_e);
    for (int i = 0; i < nIcons; i++) svc_texture->unregister(mod_ctx, icons[i].h);
    nIcons = 0;
    if (iconRes.data) svc_resource->free(mod_ctx, &iconRes);
    shop_shutdown();
    return MOD_OK;
}

}
