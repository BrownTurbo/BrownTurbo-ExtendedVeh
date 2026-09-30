#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include "../../Shared/CustomVehicleProtocol.hpp"

class CustomVehicleBindingRegistry
{
public:
	struct VehicleCustomState
	{
		uint32_t customModelId { 0 };
		CustomVeh::Protocol::VehicleStancePacket stance {};
		CustomVeh::Protocol::VehicleExtrasPacket extras {};
		CustomVeh::Protocol::VehiclePaintjobPacket paintjob {};
		CustomVeh::Protocol::VehicleNeonPacket neon {};
		CustomVeh::Protocol::VehicleWindowTintPacket windowTint {};
		CustomVeh::Protocol::VehicleWheelColorPacket wheelColor {};
		CustomVeh::Protocol::VehicleBackfirePacket backfire {};
		CustomVeh::Protocol::VehicleHornPacket horn {};
		CustomVeh::Protocol::VehicleSirenPacket siren {};
		CustomVeh::Protocol::VehicleLightsPacket lights {};
		CustomVeh::Protocol::VehicleWheelPacket wheel {};
		CustomVeh::Protocol::VehicleOffsetsPacket offsets {};
		bool hasCustomStance { false };
		bool hasCustomExtras { false };
		bool hasCustomPaintjob { false };
		bool hasCustomNeon { false };
		bool hasCustomWindowTint { false };
		bool hasCustomWheelColor { false };
		bool hasCustomBackfire { false };
		bool hasCustomHorn { false };
		bool hasCustomSiren { false };
		bool hasCustomLights { false };
		bool hasCustomWheel { false };
		bool hasCustomOffsets { false };
		CustomVeh::Protocol::VehiclePlatePacket plate {};
		bool hasCustomPlate { false };
		CustomVeh::Protocol::PlateMeshConfig frontPlateMesh {};
		CustomVeh::Protocol::PlateMeshConfig rearPlateMesh {};
		bool hasFrontPlateMesh { false };
		bool hasRearPlateMesh { false };
		std::string targetPlateTexture;
		bool hasTargetPlateTexture { false };
	};

	struct ModelAudioDefaults
	{
		bool hasHorn { false };
		int8_t hornSoundId { 0 };
		float hornPitch { 1.0f };
		bool hasSiren { false };
		bool sirenEnabled { false };
		int8_t sirenType { 1 };
	};

	CustomVehicleBindingRegistry(const CustomVehicleBindingRegistry&) = delete;
	CustomVehicleBindingRegistry& operator=(const CustomVehicleBindingRegistry&) = delete;
	CustomVehicleBindingRegistry(CustomVehicleBindingRegistry&&) = delete;
	CustomVehicleBindingRegistry& operator=(CustomVehicleBindingRegistry&&) = delete;

	static CustomVehicleBindingRegistry& Instance()
	{
		static CustomVehicleBindingRegistry instance;
		return instance;
	}

