#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

class CVehicle;

class CustomVehicleBindingManager {
public:
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

		bool hasCustomHorn { false };
		int8_t hornSoundId { 0 };
		float hornPitch { 1.0f };

		bool hasCustomSiren { false };
		bool sirenEnabled { false };
		int8_t sirenType { 1 };

		int8_t customLightingCategory { -1 };
		float customLightScaleMult { 1.0f };
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

	void Bind(uint16_t vehicleId, uint32_t customModelId);

	void Unbind(uint16_t vehicleId);

	void Process();

	Binding* Find(uint16_t vehicleId);

	Binding* FindByVehicle(CVehicle* vehicle);

	bool IsModelInUse(uint32_t customModelId);

	void SetVehicleStance(uint16_t vehicleId, float frontScale, float rearScale, float frontCamber, float rearCamber, float frontTrackWidth, float rearTrackWidth);

	void SetVehicleExtras(uint16_t vehicleId, uint8_t mask);

	void SetVehiclePaintjob(uint16_t vehicleId, int paintjobIndex);

	void SetVehicleNeon(uint16_t vehicleId, bool enabled, uint8_t r, uint8_t g, uint8_t b, float size);

	void SetVehicleWindowTint(uint16_t vehicleId, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b);

	void SetVehicleWheelColor(uint16_t vehicleId, uint8_t r, uint8_t g, uint8_t b);

	void SetVehicleBackfire(uint16_t vehicleId, bool enabled);

	void SetVehicleHorn(uint16_t vehicleId, int8_t hornSoundId, float hornPitch = 1.0f);

	void SetVehicleSiren(uint16_t vehicleId, bool enabled, int8_t sirenType = 1);

	void SetVehicleLights(uint16_t vehicleId, int8_t lightingCategory, float scaleMult = 1.0f);

	void ApplyPaintjobToVehicle(CVehicle* vehicle, int paintjobIndex);

	void ApplyWindowTintToVehicle(CVehicle* vehicle, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b);

	void ApplyWheelColorToVehicle(CVehicle* vehicle, uint8_t r, uint8_t g, uint8_t b);

	void ApplyAudioSettingsToVehicle(CVehicle* vehicle);

private:
	static inline std::mutex m_mutex;

	static inline std::unordered_map<
		uint16_t,
		Binding>
		m_bindings;

	static inline std::mutex s_baseModelMutex;
	static inline std::unordered_map<uint32_t, bool> s_baseModelIds;
};
