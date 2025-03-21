#include <memory>

#include "extension.h"
#include <CDetour/detours.h>
#include "helpers.h"
#include "sourcesdk/nav_mesh.h"
#include "sourcesdk/tf_gamerules.h"
#include "sourcesdk/basetoggle.h"
#include "sourcesdk/funcbrush.h"
#include "natives.hpp"
#include <ihandleentity.h>
#include "npc_tools_internal.h"
#include "baseentityoutput.h"
#include "pluginentityfactory.h"
#include "cbasenpc_behavior.h"
#include "serialrefresher.h"

class CTakeDamageInfoHack;
SH_DECL_MANUALEXTERN1_void(MEvent_Killed, CTakeDamageInfoHack &);

CGlobalVars* gpGlobals = nullptr;
IGameConfig* g_pGameConf = nullptr;
IBinTools* g_pBinTools = nullptr;
ISDKTools* g_pSDKTools = nullptr;
ISDKHooks* g_pSDKHooks = nullptr;
IServerGameEnts* gameents = nullptr;
IEngineTrace* enginetrace = nullptr;
IdentityToken_t * g_pCoreIdent = nullptr;
CBaseEntityList* g_pEntityList = nullptr;
IServerTools* servertools = nullptr;
IMDLCache* mdlcache = nullptr;
CSharedEdictChangeInfo* g_pSharedChangeInfo = nullptr;
IStaticPropMgrServer* staticpropmgr = nullptr;
ConVar* sourcemod_version = nullptr;
IBaseNPC_Tools* g_pBaseNPCTools = new BaseNPC_Tools_API;
std::vector<sp_nativeinfo_t> gNatives;

DEFINEHANDLEOBJ(AreasCollector, CUtlVector< CNavArea* >);

ConVar* g_cvDeveloper = nullptr;
extern ConVar* NextBotSpeedLookAheadRange;
extern ConVar* NextBotGoalLookAheadRange;
extern ConVar* NextBotLadderAlignRange;
extern ConVar* NextBotAllowAvoiding;
extern ConVar* NextBotAllowClimbing;
extern ConVar* NextBotAllowGapJumping;
extern ConVar* NextBotDebugClimbing;
extern ConVar* NextBotDebugHistory;
extern ConVar* NextBotPathDrawIncrement;
extern ConVar* NextBotPathSegmentInfluenceRadius;
extern ConVar* NextBotPlayerStop;
extern ConVar* NextBotStop;
extern ConVar* nav_solid_props;
extern ConVar* nb_update_framelimit;
extern ConVar* nb_update_maxslide;

HandleType_t g_KeyValueType;

CBaseNPCExt g_CBaseNPCExt;
SMEXT_LINK(&g_CBaseNPCExt);

IForward *g_pForwardEventKilled = nullptr;
bool (ToolsTraceFilterSimple:: *ToolsTraceFilterSimple::func_ShouldHitEntity)(IHandleEntity *pHandleEntity, int contentsMask) = nullptr;
CUtlMap<int32_t, int32_t> g_EntitiesHooks;

