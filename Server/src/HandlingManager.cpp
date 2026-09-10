#include "HandlingManager.h"
#include "Actions.h"
#include "CPlayer.h"
#include "CVehicleManager.hpp"
#include "HandlingDefault.h"
#include "PacketEnum.h"
#include "extendedveh.h"

#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "utils.h"

namespace HandlingMgr
{
std::array<stHandlingEntry, CVehicleMgr::BASE_MAX_VEHICLE_MODELS> gBaseModelHandlings;
std::unordered_map<uint32_t, stHandlingEntry> gCustomModelHandlings;

std::unordered_map<uint16_t, struct stVehicleHandlingEntry> vehicleHandlings;
std::unordered_map<uint16_t, struct stHandlingEntry> playerHandlings; // key = playerid
std::unordered_map<uint32_t, CustomVeh::Protocol::VehicleDefinition> customVehicleDefs; // key = modelId
std::unordered_set<uint32_t> customVehicleModels;
std::unordered_map<uint32_t, CustomVeh::Protocol::VehicleDefinition> stagedCustomVehicleDefs;

std::unordered_set<uint16_t> usOutgoingVehicleMods;
std::unordered_set<uint32_t> usOutgoingModelMods;

std::unordered_map<int, IVehicle*> vehiclesIdMap;

stHandlingEntry* GetModelHandlingEntry(uint32_t modelid)
{
	if (CVehicleMgr::IsBaseVehicleModel(modelid))
	{
		return &gBaseModelHandlings[CVehicleMgr::GetBaseModelIndex(modelid)];
	}
	auto it = gCustomModelHandlings.find(modelid);
	if (it != gCustomModelHandlings.end())
	{
		return &it->second;
	}
	if (CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
	{
		stHandlingEntry entry;
		HandlingDefault::copyDefaultModelHandling(modelid, &entry.handlingData);
		auto [insertedIt, _] = gCustomModelHandlings.emplace(modelid, std::move(entry));
		return &insertedIt->second;
	}
	return nullptr;
}

/*
 *  INTERNAL FUNCTIONS
 */
void __WriteHandlingEntryToBitStream(NetworkBitStream* bs, const struct stHandlingEntry entry)
{
	bs->Write((uint8_t)entry.handlingModMap.size());

	for (auto const& i : entry.handlingModMap)
	{
		bs->Write((uint8_t)i.first); // attribute
		bs->Write((uint8_t)i.second.type);
		switch (i.second.type)
		{
		case TYPE_BYTE:
			bs->Write(i.second.bval);
			break;
		case TYPE_UINT:
		case TYPE_FLAG:
			bs->Write(i.second.uival);
			break;
		case TYPE_FLOAT:
			bs->Write(i.second.fval);
			break;
		case TYPE_NONE:
			break;
		}
	}
}

void __addMod(struct stHandlingEntry* handling, CHandlingAttrib attribute, const struct stHandlingMod mod)
{
	if (handling->handlingModMap.count(attribute))
		handling->handlingModMap.at(attribute) = mod;
	else
		handling->handlingModMap.emplace(attribute, mod);

	void* offs = GetHandlingAttribPtr(&handling->handlingData, attribute);
	if (!offs)
		return;

	/* write the value to the handling data so we can Get it later on */
	switch (mod.type)
	{
	case TYPE_FLOAT:
		*(float*)offs = mod.fval;
		break;
	case TYPE_UINT:
	case TYPE_FLAG:
		*(unsigned int*)offs = mod.uival;
		break;
	case TYPE_BYTE:
		*(uint8_t*)offs = mod.bval;
		break;
	case TYPE_NONE:
		break;
	}
}

template <typename T>
bool IsHandlingType(CHandlingAttrib attrib, ICore* core)
{
	const CHandlingAttribType actualType = GetHandlingAttributeType(attrib);
	const bool validType = [&] {
		if constexpr (std::is_same_v<T, float>)
			return actualType == TYPE_FLOAT;
		if constexpr (std::is_same_v<T, unsigned int>)
			return actualType == TYPE_UINT || actualType == TYPE_FLAG;
		return actualType == TYPE_BYTE;
	}();

	if (!validType && core)
	{
		core->logLn(LogLevel::Error, "[ExtendedVeh] Invalid type specified for attribute %d", attrib);
	}
	return validType;
}

template <typename T>
bool IsValidHandlingValue(CHandlingAttrib attrib, T value)
{
	if constexpr (std::is_same_v<T, float> || std::is_same_v<T, uint8_t>)
		return ::IsValidHandlingValue(attrib, value);
	return true;
}

template <typename T>
stHandlingMod MakeHandlingMod(T value)
{
	stHandlingMod mod {};
	if constexpr (std::is_same_v<T, float>)
	{
		mod.type = TYPE_FLOAT;
		mod.fval = value;
	}
	else if constexpr (std::is_same_v<T, uint8_t>)
	{
		mod.type = TYPE_BYTE;
		mod.bval = value;
	}
	else
	{
		mod.type = TYPE_UINT;
		mod.uival = value;
	}
	return mod;
}

template <typename T>
bool SetHandlingValue(stHandlingEntry& entry, CHandlingAttrib attrib, T value)
{
	__addMod(&entry, attrib, MakeHandlingMod(value));
	return true;
}

template <typename T>
bool GetHandlingValue(const tHandlingData& handlingData, CHandlingAttrib attrib, T& value)
{
	void* ptr = GetHandlingAttribPtr(const_cast<tHandlingData*>(&handlingData), attrib);
	if (!ptr)
		return false;

	if constexpr (std::is_same_v<T, float>)
		value = *static_cast<float*>(ptr);
	else if constexpr (std::is_same_v<T, uint8_t>)
		value = *static_cast<uint8_t*>(ptr);
	else
		value = *static_cast<unsigned int*>(ptr);
	return true;
}

stHandlingEntry& GetPlayerHandlingEntry(uint16_t playerid)
{
	auto [it, inserted] = playerHandlings.try_emplace(playerid);
	if (inserted)
	{
		it->second.handlingData = gBaseModelHandlings[0].handlingData;
		it->second.handlingModMap.clear();
	}
	return it->second;
}

void SendPlayerHandling(uint16_t playerid, const stHandlingEntry& entry)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return;

	CustomVehActionPacket packet(ACTION_SET_PLAYER_HANDLING);
	packet.data.Write(playerid);
	__WriteHandlingEntryToBitStream(&packet.data, entry);
	IPlayer* player = compo->GetPlayerByID(playerid);
	if (player)
		player->sendPacket(Span<uint8_t>(packet.data.GetData(), packet.data.GetNumberOfBytesUsed()), 0, true);
}

const stHandlingEntry* GetVehicleHandlingEntry(uint16_t vehicleid)
{
	auto it = vehicleHandlings.find(vehicleid);
	if (it == vehicleHandlings.end())
		return nullptr;
	if (it->second.usesModelHandling)
		return it->second.modelHandling;
	return &it->second;
}

bool __AddModelHandlingMod(uint16_t modelid, CHandlingAttrib attribute, const struct stHandlingMod mod)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;

