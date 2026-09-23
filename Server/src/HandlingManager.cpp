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
std::unordered_map<uint32_t, ModelConfig> customVehicleConfigs;

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
			// Guard against use-after-free: gPlayers.Reset() is called in onPlayerDisconnect
			// *before* HandlingMgr::OnPlayerDisconnect, so a freed/disconnecting IPlayer*
			// will always have HasExtendedVeh() == false at this point.
			if (player && gPlayers.HasExtendedVeh(player->getID()))
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
			// Same guard as above - only send to confirmed-alive ExtendedVeh players.
			if (player && gPlayers.HasExtendedVeh(player->getID()))
				player->sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
		}
	}
}

void BroadcastVehicleCorrection(uint16_t vehicleid)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return;
	ICore* core_ = compo->getCore();
	if (!core_)
		return;

	auto vIt = vehicleHandlings.find(vehicleid);
	if (vIt != vehicleHandlings.end() && !vIt->second.handlingModMap.empty())
	{
		struct CustomVehActionPacket correction(ACTION_SET_VEHICLE_HANDLING);
		correction.data.Write(vehicleid);
		__WriteHandlingEntryToBitStream(&correction.data, vIt->second);
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
				player->sendPacket(Span<uint8_t>(correction.data.GetData(), correction.data.GetNumberOfBitsUsed()), 0, true);
		}
	}
	else
	{
		struct CustomVehActionPacket correction(ACTION_RESET_VEHICLE);
		correction.data.Write(vehicleid);
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
				player->sendPacket(Span<uint8_t>(correction.data.GetData(), correction.data.GetNumberOfBitsUsed()), 0, true);
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

void OnPlayerAuthorized(IPlayer& player)
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
			player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
		}
	}

	for (const auto& [customModel, entry] : gCustomModelHandlings)
	{
		if (!entry.handlingModMap.empty())
		{
			struct CustomVehActionPacket p(ACTION_SET_MODEL_HANDLING);
			p.data.Write((uint16_t)customModel);
			__WriteHandlingEntryToBitStream(&p.data, entry);
			player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
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

	// Synchronize all existing custom vehicle handlings to the connecting player
	for (const auto& [vehId, entry] : vehicleHandlings)
	{
		if (!entry.handlingModMap.empty() && !entry.usesModelHandling)
		{
			struct CustomVehActionPacket p(ACTION_SET_VEHICLE_HANDLING);
			p.data.Write(vehId);
			__WriteHandlingEntryToBitStream(&p.data, entry);
			player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
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
	player.sendPacket(Span<uint8_t>(p.data.GetData(), p.data.GetNumberOfBitsUsed()), 0, true);
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
			if (player && gPlayers.HasExtendedVeh(player->getID()))
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
		// Guard against use-after-free on disconnect: gPlayers.Reset(playerid) was called
		// before OnPlayerDisconnect, so a disconnecting player will have HasExtendedVeh() == false.
		if (player && gPlayers.HasExtendedVeh(playerid))
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
	customVehicleConfigs.erase(customModelId);
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

	// Auto-check and load model.ini if present in models/{customModelId}/
	LoadCustomVehicleConfig(customModelId);
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

bool SetCustomVehicleModelInfo(uint32_t customModelId, uint8_t vehicleClass, int16_t wheelModelId, float wheelScaleFront, float wheelScaleRear, uint16_t frequency, uint8_t level, uint8_t comprate, uint8_t numExtras, uint8_t wheelUpgradeClass)
{
	bool found = false;
	auto itStaged = stagedCustomVehicleDefs.find(customModelId);
	if (itStaged != stagedCustomVehicleDefs.end())
	{
		itStaged->second.modelInfo.vehicleClass = vehicleClass;
		itStaged->second.modelInfo.wheelModelId = wheelModelId;
		itStaged->second.modelInfo.wheelScaleFront = wheelScaleFront;
		itStaged->second.modelInfo.wheelScaleRear = wheelScaleRear;
		itStaged->second.modelInfo.frequency = frequency;
		itStaged->second.modelInfo.level = level;
		itStaged->second.modelInfo.comprate = comprate;
		itStaged->second.modelInfo.numExtras = numExtras;
		itStaged->second.modelInfo.wheelUpgradeClass = wheelUpgradeClass;
		found = true;
	}

	auto itComm = customVehicleDefs.find(customModelId);
	if (itComm != customVehicleDefs.end())
	{
		itComm->second.modelInfo.vehicleClass = vehicleClass;
		itComm->second.modelInfo.wheelModelId = wheelModelId;
		itComm->second.modelInfo.wheelScaleFront = wheelScaleFront;
		itComm->second.modelInfo.wheelScaleRear = wheelScaleRear;
		itComm->second.modelInfo.frequency = frequency;
		itComm->second.modelInfo.level = level;
		itComm->second.modelInfo.comprate = comprate;
		itComm->second.modelInfo.numExtras = numExtras;
		itComm->second.modelInfo.wheelUpgradeClass = wheelUpgradeClass;
		found = true;
		SendCustomVehicleDefToAll(customModelId);
	}
	return found;
}

bool SetCustomVehicleWheelModel(uint32_t customModelId, int16_t wheelModelId)
{
	bool found = false;
	auto itStaged = stagedCustomVehicleDefs.find(customModelId);
	if (itStaged != stagedCustomVehicleDefs.end()) {
		itStaged->second.modelInfo.wheelModelId = wheelModelId;
		found = true;
	}
	auto itComm = customVehicleDefs.find(customModelId);
	if (itComm != customVehicleDefs.end()) {
		itComm->second.modelInfo.wheelModelId = wheelModelId;
		found = true;
		SendCustomVehicleDefToAll(customModelId);
	}
	return found;
}

bool SetCustomVehicleWheelScale(uint32_t customModelId, float wheelScaleFront, float wheelScaleRear)
{
	bool found = false;
	auto itStaged = stagedCustomVehicleDefs.find(customModelId);
	if (itStaged != stagedCustomVehicleDefs.end()) {
		itStaged->second.modelInfo.wheelScaleFront = wheelScaleFront;
		itStaged->second.modelInfo.wheelScaleRear = wheelScaleRear;
		found = true;
	}
	auto itComm = customVehicleDefs.find(customModelId);
	if (itComm != customVehicleDefs.end()) {
		itComm->second.modelInfo.wheelScaleFront = wheelScaleFront;
		itComm->second.modelInfo.wheelScaleRear = wheelScaleRear;
		found = true;
		SendCustomVehicleDefToAll(customModelId);
	}
	return found;
}

bool GetCustomVehicleWheelModel(uint32_t customModelId, int16_t& wheelModelId)
{
	auto itStaged = stagedCustomVehicleDefs.find(customModelId);
	if (itStaged != stagedCustomVehicleDefs.end()) {
		wheelModelId = itStaged->second.modelInfo.wheelModelId;
		return true;
	}
	auto itComm = customVehicleDefs.find(customModelId);
	if (itComm != customVehicleDefs.end()) {
		wheelModelId = itComm->second.modelInfo.wheelModelId;
		return true;
	}
	return false;
}

bool GetCustomVehicleWheelScale(uint32_t customModelId, float& wheelScaleFront, float& wheelScaleRear)
{
	auto itStaged = stagedCustomVehicleDefs.find(customModelId);
	if (itStaged != stagedCustomVehicleDefs.end()) {
		wheelScaleFront = itStaged->second.modelInfo.wheelScaleFront;
		wheelScaleRear = itStaged->second.modelInfo.wheelScaleRear;
		return true;
	}
	auto itComm = customVehicleDefs.find(customModelId);
	if (itComm != customVehicleDefs.end()) {
		wheelScaleFront = itComm->second.modelInfo.wheelScaleFront;
		wheelScaleRear = itComm->second.modelInfo.wheelScaleRear;
		return true;
	}
	return false;
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
	bs.Write(def.modelInfo.vehicleClass);
	bs.Write(def.modelInfo.wheelModelId);
	bs.Write(def.modelInfo.wheelScaleFront);
	bs.Write(def.modelInfo.wheelScaleRear);
	bs.Write(def.modelInfo.frequency);
	bs.Write(def.modelInfo.level);
	bs.Write(def.modelInfo.comprate);
	bs.Write(def.modelInfo.numExtras);
	bs.Write(def.modelInfo.wheelUpgradeClass);

	if (def.flags & CustomVeh::Protocol::HasAnyAudio)
	{
		bs.Write(def.customAudio.volume);
		bs.Write(def.customAudio.minDistance);
		bs.Write(def.customAudio.maxDistance);
		bs.Write(def.customAudio.pitchMultiplier);
		bs.Write(def.customAudio.accelPitchFactor);
		bs.Write(def.customAudio.muteNative);

		if (def.flags & CustomVeh::Protocol::HasAudioEngine)
			writeAsset(def.audioEngine);
		if (def.flags & CustomVeh::Protocol::HasAudioAccel)
			writeAsset(def.audioAccel);
		if (def.flags & CustomVeh::Protocol::HasAudioDecel)
			writeAsset(def.audioDecel);
		if (def.flags & CustomVeh::Protocol::HasAudioBrake)
			writeAsset(def.audioBrake);
		if (def.flags & CustomVeh::Protocol::HasAudioCrash)
			writeAsset(def.audioCrash);
	}

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

bool LoadCustomVehicleConfig(uint32_t customModelId)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;

	fs::path iniPath = fs::path(g_modelsDir) / std::to_string(customModelId) / "model.ini";
	if (!fs::exists(iniPath))
	{
		fs::path fallback = fs::path(g_modelsDir) / (std::to_string(customModelId) + ".ini");
		if (fs::exists(fallback))
			iniPath = fallback;
		else
			return false;
	}

	uint32_t fallbackBase = 411;
	auto itStaged = stagedCustomVehicleDefs.find(customModelId);
	if (itStaged != stagedCustomVehicleDefs.end())
	{
		fallbackBase = itStaged->second.handlingBaseModel;
	}
	else
	{
		auto itComm = customVehicleDefs.find(customModelId);
		if (itComm != customVehicleDefs.end())
			fallbackBase = itComm->second.handlingBaseModel;
	}

	ModelConfig config;
	if (!ModelConfigParser::ParseFile(iniPath, config, fallbackBase))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] LoadCustomVehicleConfig: Failed to parse '%s' for model %u", iniPath.string().c_str(), customModelId);
		return false;
	}

	customVehicleConfigs[customModelId] = config;

	// Apply base models and sounds to staged definition if available
	if (itStaged != stagedCustomVehicleDefs.end())
	{
		if (config.visualBase > 0)
			itStaged->second.visualBaseModel = config.visualBase;
		if (config.audioBase > 0)
			itStaged->second.audioBaseModel = config.audioBase;
		if (config.handlingBase > 0)
			itStaged->second.handlingBaseModel = config.handlingBase;
		if (config.engineOnSound >= 0)
			itStaged->second.engineSoundId.OnSound = config.engineOnSound;
		if (config.engineOffSound >= 0)
			itStaged->second.engineSoundId.OffSound = config.engineOffSound;
		if (config.accelerateSound >= 0)
			itStaged->second.celerateSoundId.accelerateSound = config.accelerateSound;
		if (config.decelerateSound >= 0)
			itStaged->second.celerateSoundId.decelerateSound = config.decelerateSound;
	}

	// Apply IDE settings (wheels, scale, class, extras, etc.)
	if (config.hasIde)
	{
		SetCustomVehicleModelInfo(customModelId,
			config.vehicleClass,
			config.wheelModelId,
			config.wheelScaleFront,
			config.wheelScaleRear,
			config.frequency,
			config.level,
			config.compRules,
			config.numExtras,
			config.wheelUpgradeClass);
	}

	// Apply handling attributes (preserving front lights!)
	if (config.hasHandling)
	{
		stHandlingEntry& entry = gCustomModelHandlings[customModelId];
		eVehicleLightsSize originalFrontLights = entry.handlingData.m_nFrontLights;

		entry.handlingData = config.handlingData;
		entry.handlingData.m_nFrontLights = originalFrontLights; // Front lights are 100% fixed!

		for (const auto& [attrib, mod] : config.handlingMods)
		{
			if (attrib == HANDL_FRONTLIGHTS)
				continue; // DO NOT touch front lights!
			entry.handlingModMap[attrib] = mod;
		}

		// If this model is already committed and in use, mark for outgoing broadcast
		auto itComm = customVehicleDefs.find(customModelId);
		if (itComm != customVehicleDefs.end())
		{
			std::lock_guard lock(g_outgoingModsMutex);
			usOutgoingModelMods.insert(customModelId);
		}
	}

	// Apply custom audio configuration to staged or committed definition
	if (config.hasAudio)
	{
		auto applyAudioConfig = [&](CustomVeh::Protocol::VehicleDefinition& def) {
			def.customAudio.volume = config.audioVolume;
			def.customAudio.minDistance = config.audioMinDistance;
			def.customAudio.maxDistance = config.audioMaxDistance;
			def.customAudio.pitchMultiplier = config.audioPitchMultiplier;
			def.customAudio.accelPitchFactor = config.audioAccelPitchFactor;
			def.customAudio.muteNative = config.audioMuteNative;

			auto setupSlot = [&](const std::string& filename, CustomVeh::Protocol::AssetType type,
				CustomVeh::Protocol::AssetFlags flag, CustomVeh::Protocol::AssetDescriptor& desc) {
				if (filename.empty()) return;
				fs::path p = fs::path(g_modelsDir) / std::to_string(customModelId) / filename;
				if (!fs::exists(p)) {
					if (core_) core_->logLn(LogLevel::Warning, "[ExtendedVeh] Audio file '%s' does not exist for model %u", p.string().c_str(), customModelId);
					return;
				}
				desc.type = type;
				strncpy(desc.filename, filename.c_str(), sizeof(desc.filename) - 1);
				desc.filename[sizeof(desc.filename) - 1] = '\0';
				std::string shaHex;
				if (ComputeFileSha256(p.string(), shaHex)) {
					strncpy(desc.sha256, shaHex.c_str(), sizeof(desc.sha256) - 1);
					desc.sha256[sizeof(desc.sha256) - 1] = '\0';
				}
				std::error_code ec;
				desc.size = fs::file_size(p, ec);
				if (ec) desc.size = 0;
				def.flags |= flag;
			};

			if (!config.engineFile.empty())
				setupSlot(config.engineFile, CustomVeh::Protocol::AssetType::AudioEngine, CustomVeh::Protocol::HasAudioEngine, def.audioEngine);
			if (!config.accelerationFile.empty())
				setupSlot(config.accelerationFile, CustomVeh::Protocol::AssetType::AudioAccel, CustomVeh::Protocol::HasAudioAccel, def.audioAccel);
			if (!config.deaccelerationFile.empty())
				setupSlot(config.deaccelerationFile, CustomVeh::Protocol::AssetType::AudioDecel, CustomVeh::Protocol::HasAudioDecel, def.audioDecel);
			if (!config.brakeFile.empty())
				setupSlot(config.brakeFile, CustomVeh::Protocol::AssetType::AudioBrake, CustomVeh::Protocol::HasAudioBrake, def.audioBrake);
			if (!config.crashFile.empty())
				setupSlot(config.crashFile, CustomVeh::Protocol::AssetType::AudioCrash, CustomVeh::Protocol::HasAudioCrash, def.audioCrash);
		};

		if (itStaged != stagedCustomVehicleDefs.end())
			applyAudioConfig(itStaged->second);
		auto itComm = customVehicleDefs.find(customModelId);
		if (itComm != customVehicleDefs.end())
			applyAudioConfig(itComm->second);
	}

	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] LoadCustomVehicleConfig: Successfully loaded configuration for model %u from '%s'", customModelId, iniPath.string().c_str());
	}
	return true;
}

