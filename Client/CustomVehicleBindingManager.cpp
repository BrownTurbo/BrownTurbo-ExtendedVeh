#include "CustomVehicleBindingManager.h"
#include "CollisionLoader.h"
#include "handling_manager.hpp"
#include "streamingextender.hpp"
#include "utils.h"

#include <game_sa/CAutomobile.h>
#include <game_sa/CBike.h>
#include <game_sa/CBoat.h>
#include <game_sa/CColModel.h>
#include <game_sa/CCollisionData.h>
#include <game_sa/CCustomCarPlateMgr.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CPlayerPed.h>
#include <game_sa/CStreaming.h>
#include <game_sa/CTxdStore.h>
#include <game_sa/CVehicleModelInfo.h>
#include <game_sa/CWorld.h>
#include <game_sa/NodeName.h>
#include <game_sa/rw/rpworld.h>
#include <game_sa/rw/rwcore.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

static bool IsFrameOrChildOf(RwFrame* frame, RwFrame* targetParent)
{
	for (RwFrame* f = frame; f != nullptr; f = RwFrameGetParent(f)) {
		if (f == targetParent)
			return true;
	}
	return false;
}

static RpClump* CloneClumpPreservingOrder(RpClump* srcClump)
{
	if (!srcClump)
		return nullptr;

	// RenderWare's RpClumpClone inserts cloned atomics at the head of the new clump's
	// linked list, reversing their render order. Cloning twice restores the original
	// atomic sequence so alpha blending (transparent glass, windows, lights) renders
	// front-to-back without occluding interior geometries (verified: MTA:SA CRenderWareSA.cpp:401-404).
	RpClump* temp = RpClumpClone(srcClump);
	if (!temp)
		return nullptr;

	RpClump* clone = RpClumpClone(temp);
	RpClumpDestroy(temp);
	return clone;
}

static void ApplyExtrasToClump(RpClump* clump, uint8_t mask)
{
	if (!clump)
		return;

	for (int i = 1; i <= 8; ++i) {
		std::string extraName = std::format("extra{}", i);
		RwFrame* frame = CClumpModelInfo::GetFrameFromName(clump, extraName.c_str());
		if (!frame) {
			std::string extraNameUnder = std::format("extra_{}", i);
			frame = CClumpModelInfo::GetFrameFromName(clump, extraNameUnder.c_str());
		}
		if (!frame)
			continue;

		bool visible = (mask & (1 << (i - 1))) != 0;

		struct AtomicExtraContext {
			RwFrame* targetFrame;
			bool isVisible;
		} ctx { frame, visible };

		RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
			auto* c = reinterpret_cast<AtomicExtraContext*>(data);
			if (IsFrameOrChildOf(RpAtomicGetFrame(atomic), c->targetFrame)) {
				if (c->isVisible) {
					RpAtomicSetFlags(atomic, rpATOMICRENDER);
				} else {
					RpAtomicSetFlags(atomic, 0);
				}
			}
			return atomic;
		},
			&ctx);
	}
}

void CustomVehicleBindingManager::SetBaseModelId(uint32_t customModelId, uint32_t baseModelId)
{
	{
		std::lock_guard lock(s_baseModelMutex);
		s_baseModelIds[customModelId] = baseModelId;
	}

	std::lock_guard lock(m_mutex);
	for (auto& [vehicleId, binding] : m_bindings) {
		if (binding.customModelId == customModelId) {
			binding.baseModelId = baseModelId;
			binding.hasBaseModelId = true;
		}
	}
}

static inline std::unordered_map<CVehicle*, CColModel*> s_fastVehicleColMap;
static inline std::shared_mutex s_fastColMutex;
static inline std::atomic<bool> s_hasCustomCols { false };

CColModel* CustomVehicleBindingManager::GetCollisionForVehicle(CVehicle* vehicle)
{
	if (!vehicle || !s_hasCustomCols.load(std::memory_order_relaxed))
		return nullptr;
	std::shared_lock lock(s_fastColMutex);
	auto it = s_fastVehicleColMap.find(vehicle);
	if (it == s_fastVehicleColMap.end())
		return nullptr;
	CColModel* col = it->second;
	if (col && col->m_pColData && col->m_pColData->m_pLines && col->m_pColData->m_nNumLines >= 4) {
		return col;
	}
	return nullptr;
}

void CustomVehicleBindingManager::Bind(uint16_t vehicleId, uint32_t customModelId)
{
	uint32_t baseModelId = 0;
	bool hasBaseModelId = false;
	std::lock_guard lock(m_mutex);

	auto existing = m_bindings.find(vehicleId);
	if (existing != m_bindings.end()) {
		Binding& binding = existing->second;
		if (binding.customModelId == customModelId) {
			{
				std::lock_guard baseLock(s_baseModelMutex);
				auto baseIt = s_baseModelIds.find(customModelId);
				if (baseIt != s_baseModelIds.end()) {
					existing->second.hasBaseModelId = true;
					hasBaseModelId = true;
					existing->second.baseModelId = baseIt->second;
					baseModelId = baseIt->second;
				}
			}
			ClientLog(LogLevel::Debug, std::format("Duplicate bind ignored (already active): vehicle={} customModel={}", vehicleId, customModelId));
			return;
		}

		if (binding.appliedGameVehicle) {
			std::unique_lock colLock(s_fastColMutex);
			s_fastVehicleColMap.erase(binding.appliedGameVehicle);
			s_hasCustomCols.store(!s_fastVehicleColMap.empty(), std::memory_order_relaxed);
		}
		HandlingManager::DecrementModelUse(binding.customModelId);
		m_bindings.erase(existing);
	}

	Binding binding;
	binding.sampVehicleId = vehicleId;
	binding.customModelId = customModelId;
	binding.gtaModelId = customModelId;
	binding.originalModelId = -1;
	binding.appliedGameVehicle = nullptr;
	binding.modelApplied = false;

	{
		std::lock_guard baseLock(s_baseModelMutex);
		auto baseIt = s_baseModelIds.find(customModelId);
		if (baseIt != s_baseModelIds.end()) {
			binding.baseModelId = baseIt->second;
			binding.hasBaseModelId = true;
		}
	}

	auto offsetIt = s_modelOffsets.find(customModelId);
	if (offsetIt != s_modelOffsets.end()) {
		ApplyModelOffsetsToBinding(binding, offsetIt->second);
	}

	m_bindings.emplace(vehicleId, binding);
	HandlingManager::IncrementModelUse(customModelId);
	ClientLog(LogLevel::Info, std::format("Binding created: vehicle={} customModel={} baseModel={}", vehicleId, customModelId, binding.baseModelId));
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
				short savedUpgrades[15];
				for (int i = 0; i < 15; ++i) {
					savedUpgrades[i] = vehicle->m_anUpgrades[i];
					vehicle->m_anUpgrades[i] = -1;
				}

				RwMatrix savedRwMatrix;
				bool hadMatrix = (vehicle->m_matrix != nullptr);
				if (hadMatrix) {
					vehicle->m_matrix->UpdateRW(&savedRwMatrix);
				} else {
					vehicle->m_placement.UpdateRwMatrix(&savedRwMatrix);
				}
				CSimpleTransform savedPlacement = vehicle->m_placement;

				CWorld::Remove(vehicle);
				vehicle->DeleteRwObject();

				RpClump* origClump = CloneClumpPreservingOrder(origModel->m_pRwClump);
				if (origClump) {
					CVisibilityPlugins::SetupVehicleVariables(origClump);
					origModel->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
					origModel->SetEditableMaterials(origClump); // non-static: must call on model instance

					char plateText[32] = {};
					if (binding.hasCustomPlateText && binding.customPlateText[0] != '\0') {
						strncpy_s(plateText, sizeof(plateText), binding.customPlateText, _TRUNCATE);
						ApplyPlateToClump(origClump, origModel, plateText, nullptr);
					} else if (GetVehiclePlateText(vehicleId, plateText, sizeof(plateText))) {
						ApplyPlateToClump(origClump, origModel, plateText, nullptr);
					}

					RwFrame* rootFrame = RpClumpGetFrame(origClump);
					if (rootFrame) {
						std::memcpy(&rootFrame->modelling, &savedRwMatrix, sizeof(RwMatrix));
						RwFrameUpdateObjects(rootFrame);
					}

					vehicle->AttachToRwObject(reinterpret_cast<RwObject*>(origClump), false);

					if (hadMatrix && vehicle->m_matrix) {
						((void(__thiscall*)(CMatrix*, RwMatrix*))0x59AD20)(vehicle->m_matrix, &savedRwMatrix);
					}
					vehicle->m_placement = savedPlacement;

					if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
						auto* car = reinterpret_cast<CAutomobile*>(vehicle);
						car->SetupModelNodes();
						for (int p = 0; p < 3; ++p) {
							short frameId = car->m_panels[p].m_nFrameId;
							if (frameId >= 0 && (frameId >= CAR_NUM_NODES || !car->m_aCarNodes[frameId])) {
								car->m_panels[p].m_nFrameId = -1;
								car->m_panels[p].ResetPanel();
							}
						}
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
						reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
						reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
					}

					for (int i = 0; i < 15; ++i) {
						if (savedUpgrades[i] >= 1000 && savedUpgrades[i] <= 1193) {
							vehicle->AddUpgrade(savedUpgrades[i], i);
						}
					}

					CWorld::Add(vehicle);
				}
			}
			HandlingManager::OnVehicleStreamIn(vehicle, static_cast<uint16_t>(vehicleId));
		}
	}

	if (binding.appliedGameVehicle) {
		std::unique_lock colLock(s_fastColMutex);
		s_fastVehicleColMap.erase(binding.appliedGameVehicle);
		s_hasCustomCols.store(!s_fastVehicleColMap.empty(), std::memory_order_relaxed);
	}

	m_bindings.erase(it);
}

