// std headers before any game header: aurora defines nullptr as NULL when msvc reports old __cplusplus
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/flow.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/host.h"
#include "mods/svc/http.h"
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
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_bg.h"
#include "d/actor/d_a_bg_obj.h"
#include "d/actor/d_a_boomerang.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_kytag08.h"
#include "d/actor/d_a_nbomb.h"
#include "d/actor/d_a_obj_iceleaf.h"
#include "d/actor/d_a_tag_shop_item.h"
#include "d/d_a_shop_item_static.h"
#include "d/d_bomb.h"
#include "d/d_cc_uty.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_kantera_icon_meter.h"
#include "d/d_kankyo.h"
#include "d/d_cc_d.h"
#include "d/d_map_path_dmap.h"
#include "d/d_menu_fmap_map.h"
#include "d/d_menu_item_explain.h"
#include "d/d_menu_ring.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "d/d_meter_HIO.h"
#include "d/d_msg_class.h"
#include "d/d_msg_flow.h"
#include "d/d_msg_object.h"
#include "d/d_msg_string_base.h"
#include "d/d_pane_class.h"
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
IMPORT_SERVICE_VERSION(HostService, svc_host, 0);
// optional: some platforms have no network, the update check just says it couldn't reach GameBanana
IMPORT_OPTIONAL_SERVICE_VERSION(HttpService, svc_http, 0);

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

// combat
enum FlourishMode : int { FLOURISH_ON, FLOURISH_WOODEN, FLOURISH_DEFAULT, FLOURISH_OFF };
static const char* const flourishNames[] = {"On", "Wooden", "Default", "Off"};
static ConfigVarHandle flourishOption;
static ConfigVarHandle vLanternBurn;  // Deal Burn with Lantern
static ConfigVarHandle vManualShield; // Manual Shielding
static ConfigVarHandle vHandPose;  // Mortal Draw Hand Anim
static ConfigVarHandle vFireDamage;     // Deal Burn Damage (fire arrows)

// items: which bombs can ride the Gale Boomerang (normal, water, bomblings)
static ConfigVarHandle galeBombToggle[3];
static ConfigVarHandle vFireArrows;  // bow + lantern
static ConfigVarHandle vCombos;  // Enable Combinations, over all of the above

// unarmed combat (0 = registering failed, the default is used)
static ConfigVarHandle vShield, vBash, vSkill, vSwing, vDamage;
static ConfigVarHandle vJump, vChargedAll, vSpin, vSpinAll, vEnding, vEndingAll, vHead, vHeadAll, vBackAll;

// Mods page
static ConfigVarHandle vAutoCheck;

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

