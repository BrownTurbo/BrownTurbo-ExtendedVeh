#include "HandlingManager.h"
#include "Actions.h"
#include "PlayerAttrs.h"
#include "CVehicleManager.hpp"
#include "HandlingDefault.h"
#include "PacketEnum.h"
#include "extendedveh.h"
#include "ModelTransferManager.h"
#include "CustomVehicleBindingRegistry.h"

#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "utils.h"
#include "defs.h"

namespace HandlingMgr
{
std::array<stHandlingEntry, CVehicleMgr::BASE_MAX_VEHICLE_MODELS> gBaseModelHandlings;
std::unordered_map<uint32_t, stHandlingEntry> gCustomModelHandlings;

std::unordered_map<uint16_t, struct stVehicleHandlingEntry> vehicleHandlings;
std::unordered_map<uint16_t, struct stHandlingEntry> playerHandlings; // key = playerid
std::unordered_map<uint32_t, CustomVeh::Protocol::VehicleDefinition> customVehicleDefs; // key = modelId
std::unordered_set<uint32_t> customVehicleModels;
std::unordered_map<uint16_t, uint8_t> vehicleDoorStates;
std::unordered_map<uint32_t, CustomVeh::Protocol::VehicleDefinition> stagedCustomVehicleDefs;

std::unordered_set<uint16_t> usOutgoingVehicleMods;
std::unordered_set<uint32_t> usOutgoingModelMods;
std::mutex g_outgoingModsMutex;

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
void __WriteHandlingEntryToBitStream(NetworkBitStream* bs, const struct stHandlingEntry& entry)
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
	void* offs = GetHandlingAttribPtr(&handling->handlingData, attribute);
	if (!offs)
	{
		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		ICore* core_ = compo ? compo->getCore() : nullptr;
		if (core_)
		{
			core_->logLn(LogLevel::Error, "[ExtendedVeh] __addMod: Failed to resolve attribute pointer for attribute %d", static_cast<int>(attribute));
		}
		return;
	}

	if (handling->handlingModMap.count(attribute))
		handling->handlingModMap.at(attribute) = mod;
	else
		handling->handlingModMap.emplace(attribute, mod);

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
	const bool validType = [&]
	{
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
		player->sendPacket(Span<uint8_t>(packet.data.GetData(), packet.data.GetNumberOfBitsUsed()), 0, true);
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

	{
		std::lock_guard<std::mutex> lock(g_outgoingModsMutex);
		usOutgoingModelMods.emplace(modelid);
	}
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

	{
		std::lock_guard<std::mutex> lock(g_outgoingModsMutex);
		usOutgoingVehicleMods.emplace(vehicleid);
	}
	return true;
}

/* -------------------------------------------------------------------------------------------------------------------- */

/* We use ProcessTick to broadcast queued modifications all at once instead of spamming with packets */
void ProcessTick()
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return;
	ICore* core_ = compo->getCore();
	if (!core_)
		return;

	std::unordered_set<uint16_t> vehicleMods;
	std::unordered_set<uint32_t> modelMods;
	{
		std::lock_guard<std::mutex> lock(g_outgoingModsMutex);
		vehicleMods.swap(usOutgoingVehicleMods);
		modelMods.swap(usOutgoingModelMods);
	}

	if (!vehicleMods.empty() || !modelMods.empty())
	{
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] ProcessTick: Broadcasting %zu vehicle mods and %zu model mods", vehicleMods.size(), modelMods.size());
	}

	for (uint16_t vehicleid : vehicleMods)
	{
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
			if (player)
				player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
		}
	}

	for (uint32_t modelid : modelMods)
	{
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
			if (player)
				player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
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
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnCreateVehicle: vehicleid=%d", vehicleid);
	}
	IVehicle* pVeh = compo ? compo->GetVehicleByID(vehicleid) : nullptr;
	if (pVeh)
	{
		vehiclesIdMap[vehicleid] = pVeh;
		ResetVehicleHandling(*pVeh, false);
	}
}

void OnDestroyVehicle(int vehicleid)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnDestroyVehicle: vehicleid=%d", vehicleid);
	}
	IVehicle* pVeh = vehiclesIdMap[vehicleid];
	if (pVeh)
	{
		ResetVehicleHandling(*pVeh, false);
	}
	vehicleHandlings.erase(vehicleid);
	vehicleDoorStates.erase(static_cast<uint16_t>(vehicleid));
	vehiclesIdMap.erase(vehicleid);
	CustomVehicleBindingRegistry::Instance().Unbind(static_cast<uint16_t>(vehicleid));
}