void CustomVehicleBindingManager::Clear()
{
	{
		std::unique_lock colLock(s_fastColMutex);
		s_fastVehicleColMap.clear();
		s_hasCustomCols.store(false, std::memory_order_relaxed);
	}
	std::lock_guard lock(m_mutex);
	for (auto& [vehicleId, binding] : m_bindings) {
		HandlingManager::DecrementModelUse(binding.customModelId);
	}
	m_bindings.clear();
	s_modelOffsets.clear();
	s_modelDefaultPlateText.clear();
	ClientLog(LogLevel::Info, "CustomVehicleBindingManager::Clear: All bindings and model offsets cleared.");
}

CustomVehicleBindingManager::Binding* CustomVehicleBindingManager::Find(uint16_t vehicleId)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	return it != m_bindings.end() ? &it->second : nullptr;
}

CustomVehicleBindingManager::Binding* CustomVehicleBindingManager::FindByVehicle(CVehicle* vehicle)
{
	if (!vehicle)
		return nullptr;
	std::lock_guard lock(m_mutex);
	for (auto& [id, b] : m_bindings) {
		if (b.appliedGameVehicle == vehicle)
			return &b;
	}
	for (auto& [id, b] : m_bindings) {
		if (GetGameVehicleFromPool(id) == vehicle) {
			b.appliedGameVehicle = vehicle;
			return &b;
		}
	}
	return nullptr;
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

void CustomVehicleBindingManager::SetVehicleStance(uint16_t vehicleId, float frontScale, float rearScale, float frontCamber, float rearCamber, float frontTrackWidth, float rearTrackWidth)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasStance = true;
		it->second.frontWheelScale = frontScale;
		it->second.rearWheelScale = rearScale;
		it->second.frontCamber = frontCamber;
		it->second.rearCamber = rearCamber;
		it->second.frontTrackWidth = frontTrackWidth;
		it->second.rearTrackWidth = rearTrackWidth;
	}
}

void CustomVehicleBindingManager::SetVehicleInstanceOffsets(uint16_t vehicleId, float frontZ, float rearZ, float frontY, float rearY, float chassisX, float chassisY, float chassisZ)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.frontWheelOffsetZ = frontZ;
		it->second.rearWheelOffsetZ = rearZ;
		it->second.frontWheelOffsetY = frontY;
		it->second.rearWheelOffsetY = rearY;
		it->second.chassisOffsetX = chassisX;
		it->second.chassisOffsetY = chassisY;
		it->second.chassisOffsetZ = chassisZ;
		it->second.hasOffsets = true;
	}
}

void CustomVehicleBindingManager::SetVehicleExtras(uint16_t vehicleId, uint8_t mask)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasExtras = true;
		it->second.extrasMask = mask;
		if (it->second.modelApplied && it->second.appliedGameVehicle && IsVehiclePointerValid(it->second.appliedGameVehicle)) {
			RpClump* clump = reinterpret_cast<RpClump*>(it->second.appliedGameVehicle->m_pRwObject);
			if (clump) {
				ApplyExtrasToClump(clump, mask);
			}
		}
	}
}

