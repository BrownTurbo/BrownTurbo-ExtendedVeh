#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <sdk.hpp>

#include "PlayerAttrs.h"
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
#include "defs.h"

using namespace NativeHook;

namespace FuncHook
{
inline cell OnCreateVehicleHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	const int vehicleid = static_cast<int>(orig(amx, params));
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (vehicleid != INVALID_VEHICLE_ID && vehicleid != 0)
	{
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnCreateVehicleHook: Created vehicleid=%d", vehicleid);
		}
		HandlingMgr::OnCreateVehicle(vehicleid);
	}
	return static_cast<cell>(vehicleid);
}

inline cell OnAddStaticVehicleHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	const int vehicleid = static_cast<int>(orig(amx, params));
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (vehicleid != INVALID_VEHICLE_ID && vehicleid != 0)
	{
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnAddStaticVehicleHook: Created vehicleid=%d", vehicleid);
		}
		HandlingMgr::OnCreateVehicle(vehicleid);
	}
	return static_cast<cell>(vehicleid);
}

inline cell OnAddStaticVehicleExHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	const int vehicleid = static_cast<int>(orig(amx, params));
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (vehicleid != INVALID_VEHICLE_ID && vehicleid != 0)
	{
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnAddStaticVehicleExHook: Created vehicleid=%d", vehicleid);
		}
		HandlingMgr::OnCreateVehicle(vehicleid);
	}
	return static_cast<cell>(vehicleid);
}

inline cell OnDestroyVehicleHook(AMX* amx, cell* params, amx_native_fn_t orig)
{
	int vehicleid = INVALID_VEHICLE_ID;
	AMX_HEADER* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
	if (hdr && hdr->magic == 0xf1e0) // AMX_MAGIC_32
	{
		const int32_t* params32 = reinterpret_cast<const int32_t*>(params);
		vehicleid = params32[1];
	}
	else
	{
		vehicleid = static_cast<int>(params[1]);
	}
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (vehicleid != INVALID_VEHICLE_ID && vehicleid != 0)
	{
		if (core_)
		{
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnDestroyVehicleHook: Destroying vehicleid=%d", vehicleid);
		}
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

#undef PAWN_NATIVE_DEFN_
#define PAWN_NATIVE_DEFN_(used_namespace, failret, func, params)               \
                                                                               \
	template <>                                                                \
	cell AMX_NATIVE_CALL Native_##func::Call(AMX* amx, cell* args)             \
	{                                                                          \
		if (amx && args)                                                       \
		{                                                                      \
			AMX_HEADER* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);       \
			if (hdr && hdr->magic == 0xf1e0)                                   \
			{                                                                  \
				const uint32_t* args32 = reinterpret_cast<const uint32_t*>(args); \
				uint32_t byte_count = args32[0];                               \
				uint32_t num_args = byte_count / sizeof(uint32_t);             \
				cell args64[64];                                               \
				uint32_t count = (num_args < 63) ? num_args : 63;             \
				args64[0] = static_cast<cell>(count * sizeof(cell));           \
				for (uint32_t i = 1; i <= count; ++i)                          \
				{                                                              \
					args64[i] = static_cast<cell>(static_cast<uint64_t>(args32[i])); \
				}                                                              \
				return used_namespace::func.CallDoOuter<failret>(amx, args64); \
			}                                                                  \
		}                                                                      \
		return used_namespace::func.CallDoOuter<failret>(amx, args);           \
	}                                                                          \
                                                                               \
	template <>                                                                \
	Native_##func::Native_##func##_()                                          \
		: Base(#func, (AMX_NATIVE)&Call)                                       \
	{                                                                          \
	}                                                                          \
                                                                               \
	Native_##func used_namespace::func;                                        \
                                                                               \
	template <>                                                                \
	PAWN_NATIVE__RETURN(params)                                                \
	Native_##func::                                                            \
		Do(PAWN_NATIVE__PARAMETERS(params)) const;                             \
                                                                               \
	template <typename RET, typename... TS>                                    \
	typename pawn_natives::ReturnResolver<RET>::type NATIVE_##func(TS... args) \
	{                                                                          \
		try                                                                    \
		{                                                                      \
			PAWN_NATIVE__GET_RETURN(params)                                    \
			(used_namespace::func.Do(args...));                                \
		}                                                                      \
		catch (std::exception & e)                                             \
		{                                                                      \
			char msg[1024];                                                    \
			sprintf(msg, "Exception in _" #func ": \"%s\"", e.what());         \
			LOG_NATIVE_ERROR(msg);                                             \
		}                                                                      \
		catch (...)                                                            \
		{                                                                      \
			LOG_NATIVE_ERROR("Unknown exception in _" #func);                  \
		}                                                                      \
		PAWN_NATIVE__DEFAULT_RETURN(params);                                   \
	}                                                                          \
                                                                               \
	template <>                                                                \
	PAWN_NATIVE__RETURN(params)                                                \
	Native_##func::                                                            \
		Do(PAWN_NATIVE__PARAMETERS(params)) const

// Vehicle handling related funcs
// native GetHandlingAttribType(attrib);
SCRIPT_API(GetHandlingAttribType, int(int attr))
{
	CHandlingAttrib handlingAttr = static_cast<CHandlingAttrib>(attr);
	CHandlingAttribType type = GetHandlingAttributeType(handlingAttr);
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (compo) {
		ICore* core_ = compo->getCore();
		if (core_)
		{
			core_->logLn(LogLevel::Message, "[ExtendedVeh] GetHandlingAttribType(attr=%d) returning %d", attr, static_cast<int>(type));
		}
	}
	return static_cast<int>(type);
}

// native IsPlayerUsingExtendedVeh(playerid);
SCRIPT_API(IsPlayerUsingExtendedVeh, bool(IPlayer& player))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	int playerid = player.getID();
	if (core_ && core_->getPlayers().get(playerid) == nullptr)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] IsPlayerUsingExtendedVeh: Player %d is not connected", playerid);
		return false;
	}
	bool has = gPlayers.HasExtendedVeh(playerid);
	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] IsPlayerUsingExtendedVeh(playerid=%d) returning %s (hasExtendedVeh=%d)", playerid, has ? "true" : "false", has ? 1 : 0);
	return has;
}

