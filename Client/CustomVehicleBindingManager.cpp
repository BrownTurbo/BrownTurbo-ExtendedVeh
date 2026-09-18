#include "CustomVehicleBindingManager.h"
#include "CollisionLoader.h"
#include "streamingextender.hpp"
#include "handling_manager.hpp"
#include "utils.h"

#include <game_sa/CAutomobile.h>
#include <game_sa/CBike.h>
#include <game_sa/CBoat.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CStreaming.h>
#include <game_sa/CVehicleModelInfo.h>

void CustomVehicleBindingManager::Bind(uint16_t vehicleId, uint32_t customModelId)
{
	std::lock_guard lock(m_mutex);

	Binding binding;
	binding.sampVehicleId = vehicleId;
	binding.customModelId = customModelId;
	binding.gtaModelId = customModelId;
	binding.originalModelId = -1;
	binding.appliedGameVehicle = nullptr;
	binding.modelApplied = false;

	m_bindings[vehicleId] = binding;
	HandlingManager::IncrementModelUse(customModelId);
}

void CustomVehicleBindingManager::Unbind(uint16_t vehicleId)
{
	std::lock_guard lock(m_mutex);

	auto it = m_bindings.find(vehicleId);
	if (it == m_bindings.end())
		return;

	const Binding binding = it->second;
	uint32_t modelId = binding.customModelId;

	HandlingManager::DecrementModelUse(modelId);

	if (IsBaseVehicleModel(modelId) && CStreaming::ms_aInfoForModel[modelId].m_nLoadState == LOADSTATE_LOADED) {
		CStreaming::RemoveModel(modelId);
	}

	std::string err_;
	ExtendedVeh::Collision::CollisionLoader::Instance().Unload(modelId, err_);
	if (!err_.empty()) {
		SendMsg(0xFFFFFF, std::format("Error: %s; vehicleId=%d modelId=%d", err_, vehicleId, modelId).c_str());
	}

	if (binding.modelApplied && binding.originalModelId >= 0) {
		auto* vehicle = GetGameVehicleFromPool(vehicleId);
		if (vehicle && vehicle == binding.appliedGameVehicle && IsVehiclePointerValid(vehicle)) {
			auto* origModel = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(binding.originalModelId));
			if (origModel && origModel->m_pRwClump) {
				vehicle->DeleteRwObject();
				RpClump* origClump = RpClumpClone(origModel->m_pRwClump);
				if (origClump) {
					vehicle->AttachToRwObject(reinterpret_cast<RwObject*>(origClump), true);
					if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
						reinterpret_cast<CAutomobile*>(vehicle)->SetupModelNodes();
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
						reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
						reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
					}
				}
				if (origModel->m_pColModel) {
					vehicle->m_pColModel = origModel->m_pColModel;
				}
			}
			HandlingManager::ApplyModelToVehicles(static_cast<uint16_t>(binding.originalModelId), nullptr);
		}
	}

	m_bindings.erase(it);
}

CustomVehicleBindingManager::Binding* CustomVehicleBindingManager::Find(uint16_t vehicleId)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	return it != m_bindings.end() ? &it->second : nullptr;
}

bool CustomVehicleBindingManager::IsModelInUse(uint32_t customModelId)
{
	std::lock_guard lock(m_mutex);
	for (const auto& [id, b] : m_bindings) {
		if (b.customModelId == customModelId)
			return true;
	}
	return false;
}

void CustomVehicleBindingManager::Process()
{
	std::lock_guard lock(m_mutex);

	for (auto& [vehicleId, binding] : m_bindings) {
		auto* model = StreamingExtender::GetCustomModel(binding.customModelId);
		if (!model || !model->m_pRwClump)
			continue;

		auto* vehicle = GetGameVehicleFromPool(vehicleId);
		if (!vehicle || !IsVehiclePointerValid(vehicle)) {
			binding.appliedGameVehicle = nullptr;
			binding.modelApplied = false;
			continue;
		}

		if (binding.originalModelId < 0) {
			binding.originalModelId = vehicle->m_nModelIndex;
		}

		if (!binding.modelApplied || binding.appliedGameVehicle != vehicle) {
			RpClump* newClump = RpClumpClone(model->m_pRwClump);
			if (newClump) {
				vehicle->DeleteRwObject();
				vehicle->AttachToRwObject(reinterpret_cast<RwObject*>(newClump), true);

				if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
					reinterpret_cast<CAutomobile*>(vehicle)->SetupModelNodes();
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
					reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
					reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
				}

				if (model->m_pColModel) {
					vehicle->m_pColModel = model->m_pColModel;
				}

				binding.appliedGameVehicle = vehicle;
				binding.modelApplied = true;

				HandlingManager::ApplyModelToVehicles(static_cast<uint16_t>(binding.customModelId), nullptr);
			}
		}
	}
}
