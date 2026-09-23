#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <sdk.hpp>
#include <optional>

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
#define PAWN_NATIVE_DEFN_(used_namespace, failret, func, params)                     \
                                                                                     \
	template <>                                                                      \
	cell AMX_NATIVE_CALL Native_##func::Call(AMX* amx, cell* args)                   \
	{                                                                                \
		if (amx && args)                                                             \
		{                                                                            \
			AMX_HEADER* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);              \
			if (hdr && hdr->magic == 0xf1e0)                                         \
			{                                                                        \
				const uint32_t* args32 = reinterpret_cast<const uint32_t*>(args);    \
				uint32_t byte_count = args32[0];                                     \
				uint32_t num_args = byte_count / sizeof(uint32_t);                   \
				cell args64[64];                                                     \
				uint32_t count = (num_args < 63) ? num_args : 63;                    \
				args64[0] = static_cast<cell>(count * sizeof(cell));                 \
				for (uint32_t i = 1; i <= count; ++i)                                \
				{                                                                    \
					args64[i] = static_cast<cell>(static_cast<uint64_t>(args32[i])); \
				}                                                                    \
				return used_namespace::func.CallDoOuter<failret>(amx, args64);       \
			}                                                                        \
		}                                                                            \
		return used_namespace::func.CallDoOuter<failret>(amx, args);                 \
	}                                                                                \
                                                                                     \
	template <>                                                                      \
	Native_##func::Native_##func##_()                                                \
		: Base(#func, (AMX_NATIVE) & Call)                                           \
	{                                                                                \
	}                                                                                \
                                                                                     \
	Native_##func used_namespace::func;                                              \
                                                                                     \
	template <>                                                                      \
	PAWN_NATIVE__RETURN(params)                                                      \
	Native_##func::                                                                  \
		Do(PAWN_NATIVE__PARAMETERS(params)) const;                                   \
                                                                                     \
	template <typename RET, typename... TS>                                          \
	typename pawn_natives::ReturnResolver<RET>::type NATIVE_##func(TS... args)       \
	{                                                                                \
		try                                                                          \
		{                                                                            \
			PAWN_NATIVE__GET_RETURN(params)                                          \
			(used_namespace::func.Do(args...));                                      \
		}                                                                            \
		catch (std::exception & e)                                                   \
		{                                                                            \
			char msg[1024];                                                          \
			sprintf(msg, "Exception in _" #func ": \"%s\"", e.what());               \
			LOG_NATIVE_ERROR(msg);                                                   \
		}                                                                            \
		catch (...)                                                                  \
		{                                                                            \
			LOG_NATIVE_ERROR("Unknown exception in _" #func);                        \
		}                                                                            \
		PAWN_NATIVE__DEFAULT_RETURN(params);                                         \
	}                                                                                \
                                                                                     \
	template <>                                                                      \
	PAWN_NATIVE__RETURN(params)                                                      \
	Native_##func::                                                                  \
		Do(PAWN_NATIVE__PARAMETERS(params)) const

inline uint32_t ResolveBaseVehicleModel(uint32_t modelId)
{
	if (CVehicleMgr::IsCustomVehicleModel(modelId))
	{
		auto it = HandlingMgr::customVehicleDefs.find(modelId);
		if (it != HandlingMgr::customVehicleDefs.end())
		{
			if (CVehicleMgr::IsBaseVehicleModel(it->second.handlingBaseModel))
				return it->second.handlingBaseModel;
			if (CVehicleMgr::IsBaseVehicleModel(it->second.visualBaseModel))
				return it->second.visualBaseModel;
		}
	}
	return modelId;
}

inline uint32_t ResolveVehicleBaseModel(IVehicle& vehicle)
{
	int vehicleid = vehicle.getID();
	auto customOpt = CustomVehicleBindingRegistry::Instance().Get(static_cast<uint16_t>(vehicleid));
	if (customOpt.has_value())
	{
		return ResolveBaseVehicleModel(customOpt.value());
	}
	return ResolveBaseVehicleModel(static_cast<uint32_t>(vehicle.getModel()));
}

// Vehicle handling related funcs
// native GetHandlingAttribType(attrib);
SCRIPT_API(GetHandlingAttribType, int(int attr))
{
	CHandlingAttrib handlingAttr = static_cast<CHandlingAttrib>(attr);
	CHandlingAttribType type = GetHandlingAttributeType(handlingAttr);
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	if (compo)
	{
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
	if ((attrib == HANDL_FMASS || attrib == HANDL_FTURNMASS) && value < 1.0f)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d): Mass/TurnMass must be >= 1.0 (got %f)", vehicleid, static_cast<int>(attrib), value);
		return false;
	}
	if (attrib == HANDL_FBRAKEDECELERATION && value < 0.0f)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d): BrakeDeceleration must be >= 0.0 (got %f)", vehicleid, static_cast<int>(attrib), value);
		return false;
	}
	if ((attrib == HANDL_FSUSPENSIONFORCELEVEL || attrib == HANDL_FSUSPENSIONDAMPINGLEVEL || attrib == HANDL_FTRACTIONMULTIPLIER) && value <= 0.0f)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingFloat(veh=%d, attr=%d): Suspension force/damping and traction multiplier must be > 0.0 (got %f)", vehicleid, static_cast<int>(attrib), value);
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

	if (attrib == HANDL_MODELFLAGS)
	{
		uint32_t baseModel = ResolveVehicleBaseModel(vehicle);
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_PLANE) && !CVehicleMgr::IsVehicleModelFlightCapable(baseModel))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_PLANE for vehicle %d (base %u, category '%s')",
					vehicleid, baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));
			value &= ~VEHICLE_HANDLING_MODEL_IS_PLANE;
		}
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_BOAT) && !CVehicleMgr::IsVehicleModelWaterDriveCapable(baseModel))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_BOAT for vehicle %d (base %u, category '%s')",
					vehicleid, baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));
			value &= ~VEHICLE_HANDLING_MODEL_IS_BOAT;
		}
		CVehicleMgr::VehicleCategory cat = CVehicleMgr::GetVehicleModelCategory(baseModel);
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_HELI) && cat != CVehicleMgr::VehicleCategory::Helicopter)
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_HELI for vehicle %d (base %u, category '%s')",
					vehicleid, baseModel, CVehicleMgr::GetVehicleCategoryName(cat));
			value &= ~VEHICLE_HANDLING_MODEL_IS_HELI;
		}
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_BIKE) && (cat != CVehicleMgr::VehicleCategory::Bike && cat != CVehicleMgr::VehicleCategory::Bmx))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_BIKE for vehicle %d (base %u, category '%s')",
					vehicleid, baseModel, CVehicleMgr::GetVehicleCategoryName(cat));
			value &= ~VEHICLE_HANDLING_MODEL_IS_BIKE;
		}
	}

	if (attrib == HANDL_TR_NDRIVETYPE)
	{
		if (value == 'f')
			value = 'F';
		if (value == 'r')
			value = 'R';
		if (value != 'F' && value != 'R' && value != '4')
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d): Drive type must be 'F', 'R', or '4' (got %d)", vehicleid, static_cast<int>(attrib), value);
			return false;
		}
	}
	if (attrib == HANDL_TR_NENGINETYPE)
	{
		if (value == 'p')
			value = 'P';
		if (value == 'd')
			value = 'D';
		if (value == 'e')
			value = 'E';
		if (value != 'P' && value != 'D' && value != 'E')
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d): Engine type must be 'P', 'D', or 'E' (got %d)", vehicleid, static_cast<int>(attrib), value);
			return false;
		}
	}

	if (attrib == HANDL_TR_NNUMBEROFGEARS && (value < 1 || value > 5))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d): Number of gears must be between 1 and 5 (got %d)", vehicleid, static_cast<int>(attrib), value);
		return false;
	}
	if (attrib == HANDL_NPERCENTSUBMERGED && (value < 1 || value > 100))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleHandlingInt(veh=%d, attr=%d): PercentSubmerged must be between 1 and 100 (got %d)", vehicleid, static_cast<int>(attrib), value);
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
	if ((attrib == HANDL_FMASS || attrib == HANDL_FTURNMASS) && value < 1.0f)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d): Mass/TurnMass must be >= 1.0 (got %f)", modelid, static_cast<int>(attrib), value);
		return false;
	}
	if (attrib == HANDL_FBRAKEDECELERATION && value < 0.0f)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d): BrakeDeceleration must be >= 0.0 (got %f)", modelid, static_cast<int>(attrib), value);
		return false;
	}
	if ((attrib == HANDL_FSUSPENSIONFORCELEVEL || attrib == HANDL_FSUSPENSIONDAMPINGLEVEL || attrib == HANDL_FTRACTIONMULTIPLIER) && value <= 0.0f)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingFloat(model=%d, attr=%d): Suspension force/damping and traction multiplier must be > 0.0 (got %f)", modelid, static_cast<int>(attrib), value);
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

	if (attrib == HANDL_MODELFLAGS)
	{
		uint32_t baseModel = ResolveBaseVehicleModel(static_cast<uint32_t>(modelid));
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_PLANE) && !CVehicleMgr::IsVehicleModelFlightCapable(baseModel))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_PLANE for model %d (base %u, category '%s')",
					modelid, baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));
			value &= ~VEHICLE_HANDLING_MODEL_IS_PLANE;
		}
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_BOAT) && !CVehicleMgr::IsVehicleModelWaterDriveCapable(baseModel))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_BOAT for model %d (base %u, category '%s')",
					modelid, baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));
			value &= ~VEHICLE_HANDLING_MODEL_IS_BOAT;
		}
		CVehicleMgr::VehicleCategory cat = CVehicleMgr::GetVehicleModelCategory(baseModel);
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_HELI) && cat != CVehicleMgr::VehicleCategory::Helicopter)
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_HELI for model %d (base %u, category '%s')",
					modelid, baseModel, CVehicleMgr::GetVehicleCategoryName(cat));
			value &= ~VEHICLE_HANDLING_MODEL_IS_HELI;
		}
		if ((static_cast<unsigned int>(value) & VEHICLE_HANDLING_MODEL_IS_BIKE) && (cat != CVehicleMgr::VehicleCategory::Bike && cat != CVehicleMgr::VehicleCategory::Bmx))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt: Stripping VEHICLE_HANDLING_MODEL_IS_BIKE for model %d (base %u, category '%s')",
					modelid, baseModel, CVehicleMgr::GetVehicleCategoryName(cat));
			value &= ~VEHICLE_HANDLING_MODEL_IS_BIKE;
		}
	}

	if (attrib == HANDL_TR_NDRIVETYPE)
	{
		if (value == 'f')
			value = 'F';
		if (value == 'r')
			value = 'R';
		if (value != 'F' && value != 'R' && value != '4')
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d): Drive type must be 'F', 'R', or '4' (got %d)", modelid, static_cast<int>(attrib), value);
			return false;
		}
	}
	if (attrib == HANDL_TR_NENGINETYPE)
	{
		if (value == 'p')
			value = 'P';
		if (value == 'd')
			value = 'D';
		if (value == 'e')
			value = 'E';
		if (value != 'P' && value != 'D' && value != 'E')
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d): Engine type must be 'P', 'D', or 'E' (got %d)", modelid, static_cast<int>(attrib), value);
			return false;
		}
	}

	if (attrib == HANDL_TR_NNUMBEROFGEARS && (value < 1 || value > 5))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d): Number of gears must be between 1 and 5 (got %d)", modelid, static_cast<int>(attrib), value);
		return false;
	}
	if (attrib == HANDL_NPERCENTSUBMERGED && (value < 1 || value > 100))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelHandlingInt(model=%d, attr=%d): PercentSubmerged must be between 1 and 100 (got %d)", modelid, static_cast<int>(attrib), value);
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

	uint32_t baseModel = ResolveVehicleBaseModel(vehicle);
	if (enable && !CVehicleMgr::IsVehicleModelWaterDriveCapable(baseModel))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleWaterDrive: Vehicle %d (model %d, base %u, category '%s') cannot be set to water drive mode",
				vehicleid, vehicle.getModel(), baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));

		HandlingMgr::BroadcastVehicleCorrection(static_cast<uint16_t>(vehicleid));
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

	int modelid = vehicle.getModel();
	if (enable)
	{
		modelFlags |= VEHICLE_HANDLING_MODEL_IS_BOAT;
		modelFlags &= ~(0x00200000 | 0x00020000);
		HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags);
		HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_NPERCENTSUBMERGED, static_cast<uint8_t>(submergedPercent));
	}
	else
	{
		modelFlags &= ~VEHICLE_HANDLING_MODEL_IS_BOAT;
		unsigned int defModelFlags = 0;
		if (HandlingMgr::GetDefaultHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, defModelFlags))
		{
			modelFlags |= (defModelFlags & (0x00200000 | 0x00020000));
		}
		HandlingMgr::SetVehicleHandling(static_cast<uint16_t>(vehicleid), HANDL_MODELFLAGS, modelFlags);
		uint8_t defSub = 85;
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

	uint32_t baseModel = ResolveBaseVehicleModel(static_cast<uint32_t>(modelid));
	if (enable && !CVehicleMgr::IsVehicleModelWaterDriveCapable(baseModel))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelWaterDrive: Model %d (base %u, category '%s') cannot be set to water drive mode",
				modelid, baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));
		return false;
	}

	if (submergedPercent <= 0 || submergedPercent > 100)
		submergedPercent = 30;

	unsigned int modelFlags = 0;
	HandlingMgr::GetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);

	if (enable)
	{
		modelFlags |= VEHICLE_HANDLING_MODEL_IS_BOAT;
		modelFlags &= ~(0x00200000 | 0x00020000);
		HandlingMgr::SetModelHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, modelFlags);
		HandlingMgr::SetModelHandling(static_cast<uint16_t>(modelid), HANDL_NPERCENTSUBMERGED, static_cast<uint8_t>(submergedPercent));
	}
	else
	{
		modelFlags &= ~VEHICLE_HANDLING_MODEL_IS_BOAT;
		unsigned int defModelFlags = 0;
		if (HandlingMgr::GetDefaultHandling(static_cast<uint16_t>(modelid), HANDL_MODELFLAGS, defModelFlags))
		{
			modelFlags |= (defModelFlags & (0x00200000 | 0x00020000));
		}
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

	uint32_t baseModel = ResolveVehicleBaseModel(vehicle);
	if (enable && !CVehicleMgr::IsVehicleModelFlightCapable(baseModel))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetVehicleFlying: Vehicle %d (model %d, base %u, category '%s') cannot be set to flying mode",
				vehicleid, vehicle.getModel(), baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));

		HandlingMgr::BroadcastVehicleCorrection(static_cast<uint16_t>(vehicleid));
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

	uint32_t baseModel = ResolveBaseVehicleModel(static_cast<uint32_t>(modelid));
	if (enable && !CVehicleMgr::IsVehicleModelFlightCapable(baseModel))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetModelFlying: Model %d (base %u, category '%s') cannot be set to flying mode",
				modelid, baseModel, CVehicleMgr::GetVehicleCategoryName(CVehicleMgr::GetVehicleModelCategory(baseModel)));
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
	if ((attrib == HANDL_FMASS || attrib == HANDL_FTURNMASS) && value < 1.0f)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d): Mass/TurnMass must be >= 1.0 (got %f)", playerid, static_cast<int>(attrib), value);
		return false;
	}
	if (attrib == HANDL_FBRAKEDECELERATION && value < 0.0f)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d): BrakeDeceleration must be >= 0.0 (got %f)", playerid, static_cast<int>(attrib), value);
		return false;
	}
	if ((attrib == HANDL_FSUSPENSIONFORCELEVEL || attrib == HANDL_FSUSPENSIONDAMPINGLEVEL || attrib == HANDL_FTRACTIONMULTIPLIER) && value <= 0.0f)
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingFloat(player=%d, attr=%d): Suspension force/damping and traction multiplier must be > 0.0 (got %f)", playerid, static_cast<int>(attrib), value);
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

	if (attrib == HANDL_TR_NDRIVETYPE)
	{
		if (value == 'f')
			value = 'F';
		if (value == 'r')
			value = 'R';
		if (value != 'F' && value != 'R' && value != '4')
		{
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d): Drive type must be 'F', 'R', or '4' (got %d)", playerid, static_cast<int>(attrib), value);
			return false;
		}
	}
	if (attrib == HANDL_TR_NENGINETYPE)
	{
		if (value == 'p')
			value = 'P';
		if (value == 'd')
			value = 'D';
		if (value == 'e')
			value = 'E';
		if (value != 'P' && value != 'D' && value != 'E')
		{
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d): Engine type must be 'P', 'D', or 'E' (got %d)", playerid, static_cast<int>(attrib), value);
			return false;
		}
	}

	if (attrib == HANDL_TR_NNUMBEROFGEARS && (value < 1 || value > 5))
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d): Number of gears must be between 1 and 5 (got %d)", playerid, static_cast<int>(attrib), value);
		return false;
	}
	if (attrib == HANDL_NPERCENTSUBMERGED && (value < 1 || value > 100))
	{
		core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetPlayerHandlingInt(player=%d, attr=%d): PercentSubmerged must be between 1 and 100 (got %d)", playerid, static_cast<int>(attrib), value);
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

// native SetCustomVehicleModelInfo(customModelId, vehicleClass, wheelModelId, Float:wheelScaleFront, Float:wheelScaleRear, frequency, level, comprate, numExtras, wheelUpgradeClass = 0);
SCRIPT_API(SetCustomVehicleModelInfo, bool(int customModelId, int vehicleClass, int wheelModelId, float wheelScaleFront, float wheelScaleRear, int frequency, int level, int comprate, int numExtras, int wheelUpgradeClass))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleModelInfo: Invalid customModelId %d", customModelId);
		return false;
	}
	return HandlingMgr::SetCustomVehicleModelInfo(
		static_cast<uint32_t>(customModelId),
		static_cast<uint8_t>(vehicleClass),
		static_cast<int16_t>(wheelModelId),
		wheelScaleFront,
		wheelScaleRear,
		static_cast<uint16_t>(frequency),
		static_cast<uint8_t>(level),
		static_cast<uint8_t>(comprate),
		static_cast<uint8_t>(numExtras),
		static_cast<uint8_t>(wheelUpgradeClass)
	);
}