bool DefineCustomVehicleFromConfig(uint32_t customModelId, uint32_t defaultVisualBase)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;

	fs::path iniPath = fs::path(g_modelsDir) / std::to_string(customModelId) / "model.ini";
	if (!fs::exists(iniPath))
	{
		fs::path fallback = fs::path(g_modelsDir) / (std::to_string(customModelId) + ".ini");
		if (fs::exists(fallback))
			iniPath = fallback;
	}

	ModelConfig config;
	bool hasIni = false;
	if (fs::exists(iniPath))
	{
		hasIni = ModelConfigParser::ParseFile(iniPath, config, defaultVisualBase);
	}

	uint32_t visualBase = (config.visualBase > 0) ? config.visualBase : defaultVisualBase;
	uint32_t audioBase = (config.audioBase > 0) ? config.audioBase : visualBase;
	uint32_t handlingBase = (config.handlingBase > 0) ? config.handlingBase : visualBase;

	CustomVeh::Protocol::EngineSound engineSound{};
	engineSound.OnSound = config.engineOnSound;
	engineSound.OffSound = config.engineOffSound;

	BeginCustomVehicleDef(customModelId, visualBase, audioBase, handlingBase, engineSound);

	if (!SetCustomVehicleDff(customModelId))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] DefineCustomVehicleFromConfig: Missing DFF for model %u", customModelId);
		stagedCustomVehicleDefs.erase(customModelId);
		return false;
	}

	if (!SetCustomVehicleTxd(customModelId))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] DefineCustomVehicleFromConfig: Missing TXD for model %u", customModelId);
		stagedCustomVehicleDefs.erase(customModelId);
		return false;
	}

	// COL is optional
	SetCustomVehicleCol(customModelId);

	if (hasIni)
	{
		LoadCustomVehicleConfig(customModelId);
	}

	return CommitCustomVehicleDef(customModelId);
}

