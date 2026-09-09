#pragma once
#include "CVehicleManager.hpp"
#include "HandlingEnum.h"
#include "HandlingStruct.h"
#include "../../Shared/CustomVehicleProtocol.hpp"

#include <Impl/network_impl.hpp>
#include <sdk.hpp>
#include <RakNet/Encoding/str_compress.hpp>
#include <RakNet/bitstream.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace HandlingMgr
{
#pragma pack(push, 1)
struct stHandlingMod
{
	CHandlingAttribType type;
	union
	{
		float fval;
		unsigned int uival;
		uint8_t bval;
	};
};

struct stHandlingEntry
{
	struct tHandlingData handlingData;
	std::unordered_map<CHandlingAttrib, struct stHandlingMod, std::hash<uint8_t>> handlingModMap; // modifications are saved here so we only send things that have changed
};

struct stVehicleHandlingEntry : stHandlingEntry
{
	struct stHandlingEntry* modelHandling = nullptr;
	bool usesModelHandling = false; // set to true under OnCreateVehicle, set to false as soon as you change any handling attribute for this vehicle
};
#pragma pack(pop)

extern std::unordered_map<uint16_t, struct stVehicleHandlingEntry> vehicleHandlings;
extern std::unordered_map<uint16_t, struct stHandlingEntry> playerHandlings; // key = playerid
extern std::unordered_map<uint32_t, CustomVeh::Protocol::VehicleDefinition> customVehicleDefs; // key = modelId
extern std::unordered_set<uint32_t> customVehicleModels;

stHandlingEntry* GetModelHandlingEntry(uint32_t modelid);

void ProcessTick();

void InitializeModelHandlings();
void OnCreateVehicle(int vehicleid);
void OnDestroyVehicle(int vehicleid);
void OnPlayerConnect(IPlayer& player); // call this from OnPlayerConnect (or rather from ACTION_INIT handler) so model handling modifications are sent to the player
void OnPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason);
void OnVehicleStreamIn(IVehicle& vehicle, IPlayer& player); // call from OnVehicleStreamIn so handling modifications for this individual vehicle are sent to the player

bool ResetModelHandling(int modelid); // resets model handling to it's default one, NOTE: this resets any handling modifications for every vehicle of that model
void ResetVehicleHandling(IVehicle& vehicle, bool sendToPlayers = true); // resets vehicle handling to it's model handling (and clears the modifications)
bool SetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, float value);
bool SetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, unsigned int value);
bool SetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, uint8_t value);

bool SetModelHandling(uint16_t modelid, CHandlingAttrib attrib, float value);
bool SetModelHandling(uint16_t modelid, CHandlingAttrib attrib, unsigned int value);
bool SetModelHandling(uint16_t modelid, CHandlingAttrib attrib, uint8_t value);

bool GetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, float& ret);
bool GetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, unsigned int& ret);
bool GetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, uint8_t& ret);

bool GetModelHandling(uint16_t modelid, CHandlingAttrib attrib, float& ret);
bool GetModelHandling(uint16_t modelid, CHandlingAttrib attrib, unsigned int& ret);
bool GetModelHandling(uint16_t modelid, CHandlingAttrib attrib, uint8_t& ret);

bool GetDefaultHandling(uint16_t modelid, CHandlingAttrib attrib, float& ret);
bool GetDefaultHandling(uint16_t modelid, CHandlingAttrib attrib, unsigned int& ret);
bool GetDefaultHandling(uint16_t modelid, CHandlingAttrib attrib, uint8_t& ret);

bool SetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, float value);
bool SetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, unsigned int value);
bool SetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, uint8_t value);
bool ResetPlayerHandling(uint16_t playerid);
bool GetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, float& ret);
bool GetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, unsigned int& ret);
bool GetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, uint8_t& ret);

bool ResetAll(uint16_t playerid);

void UnregisterCustomVehicle(uint32_t customModelId);
void BeginCustomVehicleDef(uint32_t customModelId, uint32_t visualBase, uint32_t audioBase, uint32_t handlingBase, CustomVeh::Protocol::EngineSound engineSoundId);
bool SetCustomVehicleDff(uint32_t customModelId);
bool SetCustomVehicleTxd(uint32_t customModelId);
bool SetCustomVehicleCol(uint32_t customModelId);
bool CommitCustomVehicleDef(uint32_t customModelId);
bool IsCustomVehicle(uint32_t modelId);
void SendCustomVehicleDefToPlayer(IPlayer& player, uint32_t modelId);
void SendCustomVehicleDefToAll(uint32_t modelId);
void SendCustomVehicleDestroyToPlayer(IPlayer& player, uint32_t modelId);
void SendCustomVehicleDestroyToAll(uint32_t modelId);
}
