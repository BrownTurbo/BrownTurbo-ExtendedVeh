#pragma once

#include <cstdint>
#include <cstring>
#include <sdk.hpp>

#include "CPlayer.h"
#include "CVehicleManager.hpp"
#include "HandlingEnum.h"
#include "HandlingManager.h"
#include "Hooks.hpp"
#include "extendedveh.h"
#include "CustomVehicleBindingRegistry.h"
#include "CVehicleManager.hpp"
#include "ModelTransferManager.h"
#include "utils.h"
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Pawn/Impl/pawn_natives.hpp>

using namespace NativeHook;

namespace FuncHook
{
inline cell OnCreateVehicleHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (compo)
	{
		ICore* core_ = compo->getCore();
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] Hooked CreateVehicle");
		}
	}
	const int vehicleid = static_cast<int>(orig(amx, params));
	if (vehicleid != INVALID_VEHICLE_ID)
	{
		HandlingMgr::OnCreateVehicle(vehicleid);
	}
	return static_cast<cell>(vehicleid);
}

inline cell OnAddStaticVehicleHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (compo)
	{
		ICore* core_ = compo->getCore();
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] Hooked AddStaticVehicle");
		}
	}
	const int vehicleid = static_cast<int>(orig(amx, params));
	if (vehicleid != INVALID_VEHICLE_ID)
	{
		HandlingMgr::OnCreateVehicle(vehicleid);
	}
	return static_cast<cell>(vehicleid);
}

inline cell OnAddStaticVehicleExHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (compo)
	{
		ICore* core_ = compo->getCore();
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] Hooked AddStaticVehicleEx");
		}
	}
	const int vehicleid = static_cast<int>(orig(amx, params));
	if (vehicleid != INVALID_VEHICLE_ID)
	{
		HandlingMgr::OnCreateVehicle(vehicleid);
	}
	return static_cast<cell>(vehicleid);
}

inline cell OnDestroyVehicleHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (compo)
	{
		ICore* core_ = compo->getCore();
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] Hooked DestroyVehicle");
		}
	}
	const int vehicleid = static_cast<int>(params[1]);
	if (vehicleid != INVALID_VEHICLE_ID)
	{
		HandlingMgr::OnDestroyVehicle(vehicleid);
	}
	return orig(amx, params);
}
}

inline void RegisterNativeHooks()
{
	auto& hooks = NativeHookManager::Instance();

	hooks.RegisterHookByName("CreateVehicle", &FuncHook::OnCreateVehicleHook);
	hooks.RegisterHookByName("AddStaticVehicle", &FuncHook::OnAddStaticVehicleHook);
	hooks.RegisterHookByName("AddStaticVehicleEx", &FuncHook::OnAddStaticVehicleExHook);
	hooks.RegisterHookByName("DestroyVehicle", &FuncHook::OnDestroyVehicleHook);
}

// Vehicle handling related funcs
// native GetHandlingAttribType(attrib);
SCRIPT_API(GetHandlingAttribType, cell(int attr))
{
	CHandlingAttrib handlingAttr = static_cast<CHandlingAttrib>(attr);
	CHandlingAttribType type = GetHandlingAttributeType(handlingAttr);
	return static_cast<cell>(type);
}

// native IsPlayerUsingCHandling(playerid);
SCRIPT_API(IsPlayerUsingCHandling, bool(IPlayer& player))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			return gPlayers[playerid].hasCHandling();
		}
	}
	return false;
}

// native ResetModelHandling(modelid);
SCRIPT_API(ResetModelHandling, bool(int modelid))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;
	return HandlingMgr::ResetModelHandling(modelid);
}

// native ResetVehicleHandling(vehicleid);
SCRIPT_API(ResetVehicleHandling, bool(IVehicle& vehicle))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicle.getID()))
		return false;
	HandlingMgr::ResetVehicleHandling(vehicle);
	return true;
}

// native SetVehicleHandlingFloat(vehicleid, attrib, Float:value);
SCRIPT_API(SetVehicleHandlingFloat, bool(int vehicleid, CHandlingAttrib attrib, float value))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;
	return HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, value);
}

// native SetVehicleHandlingInt(vehicleid, attrib, value);
SCRIPT_API(SetVehicleHandlingInt, bool(int vehicleid, CHandlingAttrib attrib, int value))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	if (GetHandlingAttributeType(attrib) == TYPE_BYTE)
		return HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, (uint8_t)value);

	return HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, (unsigned int)value);
}

// native SetPlayerHandlingFloat(playerid, attrib, Flost:value);
SCRIPT_API(SetModelHandlingFloat, bool(int modelid, CHandlingAttrib attrib, float value))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;
	return HandlingMgr::SetModelHandling((uint16_t)modelid, attrib, value);
}

// native SetModelHandlingInt(modelid, attrib, value);
SCRIPT_API(SetModelHandlingInt, bool(int modelid, CHandlingAttrib attrib, int value))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;

	if (GetHandlingAttributeType(attrib) == TYPE_BYTE)
		return HandlingMgr::SetModelHandling((uint16_t)modelid, attrib, (uint8_t)value);

	return HandlingMgr::SetModelHandling((uint16_t)modelid, attrib, (unsigned int)value);
}