static bool cfgOn(ConfigVarHandle v, bool fallback = true) {
    bool on = fallback;
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

// Where Link's feet really are. Other mods can move his drawn position between his updates (lifting
// him up to stand him on his head, say), so the height comes from the floor his own ground check
// found this frame instead.
static cXyz link_feet(daAlink_c* link) {
    cXyz feet = link->current.pos;
    if (link->mLinkAcch.ChkGroundHit()) feet.y = link->mLinkAcch.GetGroundH();
    return feet;
}

// one candidate spot: nothing between Link and it, dry ground near his height
static bool try_spot(daAlink_c* link, s16 dir, f32 reach, cXyz* out) {
    const cXyz feet = link_feet(link);
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
        eponaSpot = link_feet(link);
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
static const int kBridgeRoom = 13;  // Hyrule Field room the Great Bridge of Hylia sits in
static const cXyz kBridgeSpot(-93705.0f, -5951.0f, 42867.0f);  // on the bridge, by the twilight crates
static const int kMaxBridgeBits = 40;
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

// `owner` is the room whose unloading takes the copy with it; `room` is what the actor itself gets.
// SCOe records carry a size byte per axis, in tenths
static void spawn_copy(const stage_actor_data_class* rec, bool sized, int room, int owner, u32 params, csXyz rot) {
    dStage_objectNameInf* info = dStage_searchName(rec->name);
    if (info == nullptr || bridgeBitCount == kMaxBridgeBits) return;

    cXyz pos = rec->base.position;
    cXyz scale(1.0f, 1.0f, 1.0f);
    if (sized) {
        const fopAcM_prmScale_class& tenths = ((const stage_tgsc_data_class*)rec)->scale;
        scale.x = tenths.x / 10.0f;
        scale.y = tenths.y / 10.0f;
        scale.z = tenths.z / 10.0f;
    }
    fpc_ProcID id = fopAcM_create(info->procname, params, &pos, room, &rot, &scale, info->argument);
    if (id == fpcM_ERROR_PROCESS_ID_e) return;
    bridgeBitRoom[bridgeBitCount] = (s8)owner;
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

// Three of the twilight crates just block the walkway in normal Hyrule, so they stay in twilight:
// the pair at the near end of the deck and the lone one further up on the left.
static const float kSkipCrateX[2] = {-93621.0f, -93587.0f};
static const float kSkipCrateZ[2] = {43150.0f, 46835.0f};

static bool blocking_crate(const cXyz& at) {
    for (int s = 0; s < 2; s++) {
        float dx = at.x - kSkipCrateX[s];
        float dz = at.z - kSkipCrateZ[s];
        if (dx * dx + dz * dz < 900.0f * 900.0f) return true;
    }
    return false;
}

// `room` is where the copies go; `fromStage` means `file` is stage.dzs, whose crates only count near the bridge
static void copy_bridge_jump(void* file, int room, bool fromStage) {
    static const char* const tags[2] = {"SCOe", "ACTe"};
    cXyz drop[16];
    int drops = 0;

    // the load zones into the lake first
    for (int t = 0; t < 2; t++) {
        int count = 0;
        void* list = find_chunk(file, tags[t], count);
        for (int i = 0; list && i < count; i++) {
            const stage_actor_data_class* rec = twilight_record(list, i, t == 0);
            stage_scls_info_class* exit = lake_exit(file, rec);
            if (exit == nullptr) continue;

            // land in whatever layer the lake would normally load, not the twilight one
            exit->field_0xb |= 0x0F;
            // and drop the twilight-only switch / event conditions
            u32 params = rec->base.parameters | 0xFF000000;
            csXyz rot = rec->base.angle;
            rot.x = rot.z = 0x0FFF;
            spawn_copy(rec, t == 0, fromStage ? -1 : room, room, params, rot);
            if (drops < 16) drop[drops++] = rec->base.position;
        }
    }

    // then the crates (Kkiba in the twilight data): all of them in the bridge's own room,
    // plus any that sit next to a lake load zone found somewhere else
    for (int t = 0; t < 2; t++) {
        int count = 0;
        void* list = find_chunk(file, tags[t], count);
        for (int i = 0; list && i < count; i++) {
            const stage_actor_data_class* rec = twilight_record(list, i, t == 0);
            dStage_objectNameInf* info = dStage_searchName(rec->name);
            if (info == nullptr || !is_crate(info->procname)) continue;

            cXyz at = rec->base.position;
            bool wanted = fromStage ? at.abs(kBridgeSpot) < 6000.0f : room == kBridgeRoom;
            for (int d = 0; d < drops && !wanted; d++) wanted = at.abs(drop[d]) < 3000.0f;
            if (!wanted || blocking_crate(at)) continue;
            spawn_copy(rec, t == 0, room, room, rec->base.parameters, rec->base.angle);
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
        copy_bridge_jump(file, room, false);
        // some of it may live in the stage file instead of the room's; those copies go with the room too
        void* stageFile = room == kBridgeRoom ? dComIfG_getStageRes("stage.dzs") : nullptr;
        if (stageFile) copy_bridge_jump(stageFile, room, true);
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

// Getting off the sled is the game's own backflip, both when we put the sled away and when the
// game itself does (riding off the snow). Other mods can refuse that backflip (a stamina mod with
// an empty bar, say). The game deletes the sled straight after asking for it either way, so Link
// would be left "riding" a sled that's gone: the next time he gets off it for real, the game
// crashes. So whenever a backflip asked for from the sled is refused, Link stands up off the sled
// instead (the game's own stand-still), which gets him off it properly before it goes.

DEFINE_HOOK(&daAlink_c::procBackJumpInit, BackJump);

static bool backflipFromSled;

static HookAction backflip_pre(ModContext*, void* args, void*, void*) {
    backflipFromSled = mods::arg<daAlink_c*>(args, 0)->checkBoardRide();
    return HOOK_CONTINUE;
}

static void backflip_post(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (backflipFromSled && link->checkBoardRide()) link->procWaitInit();  // the backflip was refused
    backflipFromSled = false;
}

static bool get_off_sled(daAlink_c* link) {
    if (!link->checkBoardRide()) return true;
    link->setJumpMode();
    link->procBackJumpInit(0);
    return !link->checkBoardRide();  // still on: leave the sled alone for now
}

static void drop(daAlink_c* link) {
    daObjIceLeaf_c* leaf = findLeaf();
    if (leaf && link && !get_off_sled(link)) return;  // try again next time
    mounting = diveSpawn = false;
    if (leaf) {
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

static bool tapped(daAlink_c* link, int btn) {
    return link->itemTriggerCheck(itemButton[btn]) != FALSE;
}

// first button holding the leaf: tap it to get off, or hop on if newitem_pre said we can
static void leaf_buttons(daAlink_c* link, bool want) {
    for (int btn = 0; btn < 2; btn++) {
        if (dComIfGp_getSelectItem(btn) != leafItem) continue;
        if (!out()) {
            if (want && !link->checkEventRun() && !link->checkRide()) spawn(link);
            return;
        }
        if (!tapped(link, btn)) continue;
        drop(link);
        return;
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
    wantLeaf = tapped(link, btn) || wantLeaf;
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

// Shows a picture of ours wherever the game draws the texture just loaded into texBuf (the
// texture service swaps the pixels when it's drawn). The swap is forgotten the next time
// something else is loaded into that buffer.
static void paint_icon(void* texBuf, const ResourceBuffer& picture, int size) {
    if (!texBuf || !picture.data || picture.size != (size_t)size * size * 4) return;

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

    TextureData ours = TEXTURE_DATA_INIT;
    ours.data = picture.data;
    ours.size = picture.size;
    ours.width = size;
    ours.height = size;
    ours.gx_format = GX_TF_RGBA8_PC;

    TextureReplacementHandle swapped = 0;
    if (svc_texture->register_data(mod_ctx, &key, &ours, &swapped) != MOD_OK) return;
    iconOwner[nIcons] = texBuf;
    iconTex[nIcons++] = swapped;
}

static void tex_post(ModContext*, void*, void*, void*) {
    void* texBuf = iconBuf;
    iconBuf = nullptr;
    paint_icon(texBuf, iconRes, iconSz);
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
static ItemGiveHandle potionWatch;

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
    // 0 is the shopkeeper himself, past the last slot is off the shelf
    if (cursor < 1 || cursor > dShopSystem_c::ITEM_MAX_e) return -1;
    return slot_item(shop, cursor - 1);
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
    ItemCheckResolution answer = {};
    if (svc_item->resolve_check_full(mod_ctx, kGoronCheck, dItemNo_ARROW_30_e, &answer) != MOD_OK) return true;
    // claimed = swapped for a real item; NONE is just a sync mod marking the check as taken
    u32 stocked = answer.item;
    return stocked == dItemNo_ARROW_30_e || stocked == dItemNo_NONE_e;
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
    for (int back = dShopSystem_c::ITEM_MAX_e - 1; back >= 0; back--) {
        int sold = slot_item(shop, back);
        if (is_arrow(sold)) arrowSlots |= 1u << back;
        if (is_pot(sold)) firstPotion = back;
        if (sold == dItemNo_RED_BOTTLE_e && red < 0) red = back;
    }

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

// sees every item handed out, but only listens while the goron owes a potion
static void potion_arrived(ModContext*, const ItemGiveInfo* gave, void*) {
    if (gave == nullptr || !potionPending) return;
    const char* check = gave->check_name;
    bool ours = gave->item == dItemNo_BLUE_BOTTLE_e || (check && !strcmp(check, kGoronCheck));
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
    svc_item->observe_gives(mod_ctx, potion_arrived, nullptr, &potionWatch);
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

// ======================== Sword Flourish ========================

// The flourish is only an option on the A "Put away" press while the kill timer
// (mSwordFlourishTimer) is still running, and the wooden sword is never allowed. Nothing else reads
// that timer, so:
//   On:      the timer is kept full, and the game's own put-away flourishes every time
//   Off:     the timer is kept at zero
//   Wooden:  the game handles the real swords as normal
// The game never lets the Wooden Sword flourish, so for the stick (On, or Wooden after a kill) the
// flourish is started here instead, right before Link updates.

// the game's own put-away gate, minus the timer and the wooden sword
static bool could_put_away(daAlink_c* lnk) {
    if (!mDoCPd_c::getTrigA(PAD_1) || dComIfGp_getDoStatus() != BUTTON_STATUS_PUT_AWAY) return false;
    if (lnk->mEquipItem != 0x103 || !daAlink_c::checkSwordGet() || lnk->checkWolf()) return false;
    if (lnk->checkModeFlg(0x402) || lnk->checkNoResetFlg2(daAlink_c::FLG2_UNK_1)) return false;
    return !lnk->checkCanoeRide() && !lnk->checkEventRun();
}

// runs before Link's own update every frame
static void flourish_before(daAlink_c* lnk) {
    int mode = cfgInt(flourishOption, FLOURISH_DEFAULT);
    if (mode == FLOURISH_OFF) lnk->mSwordFlourishTimer = 0;
    if (mode == FLOURISH_ON) lnk->mSwordFlourishTimer = lnk->mpHIO->mCut.m.mFlourishTime;
    if (mode != FLOURISH_ON && mode != FLOURISH_WOODEN) return;

    // the stick: the game ticks the timer down once before it checks, hence > 1
    if (!daPy_py_c::checkWoodSwordEquip()) return;
    if (mode == FLOURISH_WOODEN && lnk->mSwordFlourishTimer <= 1) return;
    if (could_put_away(lnk)) lnk->procSwordUnequipSpInit();
}

// and after it, so a kill this frame can't sneak one past Off
static void flourish_after(daAlink_c* lnk) {
    if (cfgInt(flourishOption, FLOURISH_DEFAULT) == FLOURISH_OFF) lnk->mSwordFlourishTimer = 0;
}

static HookAction flourish_exec_pre(ModContext*, void* args, void*, void*) {
    flourish_before(mods::arg<daAlink_c*>(args, 0));
    return HOOK_CONTINUE;
}

static void flourish_exec_post(ModContext*, void* args, void*, void*) {
    flourish_after(mods::arg<daAlink_c*>(args, 0));
}

// ======================== Mortal Draw Hand Anim ========================

// When Link could do a mortal draw, he rests his hand on the hilt: the start of his sword-drawing
// animation on his upper body. That pose is part of the game's "ready" state, which also puts Draw
// on A, so turning the toggle off leaves the state alone and only stops the pose from showing.
// setMatrix decides, each frame, how much of the upper-body animation his arms take before his
// body is posed; with the toggle off and that animation being the mortal draw pose, the answer
// becomes none. The mortal draw itself (Draw on A) works as normal.

DEFINE_HOOK(&daAlink_c::setMatrix, SetMatrix);

static void hand_pose_post(ModContext*, void* args, void*, void*) {
    if (cfgOn(vHandPose)) return;
    auto* lnk = mods::arg<daAlink_c*>(args, 0);
    bool ready = lnk->checkNoResetFlg0(daAlink_c::FLG0_UNK_1000000) &&
                 lnk->checkUpperAnime(dRes_ID_ALANM_BCK_WAITATOS_e) && lnk->mEquipItem == dItemNo_NONE_e;
    // the cutscene version of the stance plays out in full
    if (!ready || lnk->mProcID == daAlink_c::PROC_CUT_FAST_READY) return;
    lnk->mNowAnmPackUpper[daAlink_c::UPPER_2].setRatio(0.0f);
    lnk->field_0x3444 = 0.0f;  // what the arms' joints are given while he's being posed
}

// ======================== Gale-Bomb Combo ========================

// R on a bomb bag in the item ring, with the Gale Boomerang on X or Y, ties those bombs to the
// boomerang (the bow's combo stays the game's own). Each throw then hangs one lit bomb off the
// boomerang, the same way it carries a bomb it's flown into, so it goes off on the first thing it
// touches. Which bag is tied on lives in the save, not in the game's own mix slot.

DEFINE_HOOK(&dMenu_Ring_c::_move, RingMove);
DEFINE_HOOK(&dMeter2Draw_c::drawButtonXY, ButtonIcon);

static const u8 kBombItems[3] = {dItemNo_NORMAL_BOMB_e, dItemNo_WATER_BOMB_e, dItemNo_POKE_BOMB_e};

static int bomb_kind(u8 item) {
    for (int k = 0; k < 3; k++)
        if (kBombItems[k] == item) return k;
    return -1;
}

// 0 in the save = nothing tied on, otherwise the bag's slot + 1
static int tied_bag() {
    uint8_t stored = 0;
    size_t n = 1;
    if (svc_save->get_blob(mod_ctx, "galeBombSlot", &stored, &n) != MOD_OK || n != 1) return -1;
    return stored == 0 ? -1 : stored - 1;
}

static void tie_bag(int slot) {
    uint8_t stored = slot < 0 ? 0 : (uint8_t)(slot + 1);
    svc_save->set_blob(mod_ctx, "galeBombSlot", &stored, 1);
}

static int boomerang_button() {
    for (int b = 0; b < 2; b++) {
        u8 slot = dComIfGs_getSelectItemIndex(b);
        if (slot < 24 && dComIfGs_getItem(slot, false) == dItemNo_BOOMERANG_e) return b;
    }
    return -1;
}

// "Enable Combinations" sits over every combo toggle
static bool combos_on() { return cfgOn(vCombos); }
static bool gale_on(int kind) { return kind >= 0 && combos_on() && cfgOn(galeBombToggle[kind]); }
static bool fire_on() { return combos_on() && cfgOn(vFireArrows); }

// the bag that's tied on, still holding bombs of a kind that's switched on; -1 if not
static int armed_bag() {
    int slot = tied_bag();
    if (slot < SLOT_15 || slot > SLOT_17) return -1;
    int kind = bomb_kind(dComIfGs_getItem(slot, false));
    if (!gale_on(kind)) return -1;
    return boomerang_button() >= 0 ? slot : -1;
}

// make the HUD redraw both buttons next frame, so the little bomb appears or goes
static void redraw_buttons() {
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    if (meter == nullptr) return;
    meter->mItemStatus[0] = dItemNo_NONE_e;
    meter->mItemStatus[2] = dItemNo_NONE_e;
}

// --- Fire Arrows: the bow with the lantern, through the game's own combining ---
// The ring only combines the Hawkeye and the bombs with the bow. While the ring decides what R
// does on the lantern, the lantern's slot briefly holds the Hawkeye instead, so the ring's own
// prompt, sound, fly-in and split all happen as normal; it's put back straight after. The save
// then has the lantern's slot on the button with the bow as its mix, and the button acts as the
// plain bow. (Same trick baxlek's Fire Arrows mod uses.)

DEFINE_HOOK(&dMenu_Ring_c::setMixItem, RingMixItem);
DEFINE_HOOK(&dMenu_Ring_c::isMixItemOn, RingCanMix);
DEFINE_HOOK(&dMenu_Ring_c::isMixItemOff, RingCanSplit);
DEFINE_HOOK(&dMenu_Ring_c::setActiveCursor, RingCursor);

static int lanternPosing = -1;  // slot holding the Hawkeye in place of the lantern, -1 if none

static HookAction lantern_poses(ModContext*, void* args, void*, void*) {
    if (!fire_on() || lanternPosing >= 0) return HOOK_CONTINUE;
    auto* ring = mods::arg<dMenu_Ring_c*>(args, 0);
    u8 slot = ring->mItemSlots[ring->mCurrentSlot];
    if (dComIfGs_getItem(slot, false) != dItemNo_KANTERA_e) return HOOK_CONTINUE;
    dComIfGs_setItem(slot, dItemNo_HAWK_EYE_e);
    lanternPosing = slot;
    return HOOK_CONTINUE;
}

static bool lantern_back() {
    if (lanternPosing < 0) return false;
    dComIfGs_setItem(lanternPosing, dItemNo_KANTERA_e);
    lanternPosing = -1;
    return true;
}

static void lantern_back_post(ModContext*, void*, void*, void*) {
    lantern_back();
}

// after combining, the ring drew the flying icon as the Hawkeye; draw it again as the lantern
static void lantern_back_after_mix(ModContext*, void* args, void*, void*) {
    if (lantern_back()) mods::arg<dMenu_Ring_c*>(args, 0)->setJumpItem(false);
}

// the button with fire arrows on it: the bow, with the lantern's slot as its partner
static int fire_button() {
    if (!fire_on()) return -1;
    for (int b = 0; b < 2; b++) {
        if (dComIfGp_getSelectItem(b) != dItemNo_BOW_e) continue;
        if (dComIfGs_getItem(dComIfGs_getSelectItemIndex(b), false) == dItemNo_KANTERA_e) return b;
    }
    return -1;
}

static bool bow_on_a_button() {
    for (int b = 0; b < 2; b++)
        if (dComIfGs_getSelectItemIndex(b) == SLOT_4 || dComIfGs_getMixItemIndex(b) == SLOT_4) return true;
    return false;
}

// --- the item ring, for the Gale-Bomb combo (the boomerang isn't in the game's combining) ---

// what R would do on the highlighted slot: 1 tie on, -1 split, 0 nothing of ours
static int ring_r_action(u8 slot) {
    int kind = bomb_kind(dComIfGs_getItem(slot, false));
    // a bow on X or Y gets the bombs first, like in the normal game
    if (!gale_on(kind) || boomerang_button() < 0 || bow_on_a_button()) return 0;
    return tied_bag() == slot ? -1 : 1;
}

// the item flies from its ring slot onto the button (or back), using the ring's own
// select-item animation; the ring's pending buttons are set to what's already saved so the
// commit at the end of the flight changes nothing
static void fly_item_icon(dMenu_Ring_c* ring, int button, u8 item, bool ontoButton) {
    for (int b = 0; b < 2; b++) {
        ring->field_0x6b4[b] = dComIfGs_getSelectItemIndex(b);
        ring->field_0x6b8[b] = dComIfGs_getMixItemIndex(b);
    }
    ring->field_0x518[button] = ring->mItemSlotPosX[ring->mCurrentSlot];
    ring->field_0x528[button] = ring->mItemSlotPosY[ring->mCurrentSlot];
    ring->field_0x538[button] = g_ringHIO.mSelectItemScale;
    ring->setSelectItem(button, item);
    ring->field_0x6cd = ontoButton ? 0xFF : button;  // anything but 0xFF plays it in reverse
    ring->field_0x674[button] = 1;
}

static bool promptOurs;  // we're the ones showing the combine line in the ring
// the sparkle's speed field is really its running frame; the game zeroes it every frame it isn't
// showing its own line, so ours is carried across from before the ring's update
static f32 sparkleFrame;

static HookAction ring_pre(ModContext*, void* args, void*, void*) {
    sparkleFrame = mods::arg<dMenu_Ring_c*>(args, 0)->mPikariFlashingSpeed;
    return HOOK_CONTINUE;
}

// the ring's "combine" line: shown the same way setMixMessage does it for the bow
static void ring_prompt(dMenu_Ring_c* ring, int change) {
    CPaneMgr* line = ring->mpTextParent[4];
    if (line == nullptr) return;
    if (change == 0) {
        promptOurs = false;  // the game's own setMixMessage already decided this frame
        return;
    }
    for (int i = 0; i < 5; i++) {
        if (change > 0) {
            ring->mpComboOffString[i]->show();
            ring->mpBowArrowComboString[i]->hide();
        } else {
            ring->mpComboOffString[i]->hide();
            ring->mpBowArrowComboString[i]->show();
        }
    }
    if (!promptOurs) {
        mDoAud_seStart(Z2SE_SY_ITEM_COMBINE_ICON, nullptr, 0, 0);
        sparkleFrame = 18.0f - g_ringHIO.mPikariAnimSpeed;  // where the game starts it
    }
    promptOurs = true;
    ring->mPikariFlashingSpeed = sparkleFrame;
    line->show();
}

static void ring_post(ModContext*, void* args, void*, void*) {
    auto* ring = mods::arg<dMenu_Ring_c*>(args, 0);
    bool idle = ring->mStatus == dMenu_Ring_c::STATUS_WAIT && !ring->mPlayerIsWolf &&
                ring->mpItemExplain->getStatus() == 0;
    u8 slot = ring->mItemSlots[ring->mCurrentSlot];
    int change = idle ? ring_r_action(slot) : 0;
    ring_prompt(ring, change);
    if (!idle) return;

    // putting the boomerang on X or Y by itself splits the bombs off, like re-setting the bow does
    bool setButton = mDoCPd_c::getTrigX(PAD_1) || mDoCPd_c::getTrigY(PAD_1);
    if (setButton && tied_bag() >= 0 && dComIfGs_getItem(slot, false) == dItemNo_BOOMERANG_e) {
        tie_bag(-1);
        redraw_buttons();
        return;
    }

    if (change == 0 || !mDoCPd_c::getTrigR(PAD_1)) return;
    bool joining = change > 0;
    tie_bag(joining ? slot : -1);
    mDoAud_seStart(joining ? Z2SE_SY_ITEM_COMBINE_ON : Z2SE_SY_ITEM_COMBINE_OFF, nullptr, 0, 0);
    fly_item_icon(ring, boomerang_button(), dComIfGs_getItem(slot, false), joining);
    redraw_buttons();
}

// --- the combination pictures on X and Y ---

// Each combination has a picture of its own: res/galebomb_icon.rgba (a bomb on the Gale Boomerang,
// used for all three kinds of bomb) and res/firearrow_icon.rgba (the lantern on the bow). Both are
// square, 128 x 128. The button loads its usual single picture (the boomerang or the bow) and ours
// is shown in its place, then the picture is placed like an ordinary square item icon rather than
// with the boomerang's tilt or the bow's long diagonal offset.
static constexpr int comboIconSize = 128;
static ResourceBuffer galeBombIcon = RESOURCE_BUFFER_INIT;
static ResourceBuffer fireArrowIcon = RESOURCE_BUFFER_INIT;

static int galeIconButton = -1;  // button drawing the boomerang with bombs tied on, -1 if none
static int fireIconButton = -1;  // button drawing the fire-arrow bow, -1 if none
static void* comboIconTex;       // the picture just loaded for that button, to paint ours over
static J2DPicture* comboIconPic;
static const ResourceBuffer* comboIcon;

// whether our picture is set to show over that texture buffer
static bool icon_painted(void* buf) {
    for (int i = 0; i < nIcons; i++)
        if (iconOwner[i] == buf) return true;
    return false;
}

static HookAction button_icon_pre(ModContext*, void* args, void*, void*) {
    int button = mods::arg<int>(args, 1);
    u8 shown = mods::arg<u8>(args, 2);
    galeIconButton = shown == dItemNo_BOOMERANG_e && armed_bag() >= 0 ? button : -1;
    fireIconButton = shown == dItemNo_BOW_e && fire_button() == button ? button : -1;
    return HOOK_CONTINUE;
}

// The same sizing and placing the HUD does in changeTextureItemXY and drawButtonXY, done again for
// the picture already on the button as if it were placedAs. Our pictures are square.
static void place_picture(dMeter2Draw_c* hud, int button, u8 placedAs) {
    CPaneMgr* pane = hud->mpItemXY[button];
    ResTIMG* tex = hud->mpItemXYTex[button][hud->field_0x76c[button]][0];
    if (pane == nullptr || tex == nullptr) return;

    f32 size = dItem_data::getTexScale(placedAs) / 100.0f;
    if (g_drawHIO.mItemScaleAdjustON) size = g_drawHIO.mItemScalePercent / 100.0f;
    f32 side = tex->width > tex->height ? tex->width : tex->height;
    f32 wide = size * (side * pane->getInitSizeX() / 48.0f);
    f32 tall = size * (side * pane->getInitSizeY() / 48.0f);
    hud->field_0x6c4[button] = wide;
    hud->field_0x6d0[button] = tall;
    hud->field_0x6ac[button] = (pane->getInitSizeX() - wide) * 0.5f;
    hud->field_0x6b8[button] = (pane->getInitSizeY() - tall) * 0.5f;
    pane->resize(wide, tall);

    if (button == 0) hud->setItemParamX(placedAs);
    else hud->setItemParamY(placedAs);
    const auto& spot = hud->mItemParams[button];
    pane->getPanePtr()->rotate(pane->getSizeX() * 0.5f, pane->getSizeY() * 0.5f, ROTATE_Z, spot.rotation);
    f32 scale = spot.scale * g_drawHIO.field_0x54c;
    pane->scale(scale, scale);
    pane->paneTrans(spot.pos_x + hud->field_0x6ac[button], spot.pos_y + hud->field_0x6b8[button]);
}

// whether each button is currently drawn as its combination; until it is, it keeps being redrawn
static bool comboShown[2];

static void button_icon_post(ModContext*, void* args, void*, void*) {
    auto* hud = mods::arg<dMeter2Draw_c*>(args, 0);
    int button = mods::arg<int>(args, 1);
    bool itemsShown = mods::arg<bool>(args, 4);
    int combo = galeIconButton >= 0 ? galeIconButton : fireIconButton;
    // placed like an ordinary item with no tilt or offset of its own (the bomb arrow is one)
    if (combo >= 0 && itemsShown) place_picture(hud, combo, dItemNo_BOMB_ARROW_e);
    if (itemsShown && button >= 0 && button < 2) comboShown[button] = combo >= 0;
    galeIconButton = fireIconButton = -1;
}

// After a warp or a new area the HUD is built from scratch, sometimes before the item buttons can
// show anything, so a combined button could come back as the plain item. Until each button with a
// combination on it has actually been drawn that way, it's asked to redraw (a few times a second).
static void keep_combos_drawn() {
    static int wait = 0;
    if (dMeter2Info_getMeterClass() == nullptr || daAlink_getAlinkActorClass() == nullptr) return;
    if (wait > 0) {
        wait--;
        return;
    }
    dMeter2Draw_c* hud = dMeter2Info_getMeterClass()->getMeterDrawPtr();
    for (int b = 0; b < 2; b++) {
        bool combo = fire_button() == b || (armed_bag() >= 0 && boomerang_button() == b);
        // drawn as the combination, and our picture is still on the texture it's showing
        bool showing = comboShown[b];
        const ResourceBuffer& picture = fire_button() == b ? fireArrowIcon : galeBombIcon;
        if (showing && hud != nullptr && picture.data != nullptr)
            showing = icon_painted(hud->mpItemXYTex[b][hud->field_0x76c[b]][0]);
        if (combo && !showing) {
            redraw_buttons();
            wait = 10;
            return;
        }
    }
}

// the button's single picture loads as usual; remember where, so ours can go over it
static HookAction combo_tex_pre(ModContext*, void* args, void*, void*) {
    comboIconTex = nullptr;
    u8 item = mods::arg<u8>(args, 1);
    void* front = mods::arg<void*>(args, 2);
    bool gale = galeIconButton >= 0 && item == dItemNo_BOOMERANG_e;
    bool fire = fireIconButton >= 0 && item == dItemNo_BOW_e;
    if (front == nullptr || (!gale && !fire)) return HOOK_CONTINUE;
    comboIconTex = front;
    comboIconPic = mods::arg<J2DPicture*>(args, 3);
    comboIcon = gale ? &galeBombIcon : &fireArrowIcon;
    return HOOK_CONTINUE;
}

static void combo_tex_post(ModContext*, void*, void*, void*) {
    void* tex = comboIconTex;
    comboIconTex = nullptr;
    if (tex == nullptr) return;
    paint_icon(tex, *comboIcon, comboIconSize);
    // The picture took the texture before ours was set over it. Right after loading a new area the
    // screen can hold on to that first look, so the picture is handed the texture once more now
    // that ours is in place, which makes it look again.
    if (comboIconPic != nullptr) comboIconPic->changeTexture(static_cast<const ResTIMG*>(tex), 0);
}

// --- the bomb count, in the same spot the bow shows its arrows ---

DEFINE_HOOK(&dMeter2Draw_c::drawItemNum, ButtonCount);

static int countShown[2] = {-1, -1};  // what we last wrote on each button, -1 = not ours

// the HUD hides the count for the boomerang, so show it while bombs are tied on and keep it current;
// the fire-arrow bow goes the other way and hides its arrow count
static HookAction button_count_pre(ModContext*, void* args, void*, void*) {
    auto* hud = mods::arg<dMeter2Draw_c*>(args, 0);
    u8 button = mods::arg<u8>(args, 1);
    if (button > 1) return HOOK_CONTINUE;

    // the fire-arrow bow shows no arrow count, like the lantern itself (its oil bar is the gauge)
    if (fire_button() == button) {
        countShown[button] = -1;
        mods::arg_ref<f32>(args, 2) = 0.0f;
        return HOOK_CONTINUE;
    }

    dMeter2_c* meter = dMeter2Info_getMeterClass();
    int slot = armed_bag();
    bool ours = meter && slot >= 0 && meter->mItemStatus[button * 2] == dItemNo_BOOMERANG_e;
    if (!ours) {
        countShown[button] = -1;
        return HOOK_CONTINUE;
    }

    int bag = slot - SLOT_15;
    int left = dComIfGs_getBombNum(bag);
    if (left != countShown[button]) {
        hud->setItemNum(button, left, dComIfGs_getBombMax(dComIfGs_getItem(slot, false)));
        countShown[button] = left;
    }
    // same rule the game uses for arrows: visible unless the HUD is tucked away or in a minigame
    bool visible = meter->field_0x128 == 0 && dMeter2Info_getMiniGameItemSetFlag() != 1;
    mods::arg_ref<f32>(args, 2) = visible ? 1.0f : 0.0f;
    return HOOK_CONTINUE;
}

// --- the lantern gauge (its little lantern icon and oil bar) on the fire-arrow bow ---
// The HUD only shows it for the lantern itself; for the bow it's switched on in its usual spot.

DEFINE_HOOK(&dMeter2Draw_c::drawKanteraMeter, ButtonOilGauge);

static HookAction oil_gauge_pre(ModContext*, void* args, void*, void*) {
    u8 button = mods::arg<u8>(args, 1);
    f32& alpha = mods::arg_ref<f32>(args, 2);
    if (alpha != 0.0f || button > 1 || fire_button() != button) return HOOK_CONTINUE;
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    if (meter && meter->field_0x128 == 0 && dMeter2Info_getMiniGameItemSetFlag() != 1) alpha = 1.0f;
    return HOOK_CONTINUE;
}

// --- the fire arrows themselves ---
// An arrow on the fire-arrow bow burns as soon as it's nocked, if there's enough oil for a shot.
// Leaving the bow it takes 1/20 of a full lantern. While it burns it drags its own patch of
// lantern fire along its path (the lantern swing's hit: lights torches and candles, burns webs and
// grass) and lights up what's around it. The flame is the lantern's own, which every area loads.

DEFINE_HOOK(&daArrow_c::execute, ArrowUpdate);

// the lantern swing's fire: no damage of its own, the arrow still does that. 0x1B in the attack
// flags is what switches it on (the same flags Link's sword swing uses); with 0 it never touched
// anything. Torches, candles, webs and grass look for exactly this type with fire as its material.
static const f32 kFireReach = 50.0f;  // the lantern swing's own radius
static const dCcD_SrcCps kArrowFireSrc = {
    {
        {0x0, {{AT_TYPE_LANTERN_SWING, 0x0, 0x1B}, {0x0, 0x0}, 0x0}},
        {dCcD_SE_NONE, 0x0, 0x0, dCcD_MTRL_FIRE, 0x0},
        {dCcD_SE_NONE, 0x0, 0x0, 0x0, 0x0},
        {0x0},
    },
    {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, kFireReach},
    },
};

static const int kArrowMemory = 16;

struct BurningArrow {
    fpc_ProcID id;
    u32 flame[2];
    dCcD_Stts hitOwner;
    dCcD_Cps hit;
    cXyz lastTip;
    cXyz moved;  // how far the head went this frame; the flame's particles move with it
    bool spread;  // already set an enemy on fire; one arrow sets one fire
    bool hitReady;
    LIGHT_INFLUENCE glow;
    bool glowing;
};

static BurningArrow burning[kArrowMemory];
static int burningNext;
static fpc_ProcID judgedArrows[kArrowMemory];  // arrows already checked when they were shot
static int judgedNext;
// arrows this Link nocked himself; any other arrow (another player's in multiplayer, say) is left
// alone, so it never takes oil from this lantern
static fpc_ProcID ownArrows[kArrowMemory];
static int ownNext;

static bool is_own_arrow(fpc_ProcID id) {
    for (int i = 0; i < kArrowMemory; i++)
        if (ownArrows[i] == id) return true;
    return false;
}

static u16 fire_arrow_cost() {
    return dComIfGs_getMaxOil() / 20;
}

static bool can_fire() {
    u16 cost = fire_arrow_cost();
    return fire_button() >= 0 && cost > 0 && dComIfGs_getOil() >= cost;
}

// forget a flame; once nothing sets it again each frame, the game lets it die out
static void let_flames_go(u32* flames, int count) {
    for (int i = 0; i < count; i++) {
        JPABaseEmitter* flame = dComIfGp_particle_getEmitter(flames[i]);
        if (flame != nullptr) flame->setParticleCallBackPtr(nullptr);  // stops following the head
        flames[i] = 0;
    }
}

static void put_out(BurningArrow& fire) {
    let_flames_go(fire.flame, 2);
    if (fire.glowing) dKy_plight_cut(&fire.glow);
    fire.glowing = false;
    fire.hitReady = false;
    fire.id = fpcM_ERROR_PROCESS_ID_e;
}

static int find_burning(fpc_ProcID id) {
    for (int i = 0; i < kArrowMemory; i++)
        if (burning[i].id == id) return i;
    return -1;
}

static int light_arrow(daArrow_c* arrow, const cXyz& tip) {
    int at = burningNext;
    burningNext = (burningNext + 1) % kArrowMemory;
    BurningArrow& fire = burning[at];
    put_out(fire);  // the oldest one makes room
    fire.id = fopAcM_GetID(arrow);
    fire.flame[0] = fire.flame[1] = 0;
    fire.lastTip = tip;
    fire.moved.set(0.0f, 0.0f, 0.0f);
    fire.spread = false;
    fire.hitOwner.Init(0xFF, 0xFF, arrow);
    fire.hit.Set(kArrowFireSrc);
    fire.hit.SetStts(&fire.hitOwner);
    fire.hitReady = true;

    // a warm orange glow, about as strong as the lantern's
    fire.glow.mPosition = tip;
    fire.glow.mColor.r = 255;
    fire.glow.mColor.g = 140;
    fire.glow.mColor.b = 60;
    fire.glow.mPow = 400.0f;
    fire.glow.mFluctuation = 0.0f;
    dKy_plight_set(&fire.glow);
    fire.glowing = true;
    return at;
}

static bool was_judged(fpc_ProcID id) {
    for (int i = 0; i < kArrowMemory; i++)
        if (judgedArrows[i] == id) return true;
    return false;
}

// --- enemies and players set on fire ---
// An enemy a burning arrow sticks in (or whose fire passes close by it) burns for a few seconds:
// the lantern's flame on its body, and two scorches of fire damage, each about an Ordon Sword hit.
// The scorch is the lantern's fire hit with some power, so enemies take it like one of Link's own
// hits. Stepping into water puts it out. With Deal Burn with Lantern on, a lantern swing that
// reaches an enemy does the same.
// Who's close enough is measured here rather than waiting for the game to report a hit, since a
// lot of enemies never report being touched by the lantern's kind of fire.
//
// In online multiplayer, other players are a stand-in actor (named "cpRival", counted as an enemy)
// that passes any hit on to that player's game, fire included. So a player gets no big flame:
// they get fire hits instead, and their own game shows Link catching fire. A hit they block isn't
// passed on as one (only the block sound and push-back are), so their shield can't be set alight
// from here.

static const int kBurnFrames = 120;
static const int kScorchAt[2] = {100, 50};  // frames left when each scorch lands
static const int kMaxBurning = 8;

// The scorch hits as a sword would, with fire as its material. A lot of enemies (Bulblins among
// them) ignore the lantern's own kind of hit entirely, but every enemy takes a sword hit.
static const dCcD_SrcCps kScorchSrc = {
    {
        {0x0, {{AT_TYPE_NORMAL_SWORD, 0x2, 0x1B}, {0x0, 0x0}, 0x0}},
        {dCcD_SE_NONE, 0x0, 0x0, dCcD_MTRL_FIRE, 0x0},
        {dCcD_SE_NONE, 0x0, 0x0, 0x0, 0x0},
        {0x0},
    },
    {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 60.0f},
    },
};

// For another player the fire hit is the lantern's own kind, with the scorch's power: players
// burned from that kind of hit, and stopped burning when it was switched to the sword kind the
// enemies' scorch now uses.
static const dCcD_SrcCps kPlayerFireSrc = {
    {
        {0x0, {{AT_TYPE_LANTERN_SWING, 0x2, 0x1B}, {0x0, 0x0}, 0x0}},
        {dCcD_SE_NONE, 0x0, 0x0, dCcD_MTRL_FIRE, 0x0},
        {dCcD_SE_NONE, 0x0, 0x0, 0x0, 0x0},
        {0x0},
    },
    {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 60.0f},
    },
};

// the stand-in actor online multiplayer uses for another player, if it's loaded
static bool is_other_player(fopAc_ac_c* actor) {
    dStage_objectNameInf* rival = dStage_searchName("cpRival");
    return rival != nullptr && fopAcM_GetName(actor) == rival->procname;
}

// Players burn on the same clock as enemies, just without the big flame (their own game shows
// Link on fire): a fire hit as they catch fire, then two more while they burn. Each one is passed
// on to their game as a fire hit, so it hurts a bit each time and keeps them burning.
static const int kPlayerFireAt[3] = {kBurnFrames - 1, 80, 40};  // frames left when each one lands

struct BurningEnemy {
    fpc_ProcID id;
    int framesLeft;
    bool player;     // another player in online multiplayer
    bool fromArrow;  // a fire arrow lit it (Deal Burn Damage applies), not a lantern swing
    u32 flame[2];
    dCcD_Stts scorchOwner;
    dCcD_Cps scorch;
};

static BurningEnemy onFire[kMaxBurning];

static void stop_burning(BurningEnemy& enemy) {
    let_flames_go(enemy.flame, 2);
    enemy.framesLeft = 0;
    enemy.id = fpcM_ERROR_PROCESS_ID_e;
}

// standing in water deep enough to cover its feet
static bool in_water(fopAc_ac_c* actor) {
    cXyz feet = actor->current.pos;
    return fopAcM_wt_c::waterCheck(&feet) && fopAcM_wt_c::getWaterY() > feet.y + 10.0f;
}

// the top of an actor's body: its eyes, or Link's height above its feet if it never set them
// (the stand-ins for other players in online multiplayer, for one)
static cXyz body_top(fopAc_ac_c* actor) {
    cXyz up = actor->eyePos - actor->current.pos;
    if (up.abs2() < 1.0f || up.abs2() > 600.0f * 600.0f) up.set(0.0f, 140.0f, 0.0f);
    return actor->current.pos + up;
}

static bool lightingFromLantern;  // set_on_fire is being asked by a lantern swing, not an arrow

// true if it's an enemy (or another player) that's burning now
static bool set_on_fire(fopAc_ac_c* actor) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (actor == nullptr || link == nullptr || fopAcM_GetGroup(actor) != fopAc_ENEMY_e) return false;
    if (in_water(actor)) return false;  // nothing catches fire standing or swimming in water
    fpc_ProcID id = fopAcM_GetID(actor);

    // already burning: just keeps burning
    BurningEnemy* spot = nullptr;
    for (int i = 0; i < kMaxBurning; i++) {
        if (onFire[i].framesLeft > 0 && onFire[i].id == id) return true;
        if (spot == nullptr && onFire[i].framesLeft <= 0) spot = &onFire[i];
    }
    if (spot == nullptr) return false;  // eight at once is plenty

    spot->id = id;
    spot->framesLeft = kBurnFrames;
    spot->player = is_other_player(actor);
    spot->fromArrow = !lightingFromLantern;
    spot->flame[0] = spot->flame[1] = 0;
    spot->scorchOwner.Init(0xFF, 0xFF, link);  // counts as Link's hit
    spot->scorch.Set(spot->player ? kPlayerFireSrc : kScorchSrc);
    spot->scorch.SetStts(&spot->scorchOwner);
    return true;
}

// Deal Burn Damage off: a fire arrow still sets things alight (the flame on an enemy, and on a
// player the first fire hit, which is what sets their Link on fire), but the burn does no damage
// after that
static bool hit_due(const BurningEnemy& enemy) {
    bool burnDamage = !enemy.fromArrow || cfgOn(vFireDamage);
    if (!burnDamage) return enemy.player && enemy.framesLeft == kPlayerFireAt[0];
    if (enemy.player) {
        for (int at : kPlayerFireAt)
            if (enemy.framesLeft == at) return true;
        return false;
    }
    return enemy.framesLeft == kScorchAt[0] || enemy.framesLeft == kScorchAt[1];
}

// runs with Link's update, so the scorch is in time for this frame's hits
static void burn_enemies() {
    static const u16 kLanternFlame[2] = {0x3AF, 0x3AD};
    for (int i = 0; i < kMaxBurning; i++) {
        BurningEnemy& enemy = onFire[i];
        if (enemy.framesLeft <= 0) continue;
        fopAc_ac_c* actor = fopAcM_SearchByID(enemy.id);
        if (actor == nullptr || --enemy.framesLeft == 0 || in_water(actor)) {
            stop_burning(enemy);
            continue;
        }

        // the big flame is for enemies; a player's own game already shows their Link on fire
        if (!enemy.player) {
            // halfway between its feet and its eyes is roughly its middle
            cXyz middle = (actor->current.pos + body_top(actor)) * 0.5f;
            for (int k = 0; k < 2; k++) {
                enemy.flame[k] = dComIfGp_particle_set(enemy.flame[k], kLanternFlame[k], &middle, &actor->shape_angle, nullptr);
                JPABaseEmitter* flame = dComIfGp_particle_getEmitter(enemy.flame[k]);
                f32 size = k == 0 ? 1.5f : 0.75f;
                if (flame != nullptr) flame->setGlobalScale(JGeometry::TVec3<f32>(size, size, size));
            }
        }

        if (!hit_due(enemy)) continue;
        static_cast<cM3dGCps*>(&enemy.scorch)->Set(actor->current.pos, body_top(actor), 60.0f);
        enemy.scorch.CalcAtVec();
        enemy.scorch.ResetAtHit();
        dComIfG_Ccsp()->Set(&enemy.scorch);
    }
}

// how close a point is to the line from an actor's feet to the top of its body
static f32 distance_to_body(const cXyz& at, fopAc_ac_c* actor) {
    cXyz feet = actor->current.pos;
    cXyz up = body_top(actor) - feet;
    f32 tall = up.abs2();
    f32 along = tall > 1.0f ? (at - feet).inprod(up) / tall : 0.0f;
    if (along < 0.0f) along = 0.0f;
    if (along > 1.0f) along = 1.0f;
    cXyz nearest = feet + up * along;
    return at.abs(nearest);
}

struct FireNearby {
    cXyz at;
    f32 reach;
    bool lit;
};

// called for every actor in the scene; never stops the search early
static void* catch_fire_nearby(void* actor, void* data) {
    auto* ac = static_cast<fopAc_ac_c*>(actor);
    auto* fire = static_cast<FireNearby*>(data);
    if (fopAcM_GetGroup(ac) != fopAc_ENEMY_e) return nullptr;
    if (distance_to_body(fire->at, ac) > fire->reach) return nullptr;
    if (set_on_fire(ac)) fire->lit = true;
    return nullptr;
}

static bool set_fire_around(const cXyz& at, f32 reach) {
    FireNearby fire = {at, reach, false};
    fopAcIt_Judge(catch_fire_nearby, &fire);
    return fire.lit;
}

// Deal Burn with Lantern: the swing's fire is live only for part of the swing, with oil
static void lantern_swing_burns(daAlink_c* link) {
    if (!cfgOn(vLanternBurn) || link->mProcID != daAlink_c::PROC_KANDELAAR_SWING) return;
    if (!link->mAtSph.ChkAtSet()) return;
    lightingFromLantern = true;
    set_fire_around(link->mKandelaarFlamePos, link->mAtSph.GetR() + 30.0f);
    lightingFromLantern = false;
}

static void burn_post(ModContext*, void* args, void*, void*) {
    lantern_swing_burns(mods::arg<daAlink_c*>(args, 0));
    burn_enemies();
}

static void arrow_post(ModContext*, void* args, void*, void*) {
    auto* arrow = mods::arg<daArrow_c*>(args, 0);
    if (arrow->mArrowType != 0) return;  // bomb, light and slingshot shots stay as they are
    fpc_ProcID id = fopAcM_GetID(arrow);
    // Which state it's in is read off Link and the arrow itself, not by comparing its proc against
    // the game's function addresses: on Windows a mod only sees a jump stub for those, so the
    // comparison never matched there and the arrows never caught fire.
    daAlink_c* link = daAlink_getAlinkActorClass();
    bool nocked = link != nullptr && link->mItemAcKeep.getID() == id;
    // Flying means it actually moved this frame: an arrow that's stuck keeps its last speed, so
    // its speed alone made a grounded arrow look like it was still in the air (and the flame was
    // put an arrow's length ahead of it).
    bool flying = !nocked && (arrow->current.pos - arrow->old.pos).abs2() > 4.0f;
    if (nocked && !is_own_arrow(id)) {
        ownArrows[ownNext] = id;
        ownNext = (ownNext + 1) % kArrowMemory;
    }
    if (!is_own_arrow(id)) return;

    // The arrow's position is its back end; the head is 94 units along the shaft, the same spot
    // the game hangs a bomb arrow's fuse on. On the bow the model already sits where it's drawn.
    // In flight the model is only placed when it's drawn, a frame behind a very fast arrow, so the
    // head is worked out from where the arrow is right now instead.
    // Once it stops (stuck in the ground, a wall or an enemy), the game moves its position right
    // onto the point it hit, which is where the head is.
    static cXyz headOnModel(-3.3f, -3.0f, 94.0f);
    if (arrow->mpModel == nullptr) return;
    cXyz tip;
    if (flying) {
        mDoMtx_stack_c::transS(arrow->current.pos);
        mDoMtx_stack_c::ZXYrotM(arrow->shape_angle.x, arrow->shape_angle.y, 0);
        mDoMtx_stack_c::multVec(&headOnModel, &tip);
    } else if (nocked) {
        mDoMtx_multVec(arrow->mpModel->getBaseTRMtx(), &headOnModel, &tip);
    } else {
        tip = arrow->current.pos;
    }

    int fire = find_burning(id);
    // nocked: burns while there's oil for a shot, goes out if there isn't
    if (nocked) {
        if (fire < 0 && can_fire()) fire = light_arrow(arrow, tip);
        if (fire >= 0 && !can_fire()) {
            put_out(burning[fire]);
            fire = -1;
        }
    }
    // the shot: pays for the flame (or lights one it didn't have yet), once
    if (flying && !was_judged(id)) {
        judgedArrows[judgedNext] = id;
        judgedNext = (judgedNext + 1) % kArrowMemory;
        if (fire < 0 && can_fire()) fire = light_arrow(arrow, tip);
        if (fire >= 0) dComIfGp_setItemOilCount(-fire_arrow_cost());
    }
    if (fire < 0) return;
    BurningArrow& f = burning[fire];

    // In flight the fire reaches from where the head was last frame to a little past where it'll
    // be next frame, so whatever is in front of the arrow catches fire before (or as) the arrow
    // gets there, not after. Every enemy or player it reaches burns, each once while it burns.
    cXyz ahead = tip;
    if (flying) ahead = tip + (tip - f.lastTip) * 1.5f;
    if (flying) {
        for (int step = 0; step <= 4; step++) {
            cXyz along = f.lastTip + (ahead - f.lastTip) * (step / 4.0f);
            set_fire_around(along, kFireReach + 20.0f);
        }
    }
    // Whatever the arrow itself hits burns too (another player, say, that the fire somehow missed).
    if (!nocked && arrow->field_0x688.ChkAtHit()) set_on_fire(arrow->field_0x688.GetAtHitAc());
    // Stuck in an enemy: that enemy burns, once. (A shield block doesn't count.)
    if (!nocked && !flying && !f.spread) {
        u32 stuckIn = arrow->getHitAcID();
        if (stuckIn != 0xFFFFFFFFu && arrow->field_0x93e == 0)
            f.spread = set_on_fire(fopAcM_SearchByID(stuckIn));
    }

    // the lantern's fire hit for torches, candles, webs and grass, over the same stretch, and on
    // the head once it's stuck
    if (!nocked && f.hitReady) {
        static_cast<cM3dGCps*>(&f.hit)->Set(f.lastTip, ahead, kFireReach);
        f.hit.CalcAtVec();
        dComIfG_Ccsp()->Set(&f.hit);
    }
    f.moved = flying ? tip - f.lastTip : cXyz(0.0f, 0.0f, 0.0f);
    f.lastTip = tip;
    f.glow.mPosition = tip;

    // The lantern's flame, the same one a burning enemy gets, only smaller: a third of the enemy's
    // size once it's loose, and smaller again while it sits on the bow so it doesn't swallow
    // Link's hand.
    static const u16 kLanternFlame[2] = {0x3AF, 0x3AD};
    static const f32 kFlameSize[2] = {0.5f, 0.25f};  // the burning enemy's are 1.5 and 0.75
    f32 size = nocked ? 0.5f : 1.0f;
    for (int i = 0; i < 2; i++) {
        f.flame[i] = dComIfGp_particle_set(f.flame[i], kLanternFlame[i], &tip, &arrow->shape_angle, nullptr);
        JPABaseEmitter* flame = dComIfGp_particle_getEmitter(f.flame[i]);
        if (flame == nullptr) continue;
        f32 s = size * kFlameSize[i];
        flame->setGlobalScale(JGeometry::TVec3<f32>(s, s, s));
        // Flame already in the air moves along with the head each frame (the game's own trick for
        // the smoke off a bomb arrow's fuse), so the fire stays on the tip instead of trailing.
        flame->setParticleCallBackPtr(dPa_control_c::getParticleTracePCB());
        flame->setUserWork((uintptr_t)&f.moved);
    }
}

static int fireButtonWas = -2;  // -2 until the first look

// once a frame
static void fire_arrow_tick() {
    keep_combos_drawn();
    // Combining the lantern onto the bow (or splitting it off) doesn't change the item the HUD
    // thinks is on the button, it's the bow either way, so the HUD wouldn't redraw it by itself.
    int button = fire_button();
    if (button != fireButtonWas && dMeter2Info_getMeterClass() != nullptr) {
        redraw_buttons();
        fireButtonWas = button;
    }

    // arrows that are gone take their light with them
    for (int i = 0; i < kArrowMemory; i++) {
        if (burning[i].id == fpcM_ERROR_PROCESS_ID_e || burning[i].id == 0) continue;
        if (fopAcM_SearchByID(burning[i].id) == nullptr) put_out(burning[i]);
    }
}

static void fire_arrow_shutdown() {
    for (int i = 0; i < kMaxBurning; i++) stop_burning(onFire[i]);
    for (int i = 0; i < kArrowMemory; i++) put_out(burning[i]);
}

// --- the throw ---

static fpc_ProcID lastBoomerang = fpcM_ERROR_PROCESS_ID_e;

// Bomblings ride as the Forest Temple kind (E_BI): the game already lets the boomerang carry those,
// and its own bomblings blow up the moment they're pulled off the ground. Their art only ships with
// a few stages, so it's loaded by hand while bomblings are tied on.
static request_of_phase_process_class bomblingArt;
static bool bomblingArtHeld;

static bool bombling_art_ready() {
    bomblingArtHeld = true;
    return dComIfG_resLoad(&bomblingArt, "E_BI") == cPhs_COMPLEATE_e;
}

static void drop_bombling_art() {
    if (!bomblingArtHeld) return;
    // a load still in flight can't be cancelled, finish it first and drop it after
    if (dComIfG_resLoad(&bomblingArt, "E_BI") != cPhs_COMPLEATE_e) return;
    dComIfG_resDelete(&bomblingArt, "E_BI");
    bomblingArtHeld = false;
}

static fopAc_ac_c* make_bomb(int kind, cXyz* at) {
    if (kind == 0) return dBomb_c::createNormalBombPlayer(at);
    if (kind == 1) return dBomb_c::createWaterBombPlayer(at);
    if (!bombling_art_ready()) return nullptr;
    // spawns already hanging off the boomerang, the param the temple's bomblings get
    return (fopAc_ac_c*)fopAcM_fastCreate(fpcNm_NBOMB_e, dBomb_c::PRM_ENEMY_BOMB_BOOMERANG, at, -1, nullptr,
                                          nullptr, -1, nullptr, nullptr);
}

static void hang_bomb(daAlink_c* link) {
    daBoomerang_c* boom = daPy_py_c::getThrowBoomerangActor();
    fpc_ProcID flying = boom ? fopAcM_GetID(boom) : fpcM_ERROR_PROCESS_ID_e;
    if (flying == lastBoomerang) return;
    lastBoomerang = flying;  // one bomb per throw, checked the frame it leaves his hand
    if (boom == nullptr) return;

    int slot = armed_bag();
    if (slot < 0) return;
    int bag = slot - SLOT_15;
    int kind = bomb_kind(dComIfGs_getItem(slot, false));
    if (dComIfGs_getBombNum(bag) + dComIfGp_getItemBombNumCount(bag) <= 0) return;
    if (kind != 2 && link->mActiveBombNum >= 3) return;  // same cap as pulling one out

    cXyz under = boom->current.pos;
    under.y -= 30.0f;
    fopAc_ac_c* bomb = make_bomb(kind, &under);
    if (bomb == nullptr) return;
    dComIfGp_setItemBombNumCount(bag, -1);
    if (kind == 2) return;  // the temple bombling set itself up to ride

    // count it like Link pulled it out himself, so its explosion frees the spot again
    link->mActiveBombNum++;
    static_cast<daNbomb_c*>(bomb)->procBoomerangMoveInit(nullptr);
}

static void galebomb_exec_post(ModContext*, void* args, void*, void*) {
    hang_bomb(mods::arg<daAlink_c*>(args, 0));
}

// A new HUD (after a warp, a load or a stage change) starts blank: nothing on it is drawn as a
// combination yet, and the bomb count needs writing again.
DEFINE_HOOK(&dMeter2_c::_create, HudCreate);

static void hud_created(ModContext*, void*, void*, void*) {
    comboShown[0] = comboShown[1] = false;
    countShown[0] = countShown[1] = -1;
}

// once a frame: untie when the bombs run out, the boomerang leaves both buttons or the kind is switched off
static void galebomb_tick() {
    static int wasArmed = -2;
    int armed = armed_bag();
    // only untie with Link actually in play; mid-load the buttons can read empty for a moment
    bool inPlay = daAlink_getAlinkActorClass() != nullptr;
    if (inPlay && armed < 0 && tied_bag() >= 0) tie_bag(-1);  // armed_bag says why: empty, off or no boomerang

    // have the temple bombling art ready before the throw, not on it
    bool bomblings = armed >= 0 && bomb_kind(dComIfGs_getItem(armed, false)) == 2;
    if (bomblings) bombling_art_ready();
    else drop_bombling_art();

    if (armed != wasArmed) redraw_buttons();
    wasArmed = armed;
}

// ======================== Unarmed Combat ========================

DEFINE_HOOK(&daAlink_c::setShieldGuard, SetShieldGuard);
DEFINE_HOOK(&daAlink_c::checkItemAction, CheckItemAction);
DEFINE_HOOK(&daAlink_c::decideCommonDoStatus, DoStatus);
DEFINE_HOOK(&daAlink_c::checkNextAction, NextAction);
DEFINE_HOOK(&cc_at_check, CcAtCheck);
DEFINE_HOOK(&daAlink_c::checkCutAction, CutAction);
DEFINE_HOOK(&daAlink_c::setSwordAtCollision, SwordHit);
DEFINE_HOOK(&daAlink_c::procCutDownInit, DownInit);
DEFINE_HOOK(&daAlink_c::procCutDown, DownAir);
DEFINE_HOOK(&daAlink_c::procCutDownLand, DownLand);
DEFINE_HOOK(&daAlink_c::checkDownAttackState, DownState);
DEFINE_HOOK(&daAlink_c::checkCutHeadState, HeadState);
DEFINE_HOOK(&daAlink_c::checkCutTypeNoBlur, NoBlur);
DEFINE_HOOK(&daAlink_c::setSwordChargeEffect, FxCharge);
DEFINE_HOOK(&daAlink_c::setSwordUpColorEffect, FxUp);
DEFINE_HOOK(&daAlink_c::setSwordCutSplash, FxSplash);
DEFINE_HOOK(&daAlink_c::setCutWaterDropEffect, FxDrop);

static int comboStep, hp0;
static bool wasGuarding, swinging, swordTucked, hpValid, flipping;
static bool forcedJump, forcedTurn, forcedDown, forcedHead, forcedBack;

static bool in_cut_proc(daAlink_c* lnk) {
    return lnk->mProcID >= daAlink_c::PROC_CUT_NORMAL && lnk->mProcID <= daAlink_c::PROC_CUT_LARGE_JUMP_LAND;
}

// the skill checks accept the training flags too, so we flip those to unlock a move
static void force_flag(daAlink_c* lnk, daAlink_c::daPy_FLG3 flag, bool want, bool* mine) {
    if (want && !lnk->checkNoResetFlg3(flag)) {
        lnk->onNoResetFlg3(flag);
        *mine = true;
    } else if (!want && *mine) {
        lnk->offNoResetFlg3(flag);
        *mine = false;
    }
}

// Unarmed shield bash: the game's gate for it, minus the shield. The game also wants R free (R is
// its bash button); with manual shielding R is the shield instead and the bash is on B, so that
// part is skipped then.
static bool unarmed_bash_ready(daAlink_c* lnk, bool bashOnB = false) {
    bool ground = lnk->mLinkAcch.ChkGroundHit() || lnk->checkMagneBootsOn();
    if (!cfgOn(vBash, true) || !ground || (!bashOnB && dComIfGp_getRStatus() != 0)) return false;
    if (!cfgOn(vSkill, false) && !dComIfGs_isEventBit(dSv_event_flag_c::F_0338) &&
        !lnk->checkNoResetFlg3(daAlink_c::FLG3_TRANING_SHIELD_ATTACK))
        return false;
    if (!lnk->checkGuardActionChange() || lnk->checkUpperReadyThrowAnime()) return false;
    return !lnk->checkModeFlg(0x70C52) && !daAlink_c::checkNotBattleStage();
}

// Mods with manual shielding have Link shield while both triggers are held all the way in (L to
// target, R to shield), and shield bash with B. With the "hold" style of targeting, R alone
// shields while a target is locked. That's the input those mods shield on, so that's what's
// looked for here, together with Link actually targeting; normal shielding (targeting with L, R
// pressed to bash) never has both triggers held in.
static bool manual_shield_held(daAlink_c* lnk) {
    if (!mDoCPd_c::getHoldLockR(PAD_1)) return false;
    if (mDoCPd_c::getHoldLockL(PAD_1)) return true;
    bool holdToTarget = dComIfGs_getOptAttentionType() == 1;
    return holdToTarget && lnk->mAttention != nullptr && lnk->mAttention->LockonTarget(0) != nullptr;
}

static bool manual_shielding(daAlink_c* lnk) {
    if (lnk->checkWolf() || !lnk->checkGuardActionChange() || !manual_shield_held(lnk)) return false;
    return !daAlink_c::checkShieldGet() || lnk->checkNoResetFlg2(daAlink_c::FLG2_UNK_8000000);
}

// With no shield, in that style, B is the (unarmed) shield bash whatever weapon is out: it shows
// the game's own Shield Attack on B and starts the bash before Link's update can turn the press
// into a sword swing.
static bool unarmed_manual_bash(daAlink_c* lnk) {
    return !daAlink_c::checkShieldGet() && manual_shielding(lnk) && unarmed_bash_ready(lnk, true);
}

// --- Manual Shielding (Combat > Misc) ---
// Our own take on that style: L targets as usual, holding R as well raises the shield (Link no
// longer shields just because he's targeting), B while shielding is the shield bash, and R no
// longer bashes on its own. With the "hold" style of targeting, R alone shields while a target is
// locked. Works with a shield, and without one when Blocking while unarmed is on.

static bool our_manual_shielding() { return cfgOn(vManualShield, false); }

// the game's own bash gate with a shield (the skill, targeting, on the ground...), R aside
static bool shield_bash_ready_on_b(daAlink_c* lnk) {
    if (!daAlink_c::checkShieldGet()) return unarmed_bash_ready(lnk, true);
    bool ground = lnk->mLinkAcch.ChkGroundHit() || lnk->checkMagneBootsOn();
    bool skill = dComIfGs_isEventBit(dSv_event_flag_c::F_0338) ||
                 lnk->checkNoResetFlg3(daAlink_c::FLG3_TRANING_SHIELD_ATTACK);
    return ground && skill && lnk->checkGuardActionChange() && !lnk->checkUpperReadyThrowAnime() &&
           !lnk->checkModeFlg(0x70C52) && !daAlink_c::checkNotBattleStage();
}

static bool bash_on_b(daAlink_c* lnk) {
    if (our_manual_shielding()) return manual_shielding(lnk) && shield_bash_ready_on_b(lnk);
    return unarmed_manual_bash(lnk);  // someone else's manual shielding, no shield
}

static bool in_guard_proc(daAlink_c* lnk) {
    return lnk->mProcID == daAlink_c::PROC_GUARD_SLIP || lnk->mProcID == daAlink_c::PROC_GUARD_ATTACK ||
           lnk->mProcID == daAlink_c::PROC_GUARD_BREAK;
}

// after the game (and Blocking while unarmed) decided whether he's shielding this frame
static void manual_guard(daAlink_c* lnk) {
    if (!our_manual_shielding() || lnk->checkWolf()) return;
    bool held = manual_shield_held(lnk) && lnk->checkGuardActionChange();
    if (!held && !in_guard_proc(lnk)) lnk->offNoResetFlg2(daAlink_c::FLG2_UNK_8000000);
    if (dComIfGp_getRStatus() == BUTTON_STATUS_SHIELD_ATTACK) lnk->setRStatus(BUTTON_STATUS_NONE);
}

// R's own bash is turned away while this is on; only the bash started from B goes through
// (and any the game plays in a cutscene)
DEFINE_HOOK(&daAlink_c::procGuardAttackInit, GuardAttackInit);
static bool bashFromB;

static HookAction guard_attack_pre(ModContext*, void* args, void* ret, void*) {
    auto* lnk = mods::arg<daAlink_c*>(args, 0);
    if (!our_manual_shielding() || bashFromB || lnk->checkEventRun()) return HOOK_CONTINUE;
    if (ret) *static_cast<int*>(ret) = 0;
    return HOOK_SKIP_ORIGINAL;
}

// One bash at a time, with a breather after it, the way the bash on R can't be spammed either.
static const int kBashGap = 30;  // frames after a bash ends before the next one
static int bashWait;

static HookAction manual_bash_pre(ModContext*, void* args, void*, void*) {
    auto* lnk = mods::arg<daAlink_c*>(args, 0);
    bool bashing = lnk->mProcID == daAlink_c::PROC_GUARD_ATTACK;
    if (bashing) bashWait = kBashGap;
    else if (bashWait > 0) bashWait--;
    if (bashing || bashWait > 0 || !mDoCPd_c::getTrigB(PAD_1) || !bash_on_b(lnk)) return HOOK_CONTINUE;
    bashFromB = true;
    lnk->procGuardAttackInit();
    bashFromB = false;
    return HOOK_CONTINUE;
}

static HookAction guard_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    wasGuarding = lnk->checkUpperGuardAnime() != 0;
    return HOOK_CONTINUE;
}

// no shield = setShieldGuard clears the guard flag, and checkShieldGet is inline so we can't hook that.
// put the flag back after it runs, setCollision comes later this frame and still sees it
static void guard_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    bool unarmed = !daAlink_c::checkSwordGet() && !lnk->checkWolf();

    // cut procs need the sword "in hand" or the hit capsule never moves
    if (unarmed && in_cut_proc(lnk) && lnk->mEquipItem == dItemNo_NONE_e) {
        lnk->mEquipItem = 0x103;
        swinging = true;
    } else if (swinging && !in_cut_proc(lnk)) {
        if (lnk->mEquipItem == 0x103) lnk->mEquipItem = dItemNo_NONE_e;
        swinging = false;
        if (lnk->checkUpperAnime(dRes_ID_ALANM_BCK_BOOMTHROW_e) || lnk->checkUpperAnime(dRes_ID_ALANM_BCK_CUTT_e))
            lnk->resetUpperAnime(daAlink_c::UPPER_2, 3.0f);
    }

    bool on = unarmed && cfgOn(vSwing, true);
    bool jump = cfgOn(vJump, false);
    force_flag(lnk, daAlink_c::FLG3_TRANING_CUT_LARGE_JUMP, on && jump && cfgOn(vChargedAll, false), &forcedJump);
    force_flag(lnk, daAlink_c::FLG3_TRANING_CUT_LARGE_TURN, on && cfgOn(vSpin, false) && cfgOn(vSpinAll, false), &forcedTurn);
    force_flag(lnk, daAlink_c::FLG3_TRANING_CUT_HEAD, on && cfgOn(vHead, false) && cfgOn(vHeadAll, false), &forcedHead);
    force_flag(lnk, daAlink_c::FLG3_TRANING_CUT_BACK, on && cfgOn(vBackAll, false), &forcedBack);
    force_flag(lnk, daAlink_c::FLG3_TRANING_CUT_DOWN, on && cfgOn(vEnding, false) && cfgOn(vEndingAll, false), &forcedDown);

    // checkGuardAccept without the shield part
    bool guarding = lnk->checkNoResetFlg2(daAlink_c::FLG2_UNK_8000000);
    if (cfgOn(vShield, true) && !guarding && lnk->checkModeFlg(0x80) && !lnk->checkFmChainGrabAnime() &&
        !daAlink_c::checkNotBattleStage() && !lnk->checkGrabAnime() && !lnk->checkUpperReadyThrowAnime() &&
        !lnk->checkDkCaught2Anime() && !lnk->checkKandelaarSwingAnime() && !lnk->checkCutDashAnime() &&
        !lnk->checkCutDashChargeAnime() && (!lnk->checkEquipAnime() || wasGuarding) && !lnk->checkRideOn() &&
        lnk->checkGuardActionChange())
        lnk->onNoResetFlg2(daAlink_c::FLG2_UNK_8000000);

    manual_guard(lnk);
    if (bash_on_b(lnk)) lnk->setBStatus(BUTTON_STATUS_SHIELD_ATTACK);
}

// even steps = hand swing arm, odd steps = right hook (end of the right spin cut arm, no spin). same lunge
// for both
static int start_jab(daAlink_c* lnk) {
    int r = lnk->procCutNormalInit(4);
    if ((comboStep & 1) == 0) {
        lnk->setUpperAnimeBaseSpeed(dRes_ID_ALANM_BCK_BOOMTHROW_e, 1.0f, 3.0f);
    } else {
        const daAlinkHIO_cutTurn_c1* t = &lnk->mpHIO->mCut.mCutTurn.m;
        float from = t->mRightAttackStartFrame + (t->mRightAttackEndFrame - t->mRightAttackStartFrame) / 4;
        lnk->setUpperAnime(dRes_ID_ALANM_BCK_CUTT_e, daAlink_c::UPPER_2, 1.0f, from, t->mRightTurnAnm.mEndFrame, 3.0f);
    }
    return r;
}

static void item_action_post(ModContext*, void* args, void* retval, void*) {
    if (!retval || *static_cast<int*>(retval) != 0) return;
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);

    if (lnk->checkEquipAnime() || lnk->checkWolf()) return;
    if (lnk->checkKandelaarSwingAnime() || lnk->checkCopyRodThrowAnime()) return;
    if (lnk->checkBoomerangThrowAnime()) return;

    bool ground = lnk->mLinkAcch.ChkGroundHit() || lnk->checkMagneBootsOn();

    if (unarmed_bash_ready(lnk)) {
        lnk->setRStatus(BUTTON_STATUS_SHIELD_ATTACK);
        if (lnk->spActionTrigger()) {
            *static_cast<int*>(retval) = lnk->procGuardAttackInit();
            return;
        }
    }

    if (!cfgOn(vSwing, true) || !ground || daAlink_c::checkSwordGet()) return;
    // shielding the manual way: B belongs to the shield bash (whoever shows it), not our swing
    if (manual_shielding(lnk)) return;
    bool inCutscene = lnk->mDemo.getDemoType() != 0 || lnk->checkEventRun();
    if (inCutscene) return;
    bool onHorse = lnk->checkRideOn();
    if (onHorse || lnk->checkModeFlg(0x70C52)) return;

    // same places the game won't let you draw a sword (towns, rooms, canoe), asked through the game's own
    // checks so mods that unlock those unlock this too
    if (daAlink_c::checkNotBattleStage()) return;
    if (lnk->checkCanoeRide()) return;
    if (lnk->checkModeFlg(0x40000) && !lnk->checkEquipHeavyBoots()) return;
    if (lnk->checkEndResetFlg1(daAlink_c::ERFLG1_SWORD_TRIGGER_NON)) return;

    // carried stuff and a flying boomerang stay as they are
    if (lnk->mEquipItem == 0x102 || lnk->mEquipItem == 0x10B) return;
    if (lnk->mThrowBoomerangAcKeep.getActor()) return;

    lnk->setBStatus(BUTTON_STATUS_ATTACK);
    if (!lnk->swordTrigger()) return;

    if (lnk->mEquipItem != dItemNo_NONE_e) lnk->deleteEquipItem(TRUE, FALSE);

    // mid swing, so this is the next hit
    comboStep = swinging ? comboStep + 1 : 0;
    lnk->mEquipItem = 0x103;
    swinging = true;
    *static_cast<int*>(retval) = start_jab(lnk);
}

// unarmed lock-on A shows UNK_121, so swap the label and checkDoCutAction does the rest
static void do_status_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    bool jump = cfgOn(vJump, false), ending = cfgOn(vEnding, false), head = cfgOn(vHead, false);
    if (!cfgOn(vSwing, true) || (!jump && !ending && !head)) return;

    if (lnk->checkWolf() || daAlink_c::checkSwordGet() || lnk->mEquipItem != dItemNo_NONE_e) return;
    if (!lnk->checkAttentionLock() || dComIfGp_getDoStatus() != BUTTON_STATUS_UNK_121) return;
    if (lnk->checkNotJumpSinkLimit() || lnk->checkMagneBootsOn() || lnk->checkIronBallWaitAnime()) return;
    if (lnk->checkSmallUpperGuardAnime()) return;
    if (lnk->checkInputOnR() && lnk->getDirectionFromShapeAngle() != daAlink_c::DIR_FORWARD) return;

    if (ending && lnk->checkDownAttackState()) lnk->setDoStatusEmphasys(BUTTON_STATUS_FINISH);
    else if (head && lnk->checkCutHeadState()) lnk->setDoStatusEmphasys(BUTTON_STATUS_HELM_SPLITTER);
    else if (jump) lnk->setDoStatus(BUTTON_STATUS_UNK_134);
}

// spin off: hide the "sword" so checkNextAction can't start a spin charge or combo
static HookAction next_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    swordTucked = false;
    if (swinging && lnk->mEquipItem == 0x103 && !(cfgOn(vSwing, true) && cfgOn(vSpin, false))) {
        lnk->mEquipItem = dItemNo_NONE_e;
        swordTucked = true;
    }
    return HOOK_CONTINUE;
}

static void next_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    if (swordTucked && lnk->mEquipItem == dItemNo_NONE_e && in_cut_proc(lnk)) lnk->mEquipItem = 0x103;
    swordTucked = false;
}

// cc_at_check already took off the plain (ordon) damage, so redo it from hp0 with the multiplier.
// pct: wooden .5 (default), ordon 1, master 2, light 4 (master x2 again from the glow)
static HookAction cc_pre(ModContext*, void* args, void*, void*) {
    fopAc_ac_c* en = mods::arg<fopAc_ac_c*>(args, 0);
    hpValid = swinging && en;
    if (hpValid) hp0 = en->health;
    return HOOK_CONTINUE;
}

static void cc_post(ModContext*, void* args, void*, void*) {
    if (!hpValid) return;
    hpValid = false;
    fopAc_ac_c* en = mods::arg<fopAc_ac_c*>(args, 0);
    dCcU_AtInfo* info = mods::arg<dCcU_AtInfo*>(args, 1);
    if (!en || !info || info->mAttackPower == 0 || info->mHitType != HIT_TYPE_LINK_NORMAL_ATTACK) return;

    static const int pct[] = {50, 100, 200, 400};
    int pick = cfgInt(vDamage, 0);
    if (pick < 0) pick = 0;
    if (pick > 3) pick = 3;

    int dmg = info->mAttackPower * pct[pick] / 100;
    if (dmg < 1) dmg = 1;
    int hp = hp0 - dmg;
    if (hp <= 0) {
        hp = 0;
        info->mHitStatus = 2;
    } else if (info->mHitStatus == 2) {
        info->mHitStatus = 1;
    }
    en->health = hp;
}

static HookAction fx_skip(ModContext*, void*, void*, void*) {
    return swinging ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

static void no_blur_post(ModContext*, void*, void* retval, void*) {
    if (swinging && retval) *static_cast<int*>(retval) = 1;
}

// the sword model's axis follows the hand, so during the hook and hand swing the capsule can point behind him.
// rebuild it ourselves: short lines straight out from just in front of Link, one per capsule, stacked
// from near his feet (chickens, small critters) through hand height to a bit above it
static void sword_hit_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    if (!swinging) return;
    cXyz ahead(cM_ssin(lnk->shape_angle.y), 0.0f, cM_scos(lnk->shape_angle.y));
    f32 hand = lnk->field_0x3498.y;
    f32 heights[3] = {lnk->current.pos.y + 15.0f, hand, hand + 25.0f};
    for (int i = 0; i < 3; i++) {
        cXyz from(lnk->current.pos.x + ahead.x * 20.0f, heights[i], lnk->current.pos.z + ahead.z * 20.0f);
        lnk->mAtCps[i].SetStartEnd(from, from + ahead * 80.0f);
    }
}

// B mid swing normally picks a combo move here, we just chain our own jabs instead
static HookAction cut_action_pre(ModContext*, void* args, void* retval, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    if (!swinging || lnk->checkWolf()) return HOOK_CONTINUE;
    lnk->resetCombo(TRUE);
    comboStep++;
    int r = start_jab(lnk);
    if (retval) *static_cast<int*>(retval) = r;
    return HOOK_SKIP_ORIGINAL;
}

// A asks these two on its own whatever the prompt says, so a learned ending blow or helm splitter
// would still fire with its toggle off. answer no unless the toggle's on
static bool unarmed_move_off(daAlink_c* lnk, ConfigVarHandle move) {
    return !daAlink_c::checkSwordGet() && !lnk->checkWolf() && !(cfgOn(vSwing, true) && cfgOn(move, false));
}

static void down_state_post(ModContext*, void* args, void* retval, void*) {
    if (retval && unarmed_move_off(mods::arg<daAlink_c*>(args, 0), vEnding)) *static_cast<int*>(retval) = 0;
}

static void head_state_post(ModContext*, void* args, void* retval, void*) {
    if (retval && unarmed_move_off(mods::arg<daAlink_c*>(args, 0), vHead)) *static_cast<int*>(retval) = 0;
}

// ending blow = backflip played backwards for a front flip, and no sword-stuck anim on a miss
static void down_init_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    flipping = false;
    if (daAlink_c::checkSwordGet() || lnk->checkWolf() || lnk->mProcVar3.field_0x300e != 0) return;
    const daAlinkHIO_anm_c* a = &lnk->mpHIO->mBackJump.m.mBackflipAnm;
    lnk->setSingleAnime(daAlink_c::ANM_BACKFLIP, -a->mSpeed, a->mStartFrame, a->mEndFrame, a->mInterpolation);
    flipping = true;
}

// hold the flip's first pose till landing instead of the sword fall pose
static void down_air_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    if (!flipping || lnk->mProcID != daAlink_c::PROC_CUT_DOWN || lnk->mProcVar2.field_0x300c == 0) return;
    const daAlinkHIO_anm_c* a = &lnk->mpHIO->mBackJump.m.mBackflipAnm;
    lnk->setSingleAnime(daAlink_c::ANM_BACKFLIP, 0.0f, a->mStartFrame, a->mEndFrame, 3.0f);
    flipping = false;
}