void OnPlayerConnect(IPlayer& player)
{
	int playerid = player.getID();
	if (!gPlayers.HasExtendedVeh(playerid))
		return;

	for (uint16_t model = 0; model < CVehicleMgr::BASE_MAX_VEHICLE_MODELS; model++)
	{
		if (!gBaseModelHandlings[model].handlingModMap.empty())
		{
			struct CustomVehActionPacket p(ACTION_SET_MODEL_HANDLING);
			p.data.Write((uint16_t)(model + 400));
			__WriteHandlingEntryToBitStream(&p.data, gBaseModelHandlings[model]);
			player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, false);
		}
	}

	for (const auto& [customModel, entry] : gCustomModelHandlings)
	{
		if (!entry.handlingModMap.empty())
		{
			struct CustomVehActionPacket p(ACTION_SET_MODEL_HANDLING);
			p.data.Write((uint16_t)customModel);
			__WriteHandlingEntryToBitStream(&p.data, entry);
			player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, false);
		}
	}

	for (const auto& kv : customVehicleDefs)
	{
		SendCustomVehicleDefToPlayer(player, kv.first);
	}

	for (const auto& [vehId, mask] : vehicleDoorStates)
	{
		if (mask != 0)
		{
			for (uint8_t d = 0; d < 6; d++)
			{
				if (mask & (1 << d))
				{
					struct CustomVehActionPacket p(ACTION_SET_VEHICLE_DOOR_STATE);
					p.data.Write(vehId);
					p.data.Write(d);
					p.data.Write(true);
					player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
				}
			}
		}
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

	if (!gPlayers.HasExtendedVeh(forplayerid))
		return;

	auto doorIt = vehicleDoorStates.find(static_cast<uint16_t>(vehicleid));
	if (doorIt != vehicleDoorStates.end() && doorIt->second != 0)
	{
		for (uint8_t d = 0; d < 6; d++)
		{
			if (doorIt->second & (1 << d))
			{
				struct CustomVehActionPacket p(ACTION_SET_VEHICLE_DOOR_STATE);
				p.data.Write(static_cast<uint16_t>(vehicleid));
				p.data.Write(d);
				p.data.Write(true);
				player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
			}
		}
	}

	auto it = vehicleHandlings.find(vehicleid);
	if (it == vehicleHandlings.end() || it->second.handlingModMap.empty())
		return;

	struct CustomVehActionPacket p(ACTION_SET_VEHICLE_HANDLING);
	p.data.Write((uint16_t)vehicleid);
	__WriteHandlingEntryToBitStream(&p.data, it->second);
	player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, false);
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
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
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
			player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
		}
	}
}

/* VEHICLE DOOR FUNCTIONS */

bool SetVehicleDoorMissing(uint16_t vehicleid, uint8_t doorid, bool missing)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo || !compo->IsValidVehicle(vehicleid))
		return false;

	if (doorid == 0xFF)
	{
		return SetVehicleAllDoorsMissing(vehicleid, missing);
	}

	if (doorid > 5)
		return false;

	uint8_t& mask = vehicleDoorStates[vehicleid];
	if (missing)
		mask |= (1 << doorid);
	else
		mask &= ~(1 << doorid);

	// Sync base 4 doors to standard SA-MP damage status (bonnet=0, boot=1, front_left=2, front_right=3)
	if (doorid <= 3)
	{
		IVehicle* pVeh = compo->GetVehicleByID(vehicleid);
		if (pVeh)
		{
			int panels = 0, doors = 0, lights = 0, tyres = 0;
			pVeh->getDamageStatus(panels, doors, lights, tyres);
			uint32_t shift = doorid * 8;
			uint32_t doorByte = missing ? 3 : 0; // 3 = missing/detached in SA-MP
			doors = (doors & ~(0xFF << shift)) | (doorByte << shift);
			pVeh->setDamageStatus(panels, doors, static_cast<uint8_t>(lights), static_cast<uint8_t>(tyres));
		}
	}

	// Broadcast packet to authorized players
	ICore* core_ = compo->getCore();
	if (core_)
	{
		struct CustomVehActionPacket p(ACTION_SET_VEHICLE_DOOR_STATE);
		p.data.Write(vehicleid);
		p.data.Write(doorid);
		p.data.Write(missing);

		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
			}
		}
	}

	return true;
}