// native IsPlayerUsingCHandling(playerid);
SCRIPT_API(IsPlayerUsingCHandling, bool(IPlayer& player))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (!compo)
		return false;
	ICore* core_ = compo->getCore();
	int playerid = player.getID();
	if (core_ && core_->getPlayers().get(playerid) == nullptr)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] IsPlayerUsingCHandling: Player %d is not connected", playerid);
		return false;
	}
	bool has = gPlayers.HasExtendedVeh(playerid);
	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] IsPlayerUsingCHandling(playerid=%d) returning %s (hasExtendedVeh=%d)", playerid, has ? "true" : "false", has ? 1 : 0);
	return has;
}

// native ResetModelHandling(modelid);
SCRIPT_API(ResetModelHandling, bool(int modelid))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] ResetModelHandling: Invalid model ID %d", modelid);
		return false;
	}
	bool ret = HandlingMgr::ResetModelHandling(modelid);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] ResetModelHandling(modelid=%d) returning %s", modelid, ret ? "true" : "false");
	return ret;
}

// native ResetVehicleHandling(vehicleid);
SCRIPT_API(ResetVehicleHandling, bool(IVehicle& vehicle))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] ResetVehicleHandling: Invalid vehicle ID %d", vehicleid);
		return false;
	}
	HandlingMgr::ResetVehicleHandling(vehicle);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] ResetVehicleHandling(vehicleid=%d) succeeded", vehicleid);
	return true;
}

// native SetVehicleHandlingFloat(vehicleid, attrib, Float:value);
SCRIPT_API(SetVehicleHandlingFloat, bool(IVehicle& vehicle, CHandlingAttrib attrib, float value))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat: Invalid vehicle ID %d", vehicleid);
		return false;
	}
	if (std::isnan(value) || std::isinf(value))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d): Invalid float value (NaN or Inf)", vehicleid, static_cast<int>(attrib));
		return false;
	}
	if (!CanSetHandlingAttrib(attrib))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d): Attribute is read-only", vehicleid, static_cast<int>(attrib));
		return false;
	}
	if (GetHandlingAttributeType(attrib) != TYPE_FLOAT)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d): Attribute is not of float type", vehicleid, static_cast<int>(attrib));
		return false;
	}
	bool ret = HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, value);
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Message, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d, val=%f) succeeded", vehicleid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d, val=%f) failed in HandlingMgr", vehicleid, static_cast<int>(attrib), value);
	}
	return ret;
}

// native SetVehicleHandlingInt(vehicleid, attrib, value);
SCRIPT_API(SetVehicleHandlingInt, bool(IVehicle& vehicle, CHandlingAttrib attrib, int value))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt: Invalid vehicle ID %d", vehicleid);
		return false;
	}
	if (!CanSetHandlingAttrib(attrib))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d): Attribute is read-only", vehicleid, static_cast<int>(attrib));
		return false;
	}

	CHandlingAttribType attrType = GetHandlingAttributeType(attrib);
	if (attrType != TYPE_BYTE && attrType != TYPE_UINT && attrType != TYPE_FLAG)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d): Attribute type is not int/byte/flag", vehicleid, static_cast<int>(attrib));
		return false;
	}

	bool ret = false;
	if (attrType == TYPE_BYTE)
		ret = HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, (uint8_t)value);
	else
		ret = HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, (unsigned int)value);

	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Message, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d, val=%d) succeeded", vehicleid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d, val=%d) failed in HandlingMgr", vehicleid, static_cast<int>(attrib), value);
	}
	return ret;
}