static void down_land_post(ModContext*, void* args, void*, void*) {
    daAlink_c* lnk = mods::arg<daAlink_c*>(args, 0);
    if (daAlink_c::checkSwordGet() || lnk->checkWolf() || lnk->mProcID != daAlink_c::PROC_CUT_DOWN_LAND) return;
    if (lnk->mProcVar2.field_0x300c == 0) return;
    lnk->onModeFlg(4);
    lnk->checkNextAction(1);
}

static void unarmed_init() {
    // the guard hook carries the swing bookkeeping too, so it's the one worth shouting about
    bool guardOk = pre_hook(SetShieldGuard, guard_pre) && post_hook(SetShieldGuard, guard_post);
    if (!guardOk) mods::log::warn("unarmed guard and swing won't work");

    int lost = 0;
    lost += !post_hook(CheckItemAction, item_action_post);
    lost += !pre_hook(Exec, manual_bash_pre);
    lost += !pre_hook(GuardAttackInit, guard_attack_pre);
    lost += !post_hook(DoStatus, do_status_post);
    lost += !pre_hook(NextAction, next_pre) + !post_hook(NextAction, next_post);
    lost += !pre_hook(CcAtCheck, cc_pre) + !post_hook(CcAtCheck, cc_post);
    lost += !pre_hook(CutAction, cut_action_pre);
    lost += !post_hook(SwordHit, sword_hit_post);
    lost += !post_hook(DownState, down_state_post) + !post_hook(HeadState, head_state_post);
    lost += !post_hook(DownInit, down_init_post) + !post_hook(DownAir, down_air_post);
    lost += !post_hook(DownLand, down_land_post);
    if (lost) mods::log::warn("{} unarmed move hooks missing, some moves may act vanilla", lost);

    // blade sparkle is cosmetic; hooked() already logs a miss, nothing else to do
    post_hook(NoBlur, no_blur_post);
    pre_hook(FxCharge, fx_skip);
    pre_hook(FxUp, fx_skip);
    pre_hook(FxSplash, fx_skip);
    pre_hook(FxDrop, fx_skip);
}