bool CBaseNPCExt::SDK_OnLoad(char* error, size_t maxlength, bool late) {
	char conf_error[255];
	if (!gameconfs->LoadGameConfigFile("cbasenpc", &g_pGameConf, conf_error, sizeof(conf_error))) {
		snprintf(error, maxlength, "FAILED TO LOAD GAMEDATA ERROR: %s", conf_error);
		return false;
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	if (!CBaseEntity::Init(g_pGameConf, error, maxlength)
		|| !CBaseAnimating::Init(g_pGameConf, error, maxlength)
		|| !CBaseAnimatingOverlay::Init(g_pGameConf, error, maxlength)
		|| !CFuncBrush::Init(g_pGameConf, error, maxlength)
		|| !CBaseToggle::Init(g_pGameConf, error, maxlength)
		|| !CNavMesh::Init(g_pGameConf, error, maxlength)
		|| !CBaseCombatCharacter::Init(g_pGameConf, error, maxlength)
		|| !ToolsTraceFilterSimple::Init(g_pGameConf, error, maxlength)
		|| !CTFGameRules::Init(g_pGameConf, error, maxlength)
		|| !CBaseEntityOutput::Init(g_pGameConf, error, maxlength)
		|| !CBaseNPC_Locomotion::Init(g_pGameConf, error, maxlength)
		|| !ToolsNextBot::Init(g_pGameConf, error, maxlength)
		|| !Tools_Refresh_Init(g_pGameConf, error, maxlength)
		) {
		return false;
	}

	if ( !g_pPluginEntityFactories->Init( g_pGameConf, error, maxlength ) ) {
		return false;
	}

	if ( !g_pBaseNPCPluginActionFactories->Init( g_pGameConf, error, maxlength ) ) {
		return false;
	}

	g_pForwardEventKilled = forwards->CreateForward("CBaseCombatCharacter_EventKilled", ET_Event, 9, nullptr, Param_Cell, Param_CellByRef, Param_CellByRef, Param_FloatByRef, Param_CellByRef, Param_CellByRef, Param_Array, Param_Array, Param_Cell);
	g_pBaseNPCFactory = new CBaseNPCFactory;
	
	int iOffset = 0;
	GETGAMEDATAOFFSET("CBaseEntity::Event_Killed", iOffset);
	SH_MANUALHOOK_RECONFIGURE(MEvent_Killed, iOffset, 0, 0);
	
	CREATEHANDLETYPE(AreasCollector);

	sharesys->AddDependency(myself, "bintools.ext", true, true);
	sharesys->AddDependency(myself, "sdktools.ext", true, true);
	sharesys->AddDependency(myself, "sdkhooks.ext", true, true);
	sharesys->RegisterLibrary(myself, "cbasenpc");
	sharesys->AddInterface(myself, g_pBaseNPCTools);
	
	gNatives.reserve(1000);
	natives::setup(gNatives);
	gNatives.push_back({nullptr, nullptr});
	sharesys->AddNatives(myself, gNatives.data());

	g_pSM->LogMessage(myself, "Registered %d natives.", gNatives.size() - 1);

	SetDefLessFunc(g_EntitiesHooks);
	return true;
}

bool CBaseNPCExt::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late) {
	GET_V_IFACE_ANY(GetServerFactory, gameents, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS);
	GET_V_IFACE_ANY(GetEngineFactory, enginetrace, IEngineTrace, INTERFACEVERSION_ENGINETRACE_SERVER);
	GET_V_IFACE_ANY(GetServerFactory, servertools, IServerTools, VSERVERTOOLS_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, staticpropmgr, IStaticPropMgrServer, INTERFACEVERSION_STATICPROPMGR_SERVER);
	GET_V_IFACE_ANY(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, mdlcache, IMDLCache, MDLCACHE_INTERFACE_VERSION);

	ConVar_Register(0, this);

	sourcemod_version = g_pCVar->FindVar("sourcemod_version");

	g_cvDeveloper = g_pCVar->FindVar("developer");

	NextBotSpeedLookAheadRange = g_pCVar->FindVar("nb_speed_look_ahead_range");
	NextBotGoalLookAheadRange = g_pCVar->FindVar("nb_goal_look_ahead_range");
	NextBotLadderAlignRange = g_pCVar->FindVar("nb_ladder_align_range");
	NextBotAllowAvoiding = g_pCVar->FindVar("nb_allow_avoiding");
	NextBotAllowClimbing = g_pCVar->FindVar("nb_allow_climbing");
	NextBotAllowGapJumping = g_pCVar->FindVar("nb_allow_gap_jumping");
	NextBotDebugClimbing = g_pCVar->FindVar("nb_debug_climbing");
	NextBotDebugHistory = g_pCVar->FindVar("nb_debug_history");
	NextBotPathDrawIncrement = g_pCVar->FindVar("nb_path_draw_inc");
	NextBotPathSegmentInfluenceRadius = g_pCVar->FindVar("nb_path_segment_influence_radius");
	NextBotPlayerStop = g_pCVar->FindVar("nb_player_stop");
	NextBotStop = g_pCVar->FindVar("nb_stop");
	nav_solid_props = g_pCVar->FindVar("nav_solid_props");
	nb_update_framelimit = g_pCVar->FindVar("nb_update_framelimit");
	nb_update_maxslide = g_pCVar->FindVar("nb_update_maxslide");

	g_pSharedChangeInfo = engine->GetSharedEdictChangeInfo();
	gpGlobals = ismm->GetCGlobals();
	return true;
}