// native SetModelHandlingFloat(modelid, attrib, Float:value);
SCRIPT_API(SetModelHandlingFloat, bool(int modelid, CHandlingAttrib attrib, float value))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat: Invalid model ID %d", modelid);
		return false;
	}
	if (std::isnan(value) || std::isinf(value))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d): Invalid float value (NaN or Inf)", modelid, static_cast<int>(attrib));
		return false;
	}
	if (!CanSetHandlingAttrib(attrib))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d): Attribute is read-only", modelid, static_cast<int>(attrib));
		return false;
	}
	if (GetHandlingAttributeType(attrib) != TYPE_FLOAT)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d): Attribute is not of float type", modelid, static_cast<int>(attrib));
		return false;
	}
	bool ret = HandlingMgr::SetModelHandling((uint16_t)modelid, attrib, value);
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d, val=%f) succeeded", modelid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d, val=%f) failed in HandlingMgr", modelid, static_cast<int>(attrib), value);
	}
	return ret;
}

// native SetModelHandlingInt(modelid, attrib, value);
SCRIPT_API(SetModelHandlingInt, bool(int modelid, CHandlingAttrib attrib, int value))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt: Invalid model ID %d", modelid);
		return false;
	}
	if (!CanSetHandlingAttrib(attrib))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d): Attribute is read-only", modelid, static_cast<int>(attrib));
		return false;
	}

	CHandlingAttribType attrType = GetHandlingAttributeType(attrib);
	if (attrType != TYPE_BYTE && attrType != TYPE_UINT && attrType != TYPE_FLAG)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d): Attribute type is not int/byte/flag", modelid, static_cast<int>(attrib));
		return false;
	}

	bool ret = false;
	if (attrType == TYPE_BYTE)
		ret = HandlingMgr::SetModelHandling((uint16_t)modelid, attrib, (uint8_t)value);
	else
		ret = HandlingMgr::SetModelHandling((uint16_t)modelid, attrib, (unsigned int)value);

	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d, val=%d) succeeded", modelid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d, val=%d) failed in HandlingMgr", modelid, static_cast<int>(attrib), value);
	}
	return ret;
}

// native GetVehicleHandlingFloat(vehicleid, attrib, &Float:value);
SCRIPT_API(GetVehicleHandlingFloat, bool(IVehicle& vehicle, CHandlingAttrib attrib, float& value))
{
	value = 0.0f;
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleHandlingFloat: Invalid vehicle ID %d", vehicleid);
		return false;
	}
	if (GetHandlingAttributeType(attrib) != TYPE_FLOAT)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleHandlingFloat(veh=%d, attr=%d): Attribute is not float type", vehicleid, static_cast<int>(attrib));
		return false;
	}
	bool ret = HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, value);
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetVehicleHandlingFloat(veh=%d, attr=%d) -> %f", vehicleid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleHandlingFloat(veh=%d, attr=%d) failed to retrieve value", vehicleid, static_cast<int>(attrib));
	}
	return ret;
}

// native GetVehicleHandlingInt(vehicleid, attrib, &value);
SCRIPT_API(GetVehicleHandlingInt, bool(IVehicle& vehicle, CHandlingAttrib attrib, unsigned int& value))
{
	value = 0;
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleHandlingInt: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	CHandlingAttribType attrType = GetHandlingAttributeType(attrib);
	if (attrType != TYPE_BYTE && attrType != TYPE_UINT && attrType != TYPE_FLAG)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleHandlingInt(veh=%d, attr=%d): Attribute is not int/byte/flag type", vehicleid, static_cast<int>(attrib));
		return false;
	}

	bool ret = false;
	if (attrType == TYPE_BYTE)
	{
		uint8_t byteVal = 0;
		ret = HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, byteVal);
		value = byteVal;
	}
	else
	{
		ret = HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), attrib, value);
	}
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetVehicleHandlingInt(veh=%d, attr=%d) -> %u", vehicleid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleHandlingInt(veh=%d, attr=%d) failed to retrieve value", vehicleid, static_cast<int>(attrib));
	}
	return ret;
}