// ======================== update check ========================

// GameBanana's own site loads the mod page from this, and the version you set there comes back
// as the top-level "_sVersion".
static const char* const kModPageData = "https://gamebanana.com/apiv11/Mod/731814/ProfilePage";

enum CheckState { CHECK_NOT_YET, CHECK_WAITING, CHECK_GOT_IT, CHECK_FAILED };
static CheckState checkState = CHECK_NOT_YET;
static char latestVersion[32];
static char whyFailed[96];  // shown under the versions when the check didn't work
static bool showWhenDone;  // the button was pressed, so pop the window up once the answer is in

static const char* my_version() {
    const char* v = svc_host->mod_version(mod_ctx);
    return v && *v ? v : "unknown";
}

// "1.0.1" style versions, a leading v is fine; returns >0 when a is newer than b
static int compare_versions(const char* a, const char* b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    while (*a || *b) {
        long left = strtol(a, const_cast<char**>(&a), 10);
        long right = strtol(b, const_cast<char**>(&b), 10);
        if (left != right) return left > right ? 1 : -1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (*a && !isdigit((unsigned char)*a)) break;
        if (*b && !isdigit((unsigned char)*b)) break;
    }
    return 0;
}

static bool update_ready() {
    return checkState == CHECK_GOT_IT && compare_versions(latestVersion, my_version()) > 0;
}