// native SetCustomVehicleWheelModel(customModelId, wheelModelId);
SCRIPT_API(SetCustomVehicleWheelModel, bool(int customModelId, int wheelModelId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleWheelModel: Invalid customModelId %d", customModelId);
		return false;
	}
	return HandlingMgr::SetCustomVehicleWheelModel(static_cast<uint32_t>(customModelId), static_cast<int16_t>(wheelModelId));
}

// native SetCustomVehicleWheelScale(customModelId, Float:wheelScaleFront, Float:wheelScaleRear);
SCRIPT_API(SetCustomVehicleWheelScale, bool(int customModelId, float wheelScaleFront, float wheelScaleRear))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleWheelScale: Invalid customModelId %d", customModelId);
		return false;
	}
	return HandlingMgr::SetCustomVehicleWheelScale(static_cast<uint32_t>(customModelId), wheelScaleFront, wheelScaleRear);
}

// native GetCustomVehicleWheelModel(customModelId, &wheelModelId);
SCRIPT_API(GetCustomVehicleWheelModel, bool(int customModelId, int& wheelModelId))
{
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
		return false;
	int16_t model = -1;
	if (!HandlingMgr::GetCustomVehicleWheelModel(static_cast<uint32_t>(customModelId), model))
		return false;
	wheelModelId = static_cast<int>(model);
	return true;
}

