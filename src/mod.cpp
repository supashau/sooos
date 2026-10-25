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

// maps
#define EPONA_AREAS 4
static const char* eponaStages[] = {"F_SP116", "F_SP127", "F_SP124", "F_SP117"};
static const char* eponaKeys[] = {"castleTown", "fishingHole", "gerudoDesert", "sacredGrove"};
static const char* eponaLabels[] = {"Epona in Castle Town", "Epona at the Fishing Hole", "Epona in Gerudo Desert", "Epona in Sacred Grove"};
static const int eponaNoRoom[] = {-1, -1, -1, 2};  // F_SP117's room 2 is the Temple of Time
static ConfigVarHandle eponaToggle[EPONA_AREAS];

enum MistMode : int { MIST_OFF, MIST_SEMI, MIST_ON };
static const char* const mistNames[] = {"Off", "Semi-Default", "On"};
static ConfigVarHandle mistOption;

enum ShopMode : int { SHOP_OFF, SHOP_SEMI, SHOP_PARTIAL, SHOP_FULL };
static const char* const shopNames[] = {"Off", "Semi-Default", "Partial-Default", "On"};
static ConfigVarHandle shopOption;
static ConfigVarHandle bridgeJump;

// shop + ice leaf (0 if registering failed)
static ConfigVarHandle vBoost, vAlways, vGoron, vChud, vLeaf;

// ======================== shared ========================

static constexpr u16 kMaloOpen = 0x2210;  // F_0281, Malo Mart open in Castle Town
static constexpr u16 kDonation = 0x0F10;  // M_091, Malo Mart donation paid

// lets our own reads see past the flag hiding
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

// stage file offsets are file-relative until the loader fixes them up
static void* chunk_data(void* file, OffsetPtr& off) {
    u32 raw = (u32)(s32)off.value;
    if ((raw & 0x80000000u) == 0) return (u8*)file + raw;
    s32 rel = (raw & 0x40000000u) ? (s32)raw : (s32)(raw & 0x7FFFFFFFu);
    return (u8*)&off + rel;
}

// a room's actor list etc. by its 4-letter tag, nullptr if the room file doesn't have it
static void* find_chunk(void* roomFile, const char* tag, int& count) {
    auto* file = static_cast<dStage_fileHeader*>(roomFile);
    for (int c = 0; c < file->m_chunkCount; c++) {
        dStage_nodeHeader& node = file->m_nodes[c];
        if (memcmp(&node.m_tag, tag, 4) != 0) continue;
        count = node.m_entryNum;
        return chunk_data(roomFile, node.m_offset);
    }
    count = 0;
    return nullptr;
}

static bool hooked(ModResult r, const char* kind) {
    if (r == MOD_OK) return true;
    mods::log::error("{} hook failed", kind);
    return false;
}
#define pre_hook(H, fn) hooked(mods::hook::add_pre<H>(fn), "pre")
#define post_hook(H, fn) hooked(mods::hook::add_post<H>(fn), "post")

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
    int area = 0;
    while (stage && area < EPONA_AREAS && strcmp(stage, eponaStages[area]) != 0) area++;
    return stage && area < EPONA_AREAS ? area : -1;
}

static bool horse_allowed(int area, int room) {
    return area >= 0 && cfgOn(eponaToggle[area]) && room != eponaNoRoom[area];
}