	stHandlingEntry* entry = GetModelHandlingEntry(modelid);
	if (!entry)
		return false;

	__addMod(entry, attribute, mod);

	usOutgoingModelMods.emplace(modelid);
	return true;
}

bool __AddVehicleHandlingMod(uint16_t vehicleid, CHandlingAttrib attribute, const struct stHandlingMod mod)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo->IsValidVehicle(vehicleid))
		return false;

	auto& vEntry = vehicleHandlings[vehicleid];
	// copy the handling of the model & apply the changed value
	if (vEntry.usesModelHandling)
	{
		vEntry.usesModelHandling = false;
		if (vEntry.modelHandling)
		{
			memcpy(&vEntry.handlingData, &vEntry.modelHandling->handlingData, sizeof(struct tHandlingData));
		}
	}
	__addMod(&vEntry, attribute, mod);

	usOutgoingVehicleMods.emplace(vehicleid);
	return true;
}

/* -------------------------------------------------------------------------------------------------------------------- */

/* We use ProcessTick to broadcast queued modifications all at once instead of spamming with packets */
void ProcessTick()
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo->getCore();
	while (!usOutgoingVehicleMods.empty())
	{
		const auto it = usOutgoingVehicleMods.begin();
		uint16_t vehicleid = *it;
		usOutgoingVehicleMods.erase(it);

		auto vIt = vehicleHandlings.find(vehicleid);
		if (!compo->IsValidVehicle(vehicleid) || vIt == vehicleHandlings.end() || vIt->second.usesModelHandling)
		{
			continue;
		}
		struct CustomVehActionPacket p(ACTION_SET_VEHICLE_HANDLING);
		p.data.Write(vehicleid);
		__WriteHandlingEntryToBitStream(&p.data, vIt->second);

		for (IPlayer* player : core_->getPlayers().players())
		{
			player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
		}
	}

	while (!usOutgoingModelMods.empty())
	{
		const auto it = usOutgoingModelMods.begin();
		uint32_t modelid = *it;
		usOutgoingModelMods.erase(it);

		stHandlingEntry* mEntry = GetModelHandlingEntry(modelid);
		if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid) || !mEntry || mEntry->handlingModMap.empty())
		{
			continue;
		}

		struct CustomVehActionPacket p(ACTION_SET_MODEL_HANDLING);
		p.data.Write((uint16_t)modelid);
		__WriteHandlingEntryToBitStream(&p.data, *mEntry);

		for (IPlayer* player : core_->getPlayers().players())
		{
			player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
		}
	}
}