// native GetCustomVehicleWheelScale(customModelId, &Float:wheelScaleFront, &Float:wheelScaleRear);
SCRIPT_API(GetCustomVehicleWheelScale, bool(int customModelId, float& wheelScaleFront, float& wheelScaleRear))
{
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
		return false;
	return HandlingMgr::GetCustomVehicleWheelScale(static_cast<uint32_t>(customModelId), wheelScaleFront, wheelScaleRear);
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

    std::optional<uint32_t> customModelId = 0;
	customModelId = CustomVehicleBindingRegistry::Instance().Get(static_cast<uint16_t>(vehicleid)).has_value();
	if (customModelId != std::nullopt)
	{
        return HandlingMgr::IsCustomVehicle(customModelId.value());
	}
    return false;
}

// native bool:LoadCustomVehicleConfig(customModelId);
SCRIPT_API(LoadCustomVehicleConfig, bool(int customModelId))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] LoadCustomVehicleConfig: Invalid customModelId %d", customModelId);
		return false;
	}
	bool ret = HandlingMgr::LoadCustomVehicleConfig(static_cast<uint32_t>(customModelId));
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] LoadCustomVehicleConfig(model=%d) returning %s", customModelId, ret ? "true" : "false");
	return ret;
}

// native bool:DefineCustomVehicleFromConfig(customModelId, defaultVisualBase = 411);
SCRIPT_API(DefineCustomVehicleFromConfig, bool(int customModelId, int defaultVisualBase))
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (customModelId < CVehicleMgr::CUSTOM_MODEL_START || customModelId > CVehicleMgr::MAX_NETWORK_VEHICLES)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] DefineCustomVehicleFromConfig: Invalid customModelId %d", customModelId);
		return false;
	}
	uint32_t base = (defaultVisualBase >= 400 && defaultVisualBase <= 611) ? static_cast<uint32_t>(defaultVisualBase) : 411;
	bool ret = HandlingMgr::DefineCustomVehicleFromConfig(static_cast<uint32_t>(customModelId), base);
	if (core_)
		core_->logLn(LogLevel::Message, "[ExtendedVeh] DefineCustomVehicleFromConfig(model=%d, defaultBase=%u) returning %s", customModelId, base, ret ? "true" : "false");
	return ret;
}

// native LoadAllCustomVehicles(defaultVisualBase = 411);
SCRIPT_API(LoadAllCustomVehicles, int(int defaultVisualBase))
{
	uint32_t base = (defaultVisualBase >= 400 && defaultVisualBase <= 611) ? static_cast<uint32_t>(defaultVisualBase) : 411;
	return HandlingMgr::LoadAllCustomVehicles(base);
}