// native SetVehicleWaterDrive(vehicleid, bool:enable, submergedPercent = 30);
SCRIPT_API(SetVehicleWaterDrive, bool(IVehicle& vehicle, bool enable, int submergedPercent))
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleWaterDrive: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	if (submergedPercent <= 0 || submergedPercent > 100)
		submergedPercent = 30;

	unsigned int modelFlags = 0;
	if (!HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags))
	{
		int modelid = vehicle.getModel();
		HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);
	}

	if (enable)
	{
		modelFlags |= VEHICLE_HANDLING_MODEL_IS_BOAT;
		HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags);
		HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_NPERCENTSUBMERGED, static_cast<uint8_t>(submergedPercent));
	}
	else
	{
		modelFlags &= ~VEHICLE_HANDLING_MODEL_IS_BOAT;
		HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags);
		uint8_t defSub = 85;
		int modelid = vehicle.getModel();
		if (HandlingMgr::GetDefaultHandling(static_cast<uint16_t>(modelid), HANDL_NPERCENTSUBMERGED, defSub))
			HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_NPERCENTSUBMERGED, defSub);
	}

	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] SetVehicleWaterDrive(veh=%d, enable=%d, submerged=%d) succeeded", vehicleid, enable, submergedPercent);
	return true;
}

// native GetVehicleWaterDrive(vehicleid, &bool:enabled);
SCRIPT_API(GetVehicleWaterDrive, bool(IVehicle& vehicle, bool& enabled))
{
	enabled = false;
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleWaterDrive: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	unsigned int modelFlags = 0;
	if (HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags))
	{
		enabled = (modelFlags & VEHICLE_HANDLING_MODEL_IS_BOAT) != 0;
	}
	else
	{
		int modelid = vehicle.getModel();
		if (HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags))
			enabled = (modelFlags & VEHICLE_HANDLING_MODEL_IS_BOAT) != 0;
	}

	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetVehicleWaterDrive(veh=%d) -> enabled=%d", vehicleid, enabled);
	return true;
}

// native SetModelWaterDrive(modelid, bool:enable, submergedPercent = 30);
SCRIPT_API(SetModelWaterDrive, bool(int modelid, bool enable, int submergedPercent))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelWaterDrive: Invalid model ID %d", modelid);
		return false;
	}

	if (submergedPercent <= 0 || submergedPercent > 100)
		submergedPercent = 30;

	unsigned int modelFlags = 0;
	HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);

	if (enable)
	{
		modelFlags |= VEHICLE_HANDLING_MODEL_IS_BOAT;
		HandlingMgr::SetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);
		HandlingMgr::SetModelHandling(static_cast<uint16_t>(modelid), HANDL_NPERCENTSUBMERGED, static_cast<uint8_t>(submergedPercent));
	}
	else
	{
		modelFlags &= ~VEHICLE_HANDLING_MODEL_IS_BOAT;
		HandlingMgr::SetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);
		uint8_t defSub = 85;
		if (HandlingMgr::GetDefaultHandling(static_cast<uint16_t>(modelid), HANDL_NPERCENTSUBMERGED, defSub))
			HandlingMgr::SetModelHandling(static_cast<uint16_t>(modelid), HANDL_NPERCENTSUBMERGED, defSub);
	}

	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] SetModelWaterDrive(model=%d, enable=%d, submerged=%d) succeeded", modelid, enable, submergedPercent);
	return true;
}

// native GetModelWaterDrive(modelid, &bool:enabled);
SCRIPT_API(GetModelWaterDrive, bool(int modelid, bool& enabled))
{
	enabled = false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelWaterDrive: Invalid model ID %d", modelid);
		return false;
	}

	unsigned int modelFlags = 0;
	if (HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags))
		enabled = (modelFlags & VEHICLE_HANDLING_MODEL_IS_BOAT) != 0;

	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetModelWaterDrive(model=%d) -> enabled=%d", modelid, enabled);
	return true;
}

// native SetVehicleFlying(vehicleid, bool:enable);
SCRIPT_API(SetVehicleFlying, bool(IVehicle& vehicle, bool enable))
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleFlying: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	unsigned int modelFlags = 0;
	if (!HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags))
	{
		int modelid = vehicle.getModel();
		HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);
	}

	if (enable)
	{
		modelFlags |= VEHICLE_HANDLING_MODEL_IS_PLANE;
	}
	else
	{
		modelFlags &= ~VEHICLE_HANDLING_MODEL_IS_PLANE;
	}
	HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags);

	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] SetVehicleFlying(veh=%d, enable=%d) succeeded", vehicleid, enable);
	return true;
}

// native GetVehicleFlying(vehicleid, &bool:enabled);
SCRIPT_API(GetVehicleFlying, bool(IVehicle& vehicle, bool& enabled))
{
	enabled = false;
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleFlying: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	unsigned int modelFlags = 0;
	if (HandlingMgr::GetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags))
	{
		enabled = (modelFlags & VEHICLE_HANDLING_MODEL_IS_PLANE) != 0;
	}
	else
	{
		int modelid = vehicle.getModel();
		if (HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags))
		{
			enabled = (modelFlags & VEHICLE_HANDLING_MODEL_IS_PLANE) != 0;
		}
	}

	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetVehicleFlying(veh=%d) -> enabled=%d", vehicleid, enabled);
	return true;
}