// call right after HandlingDefault::Initialize()
void InitializeModelHandlings()
{
	for (uint16_t i = 0; i < CVehicleMgr::BASE_MAX_VEHICLE_MODELS; i++)
	{
		HandlingDefault::copyDefaultModelHandling(i + 400, &gBaseModelHandlings[i].handlingData);
		gBaseModelHandlings[i].handlingModMap.clear();
	}
	gCustomModelHandlings.clear();
}

void OnCreateVehicle(int vehicleid)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	IVehicle* pVeh = compo->GetVehicleByID(vehicleid);
	if (pVeh)
	{
		vehiclesIdMap[vehicleid] = pVeh;
		ResetVehicleHandling(*pVeh, false);
	}
}

void OnDestroyVehicle(int vehicleid)
{
	IVehicle* pVeh = vehiclesIdMap[vehicleid];
	if (pVeh)
	{
		ResetVehicleHandling(*pVeh, false);
	}
	vehiclesIdMap.erase(vehicleid);
}

void OnPlayerConnect(IPlayer& player)
{
	int playerid = player.getID();
	if (!gPlayers[playerid].hasExtendedVeh())
		return;

	for (uint16_t model = 0; model < CVehicleMgr::BASE_MAX_VEHICLE_MODELS; model++)
	{
		if (!gBaseModelHandlings[model].handlingModMap.empty())
		{
			struct CustomVehActionPacket p(ACTION_SET_MODEL_HANDLING);
			p.data.Write((uint16_t)(model + 400));
			__WriteHandlingEntryToBitStream(&p.data, gBaseModelHandlings[model]);
			player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, false);
		}
	}

	for (const auto& [customModel, entry] : gCustomModelHandlings)
	{
		if (!entry.handlingModMap.empty())
		{
			struct CustomVehActionPacket p(ACTION_SET_MODEL_HANDLING);
			p.data.Write((uint16_t)customModel);
			__WriteHandlingEntryToBitStream(&p.data, entry);
			player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, false);
		}
	}

	for (const auto& kv : customVehicleDefs)
	{
		SendCustomVehicleDefToPlayer(player, kv.first);
	}
}

void OnPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason)
{
	HandlingMgr::ResetPlayerHandling(player.getID());
}

void OnVehicleStreamIn(IVehicle& vehicle, IPlayer& player)
{
	int vehicleid = vehicle.getID();
	int modelid = vehicle.getModel();
	int forplayerid = player.getID();

	if (IsCustomVehicle(modelid))
	{
		SendCustomVehicleDefToPlayer(player, modelid);
	}

	auto it = vehicleHandlings.find(vehicleid);
	if (it == vehicleHandlings.end() || it->second.handlingModMap.empty() || !gPlayers[forplayerid].hasExtendedVeh())
		return;

	struct CustomVehActionPacket p(ACTION_SET_VEHICLE_HANDLING);
	p.data.Write((uint16_t)vehicleid);
	__WriteHandlingEntryToBitStream(&p.data, it->second);
	player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, false);
}