static int room_now() {
    return dComIfGp_roomControl_getStayNo();
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
static void latch_shop_mode();  // lives with the Chudley's code

// runs on every stage change
static void check_horse_arrival(ModContext*, void* args, void* ret, void*) {
    latch_shop_mode();
    rodeIn = false;
    int dest = horse_area(stage_now());
    int src = horse_area(oldStage);
    if (dest < 0 && src < 0) return;

    dSv_restart_c& restart = g_dComIfG_gameInfo.info.getRestart();
    u32 lastMode = restart.getLastMode();
    u32 kind = lastMode & 0xF;
    if (kind != 1 && kind != 8) return;

    if (dest == src && horse_allowed(dest, dComIfGp_getStartStageRoomNo()))
        rodeIn = true;
    else
        restart.setLastSceneInfo(restart.getLastSpeedF(), lastMode & ~0xFu, restart.getLastAngleY());
}

static void allow_horse_call(ModContext*, void* args, void* ret, void*) {
    u16 item = mods::arg<u16>(args, 0);  // static func, no this
    if (item == dItemNo_HORSE_FLUTE_e && horse_allowed(horse_area(stage_now()), room_now()))
        *(bool*)ret = true;
}

// one candidate spot: nothing between Link and it, dry ground near his height
static bool try_spot(daAlink_c* link, s16 dir, f32 reach, cXyz* out) {
    const cXyz& feet = link->current.pos;
    cXyz eye(feet.x, feet.y + 80.0f, feet.z);
    cXyz spot(feet.x + cM_ssin(dir) * reach, eye.y, feet.z + cM_scos(dir) * reach);
    if (fopAcM_lc_c::lineCheck(&eye, &spot, link)) return false;

    spot.y = feet.y + 150.0f;
    if (!fopAcM_gc_c::gndCheck(&spot) || fopAcM_gc_c::getHorseNoEntry()) return false;
    f32 ground = fopAcM_gc_c::getGroundY();
    if (ground > feet.y + 120.0f || ground < feet.y - 120.0f) return false;

    spot.y = ground;
    if (fopAcM_wt_c::waterCheck(&spot) && fopAcM_wt_c::getWaterY() > ground + 5.0f) return false;
    *out = spot;
    return true;
}

// front first, then fanning out to the sides and behind; a closer ring before a wider one
static bool pick_epona_spot(daAlink_c* link, cXyz* out) {
    static const s16 turns[] = {0, 0x2000, -0x2000, 0x4000, -0x4000, 0x6000, -0x6000, -0x8000};
    for (int tries = 0; tries < 16; tries++) {
        f32 reach = tries < 8 ? 130.0f : 230.0f;
        s16 dir = link->shape_angle.y + turns[tries % 8];
        if (try_spot(link, dir, reach, out)) return true;
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
    bool allowed = horse_allowed(area, room_now());

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
        return true;
    case MIST_SEMI:
        return dComIfGs_isEventBit(0x0602) && dComIfGs_isEventBit(0x1B08);  // temple + lantern monkey
    default:
        return false;
    }
}

// the mist itself; On clears it in the twilight too
static bool faron_mist_off() {
    if (!stage_is("F_SP108") || !mist_gone()) return false;
    return !dKy_darkworld_check() || cfgInt(mistOption, MIST_OFF) == MIST_ON;
}

// the daylight swap never runs in the twilight, its palette has to stay. On also skips the
// stretch between Ordon day 2 and clearing the twilight, when Faron runs on the story's clock
static bool faron_relit() {
    if (!stage_is("F_SP108") || dKy_darkworld_check() || !mist_gone()) return false;
    bool storyStretch = dComIfGs_isEventBit(0x4510) && !dComIfGs_isDarkClearLV(0);
    return cfgInt(mistOption, MIST_OFF) != MIST_ON || !storyStretch;
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
    if (model == nullptr || !mist_room(drawingRoom) || !faron_mist_off()) return;

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
    unpurple_group(args, ret, room, faron_mist_off());
}

static HookAction kill_mist_tag(ModContext*, void* args, void* ret, void*) {
    kytag08_class* tag = mods::arg<kytag08_class*>(args, 0);
    bool clear = faron_mist_off();

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

// swap one room's env row for the old one, pointing its picks at the merged tables
static bool merge_room(const env_tables& live, const env_tables& old, int room, const stage_envr_info_class* coroRoom) {
    if (room >= live.envrNum || room >= old.envrNum) return true;

    auto row = old.envr[room];
    for (int t = 0; t < 65; t++) {
        int src = row.pselect_id[t];
        if (src >= old.pselNum) continue;  // out of range means selection 0, leave it
        int coro = -1;
        if (coroRoom && coroRoom->pselect_id[t] < live.pselNum) coro = coroRoom->pselect_id[t];
        int idx = add_pick(live, old, src, coro);
        if (idx < 0) return false;
        row.pselect_id[t] = (u8)idx;
    }
    envRows[room] = row;
    return true;
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

    return merge_room(live, old, 5, coroRoom) && merge_room(live, old, 11, coroRoom);
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

    if (!inFaron || !faron_relit()) {
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

// --- Lake Hylia jump from the Great Bridge ---

// The twilight version of the bridge has crates to climb and a load zone that drops you into
// Lake Hylia. Once the wagon escort is done the field's normal layer doesn't have them, so they're
// copied out of the twilight layer (14, chunk tags ending in 'e') of whichever field room holds them.
static const int kMaxBridgeBits = 24;
static fpc_ProcID bridgeBits[kMaxBridgeBits];
static s8 bridgeBitRoom[kMaxBridgeBits];
static int bridgeBitCount;
static u64 bridgeRooms;  // rooms we've already copied into
static fpc_ProcID bridgeLink = fpcM_ERROR_PROCESS_ID_e;

static void* room_file(int room) {
    return dComIfG_getStageRes(dComIfG_getRoomArcName(room), "room.dzr");
}

static bool is_crate(s16 proc) {
    return proc == fpcNm_Obj_Movebox_e || proc == fpcNm_OBJ_KBOX_e || proc == fpcNm_OBJ_LBOX_e ||
           proc == fpcNm_Obj_BBox_e;
}

static void spawn_copy(const stage_actor_data_class* rec, const fopAcM_prmScale_class* size, int room, u32 params,
                       csXyz rot) {
    dStage_objectNameInf* info = dStage_searchName(rec->name);
    if (info == nullptr || bridgeBitCount == kMaxBridgeBits) return;

    cXyz pos = rec->base.position;
    cXyz scale(1.0f, 1.0f, 1.0f);
    if (size) scale.set(size->x * 0.1f, size->y * 0.1f, size->z * 0.1f);
    fpc_ProcID id = fopAcM_create(info->procname, params, &pos, room, &rot, &scale, info->argument);
    if (id == fpcM_ERROR_PROCESS_ID_e) return;
    bridgeBitRoom[bridgeBitCount] = (s8)room;
    bridgeBits[bridgeBitCount++] = id;
}

// a twilight-layer record, either a plain actor (ACTe, 0x20 bytes) or one with a size (SCOe, 0x24)
static const stage_actor_data_class* twilight_record(void* list, int i, bool sized) {
    u8* base = static_cast<u8*>(list);
    return reinterpret_cast<const stage_actor_data_class*>(base + i * (sized ? 0x24 : 0x20));
}

// the exit's target, from the room's own scene list
static stage_scls_info_class* lake_exit(void* file, const stage_actor_data_class* rec) {
    s16 proc = -1;
    if (dStage_objectNameInf* info = dStage_searchName(rec->name)) proc = info->procname;
    if (proc != fpcNm_SCENE_EXIT_e && proc != fpcNm_SCENE_EXIT2_e) return nullptr;

    int exits = 0;
    auto* scenes = static_cast<stage_scls_info_class*>(find_chunk(file, "SCLS", exits));
    u32 params = rec->base.parameters;
    int which = params & 0xFF;
    if (scenes == nullptr || which >= exits) return nullptr;
    return strncmp(scenes[which].mStage, "F_SP115", 8) == 0 ? &scenes[which] : nullptr;
}

static void copy_bridge_jump(void* file, int room) {
    static const char* const tags[2] = {"SCOe", "ACTe"};
    cXyz drop[4];
    int drops = 0;

    // the load zones into the lake first
    for (int t = 0; t < 2; t++) {
        int count = 0;
        void* list = find_chunk(file, tags[t], count);
        for (int i = 0; list && i < count; i++) {
            const stage_actor_data_class* rec = twilight_record(list, i, t == 0);
            stage_scls_info_class* exit = lake_exit(file, rec);
            if (exit == nullptr || drops == 4) continue;

            // land in whatever layer the lake would normally load, not the twilight one
            exit->field_0xb |= 0x0F;
            // and drop the twilight-only switch / event conditions
            u32 params = rec->base.parameters | 0xFF000000;
            csXyz rot = rec->base.angle;
            rot.x = rot.z = 0x0FFF;
            const fopAcM_prmScale_class* size = t == 0 ? &((const stage_tgsc_data_class*)rec)->scale : nullptr;
            spawn_copy(rec, size, room, params, rot);
            drop[drops++] = rec->base.position;
        }
    }

    // then the crates that sit near them
    for (int t = 0; t < 2 && drops > 0; t++) {
        int count = 0;
        void* list = find_chunk(file, tags[t], count);
        for (int i = 0; list && i < count; i++) {
            const stage_actor_data_class* rec = twilight_record(list, i, t == 0);
            dStage_objectNameInf* info = dStage_searchName(rec->name);
            if (info == nullptr || !is_crate(info->procname)) continue;

            cXyz at = rec->base.position;
            bool close = false;
            for (int d = 0; d < drops && !close; d++) close = at.abs(drop[d]) < 3000.0f;
            if (!close) continue;

            const fopAcM_prmScale_class* size = t == 0 ? &((const stage_tgsc_data_class*)rec)->scale : nullptr;
            spawn_copy(rec, size, room, rec->base.parameters, rec->base.angle);
        }
    }
}

// rooms come and go as you cross the field; drop our copies with them and redo them on the way back
static void forget_room(int room) {
    int kept = 0;
    for (int i = 0; i < bridgeBitCount; i++) {
        if (bridgeBitRoom[i] == room) {
            fopAc_ac_c* bit = fopAcM_SearchByID(bridgeBits[i]);
            if (bit) fopAcM_delete(bit);
            continue;
        }
        bridgeBitRoom[kept] = bridgeBitRoom[i];
        bridgeBits[kept++] = bridgeBits[i];
    }
    bridgeBitCount = kept;
    bridgeRooms &= ~(1ull << room);
}

static bool bridge_jump_wanted() {
    if (!stage_is("F_SP121") || dKy_darkworld_check() || !cfgOn(bridgeJump)) return false;
    return dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[68]);  // wagon escort done
}

static void bridge_tick() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    fpc_ProcID now = link ? fopAcM_GetID(link) : fpcM_ERROR_PROCESS_ID_e;
    if (now != bridgeLink) {  // new scene, the old copies went with it
        bridgeLink = now;
        bridgeRooms = 0;
        bridgeBitCount = 0;
    }
    if (link == nullptr) return;

    bool wanted = bridge_jump_wanted();
    if (!wanted && bridgeRooms == 0) return;  // nothing to add or take away
    for (int room = 0; room < 64; room++) {
        bool done = (bridgeRooms >> room) & 1;
        void* file = room_file(room);
        if (done && (!wanted || file == nullptr)) forget_room(room);
        if (done || !wanted || file == nullptr) continue;

        bridgeRooms |= 1ull << room;
        copy_bridge_jump(file, room);
    }
}

// ======================== Ice Leaf ========================

DEFINE_HOOK(&daAlink_c::execute, Exec);
DEFINE_HOOK(&daAlink_c::checkNewItemChange, NewItem);
DEFINE_HOOK(&daAlink_c::checkSnowCode, SnowCode);
DEFINE_HOOK(&daAlink_c::setStickData, Stick);
DEFINE_HOOK(&dSv_player_item_c::setLineUpItem, LineUp);
DEFINE_HOOK(&dMeter2Info_c::readItemTexture, ReadTex);
// item-get text box, by name like the two below; arg 1 is the message id
DEFINE_HOOK_SYMBOL("dMsgObject_c::setMessageIndexDemo", void(dMsgObject_c*, u32, bool), IceLeafGetBox);
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
static constexpr u32 leafFanfareText = 0x65 + leafItem;
static const char* getText =
    "An iced over leaf from Snowpeak.\n"
    "Rumored to be a part of what was\n"
    "once known as a \"Korok Leaf\"\n"
    "long ago.\n\n"
    "Equip it to sled across Hyrule\n"
    "on downhill slopes.";

// has zero bytes in the tags, so no strlen
// group 6 = bullet, group 0 = pad buttons (B 11, X 15, Y 16)
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


static fpc_ProcID leafId = fpcM_ERROR_PROCESS_ID_e;
static fpc_ProcID rideId = fpcM_ERROR_PROCESS_ID_e;
static bool mounting, diveSpawn;
static int mountTimer;
static ResourceBuffer iconRes = RESOURCE_BUFFER_INIT;
static void* iconBuf;
static void* iconOwner[32];
static TextureReplacementHandle iconTex[32];
static int nIcons;

static bool got, fell, customMsg, customDesc, wasAlways;
static fpc_ProcID fanfareBox = fpcM_ERROR_PROCESS_ID_e;

// same box the game opens when you pick up an item
static fpc_ProcID show_leaf_get() {
    return fopMsgM_messageSetDemo(leafFanfareText);
}
static int gotPoll;
// true while we ask the game about the spinner
static bool simSpin, wantLeaf;

static bool out() { return leafId != fpcM_ERROR_PROCESS_ID_e; }

static daObjIceLeaf_c* findLeaf() {
    return out() ? static_cast<daObjIceLeaf_c*>(fopAcM_SearchByID(leafId)) : nullptr;
}

// 120 is a guess at head height, shallows don't count as wet
static bool wet(daAlink_c* link) {
    return link->checkModeFlg(daAlink_c::MODE_SWIMMING) || link->mWaterY - link->current.pos.y > 120.0f;
}

static void stow(daAlink_c* link) {
    // 0x103 = sword, still allowed
    if (link->mEquipItem != dItemNo_NONE_e && link->mEquipItem != 0x103) link->allUnequip(TRUE);
}

static void spawn(daAlink_c* link) {
    stow(link);
    cXyz pos = link->current.pos;
    csXyz ang(0, link->shape_angle.y, 0);
    // ride status 1 skips the drop anim
    leafId = fopAcM_create(fpcNm_Obj_IceLeaf_e, 0x0001FFFF, &pos, fopAcM_GetRoomNo(link), &ang, nullptr, -1);
    if (!out()) {
        mods::log::warn("couldn't create the ice leaf");
        return;
    }
    mounting = true;
    mountTimer = 0;
    diveSpawn = wet(link);
}

static void drop(daAlink_c* link) {
    mounting = diveSpawn = false;
    daObjIceLeaf_c* leaf = findLeaf();
    if (leaf) {
        if (link && link->checkBoardRide()) {
            link->setJumpMode();
            link->procBackJumpInit(0);
        }
        // same break fx the leaf plays on its own delete, done here so it never gets skipped
        dComIfGp_particle_set(0x8C12, &leaf->eyePos, NULL, NULL);
        dComIfGp_particle_set(0x8C13, &leaf->eyePos, NULL, NULL);
        if (link) link->seStartOnlyReverb(Z2SE_AL_SNOBO_ICEBREAK);
        fopAcM_delete(leaf);
        if (link) link->mRideAcKeep.clearData();
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
    int slot = 24;
    while (slot--) {
        if (dComIfGs_getItem(slot, true) == leafItem) return true;
    }
    return false;
}

// puts the leaf in its wheel slot unless it's already owned or the slot is taken
static void give_leaf() {
    if (owned() || dComIfGs_getItem(leafSlot, false) != dItemNo_NONE_e) return;
    setLeafSlot(leafItem);
}

static void forgetIcon(void* buf) {
    for (int i = 0; i < nIcons; i++) {
        if (iconOwner[i] != buf) continue;
        svc_texture->unregister(mod_ctx, iconTex[i]);
        nIcons--;
        iconOwner[i] = iconOwner[nIcons];
        iconTex[i] = iconTex[nIcons];
        return;
    }
}

static const u8 itemButton[2] = {daAlink_c::BTN_X, daAlink_c::BTN_Y};

// first button holding the leaf: tap it to get off, or hop on if newitem_pre said we can
static void leaf_buttons(daAlink_c* link, bool want) {
    for (int btn = 0; btn < 2; btn++) {
        if (dComIfGp_getSelectItem(btn) != leafItem) continue;
        if (!out()) {
            if (want && !link->checkEventRun() && !link->checkRide()) spawn(link);
            return;
        }
        if (link->itemTriggerCheck(itemButton[btn])) {
            drop(link);
            return;
        }
    }
}

static HookAction exec_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    // leaf switched off: put it away and take it off the wheel, "hidden" (per save) brings it back later
    if (!cfgOn(vLeaf)) {
        if (out()) drop(link);
        if (dComIfGs_getItem(leafSlot, true) == leafItem && !link->checkEventRun()) {
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
            give_leaf();
        }
    }

    bool always = false;
    if (vAlways) svc_config->get_bool(mod_ctx, vAlways, &always);

    // snowpeak slide ends -> the game's own leaf is deleted while we were on it
    fopAc_ac_c* board = link->mRideAcKeep.getActor();
    bool onGameLeaf = board && fopAcM_GetName(board) == fpcNm_Obj_IceLeaf_e && fopAcM_GetID(board) != leafId;
    if (link->checkBoardRide() && onGameLeaf)
        rideId = fopAcM_GetID(board);
    else if (rideId != fpcM_ERROR_PROCESS_ID_e && !fopAcM_SearchByID(rideId)) {
        rideId = fpcM_ERROR_PROCESS_ID_e;
        fell = true;
    }

    // the snowpeak leaf broke: remember it for this save, prompt every time unless the toggle's on
    if (fell && !link->checkEventRun()) {
        fell = false;
        wrBlob("got", 1);
        got = true;
        bool fresh = !owned();
        if (fresh) give_leaf();
        if (fresh && !always && fanfareBox == fpcM_ERROR_PROCESS_ID_e) {
            fanfareBox = show_leaf_get();
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
        give_leaf();
    }

    // ownership lives in the save's own inventory, the blob only says whether the tree leaf was ever earned.
    // grants happen on the break above or from the toggle, never from the blob alone: it can still be
    // the last save's value for a moment after switching files
    if (always) give_leaf();
    else if (wasAlways && !always && !got && gotPoll > 90 && dComIfGs_getItem(leafSlot, true) == leafItem) {
        if (out()) drop(link);
        setLeafSlot(dItemNo_NONE_e);
    }
    wasAlways = always;
    if (fanfareBox != fpcM_ERROR_PROCESS_ID_e && !fopMsgM_SearchByID(fanfareBox)) fanfareBox = fpcM_ERROR_PROCESS_ID_e;

    // leaf can vanish on its own (room change etc)
    if (out() && !mounting && !findLeaf()) leafId = fpcM_ERROR_PROCESS_ID_e;

    bool heavy = link->checkHeavyStateOn(TRUE, TRUE);
    if (out() && dComIfGp_getSelectItem(0) != leafItem && dComIfGp_getSelectItem(1) != leafItem) drop(link);

    // a dunk only keeps it if it was summoned underwater in iron boots
    if (out() && !mounting && wet(link) && (!diveSpawn || !heavy)) drop(link);

    // summoning is decided in newitem_pre (same rules as the spinner), this only acts on it
    bool want = wantLeaf;
    wantLeaf = false;
    leaf_buttons(link, want);

    if (out()) stow(link);

    if (mounting) {
        daObjIceLeaf_c* leaf = findLeaf();
        if (!out() || ++mountTimer > 300) drop(link);
        else if (leaf && fopAcM_IsExecuting(leafId)) {
            mounting = false;
            link->procBoardWaitInit(leaf);
        }
    }

    return HOOK_CONTINUE;
}

static HookAction newitem_pre(ModContext*, void* args, void* retval, void*) {
    if (simSpin) return HOOK_CONTINUE;
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    u8 btn = mods::arg<u8>(args, 1);
    bool onLeaf = dComIfGp_getSelectItem(btn) == leafItem;
    if (!out() && !onLeaf) return HOOK_CONTINUE;
    if (retval) *(int*)retval = 0;
    if (out() || btn >= 2) return HOOK_SKIP_ORIGINAL;

    // ask the full hook chain (no-restriction mods included) what the spinner would do on this button
    simSpin = true;
    g_dComIfG_gameInfo.play.setSelectItem(btn, dItemNo_SPINNER_e);
    int spinner = link->checkNewItemChange(btn);
    g_dComIfG_gameInfo.play.setSelectItem(btn, leafItem);
    simSpin = false;

    if (spinner != 4) return HOOK_SKIP_ORIGINAL;  // 4 = spinner ready

    // has to run even if we already want the leaf, it's what keeps the icon from dimming
    BOOL pressedNow = link->itemTriggerCheck(itemButton[btn]);
    if (pressedNow) wantLeaf = true;
    return HOOK_SKIP_ORIGINAL;
}

// keep link still while the pickup text is up
static void stick_post(ModContext*, void* args, void*, void*) {
    if (fanfareBox == fpcM_ERROR_PROCESS_ID_e || !fopMsgM_SearchByID(fanfareBox)) return;
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    link->mStickValue = link->mMoveValue = 0.0f;
    link->mItemTrigger = link->mItemButton = 0;
}

static void snow_post(ModContext*, void* args, void* retval, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (out() && retval && link->checkBoardRide()) *(bool*)retval = true;
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
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    // riding: keep the leaf's button lit so it reads as "press to get off"
    if (out())
        for (int btn = 0; btn < 2; btn++)
            if (dComIfGp_getSelectItem(btn) == leafItem) link->field_0x2faf |= (u8)(1 << btn);
    if (!out() || mounting || !link->checkBoardRide() || !mDoCPd_c::getHoldR(PAD_1)) return;

    bool on = false;
    if (vBoost) svc_config->get_bool(mod_ctx, vBoost, &on);
    f32 sp = link->mNormalSpeed + 2.5f;
    if (on) link->mNormalSpeed = sp > 75.0f ? 75.0f : sp;
}

static HookAction msgbegin_pre(ModContext*, void* args, void*, void*) {
    customMsg = mods::arg<u32>(args, 1) == leafFanfareText;
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
    void* texBuf = mods::arg<void*>(args, 2);
    forgetIcon(texBuf);
    u8& wheelItem = mods::arg_ref<u8>(args, 1);
    iconBuf = nullptr;
    if (wheelItem != leafItem) return HOOK_CONTINUE;

    // borrow the boomerang's icon slot, tex_post paints ours over it
    wheelItem = dItemNo_BOOMERANG_e;
    iconBuf = texBuf;
    return HOOK_CONTINUE;
}

static void tex_post(ModContext*, void*, void*, void*) {
    void* texBuf = iconBuf;
    iconBuf = nullptr;
    if (!texBuf || !iconRes.data || iconRes.size != (size_t)iconSz * iconSz * 4) return;

    // where the pixels start; might still be big endian
    uint32_t pixels = ((const ResTIMG*)texBuf)->imageOffset;
    if (pixels > 0xC00)
        pixels = ((pixels >> 24) & 0xFF) | ((pixels >> 8) & 0xFF00) | ((pixels << 8) & 0xFF0000) | (pixels << 24);
    if (pixels == 0 || pixels >= 0xC00) pixels = 0x20;

    if (nIcons >= 32) {
        svc_texture->unregister(mod_ctx, iconTex[0]);
        nIcons--;
        iconOwner[0] = iconOwner[nIcons];
        iconTex[0] = iconTex[nIcons];
    }

    TextureKey key = TEXTURE_KEY_INIT;
    key.kind = TEXTURE_KEY_POINTER;
    key.pointer = (uint8_t*)texBuf + pixels;

    TextureData leafPic = TEXTURE_DATA_INIT;
    leafPic.data = iconRes.data;
    leafPic.size = iconRes.size;
    leafPic.width = iconSz;
    leafPic.height = iconSz;
    leafPic.gx_format = GX_TF_RGBA8_PC;

    TextureReplacementHandle swapped = 0;
    if (svc_texture->register_data(mod_ctx, &key, &leafPic, &swapped) != MOD_OK) return;
    iconOwner[nIcons] = texBuf;
    iconTex[nIcons++] = swapped;
}

// ======================== Castle Town shops ========================
// both of these touch the same shop

// --- Revert to Chudley's ---

DEFINE_HOOK(&dSv_event_c::isEventBit, EventBit);

// the shop interior keeps whatever option it was loaded with, like the game's own layers do;
// changing it while inside only takes effect after leaving and coming back
static int shopModeLoaded = -1;

static void latch_shop_mode() {
    shopModeLoaded = cfgInt(shopOption, SHOP_SEMI);
}

static int shop_mode(bool inside) {
    if (!stage_is(inside ? "R_SP160" : "F_SP116")) return SHOP_OFF;
    if (inside && shopModeLoaded >= 0) return shopModeLoaded;
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

// the tag's own create wrapper, fires before the shopkeeper looks at the shelf
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_tag_shop_item.cpp#daTag_ShopItem_Create", int(void*), ShelfSpawn);

static bool is_shelf_tag(const stage_actor_data_class* actor) {
    return strncmp(actor->name, "TGSPITM", 8) == 0;
}

// Malo Mart swaps a bought Magic Armor for an empty stand (item 0xFF, shown once the armor's
// sold switch is on). Chudley's layer doesn't have one, so put a copy of it where the armor is.
static void add_armor_stand(daTag_ShopItem_c* armor, stage_actor_data_class* maloActors, int count) {
    u8 soldSwitch = (u8)armor->home.angle.z;
    for (int i = 0; i < count; i++) {
        stage_actor_data_class* stand = &maloActors[i];
        u32 params = stand->base.parameters;
        if (!is_shelf_tag(stand) || (u8)params != 0xFF) continue;
        csXyz rot = stand->base.angle;
        if (((u16)rot.z >> 8) != soldSwitch) continue;

        // same shop group and slot as the armor, malo mart's flow and switches
        u32 standParams = (fopAcM_GetParam(armor) & 0xFFFFFF00) | 0xFF;
        csXyz standRot(rot.x, armor->home.angle.y, rot.z);
        fopAcM_create(fpcNm_TAG_SHOPITM_e, standParams, &armor->home.pos, fopAcM_GetRoomNo(armor), &standRot, nullptr, -1);
        return;
    }
}

// use the Malo Mart (ACT1) prompt and stock switches for the same item
static void malo_shelf_prompt(ModContext*, void* args, void* ret, void*) {
    auto* shelf = static_cast<daTag_ShopItem_c*>(mods::arg<void*>(args, 0));
    int mode = shop_mode(true);
    if (mode != SHOP_SEMI && mode != SHOP_PARTIAL) return;

    u8 sells = (u8)fopAcM_GetParam(shelf);  // low byte is the item
    if (sells == 0xFF || !real_flag(kMaloOpen)) return;  // 0xFF = a sold-out spot, ours included

    void* room = dComIfG_getStageRes(dComIfG_getRoomArcName(fopAcM_GetRoomNo(shelf)), "room.dzr");
    if (room == nullptr) return;

    auto* file = static_cast<dStage_fileHeader*>(room);
    int chunk = 0;
    while (chunk < file->m_chunkCount && memcmp(&file->m_nodes[chunk].m_tag, "ACT1", 4) != 0) chunk++;
    if (chunk == file->m_chunkCount) return;

    // ACT1 = the Malo Mart layer's actors
    dStage_nodeHeader* maloLayer = &file->m_nodes[chunk];
    auto* maloActors = static_cast<stage_actor_data_class*>(chunk_data(room, maloLayer->m_offset));
    int count = maloLayer->m_entryNum;
    for (int i = 0; i < count; i++) {
        stage_actor_data_class* actor = &maloActors[i];
        u32 params = actor->base.parameters;
        if (!is_shelf_tag(actor) || (u8)params != sells) continue;

        csXyz rot = actor->base.angle;
        shelf->home.angle.x = rot.x;  // the shelf prompt's flow id lives in angle x
        shelf->home.angle.z = rot.z;  // and its sold-out switches in z
        if (sells == dItemNo_ARMOR_e) add_armor_stand(shelf, maloActors, count);
        return;
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

static dShopSystem_c* activeShop;
static dShopSystem_c* recentShop;
static fpc_ProcID recentShopId = fpcM_ERROR_PROCESS_ID_e;
static bool oilOnPotion;
static u16 jumpTo, noBottleShop, noBottleGoron;
static bool potionPending, potionGiven;
static int potionWait;

// the two "need an empty bottle" lines and the node graph that plays them
static flw::Graph bottleNodes;
static flw::RegisteredMessage bottleLines[2];
static int bottleCount;

// every castle town / common message gets our text hook (5000 + 2500 ids)
static flw::MessageOverride textHooks[7500];
static int textHookCount;

static bool in_shop_room() {
    return stage_is("R_SP160") && dComIfGp_roomControl_getStayNo() == 0;
}

static bool is_arrow(int no) {
    return no == dItemNo_ARROW_10_e || no == dItemNo_ARROW_20_e || no == dItemNo_ARROW_30_e;
}

static bool is_pot(int no) {
    return no == dItemNo_RED_BOTTLE_e || no == dItemNo_BLUE_BOTTLE_e;
}

static int slot_item(dShopSystem_c* shop, int slot) {
    fopAc_ac_c* tag = fopAcM_SearchByID((fpc_ProcID)shop->mItemCtrl.getItemIndex(slot));
    return tag ? (int)(fopAcM_GetParam(tag) & 0xFF) : -1;
}

static int shop_item(dShopSystem_c* shop) {
    int cursor = shop->mCursorPos;
    bool onShelf = cursor > 0;  // 0 is the shopkeeper himself
    onShelf = onShelf && cursor <= dShopSystem_c::ITEM_MAX_e;
    return onShelf ? slot_item(shop, cursor - 1) : -1;
}

static Who shop_who(dShopSystem_c* shop) {
    if (!shop || !cfgOn(vChud) || !in_shop_room() || !is_arrow(shop_item(shop))) return NOBODY;
    return malo_pricing() ? MALO : CHUDLEY;
}

static dShopSystem_c* live_shop() {
    if (recentShop && fopAcM_SearchByID(recentShopId) == (fopAc_ac_c*)recentShop) return recentShop;
    return nullptr;
}

// another mod (the randomizer, with shop items on) can claim the goron's check; then he's left alone
static bool goron_ours() {
    if (!cfgOn(vGoron)) return false;
    ItemCheckResolution goronCheck;
    goronCheck.item = goronCheck.display_item = 0;
    goronCheck.was_resolved = false;
    if (svc_item->resolve_check_full(mod_ctx, kGoronCheck, dItemNo_ARROW_30_e, &goronCheck) != MOD_OK) return true;
    // claimed = swapped for a real item; NONE is just a sync mod marking the check as taken
    return goronCheck.item == dItemNo_ARROW_30_e || goronCheck.item == dItemNo_NONE_e;
}

static Who who(fopAc_ac_c* speaker) {
    if (!speaker) return NOBODY;
    if (stage_is("F_SP116") && fopAcM_GetName(speaker) == fpcNm_NPC_GRA_e) return goron_ours() ? GORON : NOBODY;
    return speaker == (fopAc_ac_c*)activeShop ? shop_who(activeShop) : NOBODY;
}

static u32 price(Who seller) {
    switch (seller) {
    case GORON: return kGoronPrice;
    case MALO: return 15;
    case CHUDLEY: return 12000;
    default: return 0;
    }
}

// The message being rewritten, cut into runs of plain text and control tags. Only one message
// gets rewritten at a time, so it all lives in fixed buffers. Color tags have zero bytes in
// them, so every run keeps its own length instead of relying on the terminator.
static const int kMaxParts = 128;
static const int kPartRoom = 512;

static char partText[kMaxParts][kPartRoom];
static int partLen[kMaxParts];
static bool isTag[kMaxParts];
static int parts;

static char joined[4096];
static int joinedLen;

// replace `cut` bytes at `at` with `with`; false (and untouched) if it wouldn't fit
static bool splice(char* buf, int* len, int at, int cut, const char* with, int withLen) {
    int grown = *len - cut + withLen;
    if (grown >= kPartRoom) return false;
    memmove(buf + at + withLen, buf + at + cut, *len - at - cut);
    memcpy(buf + at, with, withLen);
    *len = grown;
    buf[grown] = 0;
    return true;
}

static int find_in(const char* buf, int len, const char* word, int from, bool anyCase) {
    int wlen = (int)strlen(word);
    for (int at = from; at + wlen <= len; at++) {
        int k = 0;
        while (k < wlen) {
            char a = buf[at + k], b = word[k];
            if (anyCase) a = (char)tolower((unsigned char)a);
            if (a != b) break;
            k++;
        }
        if (k == wlen) return at;
    }
    return -1;
}

static int find_text(const char* buf, int len, const char* word, int from = 0) {
    return find_in(buf, len, word, from, false);
}

// `word` has to be lowercase already
static int find_nocase(const char* buf, int len, const char* word, int from = 0) {
    return find_in(buf, len, word, from, true);
}

static int first_non_space(const char* buf, int len) {
    for (int at = 0; at < len; at++)
        if (buf[at] != ' ') return at;
    return -1;
}

static void swap_all(char* buf, int* len, const char* from, const char* to) {
    int toLen = (int)strlen(to);
    int at = find_text(buf, *len, from);
    while (at >= 0) {
        if (!splice(buf, len, at, (int)strlen(from), to, toLen)) return;
        at = find_text(buf, *len, from, at + toLen);
    }
}

// little writers for building replacement text
static int put_bytes(char* out, int at, const char* bytes, int n) {
    memcpy(out + at, bytes, n);
    return at + n;
}

static int put_words(char* out, int at, const char* words) {
    return put_bytes(out, at, words, (int)strlen(words));
}

static int put_color(char* out, int at, u8 color) {
    const char tag[6] = {0x1A, 0x06, (char)0xFF, 0, 0, (char)color};
    return put_bytes(out, at, tag, 6);
}

// "Blue Potion" in blue, then back to white
static int put_blue_potion(char* out, int at) {
    at = put_color(out, at, MESSAGE_COLOR_BLUE);
    at = put_words(out, at, "Blue Potion");
    return put_color(out, at, 0);
}

static bool new_part(bool tagged) {
    if (parts == kMaxParts) return false;
    isTag[parts] = tagged;
    partLen[parts] = 0;
    partText[parts][0] = 0;
    parts++;
    return true;
}

static void copy_part(int to, int from) {
    memcpy(partText[to], partText[from], partLen[from] + 1);
    partLen[to] = partLen[from];
    isTag[to] = isTag[from];
}

static bool insert_part(int at, const char* bytes, int n) {
    if (parts == kMaxParts || n >= kPartRoom) return false;
    for (int i = parts; i > at; i--) copy_part(i, i - 1);
    memcpy(partText[at], bytes, n);
    partText[at][n] = 0;
    partLen[at] = n;
    isTag[at] = false;
    parts++;
    return true;
}

// drops parts [from, to)
static void erase_parts(int from, int to) {
    int gone = to - from;
    for (int i = from; i + gone < parts; i++) copy_part(i, i + gone);
    parts -= gone;
}

static bool starts_tag(const uint8_t* raw, size_t at, size_t len) {
    return raw[at] == 0x1A && at + 1 < len && raw[at + 1] >= 5;
}

// false if the message doesn't fit the buffers; it's left vanilla then
static bool split_text(const uint8_t* raw, size_t len) {
    parts = 0;
    size_t at = 0;
    while (at < len && raw[at]) {
        if (starts_tag(raw, at, len)) {
            int tagLen = raw[at + 1];
            if (tagLen >= kPartRoom || !new_part(true)) return false;
            memcpy(partText[parts - 1], raw + at, tagLen);
            partLen[parts - 1] = tagLen;
            at += tagLen;
            continue;
        }
        if ((parts == 0 || isTag[parts - 1]) && !new_part(false)) return false;
        int p = parts - 1;
        if (partLen[p] + 1 >= kPartRoom) return false;
        partText[p][partLen[p]++] = (char)raw[at++];
        partText[p][partLen[p]] = 0;
    }
    return true;
}

static bool join_text() {
    joinedLen = 0;
    for (int i = 0; i < parts; ++i) {
        if (joinedLen + partLen[i] >= (int)sizeof(joined)) return false;
        joinedLen = put_bytes(joined, joinedLen, partText[i], partLen[i]);
    }
    joined[joinedLen] = 0;
    return true;
}

static bool is_color(int i) {
    const char* t = partText[i];
    return isTag[i] && partLen[i] == 6 && (u8)t[2] == 0xFF && t[3] == 0 && t[4] == 0;
}

static bool mentions(const char* word) {
    for (int i = 0; i < parts; i++) {
        if (isTag[i]) continue;
        if (find_nocase(partText[i], partLen[i], word) >= 0) return true;
    }
    return false;
}

// --- prices ---

// old price -> new price, 0 keeps the old one
using PriceFn = u32 (*)(u32);

static int skip_spaces_back(const char* buf, int end) {
    while (end > 0 && buf[end - 1] == ' ') end--;
    return end;
}

// digits, or a comma sitting right after a digit ("3,000")
static bool price_char(const char* buf, int at) {
    if (isdigit((unsigned char)buf[at])) return true;
    return buf[at] == ',' && at > 0 && isdigit((unsigned char)buf[at - 1]);
}

static int number_start(const char* buf, int end) {
    int start = end;
    while (start > 0 && price_char(buf, start - 1)) start--;
    return start;
}

static int write_amount(char* out, u32 amount, bool withCommas) {
    char digits[16];
    int n = snprintf(digits, sizeof(digits), "%u", (unsigned)amount);
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (withCommas && i > 0 && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    return o;
}

// rewrites the amount in front of one "Rupee", returns where to keep searching from
static int swap_amount(char* buf, int* len, int hit, PriceFn newPrice) {
    int end = skip_spaces_back(buf, hit);
    int start = number_start(buf, end);
    if (start == end) return hit;

    bool hadComma = false;
    u32 oldAmount = 0;
    for (int i = start; i < end; i++) {
        if (buf[i] == ',') hadComma = true;
        else oldAmount = oldAmount * 10 + (u32)(buf[i] - '0');
    }

    u32 amount = newPrice(oldAmount);
    if (amount == 0) return hit;
    char shown[24];
    int shownLen = write_amount(shown, amount, hadComma || amount >= 10000);
    if (!splice(buf, len, start, end - start, shown, shownLen)) return hit;
    return start + shownLen + (hit - end);
}

static void reprice(char* buf, int* len, PriceFn newPrice) {
    int hit = find_text(buf, *len, "Rupee");
    while (hit >= 0) {
        hit = swap_amount(buf, len, hit, newPrice);
        hit = find_text(buf, *len, "Rupee", hit + 5);
    }
}

// "[purple]3,000[white] Rupees": the number is the end of an earlier piece
static void reprice_split_number(int i, PriceFn newPrice) {
    int hit = find_text(partText[i], partLen[i], "Rupee");
    if (hit < 0 || first_non_space(partText[i], partLen[i]) != hit) return;

    int j = i;
    while (j > 0 && isTag[j - 1]) j--;
    if (j == 0) return;

    char* prev = partText[j - 1];
    int end = skip_spaces_back(prev, partLen[j - 1]);
    int start = number_start(prev, end);
    if (start == end) return;

    // price "<number><spaces> Rupee" on its own, then put just the new number back
    char scratch[kPartRoom];
    int scratchLen = put_bytes(scratch, 0, prev + start, partLen[j - 1] - start);
    scratchLen = put_words(scratch, scratchLen, " Rupee");
    scratch[scratchLen] = 0;
    reprice(scratch, &scratchLen, newPrice);
    int numberLen = find_text(scratch, scratchLen, " ");
    splice(prev, &partLen[j - 1], start, end - start, scratch, numberLen);
}

static void reprice_all(PriceFn newPrice) {
    for (int i = 0; i < parts; i++) {
        if (isTag[i]) continue;
        reprice(partText[i], &partLen[i], newPrice);
        reprice_split_number(i, newPrice);
    }
}

static u32 flatPrice;
static u32 use_flat_price(u32) { return flatPrice; }

static void set_price(u32 amount) {
    flatPrice = amount;
    reprice_all(use_flat_price);
}

// chudley's tags in hd money (x4). the oil sits on the red potion's flow, so its 3,000 lands here too
static u32 chud_hd(u32 old) {
    switch (old) {
    case 2000:
    case 3000: return 12000;
    case 6000: return 24000;
    case 9000: return 36000;
    case 10000: return 40000;
    case 100000: return 400000;
    default: return 0;
    }
}

// --- wrapping, same rule as the randomizer: break at the last space past 17 font cells ---

static JUTFont* wrapFont;
static f32 wrapCell;
static f32 lineW;
static char* breakIn;
static int breakAt;

static f32 glyph_width(char c) {
    if (wrapCell <= 0.0f) return 0.55f;
    return (f32)wrapFont->getWidth((u8)c) / wrapCell;
}

// the "a" in front of the item name shouldn't end up alone on a line
static bool lone_a(const char* buf, int i) {
    if (i == 0 || buf[i - 1] != 'a') return false;
    return i == 1 || buf[i - 2] == ' ' || buf[i - 2] == '\n';
}

static void wrap_piece(int p) {
    char* buf = partText[p];
    for (int i = 0; i < partLen[p]; i++) {
        char c = buf[i];
        if (c == '\n') {
            lineW = 0.0f;
            breakIn = nullptr;
            continue;
        }
        if (c == ' ' && !lone_a(buf, i)) {
            breakIn = buf;
            breakAt = i;
        }
        lineW += glyph_width(c);
        if (lineW <= 17.0f || breakIn == nullptr) continue;

        breakIn[breakAt] = '\n';
        lineW = 0.0f;
        int k = breakIn == buf ? breakAt + 1 : i + 1;  // what's already past the new break
        while (k <= i) lineW += glyph_width(buf[k++]);
        breakIn = nullptr;
    }
}

static void rewrap() {
    wrapFont = mDoExt_getMesgFont();
    wrapCell = wrapFont ? (f32)wrapFont->getCellWidth() : 0.0f;
    lineW = 0.0f;
    breakIn = nullptr;
    breakAt = 0;
    for (int p = 0; p < parts; p++) {
        if (!isTag[p]) wrap_piece(p);
    }
}

// --- goron's lines ---

static void add_article(int at) {
    if (at == 0 || isTag[at - 1]) {
        insert_part(at, "a ", 2);
        return;
    }
    char* before = partText[at - 1];
    int& n = partLen[at - 1];
    while (n > 0 && (before[n - 1] == ' ' || isdigit((unsigned char)before[n - 1]))) n--;
    before[n] = 0;
    const char* article = (n == 0 || before[n - 1] == '\n') ? "a " : " a ";
    splice(before, &n, n, 0, article, (int)strlen(article));
}

static bool word_after_color(int colorAt) {
    if (colorAt + 1 >= parts || !is_color(colorAt)) return false;
    int next = colorAt + 1;
    return !isTag[next] && partLen[next] > 0 && isalnum((unsigned char)partText[next][0]);
}

// colored "Arrows" -> blue "Blue Potion"
static void recolor_arrow_names() {
    for (int i = 0; i < parts; i++) {
        if (!is_color(i) || partText[i][5] == 0) continue;

        // everything up to the next color change
        char inColor[2048];
        int inLen = 0;
        int j = i + 1;
        for (; j < parts && !is_color(j); j++) {
            if (isTag[j] || inLen + partLen[j] >= (int)sizeof(inColor)) continue;
            inLen = put_bytes(inColor, inLen, partText[j], partLen[j]);
        }
        if (find_nocase(inColor, inLen, "arrow") < 0) continue;

        // keep any trailing spaces the old name had
        int last = inLen - 1;
        while (last >= 0 && inColor[last] == ' ') last--;
        int spaces = last < 0 ? 0 : inLen - 1 - last;

        char name[32];
        int nameLen = put_words(name, 0, "Blue Potion");
        while (spaces-- > 0) name[nameLen++] = ' ';

        partText[i][5] = (char)MESSAGE_COLOR_BLUE;
        erase_parts(i + 1, j);
        if (!insert_part(i + 1, name, nameLen)) return;
        bool noSpace = nameLen == 11;
        if (noSpace && word_after_color(i + 2)) splice(partText[i + 3], &partLen[i + 3], 0, 0, " ", 1);
        add_article(i);
        i += 2;
    }
}

// "Arrows" right after a non-color tag gets the blue name too
static void plain_arrow_names() {
    for (int i = 1; i < parts; i++) {
        if (isTag[i] || !isTag[i - 1] || is_color(i - 1)) continue;
        char* buf = partText[i];
        int word = first_non_space(buf, partLen[i]);
        if (word < 0 || find_nocase(buf, partLen[i], "arrow", word) != word) continue;

        int cut = find_nocase(buf, partLen[i], "arrows", word) == word ? word + 6 : word + 5;
        char blue[32];
        int blueLen = put_blue_potion(blue, 0);
        splice(buf, &partLen[i], 0, cut, blue, blueLen);
        erase_parts(i - 1, i);
        add_article(i - 1);
    }
}

static const char* const arrowToPotion[][2] = {
    {"ARROWS", "POTIONS"}, {"Arrows", "Potions"}, {"arrows", "potions"}, {"Arrow", "Potion"}, {"arrow", "potion"},
};

static void potion_words(int p) {
    int at = find_nocase(partText[p], partLen[p], "30 arrows");
    if (at >= 0) {
        char swapIn[32];
        int n = put_words(swapIn, 0, "a ");
        n = put_blue_potion(swapIn, n);
        splice(partText[p], &partLen[p], at, 9, swapIn, n);
    }
    for (auto& words : arrowToPotion) swap_all(partText[p], &partLen[p], words[0], words[1]);
}

static bool goron_text() {
    if (!mentions("arrow")) return false;
    recolor_arrow_names();
    plain_arrow_names();
    for (int p = 0; p < parts; p++)
        if (!isTag[p]) potion_words(p);
    set_price(kGoronPrice);
    rewrap();
    return true;
}

// --- chudley's / malo mart lines ---

static int oil_blurb(char* out, Who seller) {
    int n = put_color(out, 0, MESSAGE_COLOR_YELLOW);
    n = put_words(out, n, "Lantern Oil");
    n = put_color(out, n, 0);
    n = put_words(out, n, " ");
    if (seller == MALO) {
        n = put_color(out, n, MESSAGE_COLOR_RED);
        n = put_words(out, n, "!Nice Deal!");
        n = put_color(out, n, 0);
        n = put_words(out, n, " ");
        n = put_color(out, n, MESSAGE_COLOR_PURPLE);
        n = put_words(out, n, "15 Rupees");
        n = put_color(out, n, 0);
        return put_words(out, n, "\nYou can't live without it!\n(Requires an empty bottle.)");
    }
    n = put_color(out, n, MESSAGE_COLOR_PURPLE);
    n = put_words(out, n, "12,000 Rupees");
    n = put_color(out, n, 0);
    return put_words(out, n, "\nThis liquid sears its way into\nhouseholds worldwide.");
}

// the shelf description: keep the leading tags (minus colors), then our blurb
static void oil_explain(Who seller) {
    int kept = 0;
    for (int i = 0; i < parts && isTag[i]; i++) {
        if (!is_color(i)) copy_part(kept++, i);
    }
    parts = kept;

    char blurb[256];
    int blurbLen = oil_blurb(blurb, seller);
    insert_part(parts, blurb, blurbLen);
}

static const char* const oilCounts[] = {"10 ", "20 ", "30 "};
static const char* const oilNames[] = {"Red Potion", "Blue Potion", "Arrows", "Arrow", "Potion"};
static const char* const oilNamesLow[] = {"red potion", "blue potion", "arrows", "arrow", "potion"};

static void oil_words(int p) {
    char* buf = partText[p];
    int* len = &partLen[p];
    char countName[24];
    for (const char* count : oilCounts) {
        snprintf(countName, sizeof(countName), "%sArrows", count);
        swap_all(buf, len, countName, "Lantern Oil");
        snprintf(countName, sizeof(countName), "%sarrows", count);
        swap_all(buf, len, countName, "lantern oil");
    }
    swap_all(buf, len, " (10)", "");
    swap_all(buf, len, " (20)", "");
    swap_all(buf, len, " (30)", "");
    for (const char* name : oilNames) swap_all(buf, len, name, "Lantern Oil");
    for (const char* name : oilNamesLow) swap_all(buf, len, name, "lantern oil");
}

static bool shop_text(dShopSystem_c* shop, Who seller) {
    if (shop->chkExplainItem() && mentions("rupee")) {
        oil_explain(seller);
        return true;
    }
    if (!mentions("arrow") && !mentions("potion") && !mentions("rupee")) return false;
    for (int p = 0; p < parts; p++) {
        if (isTag[p]) continue;
        oil_words(p);
    }
    set_price(price(seller));
    return true;
}

static bool on_text(ModContext*, const MessageOverrideContext* ctx, MessageTextData* out, void*) {
    if (!ctx->original_text || !out) return false;
    bool goron = ctx->group == kGrp && stage_is("F_SP116");
    dShopSystem_c* shop = goron ? nullptr : live_shop();
    if (!goron && !shop) return false;
    bool chudPrices = shop && in_shop_room() && cfgOn(vChud) && !malo_pricing();

    if (!split_text(ctx->original_text, ctx->original_text_size)) return false;
    bool changed = false;
    if (goron) {
        changed = goron_ours() && goron_text();
    } else {
        Who seller = shop_who(shop);
        if (seller != NOBODY) changed = shop_text(shop, seller);
        if (chudPrices && mentions("rupee")) {
            reprice_all(chud_hd);
            changed = true;
        }
    }
    if (!changed || !join_text()) return false;

    out->text = (const uint8_t*)joined;
    out->text_size = joinedLen + 1;  // with the terminator
    return true;
}

static HookAction shop_pre(ModContext*, void* args, void*, void*) {
    activeShop = mods::arg<dShopSystem_c*>(args, 0);
    recentShop = activeShop;
    recentShopId = fopAcM_GetID(activeShop);
    return HOOK_CONTINUE;
}

static void shop_post(ModContext*, void*, void*, void*) {
    activeShop = nullptr;
}

// The oil prompt hangs off a potion's flow: a red potion if there is one (the last one on the
// shelf), otherwise the first potion. Arrows get collected as a bitmask on the same pass.
static void search_post(ModContext*, void* args, void* found, void*) {
    auto* shop = mods::arg<dShopSystem_c*>(args, 0);
    if (!found || !*static_cast<bool*>(found) || !in_shop_room() || !cfgOn(vChud)) return;

    // walked back to front: the first red seen is the last one on the shelf,
    // and the last potion seen is the first one
    int red = -1, firstPotion = -1;
    u32 arrowSlots = 0;
    int slot = dShopSystem_c::ITEM_MAX_e;
    do {
        --slot;
        int sold = slot_item(shop, slot);
        if (is_arrow(sold)) arrowSlots |= 1u << slot;
        if (is_pot(sold)) firstPotion = slot;
        if (sold == dItemNo_RED_BOTTLE_e && red < 0) red = slot;
    } while (slot > 0);

    int pot = red >= 0 ? red : firstPotion;
    oilOnPotion = pot >= 0;
    if (!oilOnPotion) return;

    u16 flow = shop->mItemCtrl.getMessageIndex(pot);
    for (int slot = 0; arrowSlots != 0; slot++, arrowSlots >>= 1) {
        if (arrowSlots & 1) shop->mItemCtrl.setMessageIndex(slot, flow);
    }
}

static HookAction arc_pre(ModContext*, void* args, void*, void*) {
    auto* shelfItem = mods::arg<daShopItem_c*>(args, 0);
    if (is_arrow(shelfItem->m_itemNo) && in_shop_room() && cfgOn(vChud)) shelfItem->m_itemNo = dItemNo_OIL_BOTTLE_e;
    return HOOK_CONTINUE;
}

static HookAction rupee_pre(ModContext*, void* args, void*, void*) {
    auto* branch = mods::arg<mesg_flow_node_branch*>(args, 1);
    Who seller = who(mods::arg<fopAc_ac_c*>(args, 2));
    if (seller == NOBODY || !branch || branch->param == 0) return HOOK_CONTINUE;

    branch->param = (u16)price(seller);
    bool needsBottle = seller == GORON || !oilOnPotion;
    if (needsBottle && mods::arg<int>(args, 3) != 0 && dComIfGs_checkEmptyBottle() == 0)
        jumpTo = seller == GORON ? noBottleGoron : noBottleShop;
    return HOOK_CONTINUE;
}

static void branch_post(ModContext*, void* args, void*, void*) {
    if (!jumpTo) return;
    u16 node = jumpTo;
    jumpTo = 0;
    mods::arg<dMsgFlow_c*>(args, 0)->setNodeIndex(node, mods::arg<fopAc_ac_c**>(args, 2));
}

// "do you have a bow?" -> no, so he doesn't talk about arrows
static HookAction owned_pre(ModContext*, void* args, void* ret, void*) {
    auto* branch = mods::arg<mesg_flow_node_branch*>(args, 1);
    if (!branch || who(mods::arg<fopAc_ac_c*>(args, 2)) != GORON) return HOOK_CONTINUE;
    if ((branch->param & 0xFF) != dItemNo_BOW_e) return HOOK_CONTINUE;
    if (ret) *static_cast<u16*>(ret) = 0;
    return HOOK_SKIP_ORIGINAL;
}

static HookAction arrows_pre(ModContext*, void* args, void* ret, void*) {
    if (who(mods::arg<fopAc_ac_c*>(args, 2)) != GORON) return HOOK_CONTINUE;
    if (ret) *static_cast<u16*>(ret) = 1;
    return HOOK_SKIP_ORIGINAL;
}

static HookAction fill_pre(ModContext*, void* args, void* ret, void*) {
    if (who(mods::arg<fopAc_ac_c*>(args, 2)) != GORON) return HOOK_CONTINUE;
    potionPending = true;
    potionGiven = false;
    potionWait = 0;
    if (ret) *static_cast<int*>(ret) = 1;
    return HOOK_SKIP_ORIGINAL;
}

// every item the game hands out comes through here; we only care while the goron's potion is owed
static void watch_gifts(ModContext*, const ItemGiveInfo* handedOver, void*) {
    if (!potionPending || !handedOver) return;
    const char* check = handedOver->check_name;
    bool ours = handedOver->item == dItemNo_BLUE_BOTTLE_e || (check && !strcmp(check, kGoronCheck));
    potionGiven = potionGiven || ours;
}

// swap the item inside his "give" event instead of through the shop check: the check is a one-time
// collectible to randomizer/online sync mods ("Already collected"), the potion has to stay repeatable.
// a blue potion isn't his check item, so the game hands it over without touching the check at all
// same for the lantern oil: the shop's give event hands out oil directly, so no check gets resolved
static HookAction start_pre(ModContext*, void* args, void*, void*) {
    auto* event = mods::arg<mesg_flow_node_event*>(args, 1);
    Who seller = who(mods::arg<fopAc_ac_c*>(args, 2));
    if (!event || seller == NOBODY) return HOOK_CONTINUE;
    if (event->params[0] != 0 || event->params[1] != 1 || event->params[2] != 0) return HOOK_CONTINUE;

    u8& item = event->params[3];
    if (seller == GORON && item == dItemNo_ARROW_30_e)
        item = dItemNo_BLUE_BOTTLE_e;
    else if (seller != GORON && (is_arrow(item) || is_pot(item)))
        item = dItemNo_OIL_BOTTLE_e;
    return HOOK_CONTINUE;
}

static HookAction pay_pre(ModContext*, void* args, void*, void*) {
    auto* event = mods::arg<mesg_flow_node_event*>(args, 1);
    Who seller = who(mods::arg<fopAc_ac_c*>(args, 2));
    if (seller == NOBODY || !event) return HOOK_CONTINUE;
    u8* cost = event->params;
    if (!(cost[0] | cost[1] | cost[2] | cost[3])) return HOOK_CONTINUE;

    u32 amount = price(seller);
    cost[0] = (u8)(amount >> 24);
    cost[1] = (u8)(amount >> 16);
    cost[2] = (u8)(amount >> 8);
    cost[3] = (u8)amount;
    return HOOK_CONTINUE;
}

static ModResult reg_msg(const char* txt, u16& id) {
    auto mb = flw::MessageBuilder{flw::MessageStyle{}.trailing_data(0x0400)}.text(txt);
    // same text for every language
    auto en = mb.build(MESSAGE_LANGUAGE_ENGLISH), de = mb.build(MESSAGE_LANGUAGE_GERMAN);
    auto fr = mb.build(MESSAGE_LANGUAGE_FRENCH), es = mb.build(MESSAGE_LANGUAGE_SPANISH);
    auto it = mb.build(MESSAGE_LANGUAGE_ITALIAN), jp = mb.build(MESSAGE_LANGUAGE_JAPANESE);
    if (bottleCount >= 2) return MOD_ERROR;
    flw::RegisteredMessage& line = bottleLines[bottleCount];
    line = flw::register_message(kGrp, {en, de, fr, es, it, jp});
    if (!line) return line.result();
    id = line.id();
    bottleCount++;
    return MOD_OK;
}

// shop side never fails the mod, the leaf has to keep working on its own
static void shop_init() {
    activeShop = recentShop = nullptr;
    jumpTo = 0;
    oilOnPotion = potionPending = false;
    u16 shopMsg, goronMsg;
    if (reg_msg("Sorry, I can only sell this to\ncustomers with empty bottles.\nNext time, maybe.", shopMsg) != MOD_OK ||
        reg_msg("You'll need an empty bottle for\nthis, Brother!", goronMsg) != MOD_OK) {
        mods::log::warn("shop swap: couldn't register messages, shops stay vanilla");
        return;
    }
    {
        flw::GraphBuilder gb{kGrp};
        noBottleShop = gb.add_message(shopMsg).next(flw::kEnd).id();
        noBottleGoron = gb.add_message(goronMsg).next(flw::kEnd).id();
        bottleNodes = gb.commit();
        if (!bottleNodes) {
            mods::log::warn("shop swap: couldn't commit flow nodes, shops stay vanilla");
            return;
        }
    }
    // castle town text, plus the common group where generic shop prompts may live
    textHookCount = 0;
    for (u16 id = 4000; id < 9000; id++)
        textHooks[textHookCount++] = flw::override_message_fn(kGrp, id, MESSAGE_LANGUAGE_ENGLISH, on_text);
    for (u16 id = 0; id < 2500; id++)
        textHooks[textHookCount++] = flw::override_message_fn(0, id, MESSAGE_LANGUAGE_ENGLISH, on_text);
    static ItemGiveHandle giftWatch;  // tells the goron fallback when the potion landed
    svc_item->observe_gives(mod_ctx, watch_gifts, nullptr, &giftWatch);
    // stops at the first one that fails, like before
    bool ok = pre_hook(ShopProc, shop_pre);
    if (ok) ok = post_hook(ShopProc, shop_post);
    if (ok) ok = post_hook(ShopSearch, search_post);
    if (ok) ok = pre_hook(ShopArc, arc_pre);
    if (ok) ok = post_hook(BranchProc, branch_post);
    if (ok) ok = pre_hook(QRupee, rupee_pre);
    if (ok) ok = pre_hook(EPay, pay_pre);
    if (ok) ok = pre_hook(QOwned, owned_pre);
    if (ok) ok = pre_hook(QArrows, arrows_pre);
    if (ok) ok = pre_hook(EFillArrows, fill_pre);
    if (ok) ok = pre_hook(EStart, start_pre);
    if (!ok) mods::log::warn("shop swap: some hooks failed, shops may act vanilla");
}

static void shop_update() {
    if (!potionPending) return;
    if (!stage_is("F_SP116") || potionGiven) {
        potionPending = false;
        return;
    }
    if (dComIfGp_event_runCheck()) {
        potionWait = 0;
        return;
    }
    if (++potionWait > 30) {
        dComIfGs_setEmptyBottleItemIn(dItemNo_BLUE_BOTTLE_e);
        potionPending = false;
    }
}

static void shop_shutdown() {
    while (textHookCount > 0) textHooks[--textHookCount].reset();
    bottleNodes.reset();
    bottleLines[0].reset();
    bottleLines[1].reset();
    bottleCount = 0;
    activeShop = recentShop = nullptr;
}

// ======================== mod page ========================

// plain text lines that sit above each group of toggles

static void subtitle(UiElementHandle panel, const char* line) {
    svc_ui->pane_add_text(mod_ctx, panel, line, nullptr);
}

static bool leaf_off(ModContext*, void*);

static ModResult shop_panel(UiElementHandle panel) {
    subtitle(panel, "Regular Shop Changes");

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

    subtitle(panel, "Ice Leaf");

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

    subtitle(panel, "Epona Locations");
    UiControlDesc t = UI_CONTROL_DESC_INIT;
    t.kind = UI_CONTROL_TOGGLE;
    t.binding = UI_BINDING_CONFIG_VAR;
    for (int i = 0; i < EPONA_AREAS; i++) {
        t.label = eponaLabels[i];
        t.config_var = eponaToggle[i];
        ui->pane_add_control(mod_ctx, panel, &t, nullptr);
    }

    UiControlDesc dd = UI_CONTROL_DESC_INIT;
    dd.kind = UI_CONTROL_DROPDOWN;
    dd.binding = UI_BINDING_CONFIG_VAR;
    subtitle(panel, "Map Checks");

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

    t.label = "Jump to Lake Hylia from Bridge";
    t.config_var = bridgeJump;
    ui->pane_add_control(mod_ctx, panel, &t, nullptr);

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
    for (int i = 0; i < EPONA_AREAS; i++) {
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
    shopModeLoaded = cfgInt(shopOption, SHOP_SEMI);  // whatever room we start in counts as loaded with it

    ConfigVarDesc jump = CONFIG_VAR_DESC_INIT;
    jump.name = "bridgeJump";
    jump.type = CONFIG_VAR_BOOL;
    jump.default_bool = true;
    if (svc_config->register_var(mod_ctx, &jump, &bridgeJump) != MOD_OK)
        return mods::set_error(error, MOD_ERROR, "can't register settings");

    if (!pre_hook(HorseSavePos, keep_field_spot) || !pre_hook(SetStartStage, remember_stage) ||
        !post_hook(SetStartStage, check_horse_arrival))
        return mods::set_error(error, MOD_ERROR, "horse hooks failed");

    if (!post_hook(TownUseItem, allow_horse_call)) mods::log::warn("Horse Call stays blocked in Castle Town");
    if (!pre_hook(HorseSearchEnemy, calm_epona)) mods::log::warn("Epona may still spook at fish");

    int faronFails = 0;
    faronFails += !pre_hook(BgDraw, note_bg_room);
    faronFails += !post_hook(BgDraw, clear_bg_room);
    faronFails += !post_hook(BgMaterialProc, hide_mist_sheet);
    faronFails += !post_hook(FieldMapGroupSwitch, pause_map_switch);
    faronFails += !post_hook(MiniMapGroupSwitch, minimap_switch);
    faronFails += !pre_hook(FogTagExecute, kill_mist_tag);
    if (faronFails) mods::log::warn("Faron mist option won't fully work");

    if (!post_hook(EventBit, hide_malo_flags)) mods::log::warn("Chudley's option off");
    if (!pre_hook(BgObjExec, posters_down)) mods::log::warn("posters will stay up");
    if (!post_hook(ShelfSpawn, malo_shelf_prompt)) mods::log::warn("shelves keep Chudley's prices");

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

    res = mods::hook::add_pre<IceLeafGetBox>(msgbegin_pre);
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
    bridge_tick();
    shop_update();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    put_lighting_back();
    if (out()) drop((daAlink_c*)dComIfGp_getPlayer(0));
    if (dComIfGs_getItem(leafSlot, true) == leafItem) setLeafSlot(dItemNo_NONE_e);
    while (nIcons > 0) svc_texture->unregister(mod_ctx, iconTex[--nIcons]);
    if (iconRes.data) svc_resource->free(mod_ctx, &iconRes);
    shop_shutdown();
    return MOD_OK;
}

}