// native SetModelFlying(modelid, bool:enable);
SCRIPT_API(SetModelFlying, bool(int modelid, bool enable))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelFlying: Invalid model ID %d", modelid);
		return false;
	}

	unsigned int modelFlags = 0;
	HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);

	if (enable)
	{
		modelFlags |= VEHICLE_HANDLING_MODEL_IS_PLANE;
	}
	else
	{
		modelFlags &= ~VEHICLE_HANDLING_MODEL_IS_PLANE;
	}
	HandlingMgr::SetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);

	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] SetModelFlying(model=%d, enable=%d) succeeded", modelid, enable);
	return true;
}

// native GetModelFlying(modelid, &bool:enabled);
SCRIPT_API(GetModelFlying, bool(int modelid, bool& enabled))
{
	enabled = false;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelFlying: Invalid model ID %d", modelid);
		return false;
	}

	unsigned int modelFlags = 0;
	if (HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags))
		enabled = (modelFlags & VEHICLE_HANDLING_MODEL_IS_PLANE) != 0;

	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetModelFlying(model=%d) -> enabled=%d", modelid, enabled);
	return true;
}

// native GetModelHandlingFloat(modelid, attrib, &Float:value);
SCRIPT_API(GetModelHandlingFloat, bool(int modelid, CHandlingAttrib attrib, float& value))
{
	value = 0.0f;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelHandlingFloat: Invalid model ID %d", modelid);
		return false;
	}
	if (GetHandlingAttributeType(attrib) != TYPE_FLOAT)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelHandlingFloat(model=%d, attr=%d): Attribute is not float type", modelid, static_cast<int>(attrib));
		return false;
	}
	bool ret = HandlingMgr::GetModelHandling((uint16_t)modelid, attrib, value);
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetModelHandlingFloat(model=%d, attr=%d) -> %f", modelid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelHandlingFloat(model=%d, attr=%d) failed to retrieve value", modelid, static_cast<int>(attrib));
	}
	return ret;
}

// native GetModelHandlingInt(modelid, attrib, &value);
SCRIPT_API(GetModelHandlingInt, bool(int modelid, CHandlingAttrib attrib, unsigned int& value))
{
	value = 0;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelHandlingInt: Invalid model ID %d", modelid);
		return false;
	}

	CHandlingAttribType attrType = GetHandlingAttributeType(attrib);
	if (attrType != TYPE_BYTE && attrType != TYPE_UINT && attrType != TYPE_FLAG)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelHandlingInt(model=%d, attr=%d): Attribute is not int/byte/flag type", modelid, static_cast<int>(attrib));
		return false;
	}

	bool ret = false;
	if (attrType == TYPE_BYTE)
	{
		uint8_t byteVal = 0;
		ret = HandlingMgr::GetModelHandling((uint16_t)modelid, attrib, byteVal);
		value = byteVal;
	}
	else
	{
		ret = HandlingMgr::GetModelHandling((uint16_t)modelid, attrib, value);
	}
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetModelHandlingInt(model=%d, attr=%d) -> %u", modelid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetModelHandlingInt(model=%d, attr=%d) failed to retrieve value", modelid, static_cast<int>(attrib));
	}
	return ret;
}

// native GetDefaultHandlingFloat(modelid, attrib, &Float:value);
SCRIPT_API(GetDefaultHandlingFloat, bool(int modelid, CHandlingAttrib attrib, float& value))
{
	value = 0.0f;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetDefaultHandlingFloat: Invalid model ID %d", modelid);
		return false;
	}
	if (GetHandlingAttributeType(attrib) != TYPE_FLOAT)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetDefaultHandlingFloat(model=%d, attr=%d): Attribute is not float type", modelid, static_cast<int>(attrib));
		return false;
	}
	bool ret = HandlingMgr::GetDefaultHandling((uint16_t)modelid, attrib, value);
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetDefaultHandlingFloat(model=%d, attr=%d) -> %f", modelid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetDefaultHandlingFloat(model=%d, attr=%d) failed to retrieve value", modelid, static_cast<int>(attrib));
	}
	return ret;
}