// native GetVehicleHandlingFloat(vehicleid, attrib, &Float:value);
SCRIPT_API(GetVehicleHandlingFloat, bool(int vehicleid, CHandlingAttrib attrib, float& value))
{
	value = 0.0f;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;
	return HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, value);
}

// native GetVehicleHandlingInt(vehicleid, attrib, &value);
SCRIPT_API(GetVehicleHandlingInt, bool(int vehicleid, CHandlingAttrib attrib, unsigned int& value))
{
	value = 0;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	bool ret = false;
	if (GetHandlingAttributeType(attrib) == TYPE_BYTE)
	{
		uint8_t byteVal = 0;
		ret = HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, byteVal);
		value = byteVal;
	}
	else
	{
		ret = HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, value);
	}
	return ret;
}

// native GetModelHandlingFloat(modelid, attrib, &Float:value);
SCRIPT_API(GetModelHandlingFloat, bool(int modelid, CHandlingAttrib attrib, float& value))
{
	value = 0.0f;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;
	return HandlingMgr::GetModelHandling((uint16_t)modelid, attrib, value);
}

// native GetModelHandlingInt(modelid, attrib, &value);
SCRIPT_API(GetModelHandlingInt, bool(int modelid, CHandlingAttrib attrib, unsigned int& value))
{
	value = 0;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;

	bool ret = false;
	if (GetHandlingAttributeType(attrib) == TYPE_BYTE)
	{
		uint8_t byteVal = 0;
		ret = HandlingMgr::GetModelHandling((uint16_t)modelid, attrib, byteVal);
		value = byteVal;
	}
	else
	{
		ret = HandlingMgr::GetModelHandling((uint16_t)modelid, attrib, value);
	}
	return ret;
}

// native GetDefaultHandlingFloat(modelid, attrib, &Float:value);
SCRIPT_API(GetDefaultHandlingFloat, bool(int modelid, CHandlingAttrib attrib, float& value))
{
	value = 0.0f;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;
	return HandlingMgr::GetDefaultHandling((uint16_t)modelid, attrib, value);
}

// native GetDefaultHandlingInt(modelid, attrib, &value);
SCRIPT_API(GetDefaultHandlingInt, bool(int modelid, CHandlingAttrib attrib, unsigned int& value))
{
	value = 0;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;

	bool ret = false;
	if (GetHandlingAttributeType(attrib) == TYPE_BYTE)
	{
		uint8_t byteVal = 0;
		ret = HandlingMgr::GetDefaultHandling((uint16_t)modelid, attrib, byteVal);
		value = byteVal;
	}
	else
	{
		ret = HandlingMgr::GetDefaultHandling((uint16_t)modelid, attrib, value);
	}
	return ret;
}

// native SetPlayerHandlingFloat(playerid, attrib, Float:value);
SCRIPT_API(SetPlayerHandlingFloat, bool(IPlayer& player, CHandlingAttrib attrib, float value))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			return HandlingMgr::SetPlayerHandling(static_cast<uint16_t>(playerid), attrib, value);
		}
	}
	return false;
}

// native ResetAllHandlingForPlayer(playerid);
SCRIPT_API(ResetAllHandlingForPlayer, bool(IPlayer& player))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			return HandlingMgr::ResetAll(static_cast<uint16_t>(playerid));
		}
	}
	return false;
}

// native SetPlayerHandlingInt(playerid, attrib, value);
SCRIPT_API(SetPlayerHandlingInt, bool(IPlayer& player, CHandlingAttrib attrib, int value))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			if (GetHandlingAttributeType(attrib) == TYPE_BYTE)
				return HandlingMgr::SetPlayerHandling(static_cast<uint16_t>(playerid), attrib, (uint8_t)value);
			return HandlingMgr::SetPlayerHandling(static_cast<uint16_t>(playerid), attrib, (unsigned int)value);
		}
	}
	return false;
}

// native GetPlayerHandlingFloat(playerid, attrib, &Float:value);
SCRIPT_API(GetPlayerHandlingFloat, bool(IPlayer& player, CHandlingAttrib attrib, float& value))
{
	value = 0.0f;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			return HandlingMgr::GetPlayerHandling(static_cast<uint16_t>(playerid), attrib, value);
		}
	}
	return false;
}

// native GetPlayerHandlingInt(playerid, attrib, &value);
SCRIPT_API(GetPlayerHandlingInt, bool(IPlayer& player, CHandlingAttrib attrib, unsigned int& value))
{
	value = 0;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			bool ret = false;

			if (GetHandlingAttributeType(attrib) == TYPE_BYTE)
			{
				uint8_t byteVal = 0;
				ret = HandlingMgr::GetPlayerHandling(static_cast<uint16_t>(playerid), attrib, byteVal);
				value = byteVal;
			}
			else
			{
				ret = HandlingMgr::GetPlayerHandling(static_cast<uint16_t>(playerid), attrib, value);
			}
			return ret;
		}
	}
	return false;
}