bool CBaseNPCExt::RegisterConCommandBase(ConCommandBase* var) {
	/* Always call META_REGCVAR instead of going through the engine. */
	return META_REGCVAR(var);
}

void CBaseNPCExt::OnCoreMapStart(edict_t* edictlist, int edictCount, int clientMax) {
}

void CBaseNPCExt::OnCoreMapEnd() {
	g_pBaseNPCPluginActionFactories->OnCoreMapEnd();
	g_pPluginEntityFactories->OnCoreMapEnd();
	CNavMesh::OnCoreMapEnd();
}

void CBaseNPCExt::OnEntityCreated(CBaseEntity* pEntity, const char* classname) {
}

void CBaseNPCExt::OnEntityDestroyed(CBaseEntity* pEntity) {
	if (!pEntity) {
		return;
	}

	g_pBaseNPCTools->DeleteNPCByEntIndex(gamehelpers->EntityToBCompatRef(pEntity));

	auto iIndex = g_EntitiesHooks.Find(gamehelpers->EntityToReference(pEntity));
	if (g_EntitiesHooks.IsValidIndex(iIndex)) {
		int iHookID = g_EntitiesHooks.Element(iIndex);
		SH_REMOVE_HOOK_ID(iHookID);
		g_EntitiesHooks.RemoveAt(iIndex);
	}
}

// https://github.com/alliedmodders/sourcemod/blob/6928d21bcf746920b0f2f54e2c28b34097a66be2/core/logic/HandleSys.h#L103
struct QHandleType {
	IHandleTypeDispatch *dispatch;
	unsigned int freeID;
	unsigned int children;
	TypeAccess typeSec;
	HandleAccess hndlSec;
	unsigned int opened;
	std::unique_ptr<std::string> name;
};

// https://github.com/alliedmodders/sourcemod/blob/6928d21bcf746920b0f2f54e2c28b34097a66be2/core/logic/HandleSys.h#L125
struct HandleSystemHack {
	void** vptr;
	void *m_Handles;
	QHandleType *m_Types;
};