int LoadAllCustomVehicles(uint32_t defaultVisualBase)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;

	if (!fs::exists(g_modelsDir) || !fs::is_directory(g_modelsDir))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] LoadAllCustomVehicles: Models directory '%s' does not exist", g_modelsDir.c_str());
		return 0;
	}

	int loadedCount = 0;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(g_modelsDir, ec))
	{
		if (!entry.is_directory())
			continue;

		std::string folderName = entry.path().filename().string();
		if (folderName.empty() || !std::all_of(folderName.begin(), folderName.end(), ::isdigit))
			continue;

		try
		{
			uint32_t modelId = static_cast<uint32_t>(std::stoul(folderName));
			if (modelId >= CVehicleMgr::CUSTOM_MODEL_START && modelId <= CVehicleMgr::MAX_NETWORK_VEHICLES)
			{
				fs::path dffPath = entry.path() / "model.dff";
				fs::path iniPath = entry.path() / "model.ini";
				if (fs::exists(dffPath) || fs::exists(iniPath))
				{
					if (DefineCustomVehicleFromConfig(modelId, defaultVisualBase))
					{
						loadedCount++;
					}
				}
			}
		}
		catch (...) {}
	}

	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] LoadAllCustomVehicles: Auto-loaded %d custom vehicle model(s) from '%s'", loadedCount, g_modelsDir.c_str());
	}
	return loadedCount;
}