// native bool:GetCustomVehicleName(customModelId, output[], maxlen = sizeof(output));
SCRIPT_API(GetCustomVehicleName, bool(int customModelId, std::string& output))
{
	return HandlingMgr::GetCustomVehicleName(static_cast<uint32_t>(customModelId), output);
}

// native bool:SetCustomVehicleName(customModelId, const name[]);
SCRIPT_API(SetCustomVehicleName, bool(int customModelId, const std::string& name))
{
	return HandlingMgr::SetCustomVehicleName(static_cast<uint32_t>(customModelId), name);
}

// native bool:GetCustomVehicleConfigString(customModelId, const key[], output[], maxlen = sizeof(output));
SCRIPT_API(GetCustomVehicleConfigString, bool(int customModelId, const std::string& key, std::string& output))
{
	return HandlingMgr::GetCustomVehicleConfigString(static_cast<uint32_t>(customModelId), key, output);
}

// native bool:SetCustomVehicleConfigString(customModelId, const key[], const value[]);
SCRIPT_API(SetCustomVehicleConfigString, bool(int customModelId, const std::string& key, const std::string& value))
{
	return HandlingMgr::SetCustomVehicleConfigString(static_cast<uint32_t>(customModelId), key, value);
}

// native bool:GetCustomVehicleConfigInt(customModelId, const key[], &result);
SCRIPT_API(GetCustomVehicleConfigInt, bool(int customModelId, const std::string& key, int& result))
{
	return HandlingMgr::GetCustomVehicleConfigInt(static_cast<uint32_t>(customModelId), key, result);
}

// native bool:SetCustomVehicleConfigInt(customModelId, const key[], value);
SCRIPT_API(SetCustomVehicleConfigInt, bool(int customModelId, const std::string& key, int value))
{
	return HandlingMgr::SetCustomVehicleConfigInt(static_cast<uint32_t>(customModelId), key, value);
}

// native bool:GetCustomVehicleConfigFloat(customModelId, const key[], &Float:result);
SCRIPT_API(GetCustomVehicleConfigFloat, bool(int customModelId, const std::string& key, float& result))
{
	return HandlingMgr::GetCustomVehicleConfigFloat(static_cast<uint32_t>(customModelId), key, result);
}

// native bool:SetCustomVehicleConfigFloat(customModelId, const key[], Float:value);
SCRIPT_API(SetCustomVehicleConfigFloat, bool(int customModelId, const std::string& key, float value))
{
	return HandlingMgr::SetCustomVehicleConfigFloat(static_cast<uint32_t>(customModelId), key, value);
}

// native GetCustomVehicleColorVariationsCount(customModelId);
SCRIPT_API(GetCustomVehicleColorVariationsCount, int(int customModelId))
{
	return HandlingMgr::GetCustomVehicleColorVariationsCount(static_cast<uint32_t>(customModelId));
}

// native bool:GetCustomVehicleColorVariation(customModelId, variationIndex, &primary, &secondary, &tertiary, &quaternary);
SCRIPT_API(GetCustomVehicleColorVariation, bool(int customModelId, int variationIndex, int& primary, int& secondary, int& tertiary, int& quaternary))
{
	return HandlingMgr::GetCustomVehicleColorVariation(static_cast<uint32_t>(customModelId), variationIndex, primary, secondary, tertiary, quaternary);
}

// native bool:GetCustomVehicleDefaultColors(customModelId, &primary, &secondary, &tertiary, &quaternary);
SCRIPT_API(GetCustomVehicleDefaultColors, bool(int customModelId, int& primary, int& secondary, int& tertiary, int& quaternary))
{
	return HandlingMgr::GetCustomVehicleDefaultColors(static_cast<uint32_t>(customModelId), primary, secondary, tertiary, quaternary);
}

// native GetCustomVehicleAllowedUpgradesCount(customModelId);
SCRIPT_API(GetCustomVehicleAllowedUpgradesCount, int(int customModelId))
{
	return HandlingMgr::GetCustomVehicleAllowedUpgradesCount(static_cast<uint32_t>(customModelId));
}

// native GetCustomVehicleAllowedUpgrade(customModelId, index);
SCRIPT_API(GetCustomVehicleAllowedUpgrade, int(int customModelId, int index))
{
	return HandlingMgr::GetCustomVehicleAllowedUpgrade(static_cast<uint32_t>(customModelId), index);
}

namespace CustomVehicleNatives
{
inline bool BindCustomVehicle(IVehicle& vehicle, int customModelId)
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] BindCustomVehicle: Invalid vehicle ID %d", vehicleid);
		return false;
	}
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleModel(customModelId))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] BindCustomVehicle: Invalid customModelId %d", customModelId);
		return false;
	}
	if (CVehicleMgr::IsCustomVehicleModel(customModelId))
	{
		if (HandlingMgr::customVehicleDefs.find(customModelId) == HandlingMgr::customVehicleDefs.end())
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] BindCustomVehicle: Custom vehicle model %d has not been committed yet via CommitCustomVehicleDef", customModelId);
			return false;
		}
	}
	if (!compo)
		return false;
	CustomVehicleBindingRegistry::Instance().Bind(static_cast<uint16_t>(vehicleid), static_cast<uint32_t>(customModelId));

	if (core_)
	{
		core_->logLn(LogLevel::Message, "[ExtendedVeh] BindCustomVehicle: Bound vehicle %d to custom model %d", vehicleid, customModelId);
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
}

// native BindCustomVehicle(vehicleid, customModelId);
SCRIPT_API(BindCustomVehicle, bool(IVehicle& vehicle, int customModelId))
{
	return CustomVehicleNatives::BindCustomVehicle(vehicle, customModelId);
}