void CustomVehicleBindingManager::Process()
{
	std::lock_guard lock(m_mutex);

	for (auto& [vehicleId, binding] : m_bindings) {
		auto* model = StreamingExtender::GetCustomModel(binding.customModelId);
		if (!model) {
			ClientLog(LogLevel::Debug, std::format("WAIT: vehicle={} customModel={} has no StreamingExtender model.", vehicleId, binding.customModelId));
			continue;
		}

		if (!model->m_pRwClump) {
			ClientLog(LogLevel::Debug, std::format("WAIT: vehicle={} customModel={} model exists but m_pRwClump is NULL.", vehicleId, binding.customModelId));
			continue;
		}

		if (!model->m_pVehicleStruct) {
			ClientLog(LogLevel::Debug, std::format("WAIT: vehicle={} customModel={} m_pVehicleStruct=NULL.", vehicleId, binding.customModelId));
			continue;
		}

		auto* vehicle = GetGameVehicleFromPool(vehicleId);
		if (!vehicle || !IsVehiclePointerValid(vehicle)) {
			if (binding.appliedGameVehicle) {
				std::unique_lock colLock(s_fastColMutex);
				s_fastVehicleColMap.erase(binding.appliedGameVehicle);
				s_hasCustomCols.store(!s_fastVehicleColMap.empty(), std::memory_order_relaxed);
			}
			binding.appliedGameVehicle = nullptr;
			binding.modelApplied = false;
			binding.originalModelId = -1;
			continue;
		}

		if (binding.originalModelId < 0) {
			binding.originalModelId = vehicle->m_nModelIndex;
		}

		if (!binding.modelApplied || binding.appliedGameVehicle != vehicle) {
			if (binding.appliedGameVehicle && binding.appliedGameVehicle != vehicle) {
				std::unique_lock colLock(s_fastColMutex);
				s_fastVehicleColMap.erase(binding.appliedGameVehicle);
				s_hasCustomCols.store(!s_fastVehicleColMap.empty(), std::memory_order_relaxed);
			}
			ClientLog(LogLevel::Info, std::format("Applying visual model: vehicle={} customModel={} baseModel={} vehiclePtr=0x{:X} sourceClump=0x{:X}", vehicleId, binding.customModelId, binding.baseModelId, reinterpret_cast<std::uintptr_t>(vehicle), reinterpret_cast<std::uintptr_t>(model->m_pRwClump)));

			RpClump* newClump = CloneClumpPreservingOrder(model->m_pRwClump);
			if (newClump) {
				CVisibilityPlugins::SetupVehicleVariables(newClump);
				model->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
				model->SetEditableMaterials(newClump); // non-static: must call on model instance

				char plateText[32] = {};
				bool hasPlate = false;
				if (binding.hasCustomPlateText && binding.customPlateText[0] != '\0') {
					strncpy_s(plateText, sizeof(plateText), binding.customPlateText, _TRUNCATE);
					hasPlate = true;
				} else if (GetVehiclePlateText(vehicleId, plateText, sizeof(plateText)) && plateText[0] != '\0') {
					hasPlate = true;
				} else {
					auto defIt = s_modelDefaultPlateText.find(binding.customModelId);
					if (defIt != s_modelDefaultPlateText.end() && !defIt->second.empty()) {
						strncpy_s(plateText, sizeof(plateText), defIt->second.c_str(), _TRUNCATE);
						hasPlate = true;
					}
				}

				if (!hasPlate || plateText[0] == '\0') {
					strncpy_s(plateText, sizeof(plateText), "SAN ANDREAS", _TRUNCATE);
				}

				strncpy_s(binding.lastPlateText, sizeof(binding.lastPlateText), plateText, _TRUNCATE);
				if (binding.sampVehicleId > 0) {
					UpdateSampVehiclePlateText(binding.sampVehicleId, plateText);
				}
				ApplyPlateToClump(newClump, model, plateText, nullptr);

				if (binding.hasExtras) {
					ApplyExtrasToClump(newClump, binding.extrasMask);
				}

				short savedUpgrades[15];
				for (int i = 0; i < 15; ++i) {
					savedUpgrades[i] = vehicle->m_anUpgrades[i];
					vehicle->m_anUpgrades[i] = -1;
				}

				RwMatrix savedRwMatrix;
				bool hadMatrix = (vehicle->m_matrix != nullptr);
				if (hadMatrix) {
					vehicle->m_matrix->UpdateRW(&savedRwMatrix);
				} else {
					vehicle->m_placement.UpdateRwMatrix(&savedRwMatrix);
				}
				CSimpleTransform savedPlacement = vehicle->m_placement;

				CWorld::Remove(vehicle);

				ClientLog(LogLevel::Debug, std::format("Replacing RW object: vehicle={} oldRw=0x{:X} newRw=0x{:X} pos=({:.2f}, {:.2f}, {:.2f})", vehicleId, reinterpret_cast<std::uintptr_t>(vehicle->m_pRwObject), reinterpret_cast<std::uintptr_t>(newClump), savedRwMatrix.pos.x, savedRwMatrix.pos.y, savedRwMatrix.pos.z));

				vehicle->DeleteRwObject();
				if (!vehicle->m_pRwObject) {
					ClientLog(LogLevel::Debug, std::format("RW object deleted: vehicle={}", vehicleId));
				}

				RwFrame* rootFrame = RpClumpGetFrame(newClump);
				if (rootFrame) {
					std::memcpy(&rootFrame->modelling, &savedRwMatrix, sizeof(RwMatrix));
					RwFrameUpdateObjects(rootFrame);
				}

				// CRITICAL: Second argument must be FALSE (updateEntityMatrix = false).
				// If true, GTA:SA 0x533ED0 overwrites vehicle->m_matrix from newClump's
				// initial root frame matrix (which is 0, 0, 0). (0, 0, 0) is the Farm in Red County!
				// When overwritten, vehicle drops underground at the Farm.
				vehicle->AttachToRwObject(reinterpret_cast<RwObject*>(newClump), false);
				ClientLog(LogLevel::Debug, std::format("RW object attached: vehicle={} resultingRw=0x{:X}", vehicleId, reinterpret_cast<std::uintptr_t>(vehicle->m_pRwObject)));

				if (hadMatrix && vehicle->m_matrix) {
					((void(__thiscall*)(CMatrix*, RwMatrix*))0x59AD20)(vehicle->m_matrix, &savedRwMatrix);
				}
				vehicle->m_placement = savedPlacement;

				if (vehicle->m_pRwObject != reinterpret_cast<RwObject*>(newClump)) {
					ClientLog(LogLevel::Error, std::format("AttachToRwObject did not leave expected RW object on vehicle {}", vehicleId));
				} else {
					ClientLog(LogLevel::Info, std::format("SUCCESS: vehicle {} is visually bound to custom model {}", vehicleId, binding.customModelId));
				}

				if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
					auto* car = reinterpret_cast<CAutomobile*>(vehicle);
					car->SetupModelNodes();
					for (int p = 0; p < 3; ++p) {
						short frameId = car->m_panels[p].m_nFrameId;
						if (frameId >= 0 && (frameId >= CAR_NUM_NODES || !car->m_aCarNodes[frameId])) {
							car->m_panels[p].m_nFrameId = -1;
							car->m_panels[p].ResetPanel();
						}
					}
					if (!car->m_aCarNodes[CAR_CHASSIS]) {
						car->m_aCarNodes[CAR_CHASSIS] = CClumpModelInfo::GetFrameFromName(newClump, "chassis_dummy");
						if (!car->m_aCarNodes[CAR_CHASSIS]) {
							car->m_aCarNodes[CAR_CHASSIS] = CClumpModelInfo::GetFrameFromName(newClump, "chassis");
						}
					}
					if (car->m_aCarNodes[CAR_CHASSIS]) {
						binding.chassisBasePos = car->m_aCarNodes[CAR_CHASSIS]->modelling.pos;
						binding.hasChassisBasePos = true;
					}
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
					reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
					reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
				}

				for (int i = 0; i < 15; ++i) {
					if (savedUpgrades[i] >= 1000 && savedUpgrades[i] <= 1193) {
						vehicle->AddUpgrade(savedUpgrades[i], i);
					}
				}

				binding.popupFrames.clear();
				binding.hasPopupHeadlights = false;
				binding.popupHeadlightAngle = 0.0f;
				binding.lastHeadlightActiveTick = 0;

				const char* popupNodeNames[] = {
					"farolzr", "misc_a", "popupr", "popupl", "popup_l", "popup_r", "popup_light", "popup_light_l", "popup_light_r", "popup", "farol"
				};
				for (const char* nodeName : popupNodeNames) {
					RwFrame* frame = CClumpModelInfo::GetFrameFromName(newClump, nodeName);
					if (frame && std::find(binding.popupFrames.begin(), binding.popupFrames.end(), frame) == binding.popupFrames.end()) {
						binding.popupFrames.push_back(frame);
						binding.hasPopupHeadlights = true;
					}
				}
				if (binding.hasPopupHeadlights) {
					ClientLog(LogLevel::Info, std::format("Detected {} popup headlight frame(s) for vehicle {} (customModel={})", static_cast<unsigned int>(binding.popupFrames.size()), vehicleId, binding.customModelId));
				}

				if (binding.hasCustomWheel) {
					ApplyWheelToVehicle(vehicle, binding.customWheelModelId);
				} else if (model && model->m_nWheelModelIndex > 0) {
					bool hasAnyWheelUpgrade = false;
					for (int i = 0; i < 15; ++i) {
						if (savedUpgrades[i] == 1025 || (savedUpgrades[i] >= 1073 && savedUpgrades[i] <= 1098)) {
							hasAnyWheelUpgrade = true;
							break;
						}
					}
					if (!hasAnyWheelUpgrade) {
						ApplyWheelToVehicle(vehicle, model->m_nWheelModelIndex);
					}
				}

				CWorld::Add(vehicle);

				{
					std::unique_lock colLock(s_fastColMutex);
					if (model && model->m_pColModel && model->m_pColModel->m_pColData && model->m_pColModel->m_pColData->m_pLines && model->m_pColModel->m_pColData->m_nNumLines >= 4) {
						s_fastVehicleColMap[vehicle] = model->m_pColModel;
						s_hasCustomCols.store(true, std::memory_order_relaxed);
					}
				}

				binding.appliedGameVehicle = vehicle;
				binding.modelApplied = true;
				binding.lastPrimaryColor = vehicle->m_nPrimaryColor;
				binding.lastSecondaryColor = vehicle->m_nSecondaryColor;
				binding.lastTertiaryColor = vehicle->m_nTertiaryColor;
				binding.lastQuaternaryColor = vehicle->m_nQuaternaryColor;

				if (!binding.hasOffsets) {
					auto it = s_modelOffsets.find(binding.customModelId);
					if (it != s_modelOffsets.end()) {
						ApplyModelOffsetsToBinding(binding, it->second);
					}
				}

				if (binding.hasPaintjob) {
					ApplyPaintjobToVehicle(vehicle, binding.paintjobIndex);
				}
				if (binding.hasWindowTint) {
					ApplyWindowTintToVehicle(vehicle, binding.windowTintAlpha, binding.windowTintR, binding.windowTintG, binding.windowTintB);
				}
				if (binding.hasWheelColor) {
					ApplyWheelColorToVehicle(vehicle, binding.wheelColorR, binding.wheelColorG, binding.wheelColorB);
				}

				ApplyAudioSettingsToVehicle(vehicle);

				HandlingManager::OnVehicleStreamIn(vehicle, static_cast<uint16_t>(vehicleId));
			}
		} else {
			// Model is already applied - check if vehicle colors changed (e.g. ChangeVehicleColor or Respray)
			if (vehicle->m_nPrimaryColor != binding.lastPrimaryColor || vehicle->m_nSecondaryColor != binding.lastSecondaryColor || vehicle->m_nTertiaryColor != binding.lastTertiaryColor || vehicle->m_nQuaternaryColor != binding.lastQuaternaryColor) {
				binding.lastPrimaryColor = vehicle->m_nPrimaryColor;
				binding.lastSecondaryColor = vehicle->m_nSecondaryColor;
				binding.lastTertiaryColor = vehicle->m_nTertiaryColor;
				binding.lastQuaternaryColor = vehicle->m_nQuaternaryColor;

				RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
				if (clump && model) {
					model->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
					model->SetEditableMaterials(clump);
					if (binding.hasPaintjob) {
						ApplyPaintjobToVehicle(vehicle, binding.paintjobIndex);
					}
					if (binding.hasWindowTint) {
						ApplyWindowTintToVehicle(vehicle, binding.windowTintAlpha, binding.windowTintR, binding.windowTintG, binding.windowTintB);
					}
					if (binding.hasWheelColor) {
						ApplyWheelColorToVehicle(vehicle, binding.wheelColorR, binding.wheelColorG, binding.wheelColorB);
					}
					if (binding.lastPlateText[0] != '\0') {
						ApplyPlateToVehicle(vehicle, binding.lastPlateText);
					}
				}
			}

			// Check if license plate text changed in real-time
			char currentEffectivePlate[32] = {};
			bool hasPlate = false;
			if (binding.hasCustomPlateText && binding.customPlateText[0] != '\0') {
				strncpy_s(currentEffectivePlate, sizeof(currentEffectivePlate), binding.customPlateText, _TRUNCATE);
				hasPlate = true;
			} else if (GetVehiclePlateText(vehicleId, currentEffectivePlate, sizeof(currentEffectivePlate)) && currentEffectivePlate[0] != '\0') {
				hasPlate = true;
			} else {
				auto defIt = s_modelDefaultPlateText.find(binding.customModelId);
				if (defIt != s_modelDefaultPlateText.end() && !defIt->second.empty()) {
					strncpy_s(currentEffectivePlate, sizeof(currentEffectivePlate), defIt->second.c_str(), _TRUNCATE);
					hasPlate = true;
				}
			}

			if (hasPlate && std::strncmp(binding.lastPlateText, currentEffectivePlate, sizeof(binding.lastPlateText)) != 0) {
				ApplyPlateToVehicle(vehicle, currentEffectivePlate);
			}
		}
	}
}