/*
 * Resets handling of specified vehicle model to its original default one
 */
bool ResetModelHandling(int modelid)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;

	stHandlingEntry* mEntry = GetModelHandlingEntry(modelid);
	if (!mEntry)
		return false;

	mEntry->handlingModMap.clear();
	HandlingDefault::copyDefaultModelHandling((uint16_t)modelid, &mEntry->handlingData);

	struct CustomVehActionPacket p(ACTION_RESET_MODEL);
	p.data.Write((uint16_t)modelid);

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo->getCore();
	for (IPlayer* player : core_->getPlayers().players())
	{
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
	}
	return true;
}

/*
 * Resets the handling of specified vehicle to its model handling
 */
void ResetVehicleHandling(IVehicle& vehicle, bool sendToPlayers)
{
	int vehicleid = vehicle.getID();
	int modelid = vehicle.getModel();

	auto& vEntry = vehicleHandlings[vehicleid];
	vEntry.handlingModMap.clear();
	vEntry.modelHandling = GetModelHandlingEntry(modelid);
	vEntry.usesModelHandling = true;

	if (sendToPlayers)
	{
		struct CustomVehActionPacket p(ACTION_RESET_VEHICLE);
		p.data.Write((uint16_t)vehicleid);

		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		ICore* core_ = compo->getCore();
		for (IPlayer* player : core_->getPlayers().players())
		{
			player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
		}
	}
}

/* SET HANDLING FUNCTIONS */

bool SetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, float value)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo->IsValidVehicle(vehicleid) || !CanSetHandlingAttrib(attrib))
		return false;
	if (!IsHandlingType<float>(attrib, compo->getCore()) || !IsValidHandlingValue(attrib, value))
		return false;
	return __AddVehicleHandlingMod(vehicleid, attrib, MakeHandlingMod(value));
}

bool SetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, unsigned int value)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo->IsValidVehicle(vehicleid) || !CanSetHandlingAttrib(attrib))
		return false;
	if (!IsHandlingType<unsigned int>(attrib, compo->getCore()))
		return false;
	return __AddVehicleHandlingMod(vehicleid, attrib, MakeHandlingMod(value));
}

bool SetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, uint8_t value)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo->IsValidVehicle(vehicleid) || !CanSetHandlingAttrib(attrib))
		return false;
	if (!IsHandlingType<uint8_t>(attrib, compo->getCore()) || !IsValidHandlingValue(attrib, value))
		return false;
	return __AddVehicleHandlingMod(vehicleid, attrib, MakeHandlingMod(value));
}

bool SetModelHandling(uint16_t modelid, CHandlingAttrib attrib, float value)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid) || !CanSetHandlingAttrib(attrib))
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!IsHandlingType<float>(attrib, compo->getCore()) || !IsValidHandlingValue(attrib, value))
		return false;
	return __AddModelHandlingMod(modelid, attrib, MakeHandlingMod(value));
}

bool SetModelHandling(uint16_t modelid, CHandlingAttrib attrib, unsigned int value)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid) || !CanSetHandlingAttrib(attrib))
		return false;
	if (!IsHandlingType<unsigned int>(attrib, ExtendedVehCompo::get()->getCore()))
		return false;
	return __AddModelHandlingMod(modelid, attrib, MakeHandlingMod(value));
}

bool SetModelHandling(uint16_t modelid, CHandlingAttrib attrib, uint8_t value)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid) || !CanSetHandlingAttrib(attrib))
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!IsHandlingType<uint8_t>(attrib, compo->getCore()) || !IsValidHandlingValue(attrib, value))
		return false;
	return __AddModelHandlingMod(modelid, attrib, MakeHandlingMod(value));
}

/* GET */

bool GetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, float& ret)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo->IsValidVehicle(vehicleid))
		return false;
	if (!IsHandlingType<float>(attrib, compo->getCore()))
		return false;
	const stHandlingEntry* entry = GetVehicleHandlingEntry(vehicleid);
	return entry && GetHandlingValue(entry->handlingData, attrib, ret);
}