// Legacy alias: BindVehicleModel -> BindCustomVehicle
SCRIPT_API(BindVehicleModel, bool(IVehicle& vehicle, int customModelId))
{
	return CustomVehicleNatives::BindCustomVehicle(vehicle, customModelId);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleStance(IVehicle& vehicle, float frontScale, float rearScale, float frontCamber, float rearCamber, float frontTrackWidth, float rearTrackWidth)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	CustomVeh::Protocol::VehicleStancePacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.frontWheelScale = frontScale;
	pkt.rearWheelScale = rearScale;
	pkt.frontCamber = frontCamber;
	pkt.rearCamber = rearCamber;
	pkt.frontTrackWidth = frontTrackWidth;
	pkt.rearTrackWidth = rearTrackWidth;

	CustomVehicleBindingRegistry::Instance().SetStance(vId, pkt);

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleStance(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleStance(vehicleid, Float:frontScale, Float:rearScale, Float:frontCamber, Float:rearCamber, Float:frontTrackWidth, Float:rearTrackWidth);
SCRIPT_API(SetCustomVehicleStance, bool(IVehicle& vehicle, float frontScale, float rearScale, float frontCamber, float rearCamber, float frontTrackWidth, float rearTrackWidth))
{
	return CustomVehicleNatives::SetCustomVehicleStance(vehicle, frontScale, rearScale, frontCamber, rearCamber, frontTrackWidth, rearTrackWidth);
}

// Legacy alias: SetVehicleStance -> SetCustomVehicleStance
SCRIPT_API(SetVehicleStance, bool(IVehicle& vehicle, float frontScale, float rearScale, float frontCamber, float rearCamber, float frontTrackWidth, float rearTrackWidth))
{
	return CustomVehicleNatives::SetCustomVehicleStance(vehicle, frontScale, rearScale, frontCamber, rearCamber, frontTrackWidth, rearTrackWidth);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleStance(IVehicle& vehicle, float& frontScale, float& rearScale, float& frontCamber, float& rearCamber, float& frontTrackWidth, float& rearTrackWidth)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto stanceOpt = CustomVehicleBindingRegistry::Instance().GetStance(static_cast<uint16_t>(vehicleid));
	if (!stanceOpt)
		return false;

	frontScale = stanceOpt->frontWheelScale;
	rearScale = stanceOpt->rearWheelScale;
	frontCamber = stanceOpt->frontCamber;
	rearCamber = stanceOpt->rearCamber;
	frontTrackWidth = stanceOpt->frontTrackWidth;
	rearTrackWidth = stanceOpt->rearTrackWidth;
	return true;
}
}

// native GetCustomVehicleStance(vehicleid, &Float:frontScale, &Float:rearScale, &Float:frontCamber, &Float:rearCamber, &Float:frontTrackWidth, &Float:rearTrackWidth);
SCRIPT_API(GetCustomVehicleStance, bool(IVehicle& vehicle, float& frontScale, float& rearScale, float& frontCamber, float& rearCamber, float& frontTrackWidth, float& rearTrackWidth))
{
	return CustomVehicleNatives::GetCustomVehicleStance(vehicle, frontScale, rearScale, frontCamber, rearCamber, frontTrackWidth, rearTrackWidth);
}

// Legacy alias: GetVehicleStance -> GetCustomVehicleStance
SCRIPT_API(GetVehicleStance, bool(IVehicle& vehicle, float& frontScale, float& rearScale, float& frontCamber, float& rearCamber, float& frontTrackWidth, float& rearTrackWidth))
{
	return CustomVehicleNatives::GetCustomVehicleStance(vehicle, frontScale, rearScale, frontCamber, rearCamber, frontTrackWidth, rearTrackWidth);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleExtras(IVehicle& vehicle, int extrasMask)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	uint8_t mask = static_cast<uint8_t>(extrasMask);

	CustomVehicleBindingRegistry::Instance().SetExtras(vId, mask);

	CustomVeh::Protocol::VehicleExtrasPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.extrasMask = mask;

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleExtras(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleExtras(vehicleid, extrasMask);
SCRIPT_API(SetCustomVehicleExtras, bool(IVehicle& vehicle, int extrasMask))
{
	return CustomVehicleNatives::SetCustomVehicleExtras(vehicle, extrasMask);
}

// Legacy alias: SetVehicleExtras -> SetCustomVehicleExtras
SCRIPT_API(SetVehicleExtras, bool(IVehicle& vehicle, int extrasMask))
{
	return CustomVehicleNatives::SetCustomVehicleExtras(vehicle, extrasMask);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleExtras(IVehicle& vehicle, int& extrasMask)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto extrasOpt = CustomVehicleBindingRegistry::Instance().GetExtras(static_cast<uint16_t>(vehicleid));
	if (!extrasOpt)
		return false;

	extrasMask = static_cast<int>(*extrasOpt);
	return true;
}
}

// native GetCustomVehicleExtras(vehicleid, &extrasMask);
SCRIPT_API(GetCustomVehicleExtras, bool(IVehicle& vehicle, int& extrasMask))
{
	return CustomVehicleNatives::GetCustomVehicleExtras(vehicle, extrasMask);
}

// Legacy alias: GetVehicleExtras -> GetCustomVehicleExtras
SCRIPT_API(GetVehicleExtras, bool(IVehicle& vehicle, int& extrasMask))
{
	return CustomVehicleNatives::GetCustomVehicleExtras(vehicle, extrasMask);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehiclePaintjob(IVehicle& vehicle, int paintjobid)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	int8_t pj = static_cast<int8_t>(paintjobid);

	CustomVehicleBindingRegistry::Instance().SetPaintjob(vId, pj);
	vehicle.setPaintJob(paintjobid);

	CustomVeh::Protocol::VehiclePaintjobPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.paintjobIndex = pj;

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehiclePaintjob(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehiclePaintjob(vehicleid, paintjobid);
SCRIPT_API(SetCustomVehiclePaintjob, bool(IVehicle& vehicle, int paintjobid))
{
	return CustomVehicleNatives::SetCustomVehiclePaintjob(vehicle, paintjobid);
}

// Legacy alias: SetVehiclePaintjob -> SetCustomVehiclePaintjob
SCRIPT_API(SetVehiclePaintjob, bool(IVehicle& vehicle, int paintjobid))
{
	return CustomVehicleNatives::SetCustomVehiclePaintjob(vehicle, paintjobid);
}

namespace CustomVehicleNatives
{
inline int GetCustomVehiclePaintjob(IVehicle& vehicle)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return -1;

	auto pjOpt = CustomVehicleBindingRegistry::Instance().GetPaintjob(static_cast<uint16_t>(vehicleid));
	if (pjOpt)
		return static_cast<int>(*pjOpt);

	return vehicle.getPaintJob();
}
}

// native GetCustomVehiclePaintjob(vehicleid);
SCRIPT_API(GetCustomVehiclePaintjob, int(IVehicle& vehicle))
{
	return CustomVehicleNatives::GetCustomVehiclePaintjob(vehicle);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleNeon(IVehicle& vehicle, int enabled, int r, int g, int b, float size)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	CustomVeh::Protocol::VehicleNeonPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.enabled = enabled ? 1 : 0;
	pkt.r = static_cast<uint8_t>(r);
	pkt.g = static_cast<uint8_t>(g);
	pkt.b = static_cast<uint8_t>(b);
	pkt.size = size;

	CustomVehicleBindingRegistry::Instance().SetNeon(vId, pkt);

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleNeon(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleNeon(vehicleid, enabled, r, g, b, Float:size);
SCRIPT_API(SetCustomVehicleNeon, bool(IVehicle& vehicle, int enabled, int r, int g, int b, float size))
{
	return CustomVehicleNatives::SetCustomVehicleNeon(vehicle, enabled, r, g, b, size);
}

// Legacy alias: SetVehicleNeon -> SetCustomVehicleNeon
SCRIPT_API(SetVehicleNeon, bool(IVehicle& vehicle, int enabled, int r, int g, int b, float size))
{
	return CustomVehicleNatives::SetCustomVehicleNeon(vehicle, enabled, r, g, b, size);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleNeon(IVehicle& vehicle, int& enabled, int& r, int& g, int& b, float& size)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto neonOpt = CustomVehicleBindingRegistry::Instance().GetNeon(static_cast<uint16_t>(vehicleid));
	if (!neonOpt)
		return false;

	enabled = neonOpt->enabled ? 1 : 0;
	r = static_cast<int>(neonOpt->r);
	g = static_cast<int>(neonOpt->g);
	b = static_cast<int>(neonOpt->b);
	size = neonOpt->size;
	return true;
}
}

// native GetCustomVehicleNeon(vehicleid, &enabled, &r, &g, &b, &Float:size);
SCRIPT_API(GetCustomVehicleNeon, bool(IVehicle& vehicle, int& enabled, int& r, int& g, int& b, float& size))
{
	return CustomVehicleNatives::GetCustomVehicleNeon(vehicle, enabled, r, g, b, size);
}

// Legacy alias: GetVehicleNeon -> GetCustomVehicleNeon
SCRIPT_API(GetVehicleNeon, bool(IVehicle& vehicle, int& enabled, int& r, int& g, int& b, float& size))
{
	return CustomVehicleNatives::GetCustomVehicleNeon(vehicle, enabled, r, g, b, size);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleWindowTint(IVehicle& vehicle, int alpha, int r, int g, int b)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	CustomVeh::Protocol::VehicleWindowTintPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.alpha = static_cast<uint8_t>(alpha);
	pkt.r = static_cast<uint8_t>(r);
	pkt.g = static_cast<uint8_t>(g);
	pkt.b = static_cast<uint8_t>(b);

	CustomVehicleBindingRegistry::Instance().SetWindowTint(vId, pkt);

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleWindowTint(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleWindowTint(vehicleid, alpha, r, g, b);
SCRIPT_API(SetCustomVehicleWindowTint, bool(IVehicle& vehicle, int alpha, int r, int g, int b))
{
	return CustomVehicleNatives::SetCustomVehicleWindowTint(vehicle, alpha, r, g, b);
}

// Legacy alias: SetVehicleWindowTint -> SetCustomVehicleWindowTint
SCRIPT_API(SetVehicleWindowTint, bool(IVehicle& vehicle, int alpha, int r, int g, int b))
{
	return CustomVehicleNatives::SetCustomVehicleWindowTint(vehicle, alpha, r, g, b);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleWindowTint(IVehicle& vehicle, int& alpha, int& r, int& g, int& b)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto tintOpt = CustomVehicleBindingRegistry::Instance().GetWindowTint(static_cast<uint16_t>(vehicleid));
	if (!tintOpt)
		return false;

	alpha = static_cast<int>(tintOpt->alpha);
	r = static_cast<int>(tintOpt->r);
	g = static_cast<int>(tintOpt->g);
	b = static_cast<int>(tintOpt->b);
	return true;
}
}

// native GetCustomVehicleWindowTint(vehicleid, &alpha, &r, &g, &b);
SCRIPT_API(GetCustomVehicleWindowTint, bool(IVehicle& vehicle, int& alpha, int& r, int& g, int& b))
{
	return CustomVehicleNatives::GetCustomVehicleWindowTint(vehicle, alpha, r, g, b);
}

// Legacy alias: GetVehicleWindowTint -> GetCustomVehicleWindowTint
SCRIPT_API(GetVehicleWindowTint, bool(IVehicle& vehicle, int& alpha, int& r, int& g, int& b))
{
	return CustomVehicleNatives::GetCustomVehicleWindowTint(vehicle, alpha, r, g, b);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleWheelColor(IVehicle& vehicle, int r, int g, int b)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	CustomVeh::Protocol::VehicleWheelColorPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.r = static_cast<uint8_t>(r);
	pkt.g = static_cast<uint8_t>(g);
	pkt.b = static_cast<uint8_t>(b);

	CustomVehicleBindingRegistry::Instance().SetWheelColor(vId, pkt);

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleWheelColor(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleWheelColor(vehicleid, r, g, b);
SCRIPT_API(SetCustomVehicleWheelColor, bool(IVehicle& vehicle, int r, int g, int b))
{
	return CustomVehicleNatives::SetCustomVehicleWheelColor(vehicle, r, g, b);
}

// Legacy alias: SetVehicleWheelColor -> SetCustomVehicleWheelColor
SCRIPT_API(SetVehicleWheelColor, bool(IVehicle& vehicle, int r, int g, int b))
{
	return CustomVehicleNatives::SetCustomVehicleWheelColor(vehicle, r, g, b);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleWheelColor(IVehicle& vehicle, int& r, int& g, int& b)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto wcOpt = CustomVehicleBindingRegistry::Instance().GetWheelColor(static_cast<uint16_t>(vehicleid));
	if (!wcOpt)
		return false;

	r = static_cast<int>(wcOpt->r);
	g = static_cast<int>(wcOpt->g);
	b = static_cast<int>(wcOpt->b);
	return true;
}
}

// native GetCustomVehicleWheelColor(vehicleid, &r, &g, &b);
SCRIPT_API(GetCustomVehicleWheelColor, bool(IVehicle& vehicle, int& r, int& g, int& b))
{
	return CustomVehicleNatives::GetCustomVehicleWheelColor(vehicle, r, g, b);
}

// Legacy alias: GetVehicleWheelColor -> GetCustomVehicleWheelColor
SCRIPT_API(GetVehicleWheelColor, bool(IVehicle& vehicle, int& r, int& g, int& b))
{
	return CustomVehicleNatives::GetCustomVehicleWheelColor(vehicle, r, g, b);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleBackfire(IVehicle& vehicle, int enabled)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	bool en = (enabled != 0);

	CustomVehicleBindingRegistry::Instance().SetBackfire(vId, en);

	CustomVeh::Protocol::VehicleBackfirePacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.enabled = en ? 1 : 0;

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleBackfire(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleBackfire(vehicleid, enabled);
SCRIPT_API(SetCustomVehicleBackfire, bool(IVehicle& vehicle, int enabled))
{
	return CustomVehicleNatives::SetCustomVehicleBackfire(vehicle, enabled);
}

// Legacy alias: SetVehicleBackfire -> SetCustomVehicleBackfire
SCRIPT_API(SetVehicleBackfire, bool(IVehicle& vehicle, int enabled))
{
	return CustomVehicleNatives::SetCustomVehicleBackfire(vehicle, enabled);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleBackfire(IVehicle& vehicle, int& enabled)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto bfOpt = CustomVehicleBindingRegistry::Instance().GetBackfire(static_cast<uint16_t>(vehicleid));
	if (!bfOpt)
		return false;

	enabled = *bfOpt ? 1 : 0;
	return true;
}
}

// native GetCustomVehicleBackfire(vehicleid, &enabled);
SCRIPT_API(GetCustomVehicleBackfire, bool(IVehicle& vehicle, int& enabled))
{
	return CustomVehicleNatives::GetCustomVehicleBackfire(vehicle, enabled);
}

// Legacy alias: GetVehicleBackfire -> GetCustomVehicleBackfire
SCRIPT_API(GetVehicleBackfire, bool(IVehicle& vehicle, int& enabled))
{
	return CustomVehicleNatives::GetCustomVehicleBackfire(vehicle, enabled);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleHorn(IVehicle& vehicle, int hornSoundId, float hornPitch)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	CustomVehicleBindingRegistry::Instance().SetHorn(vId, static_cast<int8_t>(hornSoundId), hornPitch);

	CustomVeh::Protocol::VehicleHornPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.hornSoundId = static_cast<int8_t>(hornSoundId);
	pkt.hornPitch = hornPitch;

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleHorn(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleHorn(vehicleid, hornSoundId, Float:hornPitch = 1.0);
SCRIPT_API(SetCustomVehicleHorn, bool(IVehicle& vehicle, int hornSoundId, float hornPitch))
{
	return CustomVehicleNatives::SetCustomVehicleHorn(vehicle, hornSoundId, hornPitch);
}

// Legacy alias: SetVehicleHorn -> SetCustomVehicleHorn
SCRIPT_API(SetVehicleHorn, bool(IVehicle& vehicle, int hornSoundId, float hornPitch))
{
	return CustomVehicleNatives::SetCustomVehicleHorn(vehicle, hornSoundId, hornPitch);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleHorn(IVehicle& vehicle, int& hornSoundId, float& hornPitch)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto hornOpt = CustomVehicleBindingRegistry::Instance().GetHorn(static_cast<uint16_t>(vehicleid));
	if (!hornOpt)
		return false;

	hornSoundId = static_cast<int>(hornOpt->hornSoundId);
	hornPitch = hornOpt->hornPitch;
	return true;
}
}

// native GetCustomVehicleHorn(vehicleid, &hornSoundId, &Float:hornPitch);
SCRIPT_API(GetCustomVehicleHorn, bool(IVehicle& vehicle, int& hornSoundId, float& hornPitch))
{
	return CustomVehicleNatives::GetCustomVehicleHorn(vehicle, hornSoundId, hornPitch);
}

// Legacy alias: GetVehicleHorn -> GetCustomVehicleHorn
SCRIPT_API(GetVehicleHorn, bool(IVehicle& vehicle, int& hornSoundId, float& hornPitch))
{
	return CustomVehicleNatives::GetCustomVehicleHorn(vehicle, hornSoundId, hornPitch);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleSiren(IVehicle& vehicle, bool enabled, int sirenType)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	CustomVehicleBindingRegistry::Instance().SetSiren(vId, enabled, static_cast<int8_t>(sirenType));

	CustomVeh::Protocol::VehicleSirenPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.enabled = enabled ? 1 : 0;
	pkt.sirenType = static_cast<int8_t>(sirenType);

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleSiren(*player, pkt);
			}
		}
	}
	return true;
}
}

// native SetCustomVehicleSiren(vehicleid, bool:enabled, sirenType = 1);
SCRIPT_API(SetCustomVehicleSiren, bool(IVehicle& vehicle, bool enabled, int sirenType))
{
	return CustomVehicleNatives::SetCustomVehicleSiren(vehicle, enabled, sirenType);
}

// Legacy alias: SetVehicleSiren -> SetCustomVehicleSiren
SCRIPT_API(SetVehicleSiren, bool(IVehicle& vehicle, bool enabled, int sirenType))
{
	return CustomVehicleNatives::SetCustomVehicleSiren(vehicle, enabled, sirenType);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleSiren(IVehicle& vehicle, bool& enabled, int& sirenType)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto sirenOpt = CustomVehicleBindingRegistry::Instance().GetSiren(static_cast<uint16_t>(vehicleid));
	if (!sirenOpt)
		return false;

	enabled = (sirenOpt->enabled != 0);
	sirenType = static_cast<int>(sirenOpt->sirenType);
	return true;
}
}

// native GetCustomVehicleSiren(vehicleid, &bool:enabled, &sirenType);
SCRIPT_API(GetCustomVehicleSiren, bool(IVehicle& vehicle, bool& enabled, int& sirenType))
{
	return CustomVehicleNatives::GetCustomVehicleSiren(vehicle, enabled, sirenType);
}

// Legacy alias: GetVehicleSiren -> GetCustomVehicleSiren
SCRIPT_API(GetVehicleSiren, bool(IVehicle& vehicle, bool& enabled, int& sirenType))
{
	return CustomVehicleNatives::GetCustomVehicleSiren(vehicle, enabled, sirenType);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleLighting(IVehicle& vehicle, int category, float scale)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	CustomVehicleBindingRegistry::Instance().SetLights(vId, static_cast<int8_t>(category), scale);

	CustomVeh::Protocol::VehicleLightsPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.lightingCategory = static_cast<int8_t>(category);
	pkt.lightScaleMult = (scale > 0.05f) ? scale : 1.0f;

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleLights(*player, pkt);
			}
		}
	}
	return true;
}