void CustomVehicleBindingManager::SetVehiclePaintjob(uint16_t vehicleId, int paintjobIndex)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasPaintjob = true;
		it->second.paintjobIndex = paintjobIndex;
		if (it->second.modelApplied && it->second.appliedGameVehicle && IsVehiclePointerValid(it->second.appliedGameVehicle)) {
			ApplyPaintjobToVehicle(it->second.appliedGameVehicle, paintjobIndex);
		}
	}
}

void CustomVehicleBindingManager::SetVehicleNeon(uint16_t vehicleId, bool enabled, uint8_t r, uint8_t g, uint8_t b, float size)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasNeon = true;
		it->second.neonEnabled = enabled;
		it->second.neonR = r;
		it->second.neonG = g;
		it->second.neonB = b;
		it->second.neonSize = size;
	}
}

void CustomVehicleBindingManager::SetVehicleWindowTint(uint16_t vehicleId, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasWindowTint = (alpha < 255 || r > 0 || g > 0 || b > 0);
		it->second.windowTintAlpha = alpha;
		it->second.windowTintR = r;
		it->second.windowTintG = g;
		it->second.windowTintB = b;
		if (it->second.modelApplied && it->second.appliedGameVehicle && IsVehiclePointerValid(it->second.appliedGameVehicle)) {
			ApplyWindowTintToVehicle(it->second.appliedGameVehicle, alpha, r, g, b);
		}
	}
}

void CustomVehicleBindingManager::SetVehicleWheelColor(uint16_t vehicleId, uint8_t r, uint8_t g, uint8_t b)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasWheelColor = (r != 255 || g != 255 || b != 255);
		it->second.wheelColorR = r;
		it->second.wheelColorG = g;
		it->second.wheelColorB = b;
		if (it->second.modelApplied && it->second.appliedGameVehicle && IsVehiclePointerValid(it->second.appliedGameVehicle)) {
			ApplyWheelColorToVehicle(it->second.appliedGameVehicle, r, g, b);
		}
	}
}

void CustomVehicleBindingManager::SetVehicleBackfire(uint16_t vehicleId, bool enabled)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasBackfire = true;
		it->second.backfireEnabled = enabled;
	}
}

void CustomVehicleBindingManager::SetVehicleHorn(uint16_t vehicleId, int8_t hornSoundId, float hornPitch)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasCustomHorn = true;
		it->second.hornSoundId = hornSoundId;
		it->second.hornPitch = hornPitch;
		if (it->second.appliedGameVehicle) {
			ApplyAudioSettingsToVehicle(it->second.appliedGameVehicle);
		}
	}
}

void CustomVehicleBindingManager::SetVehicleSiren(uint16_t vehicleId, bool enabled, int8_t sirenType)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasCustomSiren = true;
		it->second.sirenEnabled = enabled;
		it->second.sirenType = sirenType;
		if (it->second.appliedGameVehicle) {
			ApplyAudioSettingsToVehicle(it->second.appliedGameVehicle);
		}
	}
}

void CustomVehicleBindingManager::SetVehicleLights(uint16_t vehicleId, int8_t lightingCategory, float scaleMult)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasCustomLighting = true;
		it->second.customLightingCategory = lightingCategory;
		it->second.customLightScaleMult = (scaleMult > 0.05f) ? scaleMult : 1.0f;
	}
}

void CustomVehicleBindingManager::SetVehicleWheel(uint16_t vehicleId, int16_t wheelModelId)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it == m_bindings.end()) {
		Binding b {};
		b.sampVehicleId = vehicleId;
		b.hasCustomWheel = true;
		b.customWheelModelId = wheelModelId;
		m_bindings[vehicleId] = b;
		return;
	}

	it->second.hasCustomWheel = true;
	it->second.customWheelModelId = wheelModelId;

	if (it->second.appliedGameVehicle) {
		ApplyWheelToVehicle(it->second.appliedGameVehicle, wheelModelId);
	}
}

void CustomVehicleBindingManager::ApplyWheelToVehicle(CVehicle* vehicle, int16_t wheelModelId)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle))
		return;

	if (wheelModelId <= 0) {
		for (int i = 0; i < 15; ++i) {
			short upg = vehicle->m_anUpgrades[i];
			if (upg == 1025 || (upg >= 1073 && upg <= 1098)) {
				vehicle->RemoveUpgrade(upg);
				break;
			}
		}
		return;
	}

	if (wheelModelId >= 1000 && wheelModelId <= 1193) {
		if (!CStreaming::HasModelLoaded(wheelModelId)) {
			CStreaming::RequestModel(wheelModelId, 0x16); // GAME_REQUIRED
			CStreaming::LoadAllRequestedModels(false);
		}

		if (CStreaming::HasModelLoaded(wheelModelId)) {
			vehicle->AddUpgrade(wheelModelId, -1);
			auto* binding = FindByVehicle(vehicle);
			if (binding && binding->hasWheelColor) {
				ApplyWheelColorToVehicle(vehicle, binding->wheelColorR, binding->wheelColorG, binding->wheelColorB);
			}
		}
	}
}

void CustomVehicleBindingManager::ApplyAudioSettingsToVehicle(CVehicle* vehicle)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle))
		return;

	auto* binding = FindByVehicle(vehicle);
	if (!binding)
		return;

	CAEVehicleAudioEntity* pAudio = AudioExtender::GetVehicleAudioEntity(vehicle);
	if (!pAudio)
		return;

	auto audioDef = AudioExtender::GetVehicleAudio(binding->customModelId);
	if (audioDef) {
		AudioExtender::ApplyCustomVehicleAudio(*vehicle, *audioDef);
	}

	if (binding->hasCustomHorn) {
		pAudio->m_settings.m_bHornTon = static_cast<char>(binding->hornSoundId);
		pAudio->m_settings.m_fHornHigh = binding->hornPitch;
	} else if (audioDef && audioDef->hornSoundId >= 0) {
		pAudio->m_settings.m_bHornTon = static_cast<char>(audioDef->hornSoundId);
		pAudio->m_settings.m_fHornHigh = audioDef->hornPitch;
	}

	if (binding->hasCustomSiren) {
		pAudio->m_bModelWithSiren = (binding->sirenType != 0);
		vehicle->bSirenOrAlarm = binding->sirenEnabled;
	} else if (audioDef && audioDef->sirenType > 0) {
		pAudio->m_bModelWithSiren = true;
	}
}

void CustomVehicleBindingManager::SetModelDefaultPlateText(uint32_t customModelId, const std::string& plateText)
{
	std::lock_guard lock(m_mutex);
	s_modelDefaultPlateText[customModelId] = plateText;
}

std::string CustomVehicleBindingManager::GetModelDefaultPlateText(uint32_t customModelId)
{
	std::lock_guard lock(m_mutex);
	auto it = s_modelDefaultPlateText.find(customModelId);
	return (it != s_modelDefaultPlateText.end()) ? it->second : "";
}

void CustomVehicleBindingManager::SetVehiclePlateText(uint16_t vehicleId, const char* text)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		if (text && text[0] != '\0') {
			it->second.hasCustomPlateText = true;
			strncpy_s(it->second.customPlateText, sizeof(it->second.customPlateText), text, _TRUNCATE);
		} else {
			it->second.hasCustomPlateText = false;
			it->second.customPlateText[0] = '\0';
		}

		if (it->second.modelApplied && it->second.appliedGameVehicle && IsVehiclePointerValid(it->second.appliedGameVehicle)) {
			ApplyPlateToVehicle(it->second.appliedGameVehicle, it->second.hasCustomPlateText ? it->second.customPlateText : nullptr);
		}
	}
}

