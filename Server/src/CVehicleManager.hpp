#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>

namespace CVehicleMgr
{
inline constexpr uint32_t BASE_MODEL_START = 400;
inline constexpr uint32_t BASE_MODEL_END = 611;
inline constexpr uint32_t BASE_MAX_VEHICLE_MODELS = 212;
inline constexpr uint32_t CUSTOM_MODEL_START = 20000;
inline constexpr uint32_t DEFAULT_MAX_VEHICLES = 65535;
inline constexpr uint32_t MAX_NETWORK_VEHICLES = 65535;

inline constexpr bool IsBaseVehicleModel(uint32_t modelId) noexcept
{
	return modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END;
}

inline constexpr bool IsCustomVehicleModel(uint32_t modelId) noexcept
{
	return modelId >= CUSTOM_MODEL_START;
}

inline constexpr uint32_t GetBaseModelIndex(uint32_t modelId) noexcept
{
	return modelId - BASE_MODEL_START;
}

class VehicleRegistry
{
private:
	uint32_t m_maxVehicles { DEFAULT_MAX_VEHICLES };

	// Maps custom model IDs (e.g., 20000+) to a contiguous internal index
	// starting at 212
	std::unordered_map<uint32_t, uint32_t> m_customModelIndices;
	uint32_t m_nextCustomIndex { BASE_MAX_VEHICLE_MODELS };

	mutable std::shared_mutex m_mutex;

	// Strict Singleton enforcement
	VehicleRegistry() = default;

public:
	VehicleRegistry(const VehicleRegistry&) = delete;
	VehicleRegistry& operator=(const VehicleRegistry&) = delete;

	static VehicleRegistry& Get() noexcept
	{
		static VehicleRegistry instance;
		return instance;
	}

	void SetMaxVehiclesLimit(uint32_t limit) noexcept
	{
		std::unique_lock lock(m_mutex); // C++17 CTAD
		m_maxVehicles = limit;
	}

	uint32_t GetMaxVehiclesLimit() const noexcept
	{
		std::shared_lock lock(m_mutex);
		return m_maxVehicles;
	}

	bool IsValidVehicleID(uint32_t vehicleId) const noexcept
	{
		return (vehicleId >= 1 && vehicleId <= GetMaxVehiclesLimit());
	}

	bool IsValidVehicleModel(uint32_t modelId) const noexcept
	{
		if (modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END)
		{
			return true;
		}

		std::shared_lock lock(m_mutex);
		return m_customModelIndices.find(modelId) != m_customModelIndices.end();
	}

	uint32_t RegisterCustomModel(uint32_t modelId)
	{
		if (modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END)
		{
			return modelId - BASE_MODEL_START;
		}

		std::unique_lock lock(m_mutex);

		auto it = m_customModelIndices.find(modelId);
		if (it != m_customModelIndices.end())
		{
			return it->second;
		}

		uint32_t allocatedIndex = m_nextCustomIndex++;
		m_customModelIndices.emplace(modelId, allocatedIndex);
		return allocatedIndex;
	}

	void UnregisterCustomModel(uint32_t modelId)
	{
		std::unique_lock lock(m_mutex);
		m_customModelIndices.erase(modelId);
	}

	std::optional<uint32_t> GetModelIndex(uint32_t modelId) const noexcept
	{
		if (modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END)
		{
			return static_cast<uint32_t>(modelId - BASE_MODEL_START);
		}

		std::shared_lock lock(m_mutex);
		auto it = m_customModelIndices.find(modelId);
		if (it != m_customModelIndices.end())
		{
			return it->second;
		}

		return std::nullopt;
	}
};

inline bool IS_VALID_VEHICLEID(uint32_t id) noexcept
{
	return VehicleRegistry::Get().IsValidVehicleID(id);
}

inline bool IS_VALID_VEHICLE_MODEL(uint32_t modelid) noexcept
{
	return VehicleRegistry::Get().IsValidVehicleModel(modelid);
}

inline uint32_t VEHICLE_MODEL_INDEX(uint32_t modelid)
{
	auto index = VehicleRegistry::Get().GetModelIndex(modelid);
	if (!index.has_value())
	{
		throw std::out_of_range("Attempted to resolve handling index for an "
								"unregistered custom model.");
	}
	return index.value();
}
}
