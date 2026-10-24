#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_bg.h"
#include "d/actor/d_a_bg_obj.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_kytag08.h"
#include "d/actor/d_a_tag_shop_item.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_map_path_dmap.h"
#include "d/d_menu_fmap_map.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"

#include <cstring>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

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

static fpc_ProcID lastLink = fpcM_ERROR_PROCESS_ID_e;
static fpc_ProcID myEpona = fpcM_ERROR_PROCESS_ID_e;
static cXyz eponaSpot;
static s16 eponaFacing;
static bool rodeIn;
static char oldStage[9];

static int faronSaveTable = 2;
static int drawingRoom = -1;
static bool peekRealFlags;

static const char* stage_now() {
    return dComIfGp_getStartStageName();
}

static int horse_area(const char* stage) {
    if (stage == nullptr) return -1;
    for (int i = 0; i < 3; i++)
        if (strcmp(stage, eponaStages[i]) == 0) return i;
    return -1;
}

static bool horse_allowed(int area) {
    bool on = false;
    if (area >= 0) svc_config->get_bool(mod_ctx, eponaToggle[area], &on);
    return on;
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
    int64_t mode = MIST_OFF;
    svc_config->get_int(mod_ctx, mistOption, &mode);
    switch (mode) {
    case MIST_ON:
        return !dComIfGs_isEventBit(0x4510) || dComIfGs_isDarkClearLV(0);  // not mid-twilight
    case MIST_SEMI:
        return dComIfGs_isEventBit(0x0602) && dComIfGs_isEventBit(0x1B08);  // temple + lantern monkey
    default:
        return false;
    }
}

static bool in_clear_faron() {
    const char* s = stage_now();
    return s && strcmp(s, "F_SP108") == 0 && !dKy_darkworld_check() && mist_gone();
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
    const char* s = stage_now();
    bool inFaron = link && s && !strcmp(s, "F_SP108");

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

// --- Chudley's ---

DEFINE_HOOK(&dSv_event_c::isEventBit, EventBit);

static int shop_mode(bool inside) {
    const char* s = stage_now();
    if (!s || strcmp(s, inside ? "R_SP160" : "F_SP116") != 0) return SHOP_OFF;
    int64_t mode = SHOP_SEMI;
    svc_config->get_int(mod_ctx, shopOption, &mode);
    return static_cast<int>(mode);
}

// 0x2210 = Malo Mart open in town, 0x0F10 = donation paid
static void hide_malo_flags(ModContext*, void* args, void* ret, void*) {
    u16 bit = mods::arg<u16>(args, 1);
    if (peekRealFlags || *(int*)ret == 0) return;

    if (bit == 0x2210) {
        if (shop_mode(true) >= SHOP_SEMI || shop_mode(false) >= SHOP_PARTIAL) *(int*)ret = 0;
    } else if (bit == 0x0F10) {
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

    peekRealFlags = true;
    bool maloOpen = dComIfGs_isEventBit(0x2210);
    peekRealFlags = false;
    if (!maloOpen) return;

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

// --- menu ---

static ModResult build_panel(ModContext* ctx, UiElementHandle panel, void* ud, ModError* err) {
    auto* ui = svc_ui;
    ui->pane_add_section(mod_ctx, panel, "Epona Locations");
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
    ui->pane_add_section(mod_ctx, panel, "Map Checks");

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
    return MOD_OK;
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

    if (mods::hook::add_pre<HorseSavePos>(keep_field_spot) != MOD_OK ||
        mods::hook::add_pre<SetStartStage>(remember_stage) != MOD_OK ||
        mods::hook::add_post<SetStartStage>(check_horse_arrival) != MOD_OK)
        return mods::set_error(error, MOD_ERROR, "horse hooks failed");

    if (mods::hook::add_post<TownUseItem>(allow_horse_call) != MOD_OK)
        mods::log::warn("Horse Call stays blocked in Castle Town");
    if (mods::hook::add_pre<HorseSearchEnemy>(calm_epona) != MOD_OK)
        mods::log::warn("Epona may still spook at fish");

    int faronFails = 0;
    faronFails += mods::hook::add_pre<BgDraw>(note_bg_room) != MOD_OK;
    faronFails += mods::hook::add_post<BgDraw>(clear_bg_room) != MOD_OK;
    faronFails += mods::hook::add_post<BgMaterialProc>(hide_mist_sheet) != MOD_OK;
    faronFails += mods::hook::add_post<FieldMapGroupSwitch>(pause_map_switch) != MOD_OK;
    faronFails += mods::hook::add_post<MiniMapGroupSwitch>(minimap_switch) != MOD_OK;
    faronFails += mods::hook::add_pre<FogTagExecute>(kill_mist_tag) != MOD_OK;
    if (faronFails) mods::log::warn("Faron mist option won't fully work");

    if (mods::hook::add_post<EventBit>(hide_malo_flags) != MOD_OK) mods::log::warn("Chudley's option off");
    if (mods::hook::add_pre<BgObjExec>(posters_down) != MOD_OK) mods::log::warn("posters will stay up");
    if (mods::hook::add_post<ShelfSpawn>(malo_shelf_prompt) != MOD_OK)
        mods::log::warn("shelves keep Chudley's prices");

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    epona_tick();
    faron_lighting_tick();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    put_lighting_back();
    return MOD_OK;
}
}
