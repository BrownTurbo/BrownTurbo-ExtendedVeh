#pragma once

#ifndef RW
#define RW
#endif
#include <RenderWare.h>
#include "../Shared/CustomVehicleProtocol.hpp"
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class CVehicle;
class CAutomobile;
class CColModel;
class CVehicleModelInfo;
struct RwFrame;

class CustomVehicleBindingManager {
public:
	struct ModelOffsetConfig {
		float frontWheelOffsetZ { 0.0f };
		float rearWheelOffsetZ { 0.0f };
		float frontWheelOffsetY { 0.0f };
		float rearWheelOffsetY { 0.0f };
		float chassisOffsetX { 0.0f };
		float chassisOffsetY { 0.0f };
		float chassisOffsetZ { 0.0f };
		float frontTrackWidth { 0.0f };
		float rearTrackWidth { 0.0f };
		float frontWheelScale { 1.0f };
		float rearWheelScale { 1.0f };
		float frontCamber { 0.0f };
		float rearCamber { 0.0f };
		bool hasConfig { false };
	};

	struct ModelPlateConfig {
		bool hasConfig { false };
		std::string targetTexture;
		CustomVeh::Protocol::PlateMeshConfig frontPlate {};
		CustomVeh::Protocol::PlateMeshConfig rearPlate {};
	};

	struct Binding {
		uint16_t sampVehicleId {};
		uint32_t customModelId {};

		uint32_t gtaModelId {};
		int originalModelId { -1 };
		uint32_t baseModelId {};
		bool hasBaseModelId { false };

		CVehicle* appliedGameVehicle { nullptr };
		bool modelApplied {};

		float frontWheelScale { 1.0f };
		float rearWheelScale { 1.0f };
		float frontCamber { 0.0f };
		float rearCamber { 0.0f };
		float frontTrackWidth { 0.0f };
		float rearTrackWidth { 0.0f };
		float frontWheelOffsetZ { 0.0f };
		float rearWheelOffsetZ { 0.0f };
		float frontWheelOffsetY { 0.0f };
		float rearWheelOffsetY { 0.0f };
		float chassisOffsetX { 0.0f };
		float chassisOffsetY { 0.0f };
		float chassisOffsetZ { 0.0f };
		bool hasOffsets { false };
		RwV3d chassisBasePos { 0.0f, 0.0f, 0.0f };
		bool hasChassisBasePos { false };
		uint8_t extrasMask { 0xFF };

		int paintjobIndex { -1 };

		bool neonEnabled { false };
		uint8_t neonR { 0 }, neonG { 180 }, neonB { 255 };
		float neonSize { 2.5f };

		bool hasWindowTint { false };
		uint8_t windowTintAlpha { 255 };
		uint8_t windowTintR { 0 }, windowTintG { 0 }, windowTintB { 0 };

		bool hasWheelColor { false };
		uint8_t wheelColorR { 255 }, wheelColorG { 255 }, wheelColorB { 255 };

		bool hasStance { false };
		bool hasExtras { false };
		bool hasPaintjob { false };
		bool hasNeon { false };
		bool hasBackfire { false };
		bool hasCustomLighting { false };

		uint8_t lastPrimaryColor { 255 };
		uint8_t lastSecondaryColor { 255 };
		uint8_t lastTertiaryColor { 255 };
		uint8_t lastQuaternaryColor { 255 };

		bool backfireEnabled { false };
		uint32_t lastBackfireTick { 0 };

		bool hasPopupHeadlights { false };
		float popupHeadlightAngle { 0.0f };
		float popupMaxAngle { 0.6981317f }; // GTA SA native pop-up angle (0.6981317 rad = 40.0 deg)
		uint32_t lastHeadlightActiveTick { 0 };
		std::vector<RwFrame*> popupFrames;

		bool hasCustomHorn { false };
		int8_t hornSoundId { 0 };
		float hornPitch { 1.0f };

		bool hasCustomSiren { false };
		bool sirenEnabled { false };
		int8_t sirenType { 1 };

		int8_t customLightingCategory { -1 };
		float customLightScaleMult { 1.0f };

		bool hasCustomWheel { false };
		int16_t customWheelModelId { -1 };

		bool hasCustomPlateText { false };
		char customPlateText[32] = {};
		char lastPlateText[32] = {};
		bool hasTargetPlateTexture { false };
		char targetPlateTexture[32] = {};
		bool hasFrontPlateMesh { false };
		CustomVeh::Protocol::PlateMeshConfig frontPlateMesh {};
		bool hasRearPlateMesh { false };
		CustomVeh::Protocol::PlateMeshConfig rearPlateMesh {};
	};

	static CustomVehicleBindingManager& Instance()
	{
		static CustomVehicleBindingManager instance;
		return instance;
	}

	std::unordered_map<uint16_t, Binding> GetBindings() const
	{
		std::lock_guard lock(m_mutex);
		return m_bindings;
	}

	void ForEachBinding(const std::function<void(uint16_t, const Binding&)>& fn) const
	{
		std::lock_guard lock(m_mutex);
		for (const auto& [id, b] : m_bindings) {
			fn(id, b);
		}
	}

	static void SetBaseModelId(uint32_t customModelId, uint32_t baseModelId);
	static CColModel* GetCollisionForVehicle(CVehicle* vehicle);