bool SetVehicleAllDoorsMissing(uint16_t vehicleid, bool missing)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo || !compo->IsValidVehicle(vehicleid))
		return false;

	uint8_t& mask = vehicleDoorStates[vehicleid];
	mask = missing ? 0x3F : 0x00;

	// Sync base 4 doors to standard SA-MP damage status
	IVehicle* pVeh = compo->GetVehicleByID(vehicleid);
	if (pVeh)
	{
		int panels = 0, doors = 0, lights = 0, tyres = 0;
		pVeh->getDamageStatus(panels, doors, lights, tyres);
		uint32_t doorByte = missing ? 3 : 0;
		doors = doorByte | (doorByte << 8) | (doorByte << 16) | (doorByte << 24);
		pVeh->setDamageStatus(panels, doors, static_cast<uint8_t>(lights), static_cast<uint8_t>(tyres));
	}

	// Broadcast packet to authorized players
	ICore* core_ = compo->getCore();
	if (core_)
	{
		struct CustomVehActionPacket p(ACTION_SET_VEHICLE_DOOR_STATE);
		p.data.Write(vehicleid);
		p.data.Write(static_cast<uint8_t>(0xFF));
		p.data.Write(missing);

		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
			}
		}
	}

	return true;
}

bool GetVehicleDoorMissing(uint16_t vehicleid, uint8_t doorid, bool& missing)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo || !compo->IsValidVehicle(vehicleid))
		return false;

	if (doorid > 5)
		return false;

	auto it = vehicleDoorStates.find(vehicleid);
	if (it != vehicleDoorStates.end())
	{
		missing = (it->second & (1 << doorid)) != 0;
	}
	else
	{
		missing = false;
	}
	return true;
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
	if (!SetHandlingValue(entry, attrib, value))
		return false;
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
	if (!SetHandlingValue(entry, attrib, value))
		return false;
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
	if (!SetHandlingValue(entry, attrib, value))
		return false;
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
			player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
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
	return IsHandlingType<unsigned int>(attrib, ExtendedVehCompo::get()->getCore()) && GetHandlingValue(it->second.handlingData, attrib, ret);
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
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
	}

	for (size_t i = 0; i < gBaseModelHandlings.size(); ++i)
	{
		if (gBaseModelHandlings[i].handlingModMap.empty())
			continue;
		struct CustomVehActionPacket p(ACTION_RESET_MODEL);
		p.data.Write((uint16_t)(i + 400));
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
	}

	for (const auto& [customModel, entry] : gCustomModelHandlings)
	{
		if (entry.handlingModMap.empty())
			continue;
		struct CustomVehActionPacket p(ACTION_RESET_MODEL);
		p.data.Write((uint16_t)customModel);
		player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
	}

	ResetPlayerHandling(playerid);
	return true;
}

void UnregisterCustomVehicle(uint32_t customModelId)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] UnregisterCustomVehicle: Model %u", customModelId);
	}
	customVehicleDefs.erase(customModelId);
	customVehicleModels.erase(customModelId);
	gCustomModelHandlings.erase(customModelId);
	CVehicleMgr::VehicleRegistry::Get().UnregisterCustomModel(customModelId);
	SendCustomVehicleDestroyToAll(customModelId);
}

void BeginCustomVehicleDef(uint32_t customModelId, uint32_t visualBase, uint32_t audioBase, uint32_t handlingBase, CustomVeh::Protocol::EngineSound engineSoundId)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] BeginCustomVehicleDef: Model %u (visual=%u, audio=%u, handling=%u)", customModelId, visualBase, audioBase, handlingBase);
	}
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

bool SetCustomVehicleAsset(uint32_t customModelId, std::string filename, CustomVeh::Protocol::AssetDescriptor CustomVeh::Protocol::VehicleDefinition::*asset)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;

	auto it = stagedCustomVehicleDefs.find(customModelId);
	if (it == stagedCustomVehicleDefs.end())
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleAsset: Custom vehicle model %u is not staged", customModelId);
		return false;
	}

	fs::path filePath = fs::path(filename);
	if (!fs::exists(filePath))
	{
		filePath = fs::path(g_modelsDir) / filePath;
	}
	if (!fs::exists(filePath))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleAsset: Asset file '%s' does not exist for model %u", filePath.string().c_str(), customModelId);
		return false;
	}
	if (!IsPathInsideBase(g_modelsDir, filePath))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleAsset: Asset file '%s' is outside base models directory", filePath.string().c_str());
		return false;
	}

	CustomVeh::Protocol::AssetDescriptor& descriptor = it->second.*asset;
	strncpy(descriptor.filename, filename.c_str(), sizeof(descriptor.filename) - 1);
	descriptor.filename[sizeof(descriptor.filename) - 1] = '\0';

	std::string shaHex;
	if (ComputeFileSha256(filePath.string(), shaHex))
	{
		strncpy(descriptor.sha256, shaHex.c_str(), sizeof(descriptor.sha256) - 1);
		descriptor.sha256[sizeof(descriptor.sha256) - 1] = '\0';
	}
	else
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleAsset: Failed to compute SHA-256 for '%s'", filename.c_str());
	}

	std::error_code ec;
	descriptor.size = fs::file_size(filePath, ec);
	if (ec)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleAsset: Failed to get file size for '%s': %s", filePath.string().c_str(), ec.message().c_str());
		descriptor.size = 0;
	}

	return true;
}