	bool Bind(uint16_t sampVehicleId, uint32_t customModelId)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto existing = m_states.find(sampVehicleId);
		if (existing != m_states.end() && existing->second.customModelId == customModelId)
		{
			return false;
		}
		VehicleCustomState state = m_states[sampVehicleId];
		state.customModelId = customModelId;
		auto modelIt = m_modelAudio.find(customModelId);
		if (modelIt != m_modelAudio.end())
		{
			if (modelIt->second.hasHorn && !state.hasCustomHorn)
			{
				state.horn.sampVehicleId = sampVehicleId;
				state.horn.hornSoundId = modelIt->second.hornSoundId;
				state.horn.hornPitch = modelIt->second.hornPitch;
				state.hasCustomHorn = true;
			}
			if (modelIt->second.hasSiren && !state.hasCustomSiren)
			{
				state.siren.sampVehicleId = sampVehicleId;
				state.siren.enabled = modelIt->second.sirenEnabled ? 1 : 0;
				state.siren.sirenType = modelIt->second.sirenType;
				state.hasCustomSiren = true;
			}
		}
		m_states.insert_or_assign(sampVehicleId, std::move(state));
		return true;
	}

	void Unbind(uint16_t sampVehicleId)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_states.erase(sampVehicleId);
	}

	std::optional<uint32_t> Get(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it == m_states.end() || it->second.customModelId == 0)
			return std::nullopt;
		return std::optional<uint32_t>(it->second.customModelId);
	}

	void SetStance(uint16_t sampVehicleId, const CustomVeh::Protocol::VehicleStancePacket& stance)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.stance = stance;
		state.hasCustomStance = true;
	}

	std::optional<CustomVeh::Protocol::VehicleStancePacket> GetStance(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomStance)
			return it->second.stance;
		return std::nullopt;
	}

	void SetExtras(uint16_t sampVehicleId, uint8_t mask)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.extras.sampVehicleId = sampVehicleId;
		state.extras.extrasMask = mask;
		state.hasCustomExtras = true;
	}

	std::optional<uint8_t> GetExtras(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomExtras)
			return it->second.extras.extrasMask;
		return std::nullopt;
	}

	void SetPaintjob(uint16_t sampVehicleId, int8_t pj)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.paintjob.sampVehicleId = sampVehicleId;
		state.paintjob.paintjobIndex = pj;
		state.hasCustomPaintjob = true;
	}

	std::optional<int8_t> GetPaintjob(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomPaintjob)
			return it->second.paintjob.paintjobIndex;
		return std::nullopt;
	}

	void SetNeon(uint16_t sampVehicleId, const CustomVeh::Protocol::VehicleNeonPacket& neon)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.neon = neon;
		state.hasCustomNeon = true;
	}

	std::optional<CustomVeh::Protocol::VehicleNeonPacket> GetNeon(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomNeon)
			return it->second.neon;
		return std::nullopt;
	}

	void SetWindowTint(uint16_t sampVehicleId, const CustomVeh::Protocol::VehicleWindowTintPacket& tint)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.windowTint = tint;
		state.hasCustomWindowTint = true;
	}

	std::optional<CustomVeh::Protocol::VehicleWindowTintPacket> GetWindowTint(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomWindowTint)
			return it->second.windowTint;
		return std::nullopt;
	}

	void SetWheelColor(uint16_t sampVehicleId, const CustomVeh::Protocol::VehicleWheelColorPacket& wc)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.wheelColor = wc;
		state.hasCustomWheelColor = true;
	}

	std::optional<CustomVeh::Protocol::VehicleWheelColorPacket> GetWheelColor(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomWheelColor)
			return it->second.wheelColor;
		return std::nullopt;
	}

	void SetBackfire(uint16_t sampVehicleId, bool enabled)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.backfire.sampVehicleId = sampVehicleId;
		state.backfire.enabled = enabled ? 1 : 0;
		state.hasCustomBackfire = true;
	}

	std::optional<bool> GetBackfire(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomBackfire)
			return it->second.backfire.enabled != 0;
		return std::nullopt;
	}

	void SetHorn(uint16_t sampVehicleId, int8_t soundId, float pitch)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.horn.sampVehicleId = sampVehicleId;
		state.horn.hornSoundId = soundId;
		state.horn.hornPitch = pitch;
		state.hasCustomHorn = true;
	}

	std::optional<CustomVeh::Protocol::VehicleHornPacket> GetHorn(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomHorn)
			return it->second.horn;
		return std::nullopt;
	}

	void SetSiren(uint16_t sampVehicleId, bool enabled, int8_t sirenType)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.siren.sampVehicleId = sampVehicleId;
		state.siren.enabled = enabled ? 1 : 0;
		state.siren.sirenType = sirenType;
		state.hasCustomSiren = true;
	}

	std::optional<CustomVeh::Protocol::VehicleSirenPacket> GetSiren(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomSiren)
			return it->second.siren;
		return std::nullopt;
	}

	void SetLights(uint16_t sampVehicleId, int8_t lightingCategory, float scaleMult = 1.0f)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.lights.sampVehicleId = sampVehicleId;
		state.lights.lightingCategory = lightingCategory;
		state.lights.lightScaleMult = (scaleMult > 0.05f) ? scaleMult : 1.0f;
		state.hasCustomLights = true;
	}

	std::optional<CustomVeh::Protocol::VehicleLightsPacket> GetLights(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomLights)
			return it->second.lights;
		return std::nullopt;
	}

	void SetWheel(uint16_t sampVehicleId, int16_t wheelModelId)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.wheel.sampVehicleId = sampVehicleId;
		state.wheel.wheelModelId = wheelModelId;
		state.hasCustomWheel = true;
	}

	std::optional<CustomVeh::Protocol::VehicleWheelPacket> GetWheel(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomWheel)
			return it->second.wheel;
		return std::nullopt;
	}

	void SetOffsets(uint16_t sampVehicleId, const CustomVeh::Protocol::VehicleOffsetsPacket& offsets)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.offsets = offsets;
		state.hasCustomOffsets = true;
	}

	std::optional<CustomVeh::Protocol::VehicleOffsetsPacket> GetOffsets(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomOffsets)
			return it->second.offsets;
		return std::nullopt;
	}

	void SetPlate(uint16_t sampVehicleId, const std::string& plateText)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.plate.sampVehicleId = sampVehicleId;
		std::strncpy(state.plate.plateText, plateText.c_str(), sizeof(state.plate.plateText) - 1);
		state.plate.plateText[sizeof(state.plate.plateText) - 1] = '\0';
		state.hasCustomPlate = true;
	}

	std::optional<CustomVeh::Protocol::VehiclePlatePacket> GetPlate(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasCustomPlate)
			return it->second.plate;
		return std::nullopt;
	}

	void SetPlateMesh(uint16_t sampVehicleId, bool isRear, const CustomVeh::Protocol::PlateMeshConfig& cfg)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		if (isRear)
		{
			state.rearPlateMesh = cfg;
			state.hasRearPlateMesh = true;
		}
		else
		{
			state.frontPlateMesh = cfg;
			state.hasFrontPlateMesh = true;
		}
	}

	std::optional<CustomVeh::Protocol::PlateMeshConfig> GetPlateMesh(uint16_t sampVehicleId, bool isRear) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end())
		{
			if (isRear && it->second.hasRearPlateMesh)
				return it->second.rearPlateMesh;
			if (!isRear && it->second.hasFrontPlateMesh)
				return it->second.frontPlateMesh;
		}
		return std::nullopt;
	}

	void SetPlateTexture(uint16_t sampVehicleId, const std::string& textureName)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& state = m_states[sampVehicleId];
		state.targetPlateTexture = textureName;
		state.hasTargetPlateTexture = true;
	}

	std::optional<std::string> GetPlateTexture(uint16_t sampVehicleId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_states.find(sampVehicleId);
		if (it != m_states.end() && it->second.hasTargetPlateTexture)
			return it->second.targetPlateTexture;
		return std::nullopt;
	}

	void SetModelHorn(uint32_t modelId, int8_t soundId, float pitch)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& ma = m_modelAudio[modelId];
		ma.hasHorn = true;
		ma.hornSoundId = soundId;
		ma.hornPitch = pitch;
	}

	void SetModelSiren(uint32_t modelId, bool enabled, int8_t sirenType)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto& ma = m_modelAudio[modelId];
		ma.hasSiren = true;
		ma.sirenEnabled = enabled;
		ma.sirenType = sirenType;
	}

	std::optional<ModelAudioDefaults> GetModelAudio(uint32_t modelId) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_modelAudio.find(modelId);
		if (it != m_modelAudio.end())
			return it->second;
		return std::nullopt;
	}

private:
	CustomVehicleBindingRegistry() = default;

	mutable std::mutex m_mutex;
	std::unordered_map<uint16_t, VehicleCustomState> m_states;
	std::unordered_map<uint32_t, ModelAudioDefaults> m_modelAudio;
};