inline bool GetCustomVehicleLighting(IVehicle& vehicle, int& category, float& scale)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto lightsOpt = CustomVehicleBindingRegistry::Instance().GetLights(static_cast<uint16_t>(vehicleid));
	if (!lightsOpt)
		return false;

	category = static_cast<int>(lightsOpt->lightingCategory);
	scale = lightsOpt->lightScaleMult;
	return true;
}
}

// native SetCustomVehicleLighting(vehicleid, category, Float:scale = 1.0);
SCRIPT_API(SetCustomVehicleLighting, bool(IVehicle& vehicle, int category, float scale))
{
	return CustomVehicleNatives::SetCustomVehicleLighting(vehicle, category, scale);
}

// Legacy alias: SetVehicleLighting -> SetCustomVehicleLighting
SCRIPT_API(SetVehicleLighting, bool(IVehicle& vehicle, int category, float scale))
{
	return CustomVehicleNatives::SetCustomVehicleLighting(vehicle, category, scale);
}

// native GetCustomVehicleLighting(vehicleid, &category, &Float:scale);
SCRIPT_API(GetCustomVehicleLighting, bool(IVehicle& vehicle, int& category, float& scale))
{
	return CustomVehicleNatives::GetCustomVehicleLighting(vehicle, category, scale);
}