// copies the string value that starts right after a key, skipping the ": " in between
static bool value_after(const char* body, size_t size, size_t c) {
    while (c < size && (body[c] == ' ' || body[c] == ':' || body[c] == '\t')) c++;
    if (c >= size || body[c] != '"') return false;  // null or a number, not a version
    size_t len = 0;
    for (c++; c < size && body[c] != '"' && len < sizeof(latestVersion) - 1; c++)
        latestVersion[len++] = body[c];
    latestVersion[len] = 0;
    return len > 0;
}

// the mod's own "_sVersion" sits at the top level of the reply, next to the view and like counts;
// the files further in have their own, so only depth 1 counts. the body isn't null terminated
static bool read_version(const char* body, size_t size) {
    static const char key[] = "\"_sVersion\"";
    const size_t keyLen = sizeof(key) - 1;
    latestVersion[0] = 0;
    int depth = 0;
    bool inText = false;
    for (size_t at = 0; at < size; at++) {
        char ch = body[at];
        if (inText) {
            if (ch == '\\') at++;  // escaped character, skip it
            else if (ch == '"') inText = false;
            continue;
        }
        if (ch == '{' || ch == '[') depth++;
        if (ch == '}' || ch == ']') depth--;
        if (ch != '"') continue;
        bool topKey = depth == 1 && at + keyLen <= size && memcmp(body + at, key, keyLen) == 0;
        if (topKey) return value_after(body, size, at + keyLen);
        inText = true;
    }
    return false;
}