bool GetCustomVehicleName(uint32_t customModelId, std::string& name)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && !it->second.name.empty())
	{
		name = it->second.name;
		return true;
	}
	return false;
}

bool SetCustomVehicleName(uint32_t customModelId, const std::string& name)
{
	customVehicleConfigs[customModelId].name = name;
	return true;
}

bool GetCustomVehicleConfigString(uint32_t customModelId, const std::string& key, std::string& outValue)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it == customVehicleConfigs.end())
		return false;

	std::string lowerKey = ModelConfigParser::ToLower(key);
	auto propIt = it->second.customProperties.find(lowerKey);
	if (propIt != it->second.customProperties.end())
	{
		outValue = propIt->second;
		return true;
	}
	return false;
}

bool SetCustomVehicleConfigString(uint32_t customModelId, const std::string& key, const std::string& value)
{
	std::string lowerKey = ModelConfigParser::ToLower(key);
	customVehicleConfigs[customModelId].customProperties[lowerKey] = value;
	return true;
}

bool GetCustomVehicleConfigInt(uint32_t customModelId, const std::string& key, int& outValue)
{
	std::string s;
	if (GetCustomVehicleConfigString(customModelId, key, s))
	{
		try
		{
			outValue = std::stoi(s);
			return true;
		}
		catch (...) {}
	}
	return false;
}