// native GetDefaultHandlingInt(modelid, attrib, &value);
SCRIPT_API(GetDefaultHandlingInt, bool(int modelid, CHandlingAttrib attrib, unsigned int& value))
{
	value = 0;
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetDefaultHandlingInt: Invalid model ID %d", modelid);
		return false;
	}

	CHandlingAttribType attrType = GetHandlingAttributeType(attrib);
	if (attrType != TYPE_BYTE && attrType != TYPE_UINT && attrType != TYPE_FLAG)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetDefaultHandlingInt(model=%d, attr=%d): Attribute is not int/byte/flag type", modelid, static_cast<int>(attrib));
		return false;
	}

	bool ret = false;
	if (attrType == TYPE_BYTE)
	{
		uint8_t byteVal = 0;
		ret = HandlingMgr::GetDefaultHandling((uint16_t)modelid, attrib, byteVal);
		value = byteVal;
	}
	else
	{
		ret = HandlingMgr::GetDefaultHandling((uint16_t)modelid, attrib, value);
	}
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetDefaultHandlingInt(model=%d, attr=%d) -> %u", modelid, static_cast<int>(attrib), value);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetDefaultHandlingInt(model=%d, attr=%d) failed to retrieve value", modelid, static_cast<int>(attrib));
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return false;

	if (std::isnan(value) || std::isinf(value))
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d): Invalid float value (NaN or Inf)", playerid, static_cast<int>(attrib));
		return false;
	}
	if (!CanSetHandlingAttrib(attrib))
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d): Attribute is read-only", playerid, static_cast<int>(attrib));
		return false;
	}
	if (GetHandlingAttributeType(attrib) != TYPE_FLOAT)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d): Attribute is not float type", playerid, static_cast<int>(attrib));
		return false;
	}
	bool ret = HandlingMgr::SetPlayerHandling(static_cast<uint16_t>(playerid), attrib, value);
	if (ret)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d, val=%f) succeeded", playerid, static_cast<int>(attrib), value);
	else
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d, val=%f) failed in HandlingMgr", playerid, static_cast<int>(attrib), value);
	return ret;
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return false;
	bool ret = HandlingMgr::ResetAll(static_cast<uint16_t>(playerid));
	core_->logLn(LogLevel::Debug, "[ExtendedVeh] ResetAllHandlingForPlayer(playerid=%d) returning %s", playerid, ret ? "true" : "false");
	return ret;
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return false;
	if (!CanSetHandlingAttrib(attrib))
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d): Attribute is read-only", playerid, static_cast<int>(attrib));
		return false;
	}

	CHandlingAttribType attrType = GetHandlingAttributeType(attrib);
	if (attrType != TYPE_BYTE && attrType != TYPE_UINT && attrType != TYPE_FLAG)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d): Attribute is not int/byte/flag type", playerid, static_cast<int>(attrib));
		return false;
	}

	bool ret = false;
	if (attrType == TYPE_BYTE)
		ret = HandlingMgr::SetPlayerHandling(static_cast<uint16_t>(playerid), attrib, (uint8_t)value);
	else
		ret = HandlingMgr::SetPlayerHandling(static_cast<uint16_t>(playerid), attrib, (unsigned int)value);

	if (ret)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d, val=%d) succeeded", playerid, static_cast<int>(attrib), value);
	else
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d, val=%d) failed in HandlingMgr", playerid, static_cast<int>(attrib), value);
	return ret;
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return false;

	if (GetHandlingAttributeType(attrib) != TYPE_FLOAT)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetPlayerHandlingFloat(player=%d, attr=%d): Attribute is not float type", playerid, static_cast<int>(attrib));
		return false;
	}
	bool ret = HandlingMgr::GetPlayerHandling(static_cast<uint16_t>(playerid), attrib, value);
	if (ret)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetPlayerHandlingFloat(player=%d, attr=%d) -> %f", playerid, static_cast<int>(attrib), value);
	else
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetPlayerHandlingFloat(player=%d, attr=%d) failed to retrieve value", playerid, static_cast<int>(attrib));
	return ret;
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return false;

	CHandlingAttribType attrType = GetHandlingAttributeType(attrib);
	if (attrType != TYPE_BYTE && attrType != TYPE_UINT && attrType != TYPE_FLAG)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetPlayerHandlingInt(player=%d, attr=%d): Attribute is not int/byte/flag type", playerid, static_cast<int>(attrib));
		return false;
	}

	bool ret = false;
	if (attrType == TYPE_BYTE)
	{
		uint8_t byteVal = 0;
		ret = HandlingMgr::GetPlayerHandling(static_cast<uint16_t>(playerid), attrib, byteVal);
		value = byteVal;
	}
	else
	{
		ret = HandlingMgr::GetPlayerHandling(static_cast<uint16_t>(playerid), attrib, value);
	}
	if (ret)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetPlayerHandlingInt(player=%d, attr=%d) -> %u", playerid, static_cast<int>(attrib), value);
	else
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetPlayerHandlingInt(player=%d, attr=%d) failed to retrieve value", playerid, static_cast<int>(attrib));
	return ret;
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return false;
	bool ret = HandlingMgr::ResetPlayerHandling(static_cast<uint16_t>(playerid));
	core_->logLn(LogLevel::Debug, "[ExtendedVeh] ResetPlayerHandling(playerid=%d) returning %s", playerid, ret ? "true" : "false");
	return ret;
}

