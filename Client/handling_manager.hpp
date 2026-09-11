#pragma once

// SDK
#include <game_sa/cHandlingDataMgr.h>
#include <plugin_sa.h>
#include <RenderWare.h>
#include <shared/game/CVector.h>

#include <RakHook/rakhook.hpp>
#include <RakHook/samp.hpp>
#include <RakNet/BitStream.h>
#include <RakNet/PacketEnumerations.h>
#include <RakNet/StringCompressor.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "defs.h"

#include "ModelTransferClient.h"

#include "../Shared/CustomVehicleProtocol.hpp"

// Legacy Enums
using CustomVehAction = CustomVeh::Protocol::Action;

inline constexpr CustomVehAction ACTION_INIT = CustomVehAction::Init;
inline constexpr CustomVehAction ACTION_INIT_RESPONSE = CustomVehAction::InitResponse;
inline constexpr CustomVehAction ACTION_RESET_MODEL = CustomVehAction::ResetModel;
inline constexpr CustomVehAction ACTION_RESET_VEHICLE = CustomVehAction::ResetVehicle;
inline constexpr CustomVehAction ACTION_SET_VEHICLE_HANDLING = CustomVehAction::SetVehicleHandling;
inline constexpr CustomVehAction ACTION_SET_MODEL_HANDLING = CustomVehAction::SetModelHandling;
inline constexpr CustomVehAction ACTION_SET_PLAYER_HANDLING = CustomVehAction::SetPlayerHandling;
inline constexpr CustomVehAction ACTION_RESET_PLAYER_HANDLING = CustomVehAction::ResetPlayerHandling;
inline constexpr CustomVehAction ACTION_GET_VEHICLE_HANDLING = CustomVehAction::GetVehicleHandling;
inline constexpr CustomVehAction ACTION_GET_MODEL_HANDLING = CustomVehAction::GetModelHandling;
inline constexpr CustomVehAction ACTION_GET_PLAYER_HANDLING = CustomVehAction::GetPlayerHandling;
inline constexpr CustomVehAction ACTION_RESET_ALL = CustomVehAction::ResetAll;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_DEFINE = CustomVehAction::CustomVehicleDefine;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_BIND = CustomVehAction::CustomVehicleBind;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_UNBIND = CustomVehAction::CustomVehicleUnbind;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_DESTROY = CustomVehAction::CustomVehicleDestroy;
inline constexpr CustomVehAction ACTION_ASSET_MANIFEST = CustomVehAction::AssetManifest;
inline constexpr CustomVehAction ACTION_ASSET_REQUEST = CustomVehAction::AssetRequest;
inline constexpr CustomVehAction ACTION_ASSET_RESUME = CustomVehAction::AssetResume;
inline constexpr CustomVehAction ACTION_ASSET_BEGIN = CustomVehAction::AssetBegin;
inline constexpr CustomVehAction ACTION_ASSET_CHUNK = CustomVehAction::AssetChunk;
inline constexpr CustomVehAction ACTION_ASSET_END = CustomVehAction::AssetEnd;
inline constexpr CustomVehAction ACTION_ASSET_VERIFIED = CustomVehAction::AssetVerified;
inline constexpr CustomVehAction ACTION_ASSET_CANCEL = CustomVehAction::AssetCancel;
inline constexpr CustomVehAction ACTION_ASSET_REJECTED = CustomVehAction::AssetRejected;
inline constexpr CustomVehAction ACTION_ASSET_READY = CustomVehAction::AssetReady;

enum CHandlingAttrib : unsigned char {
	HANDL_UIDENTIFIER = 1,
	HANDL_FMASS,
	HANDL_FTURNMASS,
	HANDL_FDRAGMULTIPLIER,
	HANDL_CENTREOFMASS_X,
	HANDL_CENTREOFMASS_Y,
	HANDL_CENTREOFMASS_Z,
	HANDL_NPERCENTSUBMERGED,
	HANDL_FTRACTIONMULTIPLIER,
	HANDL_FTRACTIONLOSS,
	HANDL_FTRACTIONBIAS,
	HANDL_TR_NNUMBEROFGEARS,
	HANDL_TR_FMAXVELOCITY,
	HANDL_TR_FENGINEACCELERATION,
	HANDL_TR_FENGINEINERTIA,
	HANDL_TR_NDRIVETYPE,
	HANDL_TR_NENGINETYPE,
	HANDL_FBRAKEDECELERATION,
	HANDL_FBRAKEBIAS,
	HANDL_BABS,
	HANDL_FSTEERINGLOCK,
	HANDL_FSUSPENSIONFORCELEVEL,
	HANDL_FSUSPENSIONDAMPINGLEVEL,
	HANDL_FSUSPENSIONHIGHSPDCOMDAMP,
	HANDL_FSUSPENSIONUPPERLIMIT,
	HANDL_FSUSPENSIONLOWERLIMIT,
	HANDL_FSUSPENSIONBIAS,
	HANDL_FSUSPENSIONANTIDIVEMULT,
	HANDL_FSEATOFFSETDISTANCE,
	HANDL_FCOLLISIONDAMAGEMULT,
	HANDL_UIMONETARYVALUE,
	HANDL_MODELFLAGS,
	HANDL_HANDLINGFLAGS,
	HANDL_FRONTLIGHTS,
	HANDL_REARLIGHTS,
	HANDL_ANIMGROUP
};