static void show_versions() {
    bool newer = update_ready();
    const char* heading = newer ? "UPDATE AVAILABLE" : "UP TO DATE";
    if (checkState != CHECK_GOT_IT) heading = "COULDN'T CHECK";

    // laid out like the game's own update box: plain label, bold version, a "v" in front
    const char* mine = my_version();
    const char* latest = checkState == CHECK_GOT_IT ? latestVersion : "unknown";
    const char* mineV = isdigit((unsigned char)*mine) ? "v" : "";
    const char* latestV = isdigit((unsigned char)*latest) ? "v" : "";
    char body[384];
    int used = snprintf(body, sizeof(body), "<p>YOUR VERSION: <b>%s%s</b></p><p>LATEST VERSION: <b>%s%s</b></p>",
                        mineV, mine, latestV, latest);
    if (checkState == CHECK_FAILED && used > 0 && used < (int)sizeof(body))
        snprintf(body + used, sizeof(body) - used, "<p>%s</p>", whyFailed);

    UiDialogAction close = UI_DIALOG_ACTION_INIT;
    close.label = "Close";
    UiDialogDesc box = UI_DIALOG_DESC_INIT;
    box.title = heading;
    box.body_rml = body;
    box.actions = &close;
    box.action_count = 1;
    UiDialogHandle shown = 0;
    svc_ui->dialog_push(mod_ctx, &box, &shown);
}