std::vector<CustomVehicleBindingManager::PlateMaterialInfo> CustomVehicleBindingManager::FindVehiclePlateMaterials(
	RpClump* clump,
	CVehicleModelInfo* customModel,
	const char* lastKnownPlateText)
{
	std::vector<PlateMaterialInfo> results;
	if (!clump)
		return results;

	auto containsCi = [](const std::string& haystack, std::string_view needle) -> bool {
		auto it = std::search(
			haystack.begin(), haystack.end(),
			needle.begin(), needle.end(),
			[](char ch1, char ch2) { return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2)); });
		return it != haystack.end();
	};

	auto equalsCi = [](const char* s1, const char* s2) -> bool {
		if (!s1 || !s2) return false;
		return _stricmp(s1, s2) == 0;
	};

	int targetAtomicIdx = -1;
	int targetMatIdx = -1;
	if (customModel && customModel->m_pPlateMaterial && customModel->m_pRwClump) {
		struct SearchCtx {
			int currentAIdx { 0 };
			int foundAIdx { -1 };
			int foundMIdx { -1 };
			RpMaterial* targetMat { nullptr };
		} searchCtx;
		searchCtx.targetMat = customModel->m_pPlateMaterial;

		RpClumpForAllAtomics(customModel->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
			auto* sc = reinterpret_cast<SearchCtx*>(data);
			RpGeometry* geom = RpAtomicGetGeometry(atomic);
			if (geom && geom->matList.materials) {
				for (int m = 0; m < geom->matList.numMaterials; ++m) {
					if (geom->matList.materials[m] == sc->targetMat) {
						sc->foundAIdx = sc->currentAIdx;
						sc->foundMIdx = m;
						return atomic;
					}
				}
			}
			sc->currentAIdx++;
			return atomic;
		}, &searchCtx);

		targetAtomicIdx = searchCtx.foundAIdx;
		targetMatIdx = searchCtx.foundMIdx;
	}

	struct ClumpScanContext {
		int currentAtomicIdx { 0 };
		int targetAtomicIdx { -1 };
		int targetMatIdx { -1 };
		const char* lastPlate { nullptr };
		const char* modelPlate { nullptr };
		std::vector<PlateMaterialInfo>* pResults { nullptr };
		decltype(containsCi)* pContainsCi { nullptr };
		decltype(equalsCi)* pEqualsCi { nullptr };
	} ctx;

	ctx.targetAtomicIdx = targetAtomicIdx;
	ctx.targetMatIdx = targetMatIdx;
	ctx.lastPlate = lastKnownPlateText;
	ctx.modelPlate = (customModel && customModel->m_szPlateText[0] != '\0') ? customModel->m_szPlateText : nullptr;
	ctx.pResults = &results;
	ctx.pContainsCi = &containsCi;
	ctx.pEqualsCi = &equalsCi;

	RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
		auto* c = reinterpret_cast<ClumpScanContext*>(data);
		RpGeometry* geom = RpAtomicGetGeometry(atomic);
		if (!geom || !geom->matList.materials) {
			c->currentAtomicIdx++;
			return atomic;
		}

		RwFrame* frame = RpAtomicGetFrame(atomic);
		std::string frameHierarchy;
		for (RwFrame* f = frame; f != nullptr; f = RwFrameGetParent(f)) {
			const char* n = GetFrameNodeName(f);
			if (n && *n) {
				frameHierarchy += " ";
				frameHierarchy += n;
			}
		}

		for (int m = 0; m < geom->matList.numMaterials; ++m) {
			RpMaterial* mat = geom->matList.materials[m];
			if (!mat)
				continue;

			bool isPlate = false;
			bool isBg = false;

			if (c->targetAtomicIdx >= 0 && c->currentAtomicIdx == c->targetAtomicIdx && m == c->targetMatIdx) {
				isPlate = true;
			}

			if (mat->texture) {
				const char* texName = mat->texture->name;
				if (texName && *texName) {
					std::string tn = texName;
					if ((*c->pEqualsCi)(texName, "carplate")) {
						isPlate = true;
						isBg = false;
					} else if ((*c->pEqualsCi)(texName, "carpback") || (*c->pContainsCi)(tn, "plateback")) {
						isPlate = true;
						isBg = true;
					} else if ((*c->pContainsCi)(tn, "carplate") || (*c->pContainsCi)(tn, "numberplate") ||
							   (*c->pContainsCi)(tn, "license") || (*c->pContainsCi)(tn, "licence") ||
							   (*c->pContainsCi)(tn, "nomer") || (*c->pContainsCi)(tn, "plate")) {
						isPlate = true;
						isBg = false;
					} else if (c->lastPlate && c->lastPlate[0] != '\0' && (*c->pEqualsCi)(texName, c->lastPlate)) {
						isPlate = true;
						isBg = false;
					} else if (c->modelPlate && (*c->pEqualsCi)(texName, c->modelPlate)) {
						isPlate = true;
						isBg = false;
					}
				}
			}

			if (!isPlate && !frameHierarchy.empty()) {
				if ((*c->pContainsCi)(frameHierarchy, "plate") || (*c->pContainsCi)(frameHierarchy, "nomer") || (*c->pContainsCi)(frameHierarchy, "license")) {
					isPlate = true;
					isBg = false;
				}
			}

			if (isPlate) {
				bool alreadyAdded = false;
				for (const auto& existing : *c->pResults) {
					if (existing.material == mat) {
						alreadyAdded = true;
						break;
					}
				}
				if (!alreadyAdded) {
					c->pResults->push_back({ mat, isBg });
				}
			}
		}

		c->currentAtomicIdx++;
		return atomic;
	}, &ctx);

	return results;
}

bool CustomVehicleBindingManager::ApplyPlateToClump(
	RpClump* clump,
	CVehicleModelInfo* customModel,
	const char* plateText,
	const char* lastKnownText)
{
	if (!clump || !plateText || plateText[0] == '\0')
		return false;

	auto plateMaterials = FindVehiclePlateMaterials(clump, customModel, lastKnownText);

	int textPlatesApplied = 0;
	int bgPlatesApplied = 0;

	for (auto& entry : plateMaterials) {
		if (entry.isBackground) {
			CCustomCarPlateMgr::SetupMaterialPlatebackTexture(entry.material, 0);
			bgPlatesApplied++;
		} else {
			CCustomCarPlateMgr::SetupMaterialPlateTexture(entry.material, const_cast<char*>(plateText), 0);
			textPlatesApplied++;
		}
	}

	if (textPlatesApplied > 0) {
		ClientLog(LogLevel::Info, std::format("ApplyPlateToClump: Applied '{}' to {} plate text mat(s) and {} background mat(s)", plateText, textPlatesApplied, bgPlatesApplied));
		return true;
	}

	RpMaterial* fallbackMat = CCustomCarPlateMgr::SetupClump(clump, const_cast<char*>(plateText), 0);
	if (fallbackMat) {
		ClientLog(LogLevel::Info, std::format("ApplyPlateToClump: Fallback SetupClump succeeded for '{}' (mat=0x{:X})", plateText, reinterpret_cast<std::uintptr_t>(fallbackMat)));
		return true;
	}

	ClientLog(LogLevel::Warning, std::format("ApplyPlateToClump: Failed to find any plate material for text '{}'", plateText));
	return false;
}

void CustomVehicleBindingManager::ApplyPlateToVehicle(CVehicle* vehicle, const char* text)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle))
		return;

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	Binding* binding = FindByVehicle(vehicle);
	char finalPlate[32] = {};
	bool hasPlate = false;

	if (text && text[0] != '\0') {
		strncpy_s(finalPlate, sizeof(finalPlate), text, _TRUNCATE);
		hasPlate = true;
	} else if (binding) {
		if (binding->hasCustomPlateText && binding->customPlateText[0] != '\0') {
			strncpy_s(finalPlate, sizeof(finalPlate), binding->customPlateText, _TRUNCATE);
			hasPlate = true;
		} else {
			char sampPlate[32] = {};
			if (GetVehiclePlateText(binding->sampVehicleId, sampPlate, sizeof(sampPlate)) && sampPlate[0] != '\0') {
				strncpy_s(finalPlate, sizeof(finalPlate), sampPlate, _TRUNCATE);
				hasPlate = true;
			} else {
				std::lock_guard lock(m_mutex);
				auto defIt = s_modelDefaultPlateText.find(binding->customModelId);
				if (defIt != s_modelDefaultPlateText.end() && !defIt->second.empty()) {
					strncpy_s(finalPlate, sizeof(finalPlate), defIt->second.c_str(), _TRUNCATE);
					hasPlate = true;
				}
			}
		}
	}

	if (!hasPlate || finalPlate[0] == '\0') {
		strncpy_s(finalPlate, sizeof(finalPlate), "SAN ANDREAS", _TRUNCATE);
		hasPlate = true;
	}

	CVehicleModelInfo* customModel = nullptr;
	const char* lastKnownText = nullptr;
	uint16_t sampVehicleId = 0;
	if (binding) {
		customModel = StreamingExtender::GetCustomModel(binding->customModelId);
		lastKnownText = binding->lastPlateText;
		sampVehicleId = binding->sampVehicleId;
		strncpy_s(binding->lastPlateText, sizeof(binding->lastPlateText), finalPlate, _TRUNCATE);
	}

	if (sampVehicleId > 0) {
		UpdateSampVehiclePlateText(sampVehicleId, finalPlate);
	}

	ApplyPlateToClump(clump, customModel, finalPlate, lastKnownText);
}