void CBaseNPCExt::SDK_OnAllLoaded() {
	SM_GET_LATE_IFACE(BINTOOLS, g_pBinTools);
	SM_GET_LATE_IFACE(SDKTOOLS, g_pSDKTools);
	SM_GET_LATE_IFACE(SDKHOOKS, g_pSDKHooks);

	handlesys->FindHandleType("KeyValues", &g_KeyValueType);

	// HACK: Get g_pCoreIdent from KeyValues QHandleType
	// g_KeyValueType is an index of QHandleType array m_Types
	// https://github.com/alliedmodders/sourcemod/blob/6928d21bcf746920b0f2f54e2c28b34097a66be2/core/logic/HandleSys.cpp#L254
	g_pCoreIdent = reinterpret_cast< HandleSystemHack * >(handlesys)->m_Types[g_KeyValueType].typeSec.ident;

	if (g_pSDKHooks) {
		g_pSDKHooks->AddEntityListener(this);
	} 

	g_pEntityList = (CBaseEntityList *)gamehelpers->GetGlobalEntityList();

	g_pPluginEntityFactories->SDK_OnAllLoaded();

	CBaseNPC_Entity* npc = static_cast<CBaseNPC_Entity*>(servertools->CreateEntityByName("base_npc"));
	if (npc) {
		if (npc->GetNPC()->GetID() == INVALID_NPC_ID) {
			g_pSM->LogError(myself, "Dummy NPC has no id!");
		}

		if (npc->MyNextBotPointer() == nullptr) {
			g_pSM->LogError(myself, "Dummy NPC has no nextbot interface!");
		}

		if (npc->GetNPC()->m_pMover == nullptr || npc->MyNextBotPointer()->GetLocomotionInterface() == nullptr) {
			g_pSM->LogError(myself, "Dummy NPC has no locomotion interface!");
		}
		
		if (npc->GetNPC()->m_pBody == nullptr || npc->MyNextBotPointer()->GetBodyInterface() == nullptr) {
			g_pSM->LogError(myself, "Dummy NPC has no body interface!");
		}

		servertools->RemoveEntityImmediate(npc);
		g_pSM->LogMessage(myself, "Successfully created & destroyed dummy NPC");
	} else {
		g_pSM->LogError(myself, "Failed to create dummy NPC!");
	}
}

bool CBaseNPCExt::QueryRunning(char* error, size_t maxlength) {
	SM_CHECK_IFACE(BINTOOLS, g_pBinTools);
	SM_GET_LATE_IFACE(SDKHOOKS, g_pSDKHooks);
	return true;
}

bool CBaseNPCExt::QueryInterfaceDrop(SMInterface* interface) {
	if (interface == g_pBinTools) {
		g_pBinTools = nullptr;
		return false;
	}

	if (interface == g_pSDKHooks) {
		g_pSDKHooks = nullptr;
		return false;
	}

	if (interface == g_pSDKTools) {
		g_pSDKTools = nullptr;
		return false;
	}

	return IExtensionInterface::QueryInterfaceDrop(interface);
}

void CBaseNPCExt::NotifyInterfaceDrop(SMInterface* interface) {
	if (strcmp(interface->GetInterfaceName(), SMINTERFACE_BINTOOLS_NAME) == 0) {
		g_pBinTools = nullptr;
	}
}

void CBaseNPCExt::SDK_OnUnload() {
	gameconfs->CloseGameConfigFile(g_pGameConf);
	forwards->ReleaseForward(g_pForwardEventKilled);

	Tools_Refresh_Shutdown();

	g_pBaseNPCPluginActionFactories->SDK_OnUnload();
	g_pPluginEntityFactories->SDK_OnUnload();

	delete g_pBaseNPCFactory;
	g_pBaseNPCFactory = nullptr;
	
	if (g_pSDKHooks) {
		g_pSDKHooks->RemoveEntityListener(this);
	}

	CNavMesh::SDK_OnUnload();
	
	FOR_EACH_MAP_FAST(g_EntitiesHooks, iHookID)
		SH_REMOVE_HOOK_ID(iHookID);
}

//Fix external stuff error
float IntervalTimer::Now( void ) const {
	return gpGlobals->curtime;
}

float CountdownTimer::Now( void ) const {
	return gpGlobals->curtime;
}

bool CGameTrace::DidHitWorld() const {
	return gamehelpers->EntityToBCompatRef(reinterpret_cast<CBaseEntity *>(m_pEnt)) == 0;
}

bool CGameTrace::DidHitNonWorldEntity() const {
	return m_pEnt != nullptr && !DidHitWorld();
}

float UTIL_VecToYaw(const Vector& vec) {
	if (vec.y == 0 && vec.x == 0) {
		return 0;
	}

	float yaw = atan2(vec.y, vec.x);

	yaw = RAD2DEG(yaw);

	if (yaw < 0) {
		yaw += 360;
	}

	return yaw;
}