bool SetCustomVehicleConfigInt(uint32_t customModelId, const std::string& key, int value)
{
	return SetCustomVehicleConfigString(customModelId, key, std::to_string(value));
}

bool GetCustomVehicleConfigFloat(uint32_t customModelId, const std::string& key, float& outValue)
{
	std::string s;
	if (GetCustomVehicleConfigString(customModelId, key, s))
	{
		try
		{
			outValue = std::stof(s);
			return true;
		}
		catch (...) {}
	}
	return false;
}

bool SetCustomVehicleConfigFloat(uint32_t customModelId, const std::string& key, float value)
{
	return SetCustomVehicleConfigString(customModelId, key, std::to_string(value));
}

const std::vector<std::array<uint8_t, 4>>* GetCustomVehicleColorVariations(uint32_t customModelId)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && it->second.hasCarcols)
	{
		return &it->second.colorVariations;
	}
	return nullptr;
}

const std::vector<int>* GetCustomVehicleAllowedUpgrades(uint32_t customModelId)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && it->second.hasCarmods)
	{
		return &it->second.modIds;
	}
	return nullptr;
}

int GetCustomVehicleColorVariationsCount(uint32_t customModelId)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && it->second.hasCarcols)
	{
		return static_cast<int>(it->second.colorVariations.size());
	}
	return 0;
}