void CustomVehicleBindingManager::ApplyPaintjobToVehicle(CVehicle* vehicle, int paintjobIndex)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle) || !vehicle->m_pRwObject)
		return;

	auto* binding = FindByVehicle(vehicle);
	if (!binding)
		return;

	auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
	if (!customModel) {
		ClientLog(LogLevel::Debug, std::format("WAIT: customModel={} has no StreamingExtender model.", binding->customModelId));
		return;
	}

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	if (paintjobIndex < 0) {
		CVehicleModelInfo::ms_pRemapTexture = nullptr;
		customModel->SetEditableMaterials(clump); // non-static: call on model instance
		customModel->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
		customModel->SetEditableMaterials(clump);
		return;
	}

	RwTexture* liveryTex = nullptr;

	// 1) Search in custom model's own TXD dictionary
	if (customModel->m_nTxdIndex != -1) {
		CTxdStore::PushCurrentTxd();
		CTxdStore::SetCurrentTxd(customModel->m_nTxdIndex);
		RwTexDictionary* pDict = RwTexDictionaryGetCurrent();
		if (pDict) {
			int idx1 = paintjobIndex + 1;
			int idx0 = paintjobIndex;

			std::vector<std::string> searchNames = {
				std::format("remap{}", idx1),
				std::format("remap_{}", idx1),
				std::format("remap{}", idx0),
				std::format("remap_{}", idx0),
				std::format("paintjob{}", idx1),
				std::format("paintjob_{}", idx1),
				std::format("paintjob{}", idx0),
				std::format("paintjob_{}", idx0),
				std::format("livery{}", idx1),
				std::format("livery_{}", idx1),
				std::format("livery{}", idx0),
				std::format("livery_{}", idx0),
				std::format("pj{}", idx1),
				std::format("pj_{}", idx1),
				std::format("pj{}", idx0),
				std::format("pj_{}", idx0)
			};
			if (paintjobIndex == 0) {
				searchNames.push_back("remap");
				searchNames.push_back("paintjob");
				searchNames.push_back("livery");
			}

			for (const auto& name : searchNames) {
				liveryTex = RwTexDictionaryFindNamedTexture(pDict, name.c_str());
				if (liveryTex)
					break;
			}
		}
		CTxdStore::PopCurrentTxd();
	}

	// 2) If not found in custom model TXD, check customModel->m_anRemapTxds[paintjobIndex]
	if (!liveryTex && paintjobIndex >= 0 && paintjobIndex < 4) {
		short remapTxd = customModel->m_anRemapTxds[paintjobIndex];
		if (remapTxd != -1) {
			CTxdStore::PushCurrentTxd();
			CTxdStore::SetCurrentTxd(remapTxd);
			RwTexDictionary* pDict = RwTexDictionaryGetCurrent();
			if (pDict) {
				liveryTex = RwTexDictionaryFindNamedTexture(pDict, "vehiclegrunge256");
				if (!liveryTex) {
					RwTexDictionaryForAllTextures(pDict, [](RwTexture* tex, void* data) -> RwTexture* {
						*reinterpret_cast<RwTexture**>(data) = tex;
						return nullptr;
					},
						&liveryTex);
				}
			}
			CTxdStore::PopCurrentTxd();
		}
	}

	if (liveryTex) {
		CVehicleModelInfo::ms_pRemapTexture = liveryTex;
		customModel->SetEditableMaterials(clump); // non-static: call on model instance

		struct PaintjobContext {
			RwTexture* tex;
		} ctx { liveryTex };

		RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
			auto* c = reinterpret_cast<PaintjobContext*>(data);
			RpGeometry* geom = RpAtomicGetGeometry(atomic);
			if (geom) {
				RpGeometryForAllMaterials(geom, [](RpMaterial* mat, void* data) -> RpMaterial* {
					auto* c = reinterpret_cast<PaintjobContext*>(data);
					RwTexture* curTex = RpMaterialGetTexture(mat);
					if (curTex) {
						const char* texName = RwTextureGetName(curTex);
						if (texName) {
							std::string nameLower = texName;
							std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
							if (nameLower.find("remap") != std::string::npos || nameLower.find("paintjob") != std::string::npos || nameLower.find("livery") != std::string::npos) {
								RpMaterialSetTexture(mat, c->tex);
							}
						}
					}
					return mat;
				},
					c);
			}
			return atomic;
		},
			&ctx);

		customModel->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
		customModel->SetEditableMaterials(clump); // non-static: call on model instance
	}
}

void CustomVehicleBindingManager::ApplyWindowTintToVehicle(CVehicle* vehicle, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle) || !vehicle->m_pRwObject)
		return;

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	struct TintContext {
		uint8_t alpha;
		uint8_t r;
		uint8_t g;
		uint8_t b;
	} ctx { alpha, r, g, b };

	RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
		auto* c = reinterpret_cast<TintContext*>(data);
		RpGeometry* geom = RpAtomicGetGeometry(atomic);
		if (!geom)
			return atomic;

		RpGeometryForAllMaterials(geom, [](RpMaterial* mat, void* data) -> RpMaterial* {
			auto* c = reinterpret_cast<TintContext*>(data);
			RwTexture* curTex = RpMaterialGetTexture(mat);
			bool isWindow = false;
			if (curTex) {
				const char* texName = RwTextureGetName(curTex);
				if (texName) {
					std::string nameLower = texName;
					std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
					if (nameLower.find("light") != std::string::npos || nameLower.find("lamp") != std::string::npos || nameLower.find("shad") != std::string::npos || nameLower.find("wheel") != std::string::npos || nameLower.find("tyre") != std::string::npos || nameLower.find("tire") != std::string::npos || nameLower.find("badge") != std::string::npos || nameLower.find("logo") != std::string::npos) {
						return mat;
					}
					if (nameLower.find("glass") != std::string::npos || nameLower.find("window") != std::string::npos || nameLower.find("windscreen") != std::string::npos) {
						isWindow = true;
					}
				}
			}

			const RwRGBA* curCol = RpMaterialGetColor(mat);
			if (!isWindow && curCol && curCol->alpha < 240) {
				isWindow = true;
			}

			if (isWindow) {
				RwRGBA newCol;
				newCol.red = c->r;
				newCol.green = c->g;
				newCol.blue = c->b;
				newCol.alpha = c->alpha;
				RpMaterialSetColor(mat, &newCol);
			}
			return mat;
		},
			c);

		return atomic;
	},
		&ctx);
}

void CustomVehicleBindingManager::ApplyWheelColorToVehicle(CVehicle* vehicle, uint8_t r, uint8_t g, uint8_t b)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle) || !vehicle->m_pRwObject)
		return;

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	static const char* s_wheelFrames[] = {
		"wheel_rf_dummy", "wheel_rm_dummy", "wheel_rb_dummy",
		"wheel_lf_dummy", "wheel_lm_dummy", "wheel_lb_dummy",
		"wheel_rf", "wheel_rm", "wheel_rb",
		"wheel_lf", "wheel_lm", "wheel_lb",
		"wheel_front", "wheel_rear"
	};

	std::vector<RwFrame*> targetFrames;
	for (const char* name : s_wheelFrames) {
		RwFrame* f = CClumpModelInfo::GetFrameFromName(clump, name);
		if (f)
			targetFrames.push_back(f);
	}

	if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
		auto* car = reinterpret_cast<CAutomobile*>(vehicle);
		for (int i = CAR_WHEEL_RF; i <= CAR_WHEEL_LB; ++i) {
			if (car->m_aCarNodes[i]) {
				if (std::find(targetFrames.begin(), targetFrames.end(), car->m_aCarNodes[i]) == targetFrames.end()) {
					targetFrames.push_back(car->m_aCarNodes[i]);
				}
			}
		}
	}

	if (targetFrames.empty())
		return;

	struct WheelColorContext {
		const std::vector<RwFrame*>* frames;
		uint8_t r;
		uint8_t g;
		uint8_t b;
	} ctx { &targetFrames, r, g, b };

	RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
		auto* c = reinterpret_cast<WheelColorContext*>(data);
		RwFrame* frame = RpAtomicGetFrame(atomic);
		bool isWheel = false;
		for (RwFrame* tf : *c->frames) {
			if (IsFrameOrChildOf(frame, tf)) {
				isWheel = true;
				break;
			}
		}

		if (!isWheel)
			return atomic;

		RpGeometry* geom = RpAtomicGetGeometry(atomic);
		if (!geom)
			return atomic;

		RpGeometryForAllMaterials(geom, [](RpMaterial* mat, void* data) -> RpMaterial* {
			auto* c = reinterpret_cast<WheelColorContext*>(data);
			RwTexture* curTex = RpMaterialGetTexture(mat);
			if (curTex) {
				const char* texName = RwTextureGetName(curTex);
				if (texName) {
					std::string nameLower = texName;
					std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
					if (nameLower.find("tyre") != std::string::npos || nameLower.find("tire") != std::string::npos || nameLower.find("rubber") != std::string::npos || nameLower.find("disc") != std::string::npos || nameLower.find("rotor") != std::string::npos || nameLower.find("brake") != std::string::npos || nameLower.find("caliper") != std::string::npos) {
						return mat;
					}
				}
			}

			RwRGBA newCol;
			newCol.red = c->r;
			newCol.green = c->g;
			newCol.blue = c->b;
			newCol.alpha = 255;
			RpMaterialSetColor(mat, &newCol);
			return mat;
		},
			c);

		return atomic;
	},
		&ctx);
}

