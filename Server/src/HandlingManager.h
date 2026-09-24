#pragma once
#include "CVehicleManager.hpp"
#include "HandlingEnum.h"
#include "HandlingStruct.h"
#include "ModelConfigParser.h"
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

extern std::unordered_map<uint16_t, struct stVehicleHandlingEntry> vehicleHandlings;
extern std::unordered_map<uint16_t, struct stHandlingEntry> playerHandlings; // key = playerid
extern std::unordered_map<uint32_t, CustomVeh::Protocol::VehicleDefinition> customVehicleDefs; // key = modelId
extern std::unordered_set<uint32_t> customVehicleModels;
extern std::unordered_map<uint16_t, uint8_t> vehicleDoorStates; // key = vehicleid, value = bitmask of missing doors (bits 0..5)
extern std::unordered_map<uint32_t, ModelConfig> customVehicleConfigs; // key = modelId

stHandlingEntry* GetModelHandlingEntry(uint32_t modelid);
void __WriteHandlingEntryToBitStream(NetworkBitStream* bs, const struct stHandlingEntry& entry);

void ProcessTick();
void BroadcastVehicleCorrection(uint16_t vehicleid);

void InitializeModelHandlings();
void OnCreateVehicle(int vehicleid);
void OnDestroyVehicle(int vehicleid);
void OnPlayerAuthorized(IPlayer& player); // call this from ACTION_INIT handler so model handling modifications are sent to the authorized player
void OnPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason);
void OnVehicleStreamIn(IVehicle& vehicle, IPlayer& player); // call from OnVehicleStreamIn so handling modifications for this individual vehicle are sent to the player

bool ResetModelHandling(int modelid); // resets model handling to it's default one, NOTE: this resets any handling modifications for every vehicle of that model
void ResetVehicleHandling(IVehicle& vehicle, bool sendToPlayers = true); // resets vehicle handling to it's model handling (and clears the modifications)

bool SetVehicleDoorMissing(uint16_t vehicleid, uint8_t doorid, bool missing);
bool GetVehicleDoorMissing(uint16_t vehicleid, uint8_t doorid, bool& missing);
bool SetVehicleAllDoorsMissing(uint16_t vehicleid, bool missing);
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
bool SetCustomVehicleModelInfo(uint32_t customModelId, uint8_t vehicleClass, int16_t wheelModelId, float wheelScaleFront, float wheelScaleRear, uint16_t frequency, uint8_t level, uint8_t comprate, uint8_t numExtras, uint8_t wheelUpgradeClass = 0);
bool SetCustomVehicleWheelModel(uint32_t customModelId, int16_t wheelModelId);
bool SetCustomVehicleWheelScale(uint32_t customModelId, float wheelScaleFront, float wheelScaleRear);
bool GetCustomVehicleWheelModel(uint32_t customModelId, int16_t& wheelModelId);
bool GetCustomVehicleWheelScale(uint32_t customModelId, float& wheelScaleFront, float& wheelScaleRear);
bool CommitCustomVehicleDef(uint32_t customModelId);
bool IsCustomVehicle(uint32_t modelId);
void SendCustomVehicleDefToPlayer(IPlayer& player, uint32_t modelId);
void SendCustomVehicleDefToAll(uint32_t modelId);
void SendCustomVehicleDestroyToPlayer(IPlayer& player, uint32_t modelId);
void SendCustomVehicleDestroyToAll(uint32_t modelId);

bool LoadCustomVehicleConfig(uint32_t customModelId);
bool DefineCustomVehicleFromConfig(uint32_t customModelId, uint32_t defaultVisualBase = 411);
int LoadAllCustomVehicles(uint32_t defaultVisualBase = 411);

bool GetCustomVehicleName(uint32_t customModelId, std::string& name);
bool SetCustomVehicleName(uint32_t customModelId, const std::string& name);

bool GetCustomVehicleConfigString(uint32_t customModelId, const std::string& key, std::string& outValue);
bool SetCustomVehicleConfigString(uint32_t customModelId, const std::string& key, const std::string& value);

bool GetCustomVehicleConfigInt(uint32_t customModelId, const std::string& key, int& outValue);
bool SetCustomVehicleConfigInt(uint32_t customModelId, const std::string& key, int value);

bool GetCustomVehicleConfigFloat(uint32_t customModelId, const std::string& key, float& outValue);
bool SetCustomVehicleConfigFloat(uint32_t customModelId, const std::string& key, float value);

const std::vector<std::array<uint8_t, 4>>* GetCustomVehicleColorVariations(uint32_t customModelId);
const std::vector<int>* GetCustomVehicleAllowedUpgrades(uint32_t customModelId);

int GetCustomVehicleColorVariationsCount(uint32_t customModelId);
bool GetCustomVehicleColorVariation(uint32_t customModelId, int index, int& p, int& s, int& t, int& q);
bool GetCustomVehicleDefaultColors(uint32_t customModelId, int& p, int& s, int& t, int& q);
int GetCustomVehicleAllowedUpgradesCount(uint32_t customModelId);
int GetCustomVehicleAllowedUpgrade(uint32_t customModelId, int index);

bool SetCustomVehicleLightingOffset(uint32_t customModelId, float hlX, float hlY, float hlZ, float tlX = 0.0f, float tlY = 0.0f, float tlZ = 0.0f);
bool GetCustomVehicleLightingOffset(uint32_t customModelId, float& hlX, float& hlY, float& hlZ, float& tlX, float& tlY, float& tlZ);
}