bool GetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, unsigned int& ret)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo->IsValidVehicle(vehicleid))
		return false;
	if (!IsHandlingType<unsigned int>(attrib, compo->getCore()))
		return false;
	const stHandlingEntry* entry = GetVehicleHandlingEntry(vehicleid);
	return entry && GetHandlingValue(entry->handlingData, attrib, ret);
}

bool GetVehicleHandling(uint16_t vehicleid, CHandlingAttrib attrib, uint8_t& ret)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo->IsValidVehicle(vehicleid))
		return false;
	if (!IsHandlingType<uint8_t>(attrib, compo->getCore()))
		return false;
	const stHandlingEntry* entry = GetVehicleHandlingEntry(vehicleid);
	return entry && GetHandlingValue(entry->handlingData, attrib, ret);
}

bool GetModelHandling(uint16_t modelid, CHandlingAttrib attrib, float& ret)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;
	if (!IsHandlingType<float>(attrib, ExtendedVehCompo::get()->getCore()))
		return false;
	stHandlingEntry* entry = GetModelHandlingEntry(modelid);
	return entry && GetHandlingValue(entry->handlingData, attrib, ret);
}

bool GetModelHandling(uint16_t modelid, CHandlingAttrib attrib, unsigned int& ret)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;
	if (!IsHandlingType<unsigned int>(attrib, ExtendedVehCompo::get()->getCore()))
		return false;
	stHandlingEntry* entry = GetModelHandlingEntry(modelid);
	return entry && GetHandlingValue(entry->handlingData, attrib, ret);
}

bool GetModelHandling(uint16_t modelid, CHandlingAttrib attrib, uint8_t& ret)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;
	if (!IsHandlingType<uint8_t>(attrib, ExtendedVehCompo::get()->getCore()))
		return false;
	stHandlingEntry* entry = GetModelHandlingEntry(modelid);
	return entry && GetHandlingValue(entry->handlingData, attrib, ret);
}

bool GetDefaultHandling(uint16_t modelid, CHandlingAttrib attrib, float& ret)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;
	if (!IsHandlingType<float>(attrib, ExtendedVehCompo::get()->getCore()))
		return false;
	struct tHandlingData* handling = HandlingDefault::getDefaultModelHandling(modelid);
	return handling && GetHandlingValue(*handling, attrib, ret);
}

bool GetDefaultHandling(uint16_t modelid, CHandlingAttrib attrib, unsigned int& ret)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;
	if (!IsHandlingType<unsigned int>(attrib, ExtendedVehCompo::get()->getCore()))
		return false;
	struct tHandlingData* handling = HandlingDefault::getDefaultModelHandling(modelid);
	return handling && GetHandlingValue(*handling, attrib, ret);
}

bool GetDefaultHandling(uint16_t modelid, CHandlingAttrib attrib, uint8_t& ret)
{
	if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelid))
		return false;
	if (!IsHandlingType<uint8_t>(attrib, ExtendedVehCompo::get()->getCore()))
		return false;
	struct tHandlingData* handling = HandlingDefault::getDefaultModelHandling(modelid);
	return handling && GetHandlingValue(*handling, attrib, ret);
}

bool SetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, float value)
{
	if (!IS_VALID_PLAYERID(playerid) || !CanSetHandlingAttrib(attrib))
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!IsHandlingType<float>(attrib, compo->getCore()) || !IsValidHandlingValue(attrib, value))
		return false;
	stHandlingEntry& entry = GetPlayerHandlingEntry(playerid);
	SetHandlingValue(entry, attrib, value);
	SendPlayerHandling(playerid, entry);
	return true;
}

bool SetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, unsigned int value)
{
	if (!IS_VALID_PLAYERID(playerid) || !CanSetHandlingAttrib(attrib))
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!IsHandlingType<unsigned int>(attrib, compo->getCore()))
		return false;
	stHandlingEntry& entry = GetPlayerHandlingEntry(playerid);
	SetHandlingValue(entry, attrib, value);
	SendPlayerHandling(playerid, entry);
	return true;
}

bool SetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, uint8_t value)
{
	if (!IS_VALID_PLAYERID(playerid) || !CanSetHandlingAttrib(attrib))
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!IsHandlingType<uint8_t>(attrib, compo->getCore()) || !IsValidHandlingValue(attrib, value))
		return false;
	stHandlingEntry& entry = GetPlayerHandlingEntry(playerid);
	SetHandlingValue(entry, attrib, value);
	SendPlayerHandling(playerid, entry);
	return true;
}

