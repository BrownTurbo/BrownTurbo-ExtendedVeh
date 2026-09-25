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
#include <game_sa/CStreaming.h>
#include <game_sa/CTxdStore.h>
#include <game_sa/CVehicleModelInfo.h>
#include <game_sa/CWorld.h>
#include <game_sa/rw/rpworld.h>
#include <game_sa/rw/rwcore.h>
#include <algorithm>
#include <atomic>
#include <format>
#include <shared_mutex>

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
					if (GetVehiclePlateText(vehicleId, plateText, sizeof(plateText))) {
						CCustomCarPlateMgr::SetupClump(origClump, plateText, 0);
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
						reinterpret_cast<CAutomobile*>(vehicle)->SetupModelNodes();
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
	ClientLog(LogLevel::Info, "CustomVehicleBindingManager::Clear: All bindings cleared.");
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
				if (GetVehiclePlateText(vehicleId, plateText, sizeof(plateText))) {
					CCustomCarPlateMgr::SetupClump(newClump, plateText, 0);
				}

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
					ClientLog(LogLevel::Error, std::format("ERROR: AttachToRwObject did not leave expected RW object on vehicle {}", vehicleId));
				} else {
					ClientLog(LogLevel::Info, std::format("SUCCESS: vehicle {} is visually bound to custom model {}", vehicleId, binding.customModelId));
				}

				if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
					reinterpret_cast<CAutomobile*>(vehicle)->SetupModelNodes();
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
				}
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