// Legacy alias: GetVehicleLighting -> GetCustomVehicleLighting
SCRIPT_API(GetVehicleLighting, bool(IVehicle& vehicle, int& category, float& scale))
{
	return CustomVehicleNatives::GetCustomVehicleLighting(vehicle, category, scale);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleModelHorn(int customModelId, int hornSoundId, float hornPitch)
{
	if (customModelId < 0 || !CVehicleMgr::IsCustomVehicleModel(static_cast<uint32_t>(customModelId)))
		return false;

	CustomVehicleBindingRegistry::Instance().SetModelHorn(static_cast<uint32_t>(customModelId), static_cast<int8_t>(hornSoundId), hornPitch);
	return true;
}
}

// native SetCustomVehicleModelHorn(customModelId, hornSoundId, Float:hornPitch = 1.0);
SCRIPT_API(SetCustomVehicleModelHorn, bool(int customModelId, int hornSoundId, float hornPitch))
{
	return CustomVehicleNatives::SetCustomVehicleModelHorn(customModelId, hornSoundId, hornPitch);
}

// Legacy alias: SetVehicleModelHorn -> SetCustomVehicleModelHorn
SCRIPT_API(SetVehicleModelHorn, bool(int customModelId, int hornSoundId, float hornPitch))
{
	return CustomVehicleNatives::SetCustomVehicleModelHorn(customModelId, hornSoundId, hornPitch);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleModelSiren(int customModelId, bool hasSiren, int sirenType)
{
	if (customModelId < 0 || !CVehicleMgr::IsCustomVehicleModel(static_cast<uint32_t>(customModelId)))
		return false;

	CustomVehicleBindingRegistry::Instance().SetModelSiren(static_cast<uint32_t>(customModelId), hasSiren, static_cast<int8_t>(sirenType));
	return true;
}
}

// native SetCustomVehicleModelSiren(customModelId, bool:hasSiren, sirenType = 1);
SCRIPT_API(SetCustomVehicleModelSiren, bool(int customModelId, bool hasSiren, int sirenType))
{
	return CustomVehicleNatives::SetCustomVehicleModelSiren(customModelId, hasSiren, sirenType);
}

// Legacy alias: SetVehicleModelSiren -> SetCustomVehicleModelSiren
SCRIPT_API(SetVehicleModelSiren, bool(int customModelId, bool hasSiren, int sirenType))
{
	return CustomVehicleNatives::SetCustomVehicleModelSiren(customModelId, hasSiren, sirenType);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleWheel(IVehicle& vehicle, int wheelModelId)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	uint16_t vId = static_cast<uint16_t>(vehicleid);
	int16_t wId = static_cast<int16_t>(wheelModelId);

	CustomVehicleBindingRegistry::Instance().SetWheel(vId, wId);

	CustomVeh::Protocol::VehicleWheelPacket pkt {};
	pkt.sampVehicleId = vId;
	pkt.wheelModelId = wId;

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (core_)
	{
		for (IPlayer* player : core_->getPlayers().players())
		{
			if (player && gPlayers.HasExtendedVeh(player->getID()))
			{
				CustomVehicleTransport::SendVehicleWheel(*player, pkt);
			}
		}
	}
	return true;
}