// native ResetPlayerHandling(playerid);
SCRIPT_API(ResetPlayerHandling, bool(IPlayer& player))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			return HandlingMgr::ResetPlayerHandling(static_cast<uint16_t>(playerid));
		}
	}
	return false;
}

// native BeginCustomVehicleDef(customModelId, visualBase, audioBase, handlingBase, engineOnSoundId, engineOffSoundId);
SCRIPT_API(BeginCustomVehicleDef, bool(int customModelId, int visualBase, int audioBase, int handlingBase, int engineOnSoundId, int engineOffSoundId))
{
	if (customModelId < 0)
		return false;
	CustomVeh::Protocol::EngineSound engineSoundId;
	engineSoundId.OnSound = static_cast<int16_t>(engineOnSoundId);
	engineSoundId.OffSound = static_cast<int16_t>(engineOffSoundId);
	HandlingMgr::BeginCustomVehicleDef(static_cast<uint32_t>(customModelId), static_cast<uint32_t>(visualBase), static_cast<uint32_t>(audioBase), static_cast<uint32_t>(handlingBase), engineSoundId);
	return true;
}

// native SetCustomVehicleDff(customModelId);
SCRIPT_API(SetCustomVehicleDff, bool(int customModelId))
{
	return HandlingMgr::SetCustomVehicleDff(static_cast<uint32_t>(customModelId));
}

// native SetCustomVehicleTxd(customModelId);
SCRIPT_API(SetCustomVehicleTxd, bool(int customModelId))
{
	return HandlingMgr::SetCustomVehicleTxd(static_cast<uint32_t>(customModelId));
}

// native SetCustomVehicleCol(customModelId);
SCRIPT_API(SetCustomVehicleCol, bool(int customModelId))
{
	return HandlingMgr::SetCustomVehicleCol(static_cast<uint32_t>(customModelId));
}

// native CommitCustomVehicleDef(customModelId);
SCRIPT_API(CommitCustomVehicleDef, bool(int customModelId))
{
	return HandlingMgr::CommitCustomVehicleDef(static_cast<uint32_t>(customModelId));
}

// native DestroyCustomVehicle(customModelId);
SCRIPT_API(DestroyCustomVehicle, bool(int customModelId))
{
	if (!HandlingMgr::IsCustomVehicle(static_cast<uint32_t>(customModelId)))
		return false;
	HandlingMgr::SendCustomVehicleDestroyToAll(static_cast<uint32_t>(customModelId));
	HandlingMgr::UnregisterCustomVehicle(static_cast<uint32_t>(customModelId));
	return true;
}

// native ResetAllHandling(playerid);
SCRIPT_API(ResetAllHandling, bool(IPlayer& player))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	if (!core_)
		return false;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			return HandlingMgr::ResetAll(static_cast<uint16_t>(playerid));
		}
	}
	return false;
}

// native IsCustomVehicleModel(modelid);
SCRIPT_API(IsCustomVehicleModel, bool(int modelid))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;
	return HandlingMgr::IsCustomVehicle(static_cast<uint32_t>(modelid));
}

// native IsVehicleCustom(vehicleid);
SCRIPT_API(IsVehicleCustom, bool(int vehicleid))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	IVehicle* vehicle = compo->GetVehicleByID(vehicleid);
	if (!vehicle)
		return false;
	return HandlingMgr::IsCustomVehicle(static_cast<uint32_t>(vehicle->getModel()));
}

// native BindVehicleModel(vehicleid, customModelId);
SCRIPT_API(BindVehicleModel, bool(int vehicleid, int customModelId))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(customModelId))
		return false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	IVehicle* vehicle = compo->GetVehicleByID(vehicleid);
	if (!vehicle)
		return false;
	CustomVehicleBindingRegistry::Instance().Bind(static_cast<uint16_t>(vehicleid), static_cast<uint32_t>(customModelId));
	return true;
}

// native GetFileSha256(const filename[], outputHash[]);
SCRIPT_API(GetFileSha256, bool(const std::string& filename, std::string& outHash))
{
	std::string result;
	if (!ComputeFileSha256(filename, result))
		return false;
	outHash = result;
	return true;
}

// native InvalidateModelCache(modelid, fileKind);
SCRIPT_API(InvalidateModelCache, bool(int modelId, int kind))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelId))
		return false;
	ModelTransferMgr::InvalidateCache(static_cast<uint32_t>(modelId), static_cast<ModelFileKind>(kind));
	return true;
}

// native GetClientFileStoreStatus(playerid, modelId, fileKind);
// 0=unknown, 1=success, 2=failure
SCRIPT_API(GetClientFileStoreStatus, int(IPlayer& player, int modelId, int kind))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return 0;
	ICore* core_ = compo->getCore();
	if (!core_)
		return 0;
	const auto& players_ = core_->getPlayers().players();
	int playerid = player.getID();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player_ = *it;
		if (player_->getID() == playerid)
		{
			return ModelTransferMgr::GetClientFileStoreStatus(playerid, static_cast<uint32_t>(modelId), static_cast<ModelFileKind>(kind));
		}
	}
	return 0;
}
