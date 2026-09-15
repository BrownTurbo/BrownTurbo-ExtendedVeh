#pragma once

#include <array>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

#include "CVehicleManager.hpp"
#include "HandlingStruct.h"

class CHandlingStore
{
private:
	// fast-path for base models 400-611
	std::array<tHandlingData, CVehicleMgr::BASE_MAX_VEHICLE_MODELS> m_baseModels;

	// average-case map for open.mp custom models (20000+)
	std::unordered_map<uint32_t, tHandlingData> m_customModels;

	// Read-Write lock for thread-safe network synchronization
	mutable std::shared_mutex m_mutex;

public:
	tHandlingData* GetModelHandling(uint32_t modelId)
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);

		// Base models
		if (CVehicleMgr::IsBaseVehicleModel(modelId))
		{
			return &m_baseModels[CVehicleMgr::GetBaseModelIndex(modelId)];
		}

		// Custom open.mp models
		auto it = m_customModels.find(modelId);
		if (it != m_customModels.end())
		{
			return &it->second;
		}

		return nullptr;
	}

	void SetCustomModelHandling(uint32_t modelId, const tHandlingData& data)
	{
		std::unique_lock<std::shared_mutex> lock(m_mutex);
		if (CVehicleMgr::IsBaseVehicleModel(modelId))
		{
			m_baseModels[CVehicleMgr::GetBaseModelIndex(modelId)] = data;
		}
		else
		{
			m_customModels[modelId] = data;
		}
	}
};