void CustomVehicleBindingManager::ApplyModelOffsetsToBinding(Binding& binding, const ModelOffsetConfig& cfg)
{
	binding.frontWheelOffsetZ = cfg.frontWheelOffsetZ;
	binding.rearWheelOffsetZ = cfg.rearWheelOffsetZ;
	binding.frontWheelOffsetY = cfg.frontWheelOffsetY;
	binding.rearWheelOffsetY = cfg.rearWheelOffsetY;
	binding.chassisOffsetX = cfg.chassisOffsetX;
	binding.chassisOffsetY = cfg.chassisOffsetY;
	binding.chassisOffsetZ = cfg.chassisOffsetZ;
	binding.hasOffsets = true;

	if (cfg.frontTrackWidth != 0.0f || cfg.rearTrackWidth != 0.0f || cfg.frontCamber != 0.0f || cfg.rearCamber != 0.0f || (cfg.frontWheelScale != 1.0f && cfg.frontWheelScale > 0.01f) || (cfg.rearWheelScale != 1.0f && cfg.rearWheelScale > 0.01f)) {
		binding.hasStance = true;
		binding.frontTrackWidth = cfg.frontTrackWidth;
		binding.rearTrackWidth = cfg.rearTrackWidth;
		if (cfg.frontWheelScale > 0.01f) binding.frontWheelScale = cfg.frontWheelScale;
		if (cfg.rearWheelScale > 0.01f) binding.rearWheelScale = cfg.rearWheelScale;
		binding.frontCamber = cfg.frontCamber;
		binding.rearCamber = cfg.rearCamber;
	}
}

bool CustomVehicleBindingManager::GetModelOffsets(uint32_t customModelId, ModelOffsetConfig& outCfg)
{
	std::lock_guard lock(m_mutex);
	auto it = s_modelOffsets.find(customModelId);
	if (it != s_modelOffsets.end()) {
		outCfg = it->second;
		return true;
	}
	return false;
}

void CustomVehicleBindingManager::SetModelOffsets(uint32_t customModelId, const ModelOffsetConfig& cfg)
{
	std::lock_guard lock(m_mutex);
	s_serverModelOffsets[customModelId] = cfg;
	s_modelOffsets[customModelId] = cfg;
	for (auto& [vehicleId, binding] : m_bindings) {
		if (binding.customModelId == customModelId) {
			ApplyModelOffsetsToBinding(binding, cfg);
		}
	}
}