enum CHandlingAttribType : unsigned char {
	TYPE_NONE,
	TYPE_UINT,
	TYPE_FLOAT,
	TYPE_BYTE,
	TYPE_FLAG
};

class HandlingManager {
public:
	static std::unordered_map<CVehicle*, uint16_t> m_vehicleToSAMPIdCache;
	static std::unordered_map<uint32_t, int32_t> m_modelUseCount;
	static std::mutex m_cacheMutex; // separate mutex for cache operations

	// Methods to update cache and ref counts
	static void CacheVehicleSAMPId(CVehicle* pVehicle, uint16_t sampId);
	static void RemoveVehicleFromCache(CVehicle* pVehicle);
	static void IncrementModelUse(uint32_t modelId);
	static void DecrementModelUse(uint32_t modelId);
	static int32_t GetModelUseCount(uint32_t modelId);

	struct HandlingAttribEntry {
		CHandlingAttrib attrib;
		CHandlingAttribType type;
		union {
			float f;
			uint32_t u;
			uint8_t b;
		} value;
	};

	// Static members – defined in .cpp
	static std::map<uint16_t, std::unique_ptr<tHandlingData>> m_vehicleHandlings;
	static std::map<uint16_t, std::unique_ptr<tHandlingData>> m_modelHandlings;
	static std::map<CVehicle*, std::unique_ptr<tHandlingData>> m_customHandlings;
	static std::map<uint16_t, std::unique_ptr<tHandlingData>> m_playerHandlings;

	// Structure for player‑applied handlings on vehicles
	struct PlayerAppliedInfo {
		uint16_t playerId;
		std::unique_ptr<tHandlingData> handling;
	};
	static std::map<uint16_t, PlayerAppliedInfo> m_playerAppliedHandlings;

	static bool m_isServerAuthorized; // declared, defined in .cpp
	static std::recursive_mutex m_handlingMutex;

	// Direct CVehicle* functions (public, but must be called with lock if they modify)
	static void ModifyMass(CVehicle* pVehicle, float mass);
	static void ModifyTransmission(CVehicle* pVehicle, float maxSpeed, float acceleration, int gears);
	static void ResetVehicleHandling(CVehicle* pVehicle);

	// Network packet processors
	static bool ProcessAction(CustomVehAction action, RakNet::BitStream* bs);
	static void ProcessVehicleMods(uint16_t sampVehicleId, const std::vector<HandlingAttribEntry>& entries);
	static void ProcessModelMods(uint16_t modelId, const std::vector<HandlingAttribEntry>& entries);
	static void ProcessPlayerMods(uint16_t playerId, const std::vector<HandlingAttribEntry>& entries);

	// Reset functions
	static void ResetVehicle(uint16_t sampVehicleId);
	static void ResetModel(uint16_t modelId);
	static void ResetPlayerHandling(uint16_t playerId);

	// Player‑applied handling (called on vehicle entry/exit)
	static void ApplyPlayerHandling(uint16_t playerId, CVehicle* pVehicle);
	static void RemovePlayerHandling(uint16_t playerId, CVehicle* pVehicle);

	// Cleanup
	static void OnVehicleDestructor(CVehicle* pVehicle);

	// Attribute helpers
	static bool CanSetHandlingAttrib(CHandlingAttrib attribute);
	static CHandlingAttribType GetHandlingAttribType(CHandlingAttrib attribute);
	static void* ResolveAttributePointer(tHandlingData* handling, uint8_t attribId);

private:
	// Internal utilities
	static void ApplyModelToVehicles(uint16_t modelId, tHandlingData* handling);
	static void RevertModelToOriginal(uint16_t modelId);

public:
	static uint16_t GetVehicleSAMPId(CVehicle* pVehicle);

private:
	static tHandlingData* ResolveFallbackHandling(uint16_t vehicleId, int modelId);
	static tHandlingData* ResolveFallbackHandling(CVehicle* pVehicle, uint16_t vehicleId, int modelId);

private:
	enum class PendingCommandType : uint8_t {
		SetVehicle,
		SetModel,
		SetPlayer,
		ResetVehicle,
		ResetModel,
		ResetPlayer
	};

	struct PendingCommand {
		PendingCommandType type;
		uint16_t id; // vehicleId / modelId / playerId depending on `type`
		std::vector<HandlingAttribEntry> attribs; // empty for Reset* commands
	};

	static std::deque<PendingCommand> m_pendingCommands;
	static std::mutex m_pendingMutex;

	static void QueueCommand(PendingCommand cmd);
	static std::vector<HandlingAttribEntry> ParseAttribEntries(uint8_t count, RakNet::BitStream* bs);
	static void ApplyAttribEntries(tHandlingData* handling, const std::vector<HandlingAttribEntry>& entries);

	static void IsolateVehicleHandling(CVehicle* pVehicle);

public:
	static void ProcessPendingCommands();

	static void SendHandlingPacket(CustomVehAction action, RakNet::BitStream* bs);
};
