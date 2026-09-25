#pragma once

#include "HandlingEnum.h"
#include "HandlingStruct.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace HandlingMgr
{

struct ModelConfig
{
	// [model]
	std::string name;
	uint32_t visualBase = 0;
	uint32_t audioBase = 0;
	uint32_t handlingBase = 0;
	int16_t engineOnSound = -1;
	int16_t engineOffSound = -1;
	int16_t accelerateSound = -1;
	int16_t decelerateSound = -1;

	// [ide]
	bool hasIde = false;
	std::string vehicleType = "car";
	uint8_t vehicleClass = 0; // 0..13
	uint16_t frequency = 10;
	uint8_t level = 0;
	uint8_t compRules = 0;
	int16_t wheelModelId = -1;
	float wheelScaleFront = 1.0f;
	float wheelScaleRear = 1.0f;
	uint8_t wheelUpgradeClass = 0;
	uint8_t numExtras = 0;

	// [handling]
	bool hasHandling = false;
	tHandlingData handlingData = {};
	std::unordered_map<CHandlingAttrib, stHandlingMod, std::hash<uint8_t>> handlingMods;

	// [carmods]
	bool hasCarmods = false;
	std::vector<std::string> modNames;
	std::vector<int> modIds;
	std::unordered_map<std::string, std::vector<std::string>> categorizedMods;

	// [carcols]
	bool hasCarcols = false;
	uint8_t defaultPrimaryColor = 1;
	uint8_t defaultSecondaryColor = 1;
	uint8_t defaultTertiaryColor = 0;
	uint8_t defaultQuaternaryColor = 0;
	std::vector<std::array<uint8_t, 4>> colorVariations;

	// [flags]
	bool hasFlags = false;
	bool hasSiren = false;
	int8_t sirenType = 1;
	bool hasBackfire = false;
	int8_t hornSound = 0;
	float hornPitch = 1.0f;
	int8_t lightingCategory = -1;
	float lightScale = 1.0f;

	// [lighting]
	bool hasLighting = false;
	float headlightOffsetX = 0.0f;
	float headlightOffsetY = 0.0f;
	float headlightOffsetZ = 0.0f;
	float taillightOffsetX = 0.0f;
	float taillightOffsetY = 0.0f;
	float taillightOffsetZ = 0.0f;
	float headlightCustomX = 0.0f;
	float headlightCustomY = 0.0f;
	float headlightCustomZ = 0.0f;
	float taillightCustomX = 0.0f;
	float taillightCustomY = 0.0f;
	float taillightCustomZ = 0.0f;

	// [audio]
	bool hasAudio = false;
	std::string engineFile;
	std::string accelerationFile;
	std::string deaccelerationFile;
	std::string brakeFile;
	std::string crashFile;
	float audioVolume = 1.0f;
	float audioMinDistance = 5.0f;
	float audioMaxDistance = 90.0f;
	float audioPitchMultiplier = 1.0f;
	float audioAccelPitchFactor = 0.5f;
	uint8_t audioMuteNative = 1;

	// Arbitrary custom key-value store for scripting overrides and custom properties
	std::unordered_map<std::string, std::string> customProperties;
};

class ModelConfigParser
{
public:
	static bool ParseFile(const fs::path& filePath, ModelConfig& outConfig, uint32_t fallbackBaseModel = 411);
	static bool ParseString(const std::string& content, ModelConfig& outConfig, uint32_t fallbackBaseModel = 411, const std::string& sourceName = "model.ini");

	static uint8_t ParseVehicleClass(const std::string& str);
	static int ResolveVehicleModelId(const std::string& nameOrId);
	static int ResolveUpgradeComponentId(const std::string& partName);

	static void Trim(std::string& s);
	static std::string ToLower(std::string s);
	static bool ParseBool(const std::string& val, bool* ok = nullptr);
	static uint32_t ParseUInt(const std::string& val, bool* ok = nullptr);
	static int ParseInt(const std::string& val, bool* ok = nullptr);
	static float ParseFloat(const std::string& val, bool* ok = nullptr);
	static std::vector<std::string> Split(const std::string& s, char delim);
};

} // namespace HandlingMgr