bool GetCustomVehicleColorVariation(uint32_t customModelId, int index, int& p, int& s, int& t, int& q)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && it->second.hasCarcols)
	{
		if (index >= 0 && index < static_cast<int>(it->second.colorVariations.size()))
		{
			const auto& var = it->second.colorVariations[static_cast<size_t>(index)];
			p = var[0];
			s = var[1];
			t = var[2];
			q = var[3];
			return true;
		}
	}
	return false;
}

bool GetCustomVehicleDefaultColors(uint32_t customModelId, int& p, int& s, int& t, int& q)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && it->second.hasCarcols)
	{
		p = it->second.defaultPrimaryColor;
		s = it->second.defaultSecondaryColor;
		t = it->second.defaultTertiaryColor;
		q = it->second.defaultQuaternaryColor;
		return true;
	}
	return false;
}

int GetCustomVehicleAllowedUpgradesCount(uint32_t customModelId)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && it->second.hasCarmods)
	{
		return static_cast<int>(it->second.modIds.size());
	}
	return 0;
}

int GetCustomVehicleAllowedUpgrade(uint32_t customModelId, int index)
{
	auto it = customVehicleConfigs.find(customModelId);
	if (it != customVehicleConfigs.end() && it->second.hasCarmods)
	{
		if (index >= 0 && index < static_cast<int>(it->second.modIds.size()))
		{
			return it->second.modIds[static_cast<size_t>(index)];
		}
	}
	return -1;
}
}