bool SetCustomVehicleDff(uint32_t customModelId)
{
	fs::path dffPath = GetAssetPath(customModelId, CustomVeh::Protocol::AssetType::Dff);
	if (!fs::exists(dffPath))
	{
		fs::path fallback = fs::path(g_modelsDir) / (std::to_string(customModelId) + ".dff");
		if (fs::exists(fallback))
			dffPath = fallback;
	}
	if (!fs::exists(dffPath))
	{
		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		ICore* core_ = compo ? compo->getCore() : nullptr;
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleDff: DFF file not found for model %u (tried '%s')", customModelId, dffPath.string().c_str());
		return false;
	}
	return SetCustomVehicleAsset(customModelId, dffPath.string(), &CustomVeh::Protocol::VehicleDefinition::dff);
}

bool SetCustomVehicleTxd(uint32_t customModelId)
{
	fs::path txdPath = GetAssetPath(customModelId, CustomVeh::Protocol::AssetType::Txd);
	if (!fs::exists(txdPath))
	{
		fs::path fallback = fs::path(g_modelsDir) / (std::to_string(customModelId) + ".txd");
		if (fs::exists(fallback))
			txdPath = fallback;
	}
	if (!fs::exists(txdPath))
	{
		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		ICore* core_ = compo ? compo->getCore() : nullptr;
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleTxd: TXD file not found for model %u (tried '%s')", customModelId, txdPath.string().c_str());
		return false;
	}
	return SetCustomVehicleAsset(customModelId, txdPath.string(), &CustomVeh::Protocol::VehicleDefinition::txd);
}

bool SetCustomVehicleCol(uint32_t customModelId)
{
	fs::path colPath = GetAssetPath(customModelId, CustomVeh::Protocol::AssetType::Col);
	if (!fs::exists(colPath))
	{
		fs::path fallback = fs::path(g_modelsDir) / (std::to_string(customModelId) + ".col");
		if (fs::exists(fallback))
			colPath = fallback;
		else
			return false; // COL is optional
	}
	return SetCustomVehicleAsset(customModelId, colPath.string(), &CustomVeh::Protocol::VehicleDefinition::col);
}

bool CommitCustomVehicleDef(uint32_t customModelId)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;

	auto it = stagedCustomVehicleDefs.find(customModelId);
	if (it == stagedCustomVehicleDefs.end())
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] CommitCustomVehicleDef: Custom vehicle model %u is not staged", customModelId);
		return false;
	}

	if (it->second.dff.filename[0] == '\0')
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] CommitCustomVehicleDef: Custom vehicle model %u is missing DFF asset", customModelId);
		return false;
	}
	it->second.flags |= CustomVeh::Protocol::HasDff;

	if (it->second.txd.filename[0] == '\0')
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] CommitCustomVehicleDef: Custom vehicle model %u is missing TXD asset", customModelId);
		return false;
	}
	it->second.flags |= CustomVeh::Protocol::HasTxd;

	if (it->second.col.filename[0] != '\0')
		it->second.flags |= CustomVeh::Protocol::HasCol;

	customVehicleDefs[customModelId] = it->second;
	stagedCustomVehicleDefs.erase(it);

	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] Custom vehicle model %u committed successfully (flags=0x%X)", customModelId, customVehicleDefs[customModelId].flags);

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
	NetworkBitStream& bs = pkt.data;
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
	pkt.data.Write(modelId);
	player.sendPacket(Span<uint8_t>(pkt.data.GetData(), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendCustomVehicleDestroyToAll(uint32_t modelId)
{
	ModelTransferMgr::CancelTransfersForModel(modelId);
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			SendCustomVehicleDestroyToPlayer(*player, modelId);
		}
	}
}
}