static void check_finished(bool gotIt, const char* why) {
    checkState = gotIt ? CHECK_GOT_IT : CHECK_FAILED;
    if (!gotIt) {
        snprintf(whyFailed, sizeof(whyFailed), "%s", why);
        // it ends up in the pop-up's markup, so no tags sneaking in from an error message
        for (char* ch = whyFailed; *ch; ch++)
            if (*ch == '<' || *ch == '>' || *ch == '&') *ch = ' ';
        mods::log::warn("update check failed: {}", why);
    }
    if (showWhenDone) show_versions();
    showWhenDone = false;
}

static void version_arrived(ModContext*, HttpRequestHandle, const HttpResult* reply, void*) {
    char why[96];
    if (reply == nullptr) return check_finished(false, "no reply");
    if (reply->error != HTTP_ERROR_NONE) {
        const char* msg = reply->error_message && *reply->error_message ? reply->error_message : "network error";
        snprintf(why, sizeof(why), "%s (error %d)", msg, (int)reply->error);
        return check_finished(false, why);
    }
    if (reply->status_code != 200) {
        snprintf(why, sizeof(why), "GameBanana answered HTTP %d", (int)reply->status_code);
        return check_finished(false, why);
    }
    bool found = reply->body && read_version(static_cast<const char*>(reply->body), reply->body_size);
    if (!found) {
        snprintf(why, sizeof(why), "no version in the reply (%u bytes)", (unsigned)reply->body_size);
        return check_finished(false, why);
    }
    check_finished(true, "");
}

static void check_for_updates() {
    if (checkState == CHECK_WAITING) return;
    if (svc_http == nullptr) return check_finished(false, "this Dusklight build has no web requests");

    static const HttpHeader wantJson[] = {{"Accept", "application/json"}};
    HttpRequestDesc ask = HTTP_REQUEST_DESC_INIT;
    ask.url = kModPageData;
    ask.headers = wantJson;
    ask.header_count = 1;
    ask.total_timeout_ms = 15000;
    ask.max_body_bytes = 8u << 20;  // the page data carries the whole description, give it room
    HttpRequestHandle pending = 0;
    checkState = CHECK_WAITING;
    ModResult sent = svc_http->request(mod_ctx, &ask, version_arrived, nullptr, &pending);
    if (sent == MOD_OK) return;

    char why[96];
    snprintf(why, sizeof(why), "request refused (%d), no network access here?", (int)sent);
    check_finished(false, why);
}

static void check_button(ModContext*, void*) {
    showWhenDone = true;
    check_for_updates();
}

// ======================== Geo's Hyrulean Settings ========================

// greyed-out rules for rows that depend on another toggle
static bool leaf_off(ModContext*, void*) { return !cfgOn(vLeaf); }
static bool combos_off(ModContext*, void*) { return !combos_on(); }
static bool fire_arrows_off(ModContext*, void*) { return !fire_on(); }
static bool bash_off(ModContext*, void*) { return !cfgOn(vBash, true); }
static bool swing_off(ModContext*, void*) { return !cfgOn(vSwing, true); }

// an "always have" row also needs its move switched on
static bool move_off(ModContext* ctx, void* move) {
    return swing_off(ctx, nullptr) || !cfgOn(*static_cast<ConfigVarHandle*>(move), false);
}

// whichever page is being filled right now; the window rebuilds a page each time you open its tab
static UiElementHandle page;

static void heading(const char* title) { svc_ui->pane_add_section(mod_ctx, page, title); }
static void note(const char* line) { svc_ui->pane_add_text(mod_ctx, page, line, nullptr); }

static void add_row(UiControlDesc& row, const char* label, ConfigVarHandle var) {
    if (!var) return;  // never registered, so it just keeps its default
    row.label = label;
    row.binding = UI_BINDING_CONFIG_VAR;
    row.config_var = var;
    svc_ui->pane_add_control(mod_ctx, page, &row, nullptr);
}

static void toggle(const char* label, ConfigVarHandle var, UiPredicateFn greyIf = nullptr, void* dependsOn = nullptr) {
    UiControlDesc row = UI_CONTROL_DESC_INIT;
    row.kind = UI_CONTROL_TOGGLE;
    row.is_disabled = greyIf;
    row.user_data = dependsOn;
    add_row(row, label, var);
}

