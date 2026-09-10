#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct RPC_WorldVehicleAdd_Data {
	uint16_t vehicleId;
	uint32_t modelId;
	float pos[3];
	float angle;
	uint8_t color1;
	uint8_t color2;
	float health;
	uint8_t interior;
	uint32_t doorDamage;
	uint32_t panelDamage;
	uint8_t lightDamage;
	uint8_t tireDamage;
	uint8_t addSiren;
	uint8_t modSlots[14];
	uint8_t paintjob;
	uint32_t bodyColor1;
	uint32_t bodyColor2;
	uint8_t zAngle;
};
#pragma pack(pop)

//
constexpr uint16_t PKT_EXTVEH = 251;
constexpr uint32_t EXTENDEDVEH_COMPAT_VERSION = 0x1001D;
//

constexpr uint16_t RPC_WorldPlayerAdd = 137;
constexpr uint16_t RPC_WorldPlayerRemove = 138;

constexpr uint32_t BASE_MODEL_START = 400;
constexpr uint32_t BASE_MODEL_END = 611;
constexpr uint32_t BASE_VEHICLE_MODELS = 212; // (611 - 400 + 1)
constexpr uint32_t CUSTOM_MODEL_BASE_ID = 20000;

constexpr uint32_t MAX_SAMP_VEHICLES = 2000;
constexpr uint16_t DEFAULT_MAX_VEHICLES = 2000;
constexpr uint16_t INVALID_VEHICLE_ID = 0xFFFF;
constexpr uint16_t INVALID_PLAYER_ID = 0xFFFF;

inline constexpr bool IsBaseVehicleModel(uint32_t modelId)
{
	return modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END;
}

inline constexpr bool IsBaseVehicleModel(int modelId)
{
	return IsBaseVehicleModel(static_cast<uint32_t>(modelId));
}

inline constexpr bool IsCustomVehicleModel(uint32_t modelId)
{
	return modelId >= CUSTOM_MODEL_BASE_ID;
}

inline constexpr bool IsCustomVehicleModel(int modelId)
{
	return modelId >= 0 && static_cast<uint32_t>(modelId) >= CUSTOM_MODEL_BASE_ID;
}

inline constexpr uint32_t GetBaseModelIndex(uint32_t modelId)
{
	return modelId - BASE_MODEL_START;
}

class CBaseModelInfo;
CBaseModelInfo* GetEngineModelInfo(int modelId);