bool ResetPlayerHandling(uint16_t playerid)
{
	auto it = playerHandlings.find(playerid);
	if (it == playerHandlings.end())
		return false;
	playerHandlings.erase(it);

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (compo)
	{
		struct CustomVehActionPacket p(ACTION_RESET_PLAYER_HANDLING);
		p.data.Write(playerid);
		IPlayer* player = compo->GetPlayerByID(playerid);
		if (player)
		{
			player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
		}
	}
	return true;
}

bool GetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, float& ret)
{
	auto it = playerHandlings.find(playerid);
	if (it == playerHandlings.end())
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	return IsHandlingType<float>(attrib, compo->getCore()) && GetHandlingValue(it->second.handlingData, attrib, ret);
}

bool GetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, unsigned int& ret)
{
	auto it = playerHandlings.find(playerid);
	if (it == playerHandlings.end())
		return false;
	return IsHandlingType<unsigned int>(attrib, ExtendedVehCompo::get()->getCore()) &&
		GetHandlingValue(it->second.handlingData, attrib, ret);
}

bool GetPlayerHandling(uint16_t playerid, CHandlingAttrib attrib, uint8_t& ret)
{
	auto it = playerHandlings.find(playerid);
	if (it == playerHandlings.end())
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	return IsHandlingType<uint8_t>(attrib, compo->getCore()) && GetHandlingValue(it->second.handlingData, attrib, ret);
}

bool ResetAll(uint16_t playerid)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	IPlayer* player = compo->GetPlayerByID(playerid);
	if (!player)
		return false;

	for (auto& kv : vehicleHandlings)
	{
		if (kv.second.handlingModMap.empty())
			continue;
		struct CustomVehActionPacket p(ACTION_RESET_VEHICLE);
		p.data.Write(kv.first);
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
	}

	for (size_t i = 0; i < gBaseModelHandlings.size(); ++i)
	{
		if (gBaseModelHandlings[i].handlingModMap.empty())
			continue;
		struct CustomVehActionPacket p(ACTION_RESET_MODEL);
		p.data.Write((uint16_t)(i + 400));
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
	}

	for (const auto& [customModel, entry] : gCustomModelHandlings)
	{
		if (entry.handlingModMap.empty())
			continue;
		struct CustomVehActionPacket p(ACTION_RESET_MODEL);
		p.data.Write((uint16_t)customModel);
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBytesUsed()), 0, true);
	}

	ResetPlayerHandling(playerid);
	return true;
}

void UnregisterCustomVehicle(uint32_t customModelId)
{
	customVehicleDefs.erase(customModelId);
	customVehicleModels.erase(customModelId);
	gCustomModelHandlings.erase(customModelId);
	CVehicleMgr::VehicleRegistry::Get().UnregisterCustomModel(customModelId);
	SendCustomVehicleDestroyToAll(customModelId);
}

void BeginCustomVehicleDef(uint32_t customModelId, uint32_t visualBase, uint32_t audioBase, uint32_t handlingBase, CustomVeh::Protocol::EngineSound engineSoundId)
{
	CustomVeh::Protocol::VehicleDefinition def {};
	def.customModelId = customModelId;
	def.visualBaseModel = visualBase;
	def.audioBaseModel = audioBase;
	def.handlingBaseModel = handlingBase;
	def.engineSoundId.OnSound = engineSoundId.OnSound;
	def.engineSoundId.OffSound = engineSoundId.OffSound;
	stagedCustomVehicleDefs[customModelId] = def;

	customVehicleModels.insert(customModelId);
	CVehicleMgr::VehicleRegistry::Get().RegisterCustomModel(customModelId);

	stHandlingEntry& entry = gCustomModelHandlings[customModelId];
	HandlingDefault::copyDefaultModelHandling(handlingBase, &entry.handlingData);
	entry.handlingModMap.clear();
}