static void choice(const char* label, ConfigVarHandle var, const char* const* names, int count) {
    UiControlDesc row = UI_CONTROL_DESC_INIT;
    row.kind = UI_CONTROL_DROPDOWN;
    row.options = names;
    row.option_count = count;
    if (var == vDamage) row.is_disabled = swing_off;  // the only dropdown that hangs off a toggle
    add_row(row, label, var);
}

static void quality_rows() {
    heading("Epona Locations");
    for (int area = 0; area < EPONA_AREAS; area++) toggle(eponaLabels[area], eponaToggle[area]);

    heading("Map Checks");
    choice("Remove faron woods mist", mistOption, mistNames, 3);
    choice("Revert to Chudley's", shopOption, shopNames, 4);
    toggle("Jump to Lake Hylia from Bridge", bridgeJump);

    heading("Shop Changes");
    toggle("Goron Outpost Changes", vGoron);
    toggle("Chudleys/Malomart Changes", vChud);
}

static const char* const damageNames[] = {"x0.5 Wooden Sword", "x1 Ordon Sword", "x2 Master Sword", "x4 Light Sword"};

static void combat_rows() {
    heading("Unarmed Combat");
    note("Unarmed Shielding");
    toggle("Blocking while unarmed", vShield);
    toggle("Shield bash while unarmed", vBash);
    toggle("Bash without shield bash skill", vSkill, bash_off);

    note("Unarmed Swing");
    toggle("Swing while unarmed", vSwing);
    choice("Swing damage", vDamage, damageNames, 4);
    toggle("Jump strike while unarmed", vJump, swing_off);
    toggle("Always have charged jump strike", vChargedAll, move_off, &vJump);
    toggle("Spin attack while unarmed", vSpin, swing_off);
    toggle("Always have great spin", vSpinAll, move_off, &vSpin);
    toggle("Final blow while unarmed", vEnding, swing_off);
    toggle("Always have final blow", vEndingAll, move_off, &vEnding);
    toggle("Helm splitter while unarmed", vHead, swing_off);
    toggle("Always have helm splitter", vHeadAll, move_off, &vHead);
    toggle("Always have back slice", vBackAll, swing_off);

    heading("Misc");
    choice("Sword Flourish", flourishOption, flourishNames, 4);
    toggle("Mortal Draw Hand Anim", vHandPose);
    toggle("Manual Shielding", vManualShield);
    toggle("Deal Burn with Lantern", vLanternBurn);
}

static void item_rows() {
    heading("Ice Leaf");
    toggle("Enable Ice Leaf", vLeaf);
    toggle("Always Have Ice Sled", vAlways, leaf_off);
    toggle("R Speed Boost Cheat", vBoost, leaf_off);

    heading("Item Combinations");
    toggle("Enable Combinations", vCombos);
    note("Gale-Bomb Combo");
    toggle("Gale Boomerang + Bombs", galeBombToggle[0], combos_off);
    toggle("Gale Boomerang + Water Bombs", galeBombToggle[1], combos_off);
    toggle("Gale Boomerang + Bomblings", galeBombToggle[2], combos_off);
    note("Fire Arrows");
    toggle("Bow + Lantern", vFireArrows, combos_off);
    toggle("Deal Burn Damage", vFireDamage, fire_arrows_off);
}

static const char* const tabTitles[3] = {"Quality of Life", "Combat", "Items"};
static void (*const tabRows[3])() = {quality_rows, combat_rows, item_rows};

// one builder for all three tabs, user_data is the tab number
static ModResult fill_tab(ModContext*, UiWindowHandle, UiElementHandle pane, UiElementHandle, void* which, ModError*) {
    page = pane;
    tabRows[reinterpret_cast<intptr_t>(which)]();
    return MOD_OK;
}

static UiWindowHandle settingsWindow;

static void settings_closed(ModContext*, UiWindowHandle, void*) {
    settingsWindow = 0;
}

// opened from the Geo's Hyrulean tab up top, or the button on the Mods page
static void open_settings(ModContext*, void*) {
    if (settingsWindow) return;

    UiTabDesc tabs[3];
    for (int t = 0; t < 3; t++) {
        tabs[t] = UI_TAB_DESC_INIT;
        tabs[t].title = tabTitles[t];
        tabs[t].build = fill_tab;
        tabs[t].user_data = reinterpret_cast<void*>(static_cast<intptr_t>(t));
    }

    UiWindowDesc win = UI_WINDOW_DESC_INIT;
    win.tabs = tabs;
    win.tab_count = 3;
    win.on_closed = settings_closed;
    if (svc_ui->window_push(mod_ctx, &win, &settingsWindow) == MOD_OK) return;
    mods::log::warn("settings window wouldn't open");
    settingsWindow = 0;
}

static UiElementHandle updateNote;  // the yellow line, only lives while the Mods page is built

// the Mods page: the way into the settings, plus the update check
static ModResult mods_page(ModContext*, UiElementHandle panel, void*, ModError*) {
    updateNote = 0;

    UiControlDesc open = UI_CONTROL_DESC_INIT;
    open.kind = UI_CONTROL_BUTTON;
    open.label = "Open Geo's Hyrulean Settings";
    open.on_pressed = open_settings;
    svc_ui->pane_add_control(mod_ctx, panel, &open, nullptr);

    UiControlDesc check = UI_CONTROL_DESC_INIT;
    check.kind = UI_CONTROL_BUTTON;
    check.label = "Check for Updates";
    check.on_pressed = check_button;
    svc_ui->pane_add_control(mod_ctx, panel, &check, nullptr);

    page = panel;
    toggle("Auto Check for Updates", vAutoCheck);
    svc_ui->pane_add_rml(mod_ctx, panel, "<span style=\"color: #ffd23f;\">Update available!</span>", &updateNote);
    if (updateNote) svc_ui->elem_set_visible(mod_ctx, updateNote, false);
    return MOD_OK;
}

// every frame the Mods page is open
static ModResult mods_page_tick(ModContext*, void*, ModError*) {
    bool autoOn = cfgOn(vAutoCheck);
    if (autoOn && checkState == CHECK_NOT_YET) check_for_updates();  // switched on after startup
    if (updateNote) svc_ui->elem_set_visible(mod_ctx, updateNote, autoOn && update_ready());
    return MOD_OK;
}

static void settings_init() {
    UiMenuTabDesc tab = UI_MENU_TAB_DESC_INIT;
    tab.label = "Geo's Hyrulean";
    tab.on_selected = open_settings;
    UiMenuTabHandle topTab = 0;
    if (svc_ui->register_menu_tab(mod_ctx, &tab, &topTab) != MOD_OK)
        mods::log::warn("Geo's Hyrulean tab didn't fit in the top bar; the Mods page button still works");

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = mods_page;
    panel.update = mods_page_tick;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK)
        mods::log::warn("mods panel failed, settings keep their saved values");
}

static bool add_bool_option(const char* key, bool def, ConfigVarHandle* out) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = key;
    d.type = CONFIG_VAR_BOOL;
    d.default_bool = def;
    if (svc_config->register_var(mod_ctx, &d, out) == MOD_OK) return true;
    *out = 0;
    return false;
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
    if (!(pre_hook(BackJump, backflip_pre) && post_hook(BackJump, backflip_post)))
        mods::log::warn("a refused backflip off the sled can leave Link riding nothing");
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

    // --- Unarmed Combat ---
    int missed = 0;
    missed += !add_bool_option("shielding", true, &vShield);
    missed += !add_bool_option("bashing", true, &vBash);
    missed += !add_bool_option("swingUnarmed", true, &vSwing);
    missed += !add_bool_option("ignoreSkill", false, &vSkill);
    missed += !add_bool_option("jumpSlash", false, &vJump);
    missed += !add_bool_option("chargedJumpAlways", false, &vChargedAll);
    missed += !add_bool_option("spinAttack", false, &vSpin);
    missed += !add_bool_option("spinAlways", false, &vSpinAll);
    missed += !add_bool_option("endingBlow", false, &vEnding);
    missed += !add_bool_option("endingBlowAlways", false, &vEndingAll);
    missed += !add_bool_option("helmSplitter", false, &vHead);
    missed += !add_bool_option("helmSplitterAlways", false, &vHeadAll);
    missed += !add_bool_option("backSliceAlways", false, &vBackAll);
    if (!add_int_option("damageSword", 0, &vDamage)) {
        vDamage = 0;
        missed++;
    }
    if (missed) mods::log::warn("{} unarmed settings didn't register, those use their defaults", missed);
    unarmed_init();

    // --- Combat > Misc: Sword Flourish and Deal Burn with Lantern ---
    if (!add_int_option("swordFlourish", FLOURISH_DEFAULT, &flourishOption)) flourishOption = 0;
    add_bool_option("lanternBurn", true, &vLanternBurn);
    add_bool_option("manualShielding", false, &vManualShield);
    add_bool_option("mortalDrawHandAnim", true, &vHandPose);
    if (!post_hook(SetMatrix, hand_pose_post)) mods::log::warn("Mortal Draw Hand Anim stays on");
    // rides along on Link's update, next to the Ice Leaf's own hooks there
    bool flourishOk = pre_hook(Exec, flourish_exec_pre) && post_hook(Exec, flourish_exec_post);
    if (!flourishOk) mods::log::warn("sword flourish stays at the game's normal behaviour");

    // --- Item Combinations ---
    add_bool_option("itemCombinations", true, &vCombos);
    add_bool_option("galeBombs", true, &galeBombToggle[0]);
    add_bool_option("galeWaterBombs", true, &galeBombToggle[1]);
    add_bool_option("galeBomblings", true, &galeBombToggle[2]);
    add_bool_option("fireArrows", true, &vFireArrows);
    add_bool_option("fireDamage", true, &vFireDamage);

    // each combination's own picture on its button
    if (svc_resource->load(mod_ctx, "galebomb_icon.rgba", &galeBombIcon) != MOD_OK)
        mods::log::warn("galebomb_icon.rgba missing, the Gale-Bomb button shows the plain boomerang");
    if (svc_resource->load(mod_ctx, "firearrow_icon.rgba", &fireArrowIcon) != MOD_OK)
        mods::log::warn("firearrow_icon.rgba missing, the Fire Arrows button shows the plain bow");

    // the item ring: who combines with what, and the line that says so
    bool ringOk = pre_hook(RingMove, ring_pre) && post_hook(RingMove, ring_post);
    if (!ringOk) mods::log::warn("can't tie bombs to the boomerang in the item ring");
    // the lantern poses as the Hawkeye wherever the ring decides what R does with it
    int lanternMiss = 0;
    lanternMiss += !pre_hook(RingMixItem, lantern_poses) + !post_hook(RingMixItem, lantern_back_after_mix);
    lanternMiss += !pre_hook(RingCanMix, lantern_poses) + !post_hook(RingCanMix, lantern_back_post);
    lanternMiss += !pre_hook(RingCanSplit, lantern_poses) + !post_hook(RingCanSplit, lantern_back_post);
    lanternMiss += !pre_hook(RingCursor, lantern_poses) + !post_hook(RingCursor, lantern_back_post);
    if (lanternMiss) mods::log::warn("the lantern can't be combined with the bow in the item ring");

    // Gale-Bomb
    bool iconOk = pre_hook(ButtonIcon, button_icon_pre) && post_hook(ButtonIcon, button_icon_post);
    iconOk = iconOk && pre_hook(ReadTex, combo_tex_pre) && post_hook(ReadTex, combo_tex_post);
    if (!iconOk) mods::log::warn("the boomerang button won't show the little bomb");
    if (!post_hook(Exec, galebomb_exec_post)) mods::log::warn("thrown boomerangs won't carry bombs");
    if (!pre_hook(ButtonCount, button_count_pre)) mods::log::warn("no bomb count on the boomerang button");
    if (!post_hook(HudCreate, hud_created)) mods::log::warn("combo icons may need a menu open after warping");

    // Fire Arrows
    if (!pre_hook(ButtonOilGauge, oil_gauge_pre)) mods::log::warn("no oil gauge on the fire-arrow bow");
    if (!post_hook(ArrowUpdate, arrow_post)) mods::log::warn("arrows won't catch fire");
    if (!post_hook(Exec, burn_post)) mods::log::warn("enemies won't catch fire from fire arrows");

    settings_init();

    // --- update check ---
    add_bool_option("autoUpdateCheck", true, &vAutoCheck);
    if (cfgOn(vAutoCheck)) check_for_updates();

    svc_log->info(mod_ctx, "Tools and Expansion loaded");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    epona_tick();
    faron_lighting_tick();
    bridge_tick();
    shop_update();
    galebomb_tick();
    fire_arrow_tick();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    put_lighting_back();
    if (out()) drop((daAlink_c*)dComIfGp_getPlayer(0));
    if (dComIfGs_getItem(leafSlot, true) == leafItem) setLeafSlot(dItemNo_NONE_e);
    while (nIcons > 0) svc_texture->unregister(mod_ctx, iconTex[--nIcons]);
    if (iconRes.data) svc_resource->free(mod_ctx, &iconRes);
    shop_shutdown();
    drop_bombling_art();
    fire_arrow_shutdown();
    return MOD_OK;
}

}