bool CustomVehicleBindingManager::HandleChatCommand(const std::string& fullCmd)
{
	if (fullCmd.empty())
		return false;

	std::string cleanCmd = fullCmd;
	if (!cleanCmd.empty() && cleanCmd[0] == '/')
		cleanCmd = cleanCmd.substr(1);

	std::istringstream iss(cleanCmd);
	std::string cmd;
	iss >> cmd;
	std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

	static const std::unordered_set<std::string> s_knownCommands = {
		"$vehwheelz", "$vehwheely", "$vehtrack", "$vehcamber", "$vehwheelscale",
		"$vehchassisz", "$vehchassis", "$vehoffsets", "$vehinfo",
		"$vehresetspec", "$vehreset", "$vehhelp", "$customvehhelp", "$vehplate"
	};

	if (s_knownCommands.find(cmd) == s_knownCommands.end())
		return false;

	if (!IsClientDebugMode())
		return false;

	if (cmd == "$customvehhelp" || cmd == "$vehhelp") {
		SendMsg(0xFFFFFF, "{FFFF00}$vehwheelz [model] <Z> [rearZ] {FFFFFF}- Live preview wheel height offset");
		SendMsg(0xFFFFFF, "{FFFF00}$vehwheely [model] <Y> [rearY] {FFFFFF}- Live preview wheel longitudinal offset");
		SendMsg(0xFFFFFF, "{FFFF00}$vehchassisz [model] <Z> {FFFFFF}- Live preview chassis body height");
		SendMsg(0xFFFFFF, "{FFFF00}$vehchassis [model] <X> <Y> <Z> {FFFFFF}- Live preview chassis 3D position");
		SendMsg(0xFFFFFF, "{FFFF00}$vehtrack [model] <front> [rear] {FFFFFF}- Live preview wheel track width");
		SendMsg(0xFFFFFF, "{FFFF00}$vehcamber [model] <front> [rear] {FFFFFF}- Live preview wheel camber");
		SendMsg(0xFFFFFF, "{FFFF00}$vehwheelscale [model] <front> [rear] {FFFFFF}- Live preview wheel scale");
		SendMsg(0xFFFFFF, "{FFFF00}$vehplate [model] <text> {FFFFFF}- Live preview license plate text");
		SendMsg(0xFFFFFF, "{FFFF00}$vehoffsets {FFFFFF}- Display current offsets for vehicle/model");
		SendMsg(0xFFFFFF, "{FFFF00}$vehreset [model] {FFFFFF}- Reset live preview to server defaults");
		SendMsg(0xAAAAAA, "{AAAAAA}Note: Permanent offsets are configured on the server in model.ini [offsets].");
		return true;
	}

	std::vector<std::string> args;
	std::string arg;
	while (iss >> arg) {
		args.push_back(arg);
	}

	uint32_t targetModelId = 0;
	CVehicle* playerVeh = nullptr;
	Binding* pBinding = nullptr;

	auto* localPed = FindPlayerPed();
	if (localPed && localPed->m_pVehicle && IsVehiclePointerValid(localPed->m_pVehicle)) {
		playerVeh = localPed->m_pVehicle;
		pBinding = FindByVehicle(playerVeh);
	}

	size_t argIdx = 0;
	if (argIdx < args.size()) {
		try {
			unsigned long val = std::stoul(args[argIdx]);
			if (val >= 400 && val <= 65535 && args.size() > 1) {
				targetModelId = static_cast<uint32_t>(val);
				argIdx++;
			}
		} catch (...) {}
	}

	if (targetModelId == 0) {
		if (pBinding) {
			targetModelId = pBinding->customModelId;
		} else {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} You must be inside a custom vehicle or specify model ID (e.g. $vehwheelz 20005 0.35).");
			return true;
		}
	}

	if (cmd == "$vehoffsets" || cmd == "$vehinfo") {
		std::lock_guard lock(m_mutex);
		auto it = s_modelOffsets.find(targetModelId);
		ModelOffsetConfig cfg;
		if (it != s_modelOffsets.end()) {
			cfg = it->second;
		} else if (pBinding && pBinding->customModelId == targetModelId) {
			cfg.frontWheelOffsetZ = pBinding->frontWheelOffsetZ;
			cfg.rearWheelOffsetZ = pBinding->rearWheelOffsetZ;
			cfg.frontWheelOffsetY = pBinding->frontWheelOffsetY;
			cfg.rearWheelOffsetY = pBinding->rearWheelOffsetY;
			cfg.chassisOffsetX = pBinding->chassisOffsetX;
			cfg.chassisOffsetY = pBinding->chassisOffsetY;
			cfg.chassisOffsetZ = pBinding->chassisOffsetZ;
			cfg.frontTrackWidth = pBinding->frontTrackWidth;
			cfg.rearTrackWidth = pBinding->rearTrackWidth;
			cfg.frontWheelScale = pBinding->frontWheelScale;
			cfg.rearWheelScale = pBinding->rearWheelScale;
			cfg.frontCamber = pBinding->frontCamber;
			cfg.rearCamber = pBinding->rearCamber;
		}

		SendMsg(0x00D0FF, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Offsets for Model {}{}:", targetModelId, pBinding ? std::format(" (Vehicle {})", pBinding->sampVehicleId) : "").c_str());
		SendMsg(0xFFFFFF, std::format("  Wheel Z: Front={:.3f}, Rear={:.3f} | Wheel Y: Front={:.3f}, Rear={:.3f}", cfg.frontWheelOffsetZ, cfg.rearWheelOffsetZ, cfg.frontWheelOffsetY, cfg.rearWheelOffsetY).c_str());
		SendMsg(0xFFFFFF, std::format("  Chassis: X={:.3f}, Y={:.3f}, Z={:.3f}", cfg.chassisOffsetX, cfg.chassisOffsetY, cfg.chassisOffsetZ).c_str());
		SendMsg(0xFFFFFF, std::format("  Stance: Track=[{:.3f}, {:.3f}], Camber=[{:.3f}, {:.3f}], Scale=[{:.3f}, {:.3f}]", cfg.frontTrackWidth, cfg.rearTrackWidth, cfg.frontCamber, cfg.rearCamber, cfg.frontWheelScale, cfg.rearWheelScale).c_str());
		return true;
	}

	if (cmd == "$vehreset" || cmd == "$vehresetspec") {
		std::lock_guard lock(m_mutex);
		ModelOffsetConfig restoredCfg;
		auto sIt = s_serverModelOffsets.find(targetModelId);
		if (sIt != s_serverModelOffsets.end()) {
			restoredCfg = sIt->second;
		}
		s_modelOffsets[targetModelId] = restoredCfg;

		for (auto& [vId, b] : m_bindings) {
			if (b.customModelId == targetModelId) {
				ApplyModelOffsetsToBinding(b, restoredCfg);
			}
		}

		SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} live preview reset to server defaults.", targetModelId).c_str());
		return true;
	}

	if (cmd == "$vehwheelz") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehwheelz [modelId] <offsetZ> [rearOffsetZ]");
			return true;
		}
		try {
			float frontZ = std::stof(args[argIdx]);
			float rearZ = (argIdx + 1 < args.size()) ? std::stof(args[argIdx + 1]) : frontZ;

			std::lock_guard lock(m_mutex);
			auto& cfg = s_modelOffsets[targetModelId];
			cfg.hasConfig = true;
			cfg.frontWheelOffsetZ = frontZ;
			cfg.rearWheelOffsetZ = rearZ;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					ApplyModelOffsetsToBinding(b, cfg);
				}
			}

			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Wheel Z: Front={:.3f}, Rear={:.3f} (Live Preview)", targetModelId, frontZ, rearZ).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [offsets] frontWheelOffsetZ={:.3f} rearWheelOffsetZ={:.3f}", frontZ, rearZ).c_str());
		} catch (...) {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for $vehwheelz.");
		}
		return true;
	}

	if (cmd == "$vehwheely") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehwheely [modelId] <offsetY> [rearOffsetY]");
			return true;
		}
		try {
			float frontY = std::stof(args[argIdx]);
			float rearY = (argIdx + 1 < args.size()) ? std::stof(args[argIdx + 1]) : frontY;

			std::lock_guard lock(m_mutex);
			auto& cfg = s_modelOffsets[targetModelId];
			cfg.hasConfig = true;
			cfg.frontWheelOffsetY = frontY;
			cfg.rearWheelOffsetY = rearY;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					ApplyModelOffsetsToBinding(b, cfg);
				}
			}

			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Wheel Y: Front={:.3f}, Rear={:.3f} (Live Preview)", targetModelId, frontY, rearY).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [offsets] frontWheelOffsetY={:.3f} rearWheelOffsetY={:.3f}", frontY, rearY).c_str());
		} catch (...) {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for $vehwheely.");
		}
		return true;
	}

	if (cmd == "$vehchassisz") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehchassisz [modelId] <offsetZ>");
			return true;
		}
		try {
			float chassisZ = std::stof(args[argIdx]);

			std::lock_guard lock(m_mutex);
			auto& cfg = s_modelOffsets[targetModelId];
			cfg.hasConfig = true;
			cfg.chassisOffsetZ = chassisZ;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					ApplyModelOffsetsToBinding(b, cfg);
				}
			}

			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Chassis Z: {:.3f} (Live Preview)", targetModelId, chassisZ).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [offsets] chassisOffsetZ={:.3f}", chassisZ).c_str());
		} catch (...) {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for $vehchassisz.");
		}
		return true;
	}

	if (cmd == "$vehchassis") {
		if (argIdx + 2 >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehchassis [modelId] <X> <Y> <Z>");
			return true;
		}
		try {
			float cX = std::stof(args[argIdx]);
			float cY = std::stof(args[argIdx + 1]);
			float cZ = std::stof(args[argIdx + 2]);

			std::lock_guard lock(m_mutex);
			auto& cfg = s_modelOffsets[targetModelId];
			cfg.hasConfig = true;
			cfg.chassisOffsetX = cX;
			cfg.chassisOffsetY = cY;
			cfg.chassisOffsetZ = cZ;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					ApplyModelOffsetsToBinding(b, cfg);
				}
			}

			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Chassis: ({:.3f}, {:.3f}, {:.3f}) (Live Preview)", targetModelId, cX, cY, cZ).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [offsets] chassisOffsetX={:.3f} chassisOffsetY={:.3f} chassisOffsetZ={:.3f}", cX, cY, cZ).c_str());
		} catch (...) {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for $vehchassis.");
		}
		return true;
	}

	if (cmd == "$vehtrack") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehtrack [modelId] <trackWidth> [rearTrackWidth]");
			return true;
		}
		try {
			float frontT = std::stof(args[argIdx]);
			float rearT = (argIdx + 1 < args.size()) ? std::stof(args[argIdx + 1]) : frontT;

			std::lock_guard lock(m_mutex);
			auto& cfg = s_modelOffsets[targetModelId];
			cfg.hasConfig = true;
			cfg.frontTrackWidth = frontT;
			cfg.rearTrackWidth = rearT;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					ApplyModelOffsetsToBinding(b, cfg);
				}
			}

			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Track Width: Front={:.3f}, Rear={:.3f} (Live Preview)", targetModelId, frontT, rearT).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [offsets] frontTrackWidth={:.3f} rearTrackWidth={:.3f}", frontT, rearT).c_str());
		} catch (...) {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for $vehtrack.");
		}
		return true;
	}

	if (cmd == "$vehcamber") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehcamber [modelId] <camber> [rearCamber] (in degrees or radians)");
			return true;
		}
		try {
			float frontC = std::stof(args[argIdx]);
			float rearC = (argIdx + 1 < args.size()) ? std::stof(args[argIdx + 1]) : frontC;
			if (std::abs(frontC) > 0.5f) frontC *= 0.0174532925f;
			if (std::abs(rearC) > 0.5f) rearC *= 0.0174532925f;

			std::lock_guard lock(m_mutex);
			auto& cfg = s_modelOffsets[targetModelId];
			cfg.hasConfig = true;
			cfg.frontCamber = frontC;
			cfg.rearCamber = rearC;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					ApplyModelOffsetsToBinding(b, cfg);
				}
			}

			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Camber: Front={:.3f} rad, Rear={:.3f} rad (Live Preview)", targetModelId, frontC, rearC).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [offsets] frontCamber={:.3f} rearCamber={:.3f}", frontC, rearC).c_str());
		} catch (...) {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for $vehcamber.");
		}
		return true;
	}

	if (cmd == "$vehwheelscale") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehwheelscale [modelId] <scale> [rearScale]");
			return true;
		}
		try {
			float frontS = std::stof(args[argIdx]);
			float rearS = (argIdx + 1 < args.size()) ? std::stof(args[argIdx + 1]) : frontS;

			std::lock_guard lock(m_mutex);
			auto& cfg = s_modelOffsets[targetModelId];
			cfg.hasConfig = true;
			cfg.frontWheelScale = frontS;
			cfg.rearWheelScale = rearS;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					ApplyModelOffsetsToBinding(b, cfg);
				}
			}

			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Wheel Scale: Front={:.3f}, Rear={:.3f} (Live Preview)", targetModelId, frontS, rearS).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [offsets] frontWheelScale={:.3f} rearWheelScale={:.3f}", frontS, rearS).c_str());
		} catch (...) {
			SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for $vehwheelscale.");
		}
		return true;
	}

	if (cmd == "$vehplate") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehplate [modelId] <text>");
			return true;
		}
		std::string newPlate = args[argIdx];
		for (size_t i = argIdx + 1; i < args.size(); ++i) {
			newPlate += " " + args[i];
		}

		{
			std::lock_guard lock(m_mutex);
			s_modelDefaultPlateText[targetModelId] = newPlate;

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId && b.appliedGameVehicle && IsVehiclePointerValid(b.appliedGameVehicle)) {
					ApplyPlateToVehicle(b.appliedGameVehicle, newPlate.c_str());
				}
			}
		}

		SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Plate Text: '{}' (Live Preview)", targetModelId, newPlate).c_str());
		SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [model] plateText={}", newPlate).c_str());
		return true;
	}

	return false;
}