bool SetCustomVehicleAsset(uint32_t customModelId, std::string filename, CustomVeh::Protocol::AssetDescriptor CustomVeh::Protocol::VehicleDefinition::* asset)
{
	auto it = stagedCustomVehicleDefs.find(customModelId);
	if (it == stagedCustomVehicleDefs.end())
		return false;

	CustomVeh::Protocol::AssetDescriptor& descriptor = it->second.*asset;
	descriptor.filename = std::move(filename);
	ComputeFileSha256(descriptor.filename, descriptor.sha256);
	return true;
}

bool SetCustomVehicleDff(uint32_t customModelId)
{
	return SetCustomVehicleAsset(customModelId, GetAssetPath(customModelId, CustomVeh::Protocol::AssetType::Dff).string(), &CustomVeh::Protocol::VehicleDefinition::dff);
}

bool SetCustomVehicleTxd(uint32_t customModelId)
{
	return SetCustomVehicleAsset(customModelId, GetAssetPath(customModelId, CustomVeh::Protocol::AssetType::Txd).string(), &CustomVeh::Protocol::VehicleDefinition::txd);
}

bool SetCustomVehicleCol(uint32_t customModelId)
{
	return SetCustomVehicleAsset(customModelId, GetAssetPath(customModelId, CustomVeh::Protocol::AssetType::Col).string(), &CustomVeh::Protocol::VehicleDefinition::col);
}

bool CommitCustomVehicleDef(uint32_t customModelId)
{
	auto it = stagedCustomVehicleDefs.find(customModelId);
	if (it == stagedCustomVehicleDefs.end())
		return false;

	customVehicleDefs[customModelId] = it->second;
	stagedCustomVehicleDefs.erase(it);

	SendCustomVehicleDefToAll(customModelId);
	return true;
}

bool IsCustomVehicle(uint32_t modelId)
{
	return customVehicleModels.find(modelId) != customVehicleModels.end();
}

void SendCustomVehicleDefToPlayer(IPlayer& player, uint32_t modelId)
{
	auto it = customVehicleDefs.find(modelId);
	if (it == customVehicleDefs.end())
		return;

	CustomVehActionPacket pkt(ACTION_CUSTOM_VEHICLE_DEFINE);
	NetworkBitStream bs = pkt.data;
	auto writeToStream = [&bs](const std::string& s, size_t fixedLen)
	{
		std::string padded = s;
		padded.resize(fixedLen, '\0');
		bs.Write(padded.data(), static_cast<int>(fixedLen));
	};
	auto writeAsset = [&bs, &writeToStream](const auto& asset)
	{
		bs.Write(asset.type);
		bs.Write(asset.size);
		bs.Write(asset.compressedSize);
		bs.Write(asset.chunkSize);
		bs.Write(asset.chunkCount);
		writeToStream(asset.sha256, CustomVeh::Protocol::SHA256_BUFFER_SIZE);
		writeToStream(asset.filename, CustomVeh::Protocol::FILENAME_SIZE);
	};

	const auto& def = it->second;
	bs.Write(def.customModelId);
	bs.Write(def.visualBaseModel);
	bs.Write(def.handlingBaseModel);
	bs.Write(def.audioBaseModel);
	bs.Write(def.engineSoundId.OnSound);
	bs.Write(def.engineSoundId.OffSound);
	bs.Write(def.celerateSoundId.accelerateSound);
	bs.Write(def.celerateSoundId.decelerateSound);
	bs.Write(def.flags);
	writeAsset(def.dff);
	writeAsset(def.txd);
	writeAsset(def.col);
	player.sendPacket(Span<uint8_t>(pkt.data.GetData(), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendCustomVehicleDefToAll(uint32_t modelId)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core = compo->getCore();
	for (IPlayer* player : core->getPlayers().players())
	{
		SendCustomVehicleDefToPlayer(*player, modelId);
	}
}

void SendCustomVehicleDestroyToPlayer(IPlayer& player, uint32_t modelId)
{
	CustomVehActionPacket pkt(ACTION_CUSTOM_VEHICLE_DESTROY);
	NetworkBitStream bs = pkt.data;
	bs.Write(modelId);
	player.sendPacket(Span<uint8_t>(pkt.data.GetData(), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendCustomVehicleDestroyToAll(uint32_t modelId)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo->getCore();
	for (IPlayer* player : core_->getPlayers().players())
	{
		SendCustomVehicleDestroyToPlayer(*player, modelId);
	}
}
}