// native BeginCustomVehicleDef(customModelId, visualBase, audioBase, handlingBase, engineOnSoundId, engineOffSoundId);
SCRIPT_API(BeginCustomVehicleDef, bool(int customModelId, int visualBase, int audioBase, int handlingBase, int engineOnSoundId, int engineOffSoundId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Error, "[ExtendedVeh] BeginCustomVehicleDef: Invalid customModelId=%d (must be %d-%d)", customModelId, CVehicleMgr::CUSTOM_MODEL_START, CVehicleMgr::MAX_NETWORK_VEHICLES);
		return false;
	}
	if (!CVehicleMgr::IsBaseVehicleModel(static_cast<uint32_t>(visualBase)))
	{
		if (core_)
			core_->logLn(LogLevel::Error, "[ExtendedVeh] BeginCustomVehicleDef: Invalid visualBase model %d (must be 400-611)", visualBase);
		return false;
	}
	if (!CVehicleMgr::IsBaseVehicleModel(static_cast<uint32_t>(handlingBase)))
	{
		if (core_)
			core_->logLn(LogLevel::Error, "[ExtendedVeh] BeginCustomVehicleDef: Invalid handlingBase model %d (must be 400-611)", handlingBase);
		return false;
	}
	if (!CVehicleMgr::IsBaseVehicleModel(static_cast<uint32_t>(audioBase)))
	{
		if (core_)
			core_->logLn(LogLevel::Error, "[ExtendedVeh] BeginCustomVehicleDef: Invalid audioBase model %d (must be 400-611)", audioBase);
		return false;
	}

	CustomVeh::Protocol::EngineSound engineSoundId;
	engineSoundId.OnSound = static_cast<int16_t>(engineOnSoundId);
	engineSoundId.OffSound = static_cast<int16_t>(engineOffSoundId);
	HandlingMgr::BeginCustomVehicleDef(static_cast<uint32_t>(customModelId), static_cast<uint32_t>(visualBase), static_cast<uint32_t>(audioBase), static_cast<uint32_t>(handlingBase), engineSoundId);
	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] BeginCustomVehicleDef: Staged custom model %d (visual=%d, audio=%d, handling=%d)", customModelId, visualBase, audioBase, handlingBase);
	return true;
}

// native SetCustomVehicleDff(customModelId);
SCRIPT_API(SetCustomVehicleDff, bool(int customModelId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleDff: Invalid customModelId %d", customModelId);
		return false;
	}
	bool ret = HandlingMgr::SetCustomVehicleDff(static_cast<uint32_t>(customModelId));
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetCustomVehicleDff(model=%d) returning %s", customModelId, ret ? "true" : "false");
	return ret;
}

// native SetCustomVehicleTxd(customModelId);
SCRIPT_API(SetCustomVehicleTxd, bool(int customModelId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleTxd: Invalid customModelId %d", customModelId);
		return false;
	}
	bool ret = HandlingMgr::SetCustomVehicleTxd(static_cast<uint32_t>(customModelId));
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetCustomVehicleTxd(model=%d) returning %s", customModelId, ret ? "true" : "false");
	return ret;
}

// native SetCustomVehicleCol(customModelId);
SCRIPT_API(SetCustomVehicleCol, bool(int customModelId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleCol: Invalid customModelId %d", customModelId);
		return false;
	}
	bool ret = HandlingMgr::SetCustomVehicleCol(static_cast<uint32_t>(customModelId));
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetCustomVehicleCol(model=%d) returning %s", customModelId, ret ? "true" : "false");
	return ret;
}

// native CommitCustomVehicleDef(customModelId);
SCRIPT_API(CommitCustomVehicleDef, bool(int customModelId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Error, "[ExtendedVeh] CommitCustomVehicleDef: Invalid customModelId %d", customModelId);
		return false;
	}
	bool ret = HandlingMgr::CommitCustomVehicleDef(static_cast<uint32_t>(customModelId));
	if (core_)
	{
		if (ret)
			core_->logLn(LogLevel::Message, "[ExtendedVeh] CommitCustomVehicleDef: Successfully committed custom model %d", customModelId);
		else
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] CommitCustomVehicleDef: Failed to commit custom model %d (not staged or invalid assets)", customModelId);
	}
	return ret;
}