inline bool GetCustomVehicleWheel(IVehicle& vehicle, int& wheelModelId)
{
	int vehicleid = vehicle.getID();
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
		return false;

	auto wheelOpt = CustomVehicleBindingRegistry::Instance().GetWheel(static_cast<uint16_t>(vehicleid));
	if (!wheelOpt)
		return false;

	wheelModelId = static_cast<int>(wheelOpt->wheelModelId);
	return true;
}
}

// native SetCustomVehicleWheel(vehicleid, wheelModelId);
SCRIPT_API(SetCustomVehicleWheel, bool(IVehicle& vehicle, int wheelModelId))
{
	return CustomVehicleNatives::SetCustomVehicleWheel(vehicle, wheelModelId);
}

// Legacy alias: SetVehicleWheel -> SetCustomVehicleWheel
SCRIPT_API(SetVehicleWheel, bool(IVehicle& vehicle, int wheelModelId))
{
	return CustomVehicleNatives::SetCustomVehicleWheel(vehicle, wheelModelId);
}

// native GetCustomVehicleWheel(vehicleid, &wheelModelId);
SCRIPT_API(GetCustomVehicleWheel, bool(IVehicle& vehicle, int& wheelModelId))
{
	return CustomVehicleNatives::GetCustomVehicleWheel(vehicle, wheelModelId);
}

// Legacy alias: GetVehicleWheel -> GetCustomVehicleWheel
SCRIPT_API(GetVehicleWheel, bool(IVehicle& vehicle, int& wheelModelId))
{
	return CustomVehicleNatives::GetCustomVehicleWheel(vehicle, wheelModelId);
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

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleDoorMissing(IVehicle& vehicle, int doorid, bool missing)
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleDoorMissing: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	uint32_t baseModel = ResolveVehicleBaseModel(vehicle);
	CVehicleMgr::VehicleCategory cat = CVehicleMgr::GetVehicleModelCategory(baseModel);
	if (cat == CVehicleMgr::VehicleCategory::Bike || cat == CVehicleMgr::VehicleCategory::Bmx || cat == CVehicleMgr::VehicleCategory::Boat || cat == CVehicleMgr::VehicleCategory::Trailer || cat == CVehicleMgr::VehicleCategory::Train)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleDoorMissing: Vehicle %d (category '%s') has no doors",
				vehicleid, CVehicleMgr::GetVehicleCategoryName(cat));
		return false;
	}
	if (doorid < 0 || (doorid > 5 && doorid != 0xFF))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleDoorMissing: Invalid door ID %d (must be 0-5 or 255)", doorid);
		return false;
	}

	bool ret = HandlingMgr::SetVehicleDoorMissing(static_cast<uint16_t>(vehicleid), static_cast<uint8_t>(doorid), missing);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetCustomVehicleDoorMissing(veh=%d, door=%d, missing=%d) -> %s", vehicleid, doorid, missing, ret ? "true" : "false");
	return ret;
}
}

// native SetCustomVehicleDoorMissing(vehicleid, doorid, bool:missing);
SCRIPT_API(SetCustomVehicleDoorMissing, bool(IVehicle& vehicle, int doorid, bool missing))
{
	return CustomVehicleNatives::SetCustomVehicleDoorMissing(vehicle, doorid, missing);
}

// Legacy alias: SetVehicleDoorMissing -> SetCustomVehicleDoorMissing
SCRIPT_API(SetVehicleDoorMissing, bool(IVehicle& vehicle, int doorid, bool missing))
{
	return CustomVehicleNatives::SetCustomVehicleDoorMissing(vehicle, doorid, missing);
}

namespace CustomVehicleNatives
{
inline bool GetCustomVehicleDoorMissing(IVehicle& vehicle, int doorid, bool& missing)
{
	missing = false;
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetCustomVehicleDoorMissing: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	uint32_t baseModel = ResolveVehicleBaseModel(vehicle);
	CVehicleMgr::VehicleCategory cat = CVehicleMgr::GetVehicleModelCategory(baseModel);
	if (cat == CVehicleMgr::VehicleCategory::Bike || cat == CVehicleMgr::VehicleCategory::Bmx || cat == CVehicleMgr::VehicleCategory::Boat || cat == CVehicleMgr::VehicleCategory::Trailer || cat == CVehicleMgr::VehicleCategory::Train)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetCustomVehicleDoorMissing: Vehicle %d (category '%s') has no doors",
				vehicleid, CVehicleMgr::GetVehicleCategoryName(cat));
		return false;
	}
	if (doorid < 0 || doorid > 5)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] GetCustomVehicleDoorMissing: Invalid door ID %d (must be 0-5)", doorid);
		return false;
	}

	bool ret = HandlingMgr::GetVehicleDoorMissing(static_cast<uint16_t>(vehicleid), static_cast<uint8_t>(doorid), missing);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] GetCustomVehicleDoorMissing(veh=%d, door=%d) -> missing=%d, ret=%s", vehicleid, doorid, missing, ret ? "true" : "false");
	return ret;
}
}

// native GetCustomVehicleDoorMissing(vehicleid, doorid, &bool:missing);
SCRIPT_API(GetCustomVehicleDoorMissing, bool(IVehicle& vehicle, int doorid, bool& missing))
{
	return CustomVehicleNatives::GetCustomVehicleDoorMissing(vehicle, doorid, missing);
}

// Legacy alias: GetVehicleDoorMissing -> GetCustomVehicleDoorMissing
SCRIPT_API(GetVehicleDoorMissing, bool(IVehicle& vehicle, int doorid, bool& missing))
{
	return CustomVehicleNatives::GetCustomVehicleDoorMissing(vehicle, doorid, missing);
}

namespace CustomVehicleNatives
{
inline bool SetCustomVehicleAllDoorsMissing(IVehicle& vehicle, bool missing)
{
	int vehicleid = vehicle.getID();
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	if (!CVehicleMgr::VehicleRegistry::Get().IsValidVehicleID(vehicleid))
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleAllDoorsMissing: Invalid vehicle ID %d", vehicleid);
		return false;
	}

	uint32_t baseModel = ResolveVehicleBaseModel(vehicle);
	CVehicleMgr::VehicleCategory cat = CVehicleMgr::GetVehicleModelCategory(baseModel);
	if (cat == CVehicleMgr::VehicleCategory::Bike || cat == CVehicleMgr::VehicleCategory::Bmx || cat == CVehicleMgr::VehicleCategory::Boat || cat == CVehicleMgr::VehicleCategory::Trailer || cat == CVehicleMgr::VehicleCategory::Train)
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] SetCustomVehicleAllDoorsMissing: Vehicle %d (category '%s') has no doors",
				vehicleid, CVehicleMgr::GetVehicleCategoryName(cat));
		return false;
	}

	bool ret = HandlingMgr::SetVehicleAllDoorsMissing(static_cast<uint16_t>(vehicleid), missing);
	if (core_)
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] SetCustomVehicleAllDoorsMissing(veh=%d, missing=%d) -> %s", vehicleid, missing, ret ? "true" : "false");
	return ret;
}
}

// native SetCustomVehicleAllDoorsMissing(vehicleid, bool:missing);
SCRIPT_API(SetCustomVehicleAllDoorsMissing, bool(IVehicle& vehicle, bool missing))
{
	return CustomVehicleNatives::SetCustomVehicleAllDoorsMissing(vehicle, missing);
}

// Legacy alias: SetVehicleAllDoorsMissing -> SetCustomVehicleAllDoorsMissing
SCRIPT_API(SetVehicleAllDoorsMissing, bool(IVehicle& vehicle, bool missing))
{
	return CustomVehicleNatives::SetCustomVehicleAllDoorsMissing(vehicle, missing);
}