	void Bind(uint16_t vehicleId, uint32_t customModelId);

	void Unbind(uint16_t vehicleId);

	void Clear();

	void Process();

	Binding* Find(uint16_t vehicleId);

	Binding* FindByVehicle(CVehicle* vehicle);

	bool IsModelInUse(uint32_t customModelId);

	void SetVehicleStance(uint16_t vehicleId, float frontScale, float rearScale, float frontCamber, float rearCamber, float frontTrackWidth, float rearTrackWidth);
	void SetVehicleInstanceOffsets(uint16_t vehicleId, float frontZ, float rearZ, float frontY, float rearY, float chassisX, float chassisY, float chassisZ);

	void SetVehicleExtras(uint16_t vehicleId, uint8_t mask);

	void SetVehiclePaintjob(uint16_t vehicleId, int paintjobIndex);

	void SetVehicleNeon(uint16_t vehicleId, bool enabled, uint8_t r, uint8_t g, uint8_t b, float size);

	void SetVehicleWindowTint(uint16_t vehicleId, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b);

	void SetVehicleWheelColor(uint16_t vehicleId, uint8_t r, uint8_t g, uint8_t b);

	void SetVehicleBackfire(uint16_t vehicleId, bool enabled);

	void SetVehicleHorn(uint16_t vehicleId, int8_t hornSoundId, float hornPitch = 1.0f);

	void SetVehicleSiren(uint16_t vehicleId, bool enabled, int8_t sirenType = 1);

	void SetVehicleLights(uint16_t vehicleId, int8_t lightingCategory, float scaleMult = 1.0f);

	void SetVehicleWheel(uint16_t vehicleId, int16_t wheelModelId);

	void ApplyPaintjobToVehicle(CVehicle* vehicle, int paintjobIndex);

	void ApplyWindowTintToVehicle(CVehicle* vehicle, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b);

	void ApplyWheelColorToVehicle(CVehicle* vehicle, uint8_t r, uint8_t g, uint8_t b);

	void ApplyWheelToVehicle(CVehicle* vehicle, int16_t wheelModelId);

	void ApplyAudioSettingsToVehicle(CVehicle* vehicle);
	void ApplyPlateToVehicle(CVehicle* vehicle, const char* text = nullptr);
	void OnVehicleFixed(CAutomobile* vehicle);
	void SetVehiclePlateText(uint16_t vehicleId, const char* text);
	void SetVehiclePlateMesh(uint16_t vehicleId, bool isRear, const CustomVeh::Protocol::PlateMeshConfig& cfg);
	void SetVehiclePlateTexture(uint16_t vehicleId, const char* textureName);

	struct PlateMaterialInfo {
		RpMaterial* material { nullptr };
		bool isBackground { false };
	};

	static std::vector<PlateMaterialInfo> FindVehiclePlateMaterials(
		RpClump* clump,
		CVehicleModelInfo* customModel,
		const char* lastKnownPlateText = nullptr,
		const char* targetTexture = nullptr);

	static bool ApplyPlateToClump(
		RpClump* clump,
		CVehicleModelInfo* customModel,
		const char* plateText,
		const char* lastKnownText = nullptr,
		uint32_t customModelId = 0,
		const char* targetTexture = nullptr,
		const CustomVeh::Protocol::PlateMeshConfig* frontPlate = nullptr,
		const CustomVeh::Protocol::PlateMeshConfig* rearPlate = nullptr);

	static void SetModelDefaultPlateText(uint32_t customModelId, const std::string& plateText);
	static std::string GetModelDefaultPlateText(uint32_t customModelId);

	static void SetModelPlateConfig(uint32_t customModelId, const ModelPlateConfig& cfg);
	static bool GetModelPlateConfig(uint32_t customModelId, ModelPlateConfig& outCfg);
	static void SetModelTargetPlateTexture(uint32_t customModelId, const std::string& textureName);
	static std::string GetModelTargetPlateTexture(uint32_t customModelId);

	static RpAtomic* CreatePlateQuadAtomic(
		RpClump* clump,
		const CustomVeh::Protocol::PlateMeshConfig& cfg,
		const char* plateText,
		bool isRear);

	static bool GetModelOffsets(uint32_t customModelId, ModelOffsetConfig& outCfg);
	static void SetModelOffsets(uint32_t customModelId, const ModelOffsetConfig& cfg);
	static void ApplyModelOffsetsToBinding(Binding& binding, const ModelOffsetConfig& cfg);
	bool HandleChatCommand(const std::string& fullCmd);

private:
	static inline std::recursive_mutex m_mutex;

	static inline std::unordered_map<
		uint16_t,
		Binding>
		m_bindings;

	static inline std::mutex s_baseModelMutex;
	static inline std::unordered_map<uint32_t, uint32_t> s_baseModelIds;
	static inline std::unordered_map<uint32_t, ModelOffsetConfig> s_modelOffsets;
	static inline std::unordered_map<uint32_t, ModelOffsetConfig> s_serverModelOffsets;
	static inline std::unordered_map<uint32_t, std::string> s_modelDefaultPlateText;
	static inline std::unordered_map<uint32_t, ModelPlateConfig> s_modelPlateConfigs;
	static inline std::unordered_map<uint32_t, ModelPlateConfig> s_serverModelPlateConfigs;
};