// native DestroyCustomVehicle(customModelId);
SCRIPT_API(DestroyCustomVehicle, bool(int customModelId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!HandlingMgr::IsCustomVehicle(static_cast<uint32_t>(customModelId)))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] DestroyCustomVehicle: Model ID %d is not a registered custom vehicle", customModelId);
		return false;
	}
	HandlingMgr::UnregisterCustomVehicle(static_cast<uint32_t>(customModelId));
	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] DestroyCustomVehicle: Unregistered custom vehicle model %d", customModelId);
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return false;
	bool ret = HandlingMgr::ResetAll(static_cast<uint16_t>(playerid));
	core_->logLn(LogLevel::Debug, "[ExtendedVeh] ResetAllHandling(playerid=%d) returning %s", playerid, ret ? "true" : "false");
	return ret;
}

// native IsCustomVehicleModel(modelid);
SCRIPT_API(IsCustomVehicleModel, bool(int modelid))
{
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelid))
		return false;
	return HandlingMgr::IsCustomVehicle(static_cast<uint32_t>(modelid));
}

// native IsVehicleCustom(vehicleid);
SCRIPT_API(IsVehicleCustom, bool(IVehicle& vehicle))
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;
	return CustomVehicleBindingRegistry::Instance().Get(static_cast<uint16_t>(vehicleid)).has_value();
}

// native BindVehicleModel(vehicleid, customModelId);
SCRIPT_API(BindVehicleModel, bool(IVehicle& vehicle, int customModelId))
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] BindVehicleModel: Invalid vehicle ID %d", vehicleid);
		return false;
	}
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(customModelId))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] BindVehicleModel: Invalid customModelId %d", customModelId);
		return false;
	}
	if (!compo)
		return false;
	CustomVehicleBindingRegistry::Instance().Bind(static_cast<uint16_t>(vehicleid), static_cast<uint32_t>(customModelId));

	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] BindVehicleModel: Bound vehicle %d to custom model %d", vehicleid, customModelId);
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleBind(*player, static_cast<uint16_t>(vehicleid), static_cast<uint32_t>(customModelId));
			}
		}
	}
	return true;
}

// native GetFileSha256(const filename[], outputHash[]);
SCRIPT_API(GetFileSha256, bool(const std::string& filename, std::string& outHash))
{
	std::string result;
	if (!ComputeFileSha256(filename, result))
	{
		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		if (compo && compo->getCore())
			compo->getCore()->logLn(LogLevel::Warning, "[ExtendedVeh] GetFileSha256: Failed to compute SHA256 for '%s'", filename.c_str());
		return false;
	}
	outHash = result;
	return true;
}

// native InvalidateModelCache(modelid, fileKind);
SCRIPT_API(InvalidateModelCache, bool(int modelId, int kind))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(modelId))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] InvalidateModelCache: Invalid model ID %d", modelId);
		return false;
	}
	ModelTransferMgr::InvalidateCache(static_cast<uint32_t>(modelId), static_cast<ModelFileKind>(kind));
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] InvalidateModelCache: Invalidated model %d kind %d", modelId, kind);
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
	int playerid = player.getID();
	if (core_->getPlayers().get(playerid) == nullptr)
		return 0;
	return ModelTransferMgr::GetClientFileStoreStatus(playerid, static_cast<uint32_t>(modelId), static_cast<ModelFileKind>(kind));
}

// native SetVehicleDoorMissing(vehicleid, doorid, bool:missing);
SCRIPT_API(SetVehicleDoorMissing, bool(IVehicle& vehicle, int doorid, bool missing))
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleDoorMissing: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	bool ret = HandlingMgr::SetVehicleDoorMissing(static_cast<uint16_t>(vehicleid), static_cast<uint8_t>(doorid), missing);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetVehicleDoorMissing(veh=%d, door=%d, missing=%d) -> %s", vehicleid, doorid, missing, ret ? "true" : "false");
	return ret;
}

// native GetVehicleDoorMissing(vehicleid, doorid, &bool:missing);
SCRIPT_API(GetVehicleDoorMissing, bool(IVehicle& vehicle, int doorid, bool& missing))
{
	missing = false;
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetVehicleDoorMissing: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	bool ret = HandlingMgr::GetVehicleDoorMissing(static_cast<uint16_t>(vehicleid), static_cast<uint8_t>(doorid), missing);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetVehicleDoorMissing(veh=%d, door=%d) -> missing=%d, ret=%s", vehicleid, doorid, missing, ret ? "true" : "false");
	return ret;
}

// native SetVehicleAllDoorsMissing(vehicleid, bool:missing);
SCRIPT_API(SetVehicleAllDoorsMissing, bool(IVehicle& vehicle, bool missing))
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleAllDoorsMissing: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	bool ret = HandlingMgr::SetVehicleAllDoorsMissing(static_cast<uint16_t>(vehicleid), missing);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetVehicleAllDoorsMissing(veh=%d, missing=%d) -> %s", vehicleid, missing, ret ? "true" : "false");
	return ret;
}

