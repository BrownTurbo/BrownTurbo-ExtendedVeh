#include "CustomVehicleBindingManager.h"
#include "CollisionLoader.h"
#include "handling_manager.hpp"
#include "streamingextender.hpp"
#include "utils.h"

#include <game_sa/CAutomobile.h>
#include <game_sa/CBike.h>
#include <game_sa/CBoat.h>
#include <game_sa/CClock.h>
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
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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

static void ApplyPlateTextPixelSize(RpMaterial* material, uint8_t pixelSize)
{
	constexpr int baseWidth = 64;
	constexpr int baseHeight = 16;
	if (!material || pixelSize == 0 || pixelSize >= baseHeight)
		return;

	RwTexture* texture = RpMaterialGetTexture(material);
	RwRaster* raster = texture ? RwTextureGetRaster(texture) : nullptr;
	if (!raster || raster->width != baseWidth || raster->height != baseHeight || raster->depth != 32 || raster->stride < baseWidth * 4)
		return;

	RwUInt8* pixels = RwRasterLock(raster, 0, rwRASTERLOCKREADWRITE);
	if (!pixels)
		return;

	const size_t bufferSize = static_cast<size_t>(raster->stride) * baseHeight;
	std::vector<RwUInt8> original(pixels, pixels + bufferSize);
	std::memset(pixels, 0, bufferSize);

	const int outputHeight = pixelSize;
	const int outputWidth = std::max(1, (baseWidth * outputHeight + baseHeight / 2) / baseHeight);
	const int xOffset = (baseWidth - outputWidth) / 2;
	const int yOffset = (baseHeight - outputHeight) / 2;
	for (int y = 0; y < outputHeight; ++y) {
		const int sourceY = y * baseHeight / outputHeight;
		for (int x = 0; x < outputWidth; ++x) {
			const int sourceX = x * baseWidth / outputWidth;
			const size_t sourceOffset = static_cast<size_t>(sourceY) * raster->stride + sourceX * 4;
			const size_t targetOffset = static_cast<size_t>(y + yOffset) * raster->stride + (x + xOffset) * 4;
			std::memcpy(pixels + targetOffset, original.data() + sourceOffset, 4);
		}
	}
	RwRasterUnlock(raster);
}
 
static std::unordered_map<RpMaterial*, RwTexture*> s_originalMaterialTextures;
static std::unordered_map<uint32_t, RwTexture*> s_modelBaseBodyTextures;

static void PrecacheOriginalClumpMaterials(RpClump* clump)
{
	if (!clump)
		return;

	RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
		RpGeometry* geom = RpAtomicGetGeometry(atomic);
		if (geom) {
			RpGeometryForAllMaterials(geom, [](RpMaterial* mat, void* data) -> RpMaterial* {
				if (s_originalMaterialTextures.find(mat) == s_originalMaterialTextures.end()) {
					RwTexture* curTex = RpMaterialGetTexture(mat);
					s_originalMaterialTextures[mat] = curTex;
					if (curTex) {
						RwTextureAddRef(curTex);
					}
				}
				return mat;
			}, nullptr);
		}
		return atomic;
	}, nullptr);
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

static bool IsExtraFrame(RwFrame* frame)
{
	for (RwFrame* f = frame; f != nullptr; f = RwFrameGetParent(f)) {
		const char* name = GetFrameNodeName(f);
		if (name) {
			std::string lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
			if (lower.find("extra") != std::string::npos) {
				return true;
			}
		}
	}
	return false;
}

static void ApplyExtrasToClump(RpClump* clump, uint8_t mask)
{
	if (!clump)
		return;

	// First pass: Hide ALL atomics belonging to any extra hierarchy
	RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
		RwFrame* frame = RpAtomicGetFrame(atomic);
		if (frame && IsExtraFrame(frame)) {
			RpAtomicSetFlags(atomic, 0);
		}
		return atomic;
	},
		nullptr);

	// Second pass: For any bit enabled in mask (bits 0..7), enable that extra's atomics
	for (int i = 1; i <= 8; ++i) {
		bool visible = (mask & (1 << (i - 1))) != 0;
		if (!visible)
			continue;

		std::string extraName = std::format("extra{}", i);
		RwFrame* frame = CClumpModelInfo::GetFrameFromName(clump, extraName.c_str());
		if (!frame) {
			std::string extraNameUnder = std::format("extra_{}", i);
			frame = CClumpModelInfo::GetFrameFromName(clump, extraNameUnder.c_str());
		}
		if (!frame) {
			std::string extraNameCap = std::format("EXTRA{}", i);
			frame = CClumpModelInfo::GetFrameFromName(clump, extraNameCap.c_str());
		}
		if (!frame) {
			std::string extraNameCapUnder = std::format("EXTRA_{}", i);
			frame = CClumpModelInfo::GetFrameFromName(clump, extraNameCapUnder.c_str());
		}
		if (!frame)
			continue;

		struct AtomicExtraContext {
			RwFrame* targetFrame;
		} ctx { frame };

		RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
			auto* c = reinterpret_cast<AtomicExtraContext*>(data);
			if (IsFrameOrChildOf(RpAtomicGetFrame(atomic), c->targetFrame)) {
				RpAtomicSetFlags(atomic, rpATOMICRENDER);
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

	auto plateIt = s_modelPlateConfigs.find(customModelId);
	if (plateIt != s_modelPlateConfigs.end()) {
		if (plateIt->second.frontPlate.enabled) {
			binding.frontPlateMesh = plateIt->second.frontPlate;
			binding.hasFrontPlateMesh = true;
		}
		if (plateIt->second.rearPlate.enabled) {
			binding.rearPlateMesh = plateIt->second.rearPlate;
			binding.hasRearPlateMesh = true;
		}
		if (!plateIt->second.targetTexture.empty()) {
			strncpy_s(binding.targetPlateTexture, sizeof(binding.targetPlateTexture), plateIt->second.targetTexture.c_str(), _TRUNCATE);
			binding.hasTargetPlateTexture = true;
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
				unsigned char savedNitroBoosts = vehicle->m_nNitroBoosts;

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

					RpClump* savedClump = origModel->m_pRwClump;
					origModel->m_pRwClump = origClump;
					origModel->SetAtomicRenderCallbacks();
					origModel->m_pRwClump = savedClump;

					RpClumpForAllAtomics(origClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
						RwFrame* frame = RpAtomicGetFrame(atomic);
						if (frame) {
							const char* name = GetFrameNodeName(frame);
							if (name && strstr(name, "_vlo")) {
								RpAtomicSetFlags(atomic, 0);
								CVisibilityPlugins::SetAtomicRenderCallback(atomic, (RpAtomic * (*)(RpAtomic*))0x7331E0);
							}
						}
						return atomic;
					},
						nullptr);

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
						car->SetupSuspensionLines();
						car->ResetSuspension();
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
						reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
						reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
					}

					for (int i = 0; i < 15; ++i) {
						int upg = savedUpgrades[i];
						if (upg >= 1000 && upg <= 1193) {
							if (!CStreaming::HasModelLoaded(upg)) {
								CStreaming::RequestModel(upg, 0x16);
								CStreaming::LoadAllRequestedModels(false);
							}
							if (CStreaming::HasModelLoaded(upg)) {
								vehicle->AddVehicleUpgrade(upg);
							}
						}
					}
					if (savedNitroBoosts > 0) {
						vehicle->m_nNitroBoosts = savedNitroBoosts;
						vehicle->m_nHandlingFlagsIntValue = static_cast<eVehicleHandlingFlags>(vehicle->m_nHandlingFlagsIntValue | 0x80000);
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
		if (b.modelApplied && GetGameVehicleFromPool(id) == vehicle) {
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

void CustomVehicleBindingManager::OnVehicleFixed(CVehicle* vehicle)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle))
		return;

	std::lock_guard lock(m_mutex);
	for (auto& [id, b] : m_bindings) {
		if (b.appliedGameVehicle == vehicle || GetGameVehicleFromPool(id) == vehicle) {
			if (!b.modelApplied || !vehicle->m_pRwObject)
				return;

			RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
			auto* customModel = StreamingExtender::GetCustomModel(b.customModelId);
			if (!customModel || !customModel->m_pRwClump)
				return;

			// 1. Restore pristine matrices for known movable/damageable dummy nodes from template DFF.
			// NEVER perform blind structural tree walks or sibling pointer walks on live vehicle clumps,
			// because dynamic plates (bt_platefront/rear) and tuning upgrades create mismatched child counts
			// that desync sibling pairing and displace components (e.g. doors, glass, sunroof) across the tree.
			static const struct {
				int nodeIdx;
				const char* dummyName;
			} kDamageNodes[] = {
				{ CAR_BONNET, "bonnet_dummy" },
				{ CAR_BOOT, "boot_dummy" },
				{ CAR_DOOR_LF, "door_lf_dummy" },
				{ CAR_DOOR_RF, "door_rf_dummy" },
				{ CAR_DOOR_LR, "door_lr_dummy" },
				{ CAR_DOOR_RR, "door_rr_dummy" },
				{ CAR_BUMP_FRONT, "bump_front_dummy" },
				{ CAR_BUMP_REAR, "bump_rear_dummy" },
				{ CAR_WING_LF, "wing_lf_dummy" },
				{ CAR_WING_RF, "wing_rf_dummy" },
				{ CAR_WINDSCREEN, "windscreen_dummy" },
			};

			for (const auto& node : kDamageNodes) {
				RwFrame* tmplFrame = CClumpModelInfo::GetFrameFromName(customModel->m_pRwClump, node.dummyName);
				if (!tmplFrame)
					continue;

				RwFrame* vehFrame = nullptr;
				if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
					auto* car = reinterpret_cast<CAutomobile*>(vehicle);
					if (node.nodeIdx >= 0 && node.nodeIdx < CAR_NUM_NODES) {
						vehFrame = car->m_aCarNodes[node.nodeIdx];
					}
				}
				if (!vehFrame) {
					vehFrame = CClumpModelInfo::GetFrameFromName(clump, node.dummyName);
				}

				if (vehFrame) {
					vehFrame->modelling = tmplFrame->modelling;
				}
			}

			RwFrame* rootFrame = RpClumpGetFrame(clump);
			if (rootFrame) {
				RwFrameUpdateObjects(rootFrame);
			}

			// 2. Reset vehicle subclass nodes and door/panel/damage states
			if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
				auto* car = reinterpret_cast<CAutomobile*>(vehicle);
				car->SetupModelNodes();

				// Reset all 6 doors cleanly
				static const struct {
					int nodeIdx;
					eDoors door;
				} kDoors[] = {
					{ CAR_BONNET, BONNET },
					{ CAR_BOOT, BOOT },
					{ CAR_DOOR_LF, DOOR_FRONT_LEFT },
					{ CAR_DOOR_RF, DOOR_FRONT_RIGHT },
					{ CAR_DOOR_LR, DOOR_REAR_LEFT },
					{ CAR_DOOR_RR, DOOR_REAR_RIGHT }
				};

				for (const auto& d : kDoors) {
					car->FixDoor(d.nodeIdx, d.door);
					car->m_doors[d.door].m_fAngle = car->m_doors[d.door].m_fClosedAngle;
					car->m_doors[d.door].m_fPrevAngle = car->m_doors[d.door].m_fClosedAngle;
					car->m_doors[d.door].m_fAngVel = 0.0f;
					car->m_doors[d.door].m_nDoorState = DOOR_NOTHING;

					RwFrame* doorFrame = car->m_aCarNodes[d.nodeIdx];
					if (doorFrame) {
						car->SetComponentVisibility(doorFrame, 1);
					}
				}

				// Reset all 5 panels cleanly
				static const struct {
					int nodeIdx;
					ePanels panel;
				} kPanels[] = {
					{ CAR_BUMP_FRONT, BUMP_FRONT },
					{ CAR_BUMP_REAR, BUMP_REAR },
					{ CAR_WING_LF, WING_FRONT_LEFT },
					{ CAR_WING_RF, WING_FRONT_RIGHT },
					{ CAR_WINDSCREEN, WINDSCREEN }
				};

				for (const auto& p : kPanels) {
					car->FixPanel(p.nodeIdx, p.panel);
					RwFrame* panelFrame = car->m_aCarNodes[p.nodeIdx];
					if (panelFrame) {
						car->SetComponentVisibility(panelFrame, 1);
					}
				}

				// Reset bouncing panels
				for (int p = 0; p < 3; ++p) {
					car->m_panels[p].m_nFrameId = -1;
					car->m_panels[p].ResetPanel();
				}

				// Reset damage manager & flag
				car->m_damageManager.ResetDamageStatus();
				car->bIsDamaged = false;
				reinterpret_cast<uint8_t*>(car)[0x42A] &= ~1;

				// Fix tyres
				for (int w = 0; w < 4; ++w) {
					car->FixTyre(static_cast<eWheels>(w));
				}

				// Kill fire and overheat particles
				car->m_fBurningTimer = 0.0f;
				if (car->m_pFireParticle) {
					car->m_pFireParticle->Kill();
					car->m_pFireParticle = nullptr;
				}
				if (car->m_pOverheatParticle) {
					car->m_pOverheatParticle->Kill();
					car->m_pOverheatParticle = nullptr;
				}
			} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
				reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
			} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
				reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
			}

			// 3. Reset atomic callbacks from model info
			RpClump* savedClump = customModel->m_pRwClump;
			customModel->m_pRwClump = clump;
			customModel->SetAtomicRenderCallbacks();
			customModel->m_pRwClump = savedClump;

			// 4. Suppress _vlo LOD atomics (flags = 0, callback = 0x7331E0)
			RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
				if (!atomic || RwObjectGetType(atomic) != rpATOMIC)
					return atomic;
				RwFrame* frame = RpAtomicGetFrame(atomic);
				if (frame) {
					const char* name = GetFrameNodeName(frame);
					if (name && strstr(name, "_vlo")) {
						RpAtomicSetFlags(atomic, 0);
						CVisibilityPlugins::SetAtomicRenderCallback(atomic, (RpAtomic * (*)(RpAtomic*))0x7331E0);
					}
				}
				return atomic;
			},
				nullptr);

			// 5. Restore atomic visibility flags and pristine geometry vertices from template clump (customModel->m_pRwClump).
			// Name-based matching prevents atomic sequence desync from dynamic plates, custom wheels, or tuning parts.
			// Hides _dam meshes, restores _ok meshes, preserves baseline tuning parts (e.g. bumper_f0),
			// and restores undeformed morph target vertex coordinates to eliminate collision crumpling.
			std::unordered_map<std::string, std::vector<RpAtomic*>> tmplAtomicMap;
			RpClumpForAllAtomics(customModel->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
				if (!atomic || RwObjectGetType(atomic) != rpATOMIC)
					return atomic;
				auto* map = reinterpret_cast<std::unordered_map<std::string, std::vector<RpAtomic*>>*>(data);
				RwFrame* frame = RpAtomicGetFrame(atomic);
				const char* name = frame ? GetFrameNodeName(frame) : nullptr;
				if (name && *name) {
					(*map)[name].push_back(atomic);
				}
				return atomic;
			},
				&tmplAtomicMap);

			RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
				if (!atomic || RwObjectGetType(atomic) != rpATOMIC)
					return atomic;
				auto* map = reinterpret_cast<std::unordered_map<std::string, std::vector<RpAtomic*>>*>(data);
				RwFrame* frame = RpAtomicGetFrame(atomic);
				const char* name = frame ? GetFrameNodeName(frame) : nullptr;
				if (!name || !*name)
					return atomic;

				if (strstr(name, "bt_platefront") || strstr(name, "bt_platerear")) {
					return atomic; // Skip dynamic plate atomics
				}

				std::string nameLower = name;
				std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

				// Explicit damaged mesh handling: force-hide
				if (nameLower.find("_dam") != std::string::npos) {
					RpAtomicSetFlags(atomic, 0);
					CVisibilityPlugins::SetUserValue(atomic, 2);
					return atomic;
				}

				// Explicit undamaged mesh handling: force-show
				if (nameLower.find("_ok") != std::string::npos) {
					uint32_t flags = RpAtomicGetFlags(atomic) | rpATOMICRENDER;
					RpAtomicSetFlags(atomic, flags);
					CVisibilityPlugins::SetUserValue(atomic, 1);
				}

				// Match by name in pristine template clump
				auto it = map->find(name);
				if (it != map->end()) {
					RpGeometry* vehGeom = RpAtomicGetGeometry(atomic);
					RpAtomic* tmplAtomic = nullptr;
					for (RpAtomic* candidate : it->second) {
						RpGeometry* candidateGeom = RpAtomicGetGeometry(candidate);
						if (!vehGeom || !candidateGeom || vehGeom->numMorphTargets == 0 || candidateGeom->numMorphTargets == 0 ||
							vehGeom->numVertices != candidateGeom->numVertices ||
							vehGeom->numTriangles != candidateGeom->numTriangles ||
							vehGeom->numMorphTargets != candidateGeom->numMorphTargets) {
							continue;
						}
						if (tmplAtomic) {
							tmplAtomic = nullptr;
							break;
						}
						tmplAtomic = candidate;
					}

					if (tmplAtomic) {
						uint32_t tmplFlags = RpAtomicGetFlags(tmplAtomic);
						if (nameLower.find("_dam") != std::string::npos) {
							tmplFlags = 0;
						} else if (nameLower.find("_ok") != std::string::npos) {
							tmplFlags |= rpATOMICRENDER;
						}
						RpAtomicSetFlags(atomic, tmplFlags);

						RpGeometry* tmplGeom = RpAtomicGetGeometry(tmplAtomic);
						if (vehGeom && tmplGeom && vehGeom->numVertices == tmplGeom->numVertices && vehGeom->numVertices > 0) {
							RpGeometry* lockedVeh = RpGeometryLock(vehGeom, rpGEOMETRYLOCKVERTICES);
							RpGeometry* lockedTmpl = RpGeometryLock(tmplGeom, rpGEOMETRYLOCKVERTICES);
							if (lockedVeh && lockedTmpl) {
								if (lockedVeh->morphTarget && lockedTmpl->morphTarget &&
									lockedVeh->morphTarget[0].verts && lockedTmpl->morphTarget[0].verts) {
									memcpy(lockedVeh->morphTarget[0].verts, lockedTmpl->morphTarget[0].verts, sizeof(RwV3d) * lockedVeh->numVertices);
									lockedVeh->morphTarget[0].boundingSphere = lockedTmpl->morphTarget[0].boundingSphere;
								}
							}
							if (lockedTmpl)
								RpGeometryUnlock(lockedTmpl);
							if (lockedVeh)
								RpGeometryUnlock(lockedVeh);
						}
					}
				}

				return atomic;
			},
				&tmplAtomicMap);

			// 6. Re-apply extras mask if a custom mask was set
			if (b.hasExtras) {
				ApplyExtrasToClump(clump, b.extrasMask);
			}

			// 9. Re-apply vehicle materials & custom coatings (base colors, paintjob, window tint, wheel color, license plate)
			customModel->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
			customModel->SetEditableMaterials(clump);
			ApplyVehicleColors(vehicle, vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);

			if (b.paintjobIndex >= 0) {
				ApplyPaintjobToVehicle(vehicle, b.paintjobIndex);
			}
			if (b.hasWindowTint) {
				ApplyWindowTintToVehicle(vehicle, b.windowTintAlpha, b.windowTintR, b.windowTintG, b.windowTintB);
			}
			if (b.hasWheelColor) {
				ApplyWheelColorToVehicle(vehicle, b.wheelColorR, b.wheelColorG, b.wheelColorB);
			}
			if (b.hasCustomPlateText && b.customPlateText[0] != '\0') {
				ApplyPlateToVehicle(vehicle, b.customPlateText);
			} else if (b.lastPlateText[0] != '\0') {
				ApplyPlateToVehicle(vehicle, b.lastPlateText);
			}
			UpdateVehiclePlateVisibility(vehicle);

			// 10. Re-apply installed upgrades so stock _ok meshes replaced by tuning parts
			// (bumpers, spoilers, etc.) stay hidden after the visibility restore above.
			// AddVehicleUpgrade is hooked and applies DummySwapGuard for custom models.
			{
				short savedUpgrades[15];
				for (int i = 0; i < 15; ++i) {
					savedUpgrades[i] = vehicle->m_anUpgrades[i];
				}
				unsigned char savedNitroBoosts = vehicle->m_nNitroBoosts;

				for (int i = 0; i < 15; ++i) {
					int upg = savedUpgrades[i];
					if (upg < 1000 || upg > 1193)
						continue;
					//if (upg == 1008 || upg == 1009 || upg == 1010)
					//	continue;
					if (!CStreaming::HasModelLoaded(upg)) {
						CStreaming::RequestModel(upg, 0x16);
						CStreaming::LoadAllRequestedModels(false);
					}
					if (!CStreaming::HasModelLoaded(upg))
						continue;
					vehicle->RemoveVehicleUpgrade(upg);
					vehicle->AddVehicleUpgrade(upg);
				}

				if (savedNitroBoosts > 0)
					vehicle->m_nNitroBoosts = savedNitroBoosts;
			}

			// 11. Synchronize popup headlights
			if (b.hasPopupHeadlights && b.numPopupFrames > 0 && vehicle->m_pRwObject) {
				bool isNight = (CClock::ms_nGameClockHours >= 20 || CClock::ms_nGameClockHours < 7);
				bool lightsOn = (vehicle->bLightsOn != 0) || (vehicle->bEngineOn != 0 && isNight);
				float targetAngle = lightsOn ? b.popupMaxAngle : 0.0f;
				b.popupHeadlightAngle = targetAngle;
				RwFrame* vehRoot = RpClumpGetFrame(reinterpret_cast<RpClump*>(vehicle->m_pRwObject));
				for (uint8_t i = 0; i < b.numPopupFrames; ++i) {
					RwFrame* frame = b.popupFrames[i];
					if (frame && vehRoot && frame->root == vehRoot) {
						if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
							reinterpret_cast<CAutomobile*>(vehicle)->SetComponentRotation(frame, 0, targetAngle, true);
						}
					}
				}
			}

			return;
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
			binding.numPopupFrames = 0;
			for (size_t i = 0; i < Binding::MAX_POPUP_FRAMES; ++i) binding.popupFrames[i] = nullptr;
			binding.hasPopupHeadlights = false;
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
				binding.numPopupFrames = 0;
				for (size_t i = 0; i < Binding::MAX_POPUP_FRAMES; ++i) binding.popupFrames[i] = nullptr;
				binding.hasPopupHeadlights = false;
			}
			ClientLog(LogLevel::Info, std::format("Applying visual model: vehicle={} customModel={} baseModel={} vehiclePtr=0x{:X} sourceClump=0x{:X}", vehicleId, binding.customModelId, binding.baseModelId, reinterpret_cast<std::uintptr_t>(vehicle), reinterpret_cast<std::uintptr_t>(model->m_pRwClump)));

			RpClump* newClump = CloneClumpPreservingOrder(model->m_pRwClump);
			if (newClump) {
				PrecacheOriginalClumpMaterials(newClump);
				CVisibilityPlugins::SetupVehicleVariables(newClump);
				model->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
				model->SetEditableMaterials(newClump); // non-static: must call on model instance

				RpClump* savedClump = model->m_pRwClump;
				model->m_pRwClump = newClump;
				model->SetAtomicRenderCallbacks();
				model->m_pRwClump = savedClump;

				RpClumpForAllAtomics(newClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
					RwFrame* frame = RpAtomicGetFrame(atomic);
					if (frame) {
						const char* name = GetFrameNodeName(frame);
						if (name && strstr(name, "_vlo")) {
							RpAtomicSetFlags(atomic, 0);
							CVisibilityPlugins::SetAtomicRenderCallback(atomic, (RpAtomic * (*)(RpAtomic*))0x7331E0);
						}
					}
					return atomic;
				},
					nullptr);

				char plateText[32] = {};
				bool hasPlate = false;
				if (binding.hasCustomPlateText && binding.customPlateText[0] != '\0') {
					strncpy_s(plateText, sizeof(plateText), binding.customPlateText, _TRUNCATE);
					hasPlate = true;
				} else {
					char sampPlate[32] = {};
					bool gotSamp = GetVehiclePlateText(vehicleId, sampPlate, sizeof(sampPlate)) && sampPlate[0] != '\0';
					if (gotSamp && _stricmp(sampPlate, "SAN ANDREAS") != 0) {
						strncpy_s(plateText, sizeof(plateText), sampPlate, _TRUNCATE);
						hasPlate = true;
					} else {
						auto defIt = s_modelDefaultPlateText.find(binding.customModelId);
						if (defIt != s_modelDefaultPlateText.end() && !defIt->second.empty()) {
							strncpy_s(plateText, sizeof(plateText), defIt->second.c_str(), _TRUNCATE);
							hasPlate = true;
						} else if (gotSamp) {
							strncpy_s(plateText, sizeof(plateText), sampPlate, _TRUNCATE);
							hasPlate = true;
						}
					}
				}

				if (!hasPlate || plateText[0] == '\0') {
					strncpy_s(plateText, sizeof(plateText), "SAN ANDREAS", _TRUNCATE);
				}

				strncpy_s(binding.lastPlateText, sizeof(binding.lastPlateText), plateText, _TRUNCATE);
				if (binding.sampVehicleId > 0) {
					UpdateSampVehiclePlateText(binding.sampVehicleId, plateText);
				}
				const char* targetTex = (binding.hasTargetPlateTexture && binding.targetPlateTexture[0] != '\0') ? binding.targetPlateTexture : nullptr;
				const CustomVeh::Protocol::PlateMeshConfig* pFrontPlate = binding.hasFrontPlateMesh ? &binding.frontPlateMesh : nullptr;
				const CustomVeh::Protocol::PlateMeshConfig* pRearPlate = binding.hasRearPlateMesh ? &binding.rearPlateMesh : nullptr;
				ApplyPlateToClump(newClump, model, plateText, nullptr, binding.customModelId, targetTex, pFrontPlate, pRearPlate);

				if (binding.hasExtras) {
					ApplyExtrasToClump(newClump, binding.extrasMask);
				}

				short savedUpgrades[15];
				for (int i = 0; i < 15; ++i) {
					savedUpgrades[i] = vehicle->m_anUpgrades[i];
					vehicle->m_anUpgrades[i] = -1;
				}
				unsigned char savedNitroBoosts = vehicle->m_nNitroBoosts;

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
						if (!car->m_aCarNodes[CAR_CHASSIS]) {
							car->m_aCarNodes[CAR_CHASSIS] = CClumpModelInfo::GetFrameFromName(newClump, "body");
						}
					}
					if (car->m_aCarNodes[CAR_CHASSIS]) {
						binding.chassisBasePos = car->m_aCarNodes[CAR_CHASSIS]->modelling.pos;
						binding.hasChassisBasePos = true;
					}

					// Update suspension parameters for custom model
					tHandlingData* handling = car->m_pHandlingData;
					if (handling && model) {
						float upper = handling->m_fSuspensionUpperLimit;
						float lower = handling->m_fSuspensionLowerLimit;
						for (int w = 0; w < 4; ++w) {
							float wheelRadius = (w == 0 || w == 1) ? (model->m_fWheelSizeFront * 0.5f) : (model->m_fWheelSizeRear * 0.5f);
							if (wheelRadius < 0.1f) wheelRadius = 0.35f;
							car->m_aSuspensionSpringLength[w] = upper - lower;
							car->m_aSuspensionLineLength[w] = (upper - lower) + wheelRadius;
						}
						car->ResetSuspension();
					}
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
					reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
					reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
				}

				for (int i = 0; i < 15; ++i) {
					int upg = savedUpgrades[i];
					if (upg >= 1000 && upg <= 1193) {
						if (!CStreaming::HasModelLoaded(upg)) {
							CStreaming::RequestModel(upg, 0x16);
							CStreaming::LoadAllRequestedModels(false);
						}
						if (CStreaming::HasModelLoaded(upg)) {
							vehicle->AddVehicleUpgrade(upg);
						}
					}
				}
				if (savedNitroBoosts > 0) {
					vehicle->m_nNitroBoosts = savedNitroBoosts;
					vehicle->m_nHandlingFlagsIntValue = static_cast<eVehicleHandlingFlags>(vehicle->m_nHandlingFlagsIntValue | 0x80000);
				}

				binding.numPopupFrames = 0;
				for (size_t i = 0; i < Binding::MAX_POPUP_FRAMES; ++i) binding.popupFrames[i] = nullptr;
				binding.hasPopupHeadlights = false;
				binding.popupHeadlightAngle = 0.0f;
				binding.lastHeadlightActiveTick = 0;

				const char* popupNodeNames[] = {
					"farolzr", "popupr", "popupl", "popup_l", "popup_r", "popup_light", "popup_light_l", "popup_light_r", "popup"
				};
				for (const char* nodeName : popupNodeNames) {
					RwFrame* frame = CClumpModelInfo::GetFrameFromName(newClump, nodeName);
					if (frame && binding.numPopupFrames < Binding::MAX_POPUP_FRAMES) {
						bool alreadyAdded = false;
						for (uint8_t i = 0; i < binding.numPopupFrames; ++i) {
							if (binding.popupFrames[i] == frame) {
								alreadyAdded = true;
								break;
							}
						}
						if (!alreadyAdded) {
							binding.popupFrames[binding.numPopupFrames++] = frame;
							binding.hasPopupHeadlights = true;
						}
					}
				}
				if (binding.hasPopupHeadlights) {
					ClientLog(LogLevel::Info, std::format("Detected {} popup headlight frame(s) for vehicle {} (customModel={})", static_cast<unsigned int>(binding.numPopupFrames), vehicleId, binding.customModelId));
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

				ApplyVehicleColors(vehicle, vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);

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
				UpdateVehiclePlateVisibility(vehicle);
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
					ApplyVehicleColors(vehicle, vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
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
			} else {
				char sampPlate[32] = {};
				bool gotSamp = GetVehiclePlateText(vehicleId, sampPlate, sizeof(sampPlate)) && sampPlate[0] != '\0';
				if (gotSamp && _stricmp(sampPlate, "SAN ANDREAS") != 0) {
					strncpy_s(currentEffectivePlate, sizeof(currentEffectivePlate), sampPlate, _TRUNCATE);
					hasPlate = true;
				} else {
					auto defIt = s_modelDefaultPlateText.find(binding.customModelId);
					if (defIt != s_modelDefaultPlateText.end() && !defIt->second.empty()) {
						strncpy_s(currentEffectivePlate, sizeof(currentEffectivePlate), defIt->second.c_str(), _TRUNCATE);
						hasPlate = true;
					} else if (gotSamp) {
						strncpy_s(currentEffectivePlate, sizeof(currentEffectivePlate), sampPlate, _TRUNCATE);
						hasPlate = true;
					}
				}
			}

			if (!hasPlate || currentEffectivePlate[0] == '\0') {
				strncpy_s(currentEffectivePlate, sizeof(currentEffectivePlate), "SAN ANDREAS", _TRUNCATE);
				hasPlate = true;
			}

			if (hasPlate && std::strncmp(binding.lastPlateText, currentEffectivePlate, sizeof(binding.lastPlateText)) != 0) {
				ApplyPlateToVehicle(vehicle, currentEffectivePlate);
			}

			UpdateVehiclePlateVisibility(vehicle);
		}
	}
}

void CustomVehicleBindingManager::SetVehiclePaintjob(uint16_t vehicleId, int paintjobIndex)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it != m_bindings.end()) {
		it->second.hasPaintjob = (paintjobIndex >= 0);
		it->second.paintjobIndex = paintjobIndex;
		it->second.resolvedPaintjobIndex = -2;
		it->second.paintjobTexture = nullptr;
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
			vehicle->AddVehicleUpgrade(wheelModelId);
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

	for (auto& [vehicleId, binding] : m_bindings) {
		if (binding.customModelId == customModelId && !binding.hasCustomPlateText) {
			strncpy_s(binding.lastPlateText, sizeof(binding.lastPlateText), plateText.c_str(), _TRUNCATE);
			if (binding.sampVehicleId > 0) {
				UpdateSampVehiclePlateText(binding.sampVehicleId, plateText.c_str());
			}
			if (binding.appliedGameVehicle && IsVehiclePointerValid(binding.appliedGameVehicle)) {
				Instance().ApplyPlateToVehicle(binding.appliedGameVehicle, plateText.c_str());
			}
		}
	}
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

void CustomVehicleBindingManager::SetModelPlateConfig(uint32_t customModelId, const ModelPlateConfig& cfg)
{
	std::lock_guard lock(m_mutex);
	s_serverModelPlateConfigs[customModelId] = cfg;
	s_modelPlateConfigs[customModelId] = cfg;

	for (auto& [vehicleId, binding] : m_bindings) {
		if (binding.customModelId == customModelId) {
			if (!binding.hasFrontPlateMesh && cfg.frontPlate.enabled) {
				binding.frontPlateMesh = cfg.frontPlate;
				binding.hasFrontPlateMesh = true;
			}
			if (!binding.hasRearPlateMesh && cfg.rearPlate.enabled) {
				binding.rearPlateMesh = cfg.rearPlate;
				binding.hasRearPlateMesh = true;
			}
			if (!binding.hasTargetPlateTexture && !cfg.targetTexture.empty()) {
				strncpy_s(binding.targetPlateTexture, sizeof(binding.targetPlateTexture), cfg.targetTexture.c_str(), _TRUNCATE);
				binding.hasTargetPlateTexture = true;
			}

			if (binding.appliedGameVehicle && IsVehiclePointerValid(binding.appliedGameVehicle)) {
				Instance().ApplyPlateToVehicle(binding.appliedGameVehicle, binding.hasCustomPlateText ? binding.customPlateText : nullptr);
			}
		}
	}
}

bool CustomVehicleBindingManager::GetModelPlateConfig(uint32_t customModelId, ModelPlateConfig& outCfg)
{
	std::lock_guard lock(m_mutex);
	auto it = s_modelPlateConfigs.find(customModelId);
	if (it != s_modelPlateConfigs.end()) {
		outCfg = it->second;
		return true;
	}
	return false;
}

void CustomVehicleBindingManager::SetModelTargetPlateTexture(uint32_t customModelId, const std::string& textureName)
{
	std::lock_guard lock(m_mutex);
	s_modelPlateConfigs[customModelId].targetTexture = textureName;
	s_modelPlateConfigs[customModelId].hasConfig = true;

	for (auto& [vehicleId, binding] : m_bindings) {
		if (binding.customModelId == customModelId) {
			if (!binding.hasTargetPlateTexture) {
				strncpy_s(binding.targetPlateTexture, sizeof(binding.targetPlateTexture), textureName.c_str(), _TRUNCATE);
			}
			if (binding.appliedGameVehicle && IsVehiclePointerValid(binding.appliedGameVehicle)) {
				Instance().ApplyPlateToVehicle(binding.appliedGameVehicle, binding.hasCustomPlateText ? binding.customPlateText : nullptr);
			}
		}
	}
}

std::string CustomVehicleBindingManager::GetModelTargetPlateTexture(uint32_t customModelId)
{
	std::lock_guard lock(m_mutex);
	auto it = s_modelPlateConfigs.find(customModelId);
	return (it != s_modelPlateConfigs.end()) ? it->second.targetTexture : "";
}

void CustomVehicleBindingManager::SetVehiclePlateMesh(uint16_t vehicleId, bool isRear, const CustomVeh::Protocol::PlateMeshConfig& cfg)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it == m_bindings.end())
		return;

	if (isRear) {
		it->second.rearPlateMesh = cfg;
		it->second.hasRearPlateMesh = true;
	} else {
		it->second.frontPlateMesh = cfg;
		it->second.hasFrontPlateMesh = true;
	}

	if (it->second.appliedGameVehicle && IsVehiclePointerValid(it->second.appliedGameVehicle)) {
		RpClump* clump = reinterpret_cast<RpClump*>(it->second.appliedGameVehicle->m_pRwObject);
		if (clump) {
			const char* plateText = it->second.hasCustomPlateText && it->second.customPlateText[0] != '\0'
				? it->second.customPlateText
				: (it->second.lastPlateText[0] != '\0' ? it->second.lastPlateText : "SAN ANDREAS");
			uint8_t textSize = 16;
			auto modelCfg = s_modelPlateConfigs.find(it->second.customModelId);
			if (modelCfg != s_modelPlateConfigs.end())
				textSize = modelCfg->second.plateTextSize;
			CreatePlateQuadAtomic(clump, cfg, plateText, isRear, textSize);
		}
	}
}

void CustomVehicleBindingManager::SetVehiclePlateTexture(uint16_t vehicleId, const char* textureName)
{
	std::lock_guard lock(m_mutex);
	auto it = m_bindings.find(vehicleId);
	if (it == m_bindings.end())
		return;

	if (textureName && textureName[0] != '\0') {
		strncpy_s(it->second.targetPlateTexture, sizeof(it->second.targetPlateTexture), textureName, _TRUNCATE);
		it->second.hasTargetPlateTexture = true;
	} else {
		it->second.targetPlateTexture[0] = '\0';
		it->second.hasTargetPlateTexture = false;
	}

	if (it->second.appliedGameVehicle && IsVehiclePointerValid(it->second.appliedGameVehicle)) {
		ApplyPlateToVehicle(it->second.appliedGameVehicle, it->second.hasCustomPlateText ? it->second.customPlateText : nullptr);
	}
}

static RwFrame* FindPlateParentFrame(RpClump* clump, bool isRear)
{
	if (!clump) return nullptr;
	RwFrame* parentFrame = nullptr;
	if (!isRear) {
		parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bump_front_dummy");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bump_front");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bump_front_ok");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bumper_f0");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bumper_front");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bumper_f");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "f_bumper");
	} else {
		parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bump_rear_dummy");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bump_rear");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bump_rear_ok");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bumper_r0");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bumper_rear");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "bumper_r");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "r_bumper");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "boot_dummy");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "boot");
		if (!parentFrame) parentFrame = CClumpModelInfo::GetFrameFromName(clump, "boot_ok");
	}
	if (!parentFrame) {
		parentFrame = CClumpModelInfo::GetFrameFromName(clump, "chassis_dummy");
	}
	if (!parentFrame) {
		parentFrame = CClumpModelInfo::GetFrameFromName(clump, "chassis");
	}
	if (!parentFrame) {
		parentFrame = reinterpret_cast<RwFrame*>(RpClumpGetFrame(clump));
	}
	return parentFrame;
}

static void CalculatePlateModellingMatrix(
	RpClump* clump,
	RwFrame* parentFrame,
	const CustomVeh::Protocol::PlateMeshConfig& cfg,
	RwMatrix* outLocalMat)
{
	if (!outLocalMat) return;

	RwMatrix targetMat;
	RwMatrixSetIdentity(&targetMat);
	const RwV3d axisX = { 1.0f, 0.0f, 0.0f };
	const RwV3d axisY = { 0.0f, 1.0f, 0.0f };
	const RwV3d axisZ = { 0.0f, 0.0f, 1.0f };
	if (cfg.rotX != 0.0f)
		RwMatrixRotate(&targetMat, &axisX, cfg.rotX, rwCOMBINEPOSTCONCAT);
	if (cfg.rotY != 0.0f)
		RwMatrixRotate(&targetMat, &axisY, cfg.rotY, rwCOMBINEPOSTCONCAT);
	if (cfg.rotZ != 0.0f)
		RwMatrixRotate(&targetMat, &axisZ, cfg.rotZ, rwCOMBINEPOSTCONCAT);
	const RwV3d pos = { cfg.offsetX, cfg.offsetY, cfg.offsetZ };
	RwMatrixTranslate(&targetMat, &pos, rwCOMBINEPOSTCONCAT);

	// Try to use pristine template clump if this clump belongs to a known bound vehicle.
	// Template frames are ALWAYS in pristine rest pose (never dented, rotated by CBouncingPanel, or damaged).
	RpClump* refClump = clump;
	RwFrame* refParent = parentFrame;
	RwFrame* refChassis = nullptr;

	RpClump* pristineClump = nullptr;
	CustomVehicleBindingManager::Instance().ForEachBinding([&](uint16_t, const CustomVehicleBindingManager::Binding& b) {
		if (!pristineClump && b.appliedGameVehicle && b.appliedGameVehicle->m_pRwObject == reinterpret_cast<RwObject*>(clump)) {
			auto* cm = StreamingExtender::GetCustomModel(b.customModelId);
			if (cm && cm->m_pRwClump) {
				pristineClump = cm->m_pRwClump;
			}
		}
	});

	if (pristineClump) {
		const char* parentNodeName = parentFrame ? GetFrameNodeName(parentFrame) : nullptr;
		RwFrame* pristineParent = parentNodeName ? CClumpModelInfo::GetFrameFromName(pristineClump, parentNodeName) : nullptr;
		RwFrame* pristineChassis = CClumpModelInfo::GetFrameFromName(pristineClump, "chassis_dummy");
		if (!pristineChassis)
			pristineChassis = CClumpModelInfo::GetFrameFromName(pristineClump, "chassis");
		if (!pristineChassis)
			pristineChassis = reinterpret_cast<RwFrame*>(RpClumpGetFrame(pristineClump));

		if (pristineParent && pristineChassis) {
			refClump = pristineClump;
			refParent = pristineParent;
			refChassis = pristineChassis;
		}
	}

	if (!refChassis) {
		refChassis = CClumpModelInfo::GetFrameFromName(refClump, "chassis_dummy");
		if (!refChassis)
			refChassis = CClumpModelInfo::GetFrameFromName(refClump, "chassis");
		if (!refChassis)
			refChassis = reinterpret_cast<RwFrame*>(RpClumpGetFrame(refClump));
	}

	if (refParent && refChassis && refParent != refChassis) {
		RwFrameUpdateObjects(reinterpret_cast<RwFrame*>(RpClumpGetFrame(refClump)));
		const RwMatrix* ltmParent = RwFrameGetLTM(refParent);
		const RwMatrix* ltmChassis = RwFrameGetLTM(refChassis);

		if (ltmParent && ltmChassis) {
			RwMatrix invParent;
			if (RwMatrixInvert(&invParent, ltmParent)) {
				RwMatrix worldTarget;
				RwMatrixMultiply(&worldTarget, &targetMat, ltmChassis);
				RwMatrixMultiply(outLocalMat, &worldTarget, &invParent);
				return;
			}
		}
	}

	*outLocalMat = targetMat;
}

RpAtomic* CustomVehicleBindingManager::CreatePlateQuadAtomic(
	RpClump* clump,
	const CustomVeh::Protocol::PlateMeshConfig& cfg,
	const char* plateText,
	bool isRear,
	uint8_t textSize)
{
	if (!clump)
		return nullptr;

	const char* nodeName = isRear ? "bt_platerear" : "bt_platefront";
	RwFrame* existingFrame = CClumpModelInfo::GetFrameFromName(clump, nodeName);

	if (!cfg.enabled) {
		if (existingFrame) {
			struct FindAtomicCtx {
				RwFrame* targetFrame;
				RpAtomic* foundAtomic;
			} ctx { existingFrame, nullptr };

			RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
				auto* c = reinterpret_cast<FindAtomicCtx*>(data);
				if (RpAtomicGetFrame(atomic) == c->targetFrame) {
					c->foundAtomic = atomic;
					return nullptr;
				}
				return atomic;
			},
				&ctx);

			if (ctx.foundAtomic) {
				RpClumpRemoveAtomic(clump, ctx.foundAtomic);
				RpAtomicDestroy(ctx.foundAtomic);
			}
			RwFrame* parent = RwFrameGetParent(existingFrame);
			if (parent) {
				RwFrameRemoveChild(existingFrame);
			}
			RwFrameDestroy(existingFrame);
		}
		return nullptr;
	}

	// If atomic already exists on clump, update its transform and plate texture
	if (existingFrame) {
		RwFrame* parentFrame = FindPlateParentFrame(clump, isRear);
		RwFrame* currentParent = RwFrameGetParent(existingFrame);
		if (parentFrame && currentParent != parentFrame) {
			if (currentParent)
				RwFrameRemoveChild(existingFrame);
			RwFrameAddChild(parentFrame, existingFrame);
		}

		RwMatrix localMat;
		CalculatePlateModellingMatrix(clump, parentFrame, cfg, &localMat);
		existingFrame->modelling = localMat;
		RwFrame* rootFrame = RpClumpGetFrame(clump);
		if (rootFrame) {
			RwFrameUpdateObjects(rootFrame);
		}

		struct FindAtomicCtx {
			RwFrame* targetFrame;
			RpAtomic* foundAtomic;
		} ctx { existingFrame, nullptr };

		RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
			auto* c = reinterpret_cast<FindAtomicCtx*>(data);
			if (RpAtomicGetFrame(atomic) == c->targetFrame) {
				c->foundAtomic = atomic;
				return nullptr;
			}
			return atomic;
		},
			&ctx);

		if (ctx.foundAtomic) {
			CVisibilityPlugins::SetUserValue(ctx.foundAtomic, 1);
			RpGeometry* geom = RpAtomicGetGeometry(ctx.foundAtomic);
			if (geom) {
				if (geom->numVertices == 4 && geom->numTriangles == 4) {
					float s = (cfg.scale > 0.001f) ? cfg.scale : 1.0f;
					const float halfW = 0.160f * s;
					const float halfH = 0.080f * s;

					RpGeometryLock(geom, rpGEOMETRYLOCKALL);
					RwV3d* verts = geom->morphTarget[0].verts;
					verts[0] = { halfW, 0.0f, halfH }; // Top-Left
					verts[1] = { -halfW, 0.0f, halfH }; // Top-Right
					verts[2] = { -halfW, 0.0f, -halfH }; // Bottom-Right
					verts[3] = { halfW, 0.0f, -halfH }; // Bottom-Left

					geom->morphTarget[0].boundingSphere.center = { 0.0f, 0.0f, 0.0f };
					geom->morphTarget[0].boundingSphere.radius = std::sqrt(halfW * halfW + halfH * halfH) + 0.1f;
					RpGeometryUnlock(geom);

					if (geom->matList.materials && geom->matList.numMaterials >= 1) {
						if (plateText && plateText[0] != '\0') {
							CCustomCarPlateMgr::SetupMaterialPlateTexture(geom->matList.materials[0], const_cast<char*>(plateText), 0);
							ApplyPlateTextPixelSize(geom->matList.materials[0], textSize);
						}
					}
					ClientLog(LogLevel::Info, std::format("CreatePlateQuadAtomic: Updated {} 3D plate on clump (pos={:.2f}, {:.2f}, {:.2f}, rot={:.1f}, {:.1f}, {:.1f}, scale={:.2f})", isRear ? "rear" : "front", cfg.offsetX, cfg.offsetY, cfg.offsetZ, cfg.rotX, cfg.rotY, cfg.rotZ, cfg.scale));
					return ctx.foundAtomic;
				} else {
					RpClumpRemoveAtomic(clump, ctx.foundAtomic);
					RpAtomicDestroy(ctx.foundAtomic);
					RwFrame* parent = RwFrameGetParent(existingFrame);
					if (parent) {
						RwFrameRemoveChild(existingFrame);
					}
					RwFrameDestroy(existingFrame);
					existingFrame = nullptr;
				}
			}
		}
		if (existingFrame) {
			ClientLog(LogLevel::Info, std::format("CreatePlateQuadAtomic: Updated {} 3D plate quad on clump (pos={:.2f}, {:.2f}, {:.2f}, rot={:.1f}, {:.1f}, {:.1f}, scale={:.2f})", isRear ? "rear" : "front", cfg.offsetX, cfg.offsetY, cfg.offsetZ, cfg.rotX, cfg.rotY, cfg.rotZ, cfg.scale));
			return ctx.foundAtomic;
		}
	}

	// Create new 3D plate atomic with a clean single-layer 2:1 aspect ratio quad.
	// GTA SA's CCustomCarPlateMgr::SetupMaterialPlateTexture creates the full plate texture
	// (background + stamped text) at standard vehicle plate proportions (2:1).
	RpGeometry* geom = RpGeometryCreate(4, 4, rpGEOMETRYPOSITIONS | rpGEOMETRYTEXTURED | rpGEOMETRYNORMALS | rpGEOMETRYMODULATEMATERIALCOLOR | rpGEOMETRYPRELIT);
	if (!geom)
		return nullptr;

	RpMaterial* mat = RpMaterialCreate();
	if (!mat) {
		RpGeometryDestroy(geom);
		return nullptr;
	}

	RwRGBA white = { 255, 255, 255, 255 };
	RpMaterialSetColor(mat, &white);

	if (plateText && plateText[0] != '\0') {
		CCustomCarPlateMgr::SetupMaterialPlateTexture(mat, const_cast<char*>(plateText), 0);
		ApplyPlateTextPixelSize(mat, textSize);
	}

	float s = (cfg.scale > 0.001f) ? cfg.scale : 1.0f;
	const float halfW = 0.160f * s;
	const float halfH = 0.080f * s;

	RpGeometryLock(geom, rpGEOMETRYLOCKALL);

	RwV3d* verts = geom->morphTarget[0].verts;
	verts[0] = { halfW, 0.0f, halfH }; // Top-Left
	verts[1] = { -halfW, 0.0f, halfH }; // Top-Right
	verts[2] = { -halfW, 0.0f, -halfH }; // Bottom-Right
	verts[3] = { halfW, 0.0f, -halfH }; // Bottom-Left

	RwV3d* normals = geom->morphTarget[0].normals;
	for (int i = 0; i < 4; ++i) {
		normals[i] = { 0.0f, 1.0f, 0.0f };
	}

	RwTexCoords* uvs = geom->texCoords[0];
	uvs[0] = { 0.0f, 0.0f }; // Top-Left
	uvs[1] = { 1.0f, 0.0f }; // Top-Right
	uvs[2] = { 1.0f, 1.0f }; // Bottom-Right
	uvs[3] = { 0.0f, 1.0f }; // Bottom-Left

	geom->morphTarget[0].boundingSphere.center = { 0.0f, 0.0f, 0.0f };
	geom->morphTarget[0].boundingSphere.radius = std::sqrt(halfW * halfW + halfH * halfH) + 0.1f;

	if (geom->preLitLum) {
		for (int i = 0; i < 4; ++i) {
			geom->preLitLum[i] = { 255, 255, 255, 255 };
		}
	}

	RpTriangle* tris = geom->triangles;
	// Front faces (clockwise when viewed from front)
	RpGeometryTriangleSetVertexIndices(geom, &tris[0], 0, 1, 2);
	RpGeometryTriangleSetMaterial(geom, &tris[0], mat);

	RpGeometryTriangleSetVertexIndices(geom, &tris[1], 0, 2, 3);
	RpGeometryTriangleSetMaterial(geom, &tris[1], mat);

	// Back faces (reverse winding so visible if viewed from behind)
	RpGeometryTriangleSetVertexIndices(geom, &tris[2], 2, 1, 0);
	RpGeometryTriangleSetMaterial(geom, &tris[2], mat);

	RpGeometryTriangleSetVertexIndices(geom, &tris[3], 3, 2, 0);
	RpGeometryTriangleSetMaterial(geom, &tris[3], mat);

	RpGeometryUnlock(geom);
	RpMaterialDestroy(mat); // Decrement initial ref count; geom holds ref

	RpAtomic* atomic = RpAtomicCreate();
	if (!atomic) {
		RpGeometryDestroy(geom);
		return nullptr;
	}
	RpAtomicSetGeometry(atomic, geom, 0);
	RpGeometryDestroy(geom); // Decrement initial ref count; atomic holds ref

	RwFrame* frame = RwFrameCreate();
	if (!frame) {
		RpAtomicDestroy(atomic);
		return nullptr;
	}
	SetFrameNodeName(frame, nodeName);

	RwFrame* parentFrame = FindPlateParentFrame(clump, isRear);
	if (parentFrame) {
		RwFrameAddChild(parentFrame, frame);
	}

	RwMatrix localMat;
	CalculatePlateModellingMatrix(clump, parentFrame, cfg, &localMat);
	frame->modelling = localMat;
	RwFrame* rootFrame = RpClumpGetFrame(clump);
	if (rootFrame) {
		RwFrameUpdateObjects(rootFrame);
	}

	RpAtomicSetFrame(atomic, frame);
	CVisibilityPlugins::SetUserValue(atomic, 1);
	RpClumpAddAtomic(clump, atomic);
	ClientLog(LogLevel::Info, std::format("CreatePlateQuadAtomic: Created {} 3D plate quad on clump (parent='{}', pos={:.2f}, {:.2f}, {:.2f}, rot={:.1f}, {:.1f}, {:.1f}, scale={:.2f})",
		isRear ? "rear" : "front", parentFrame ? (GetFrameNodeName(parentFrame) ? GetFrameNodeName(parentFrame) : "unknown") : "none", cfg.offsetX, cfg.offsetY, cfg.offsetZ, cfg.rotX, cfg.rotY, cfg.rotZ, cfg.scale));
	return atomic;
}

std::vector<CustomVehicleBindingManager::PlateMaterialInfo> CustomVehicleBindingManager::FindVehiclePlateMaterials(
	RpClump* clump,
	CVehicleModelInfo* customModel,
	const char* lastKnownPlateText,
	const char* targetTexture)
{
	std::vector<PlateMaterialInfo> results;
	if (!clump)
		return results;

	auto containsCi = [](const std::string& haystack, std::string_view needle) -> bool {
		auto it = std::search(
			haystack.begin(), haystack.end(),
			needle.begin(), needle.end(),
			[](char ch1, char ch2) {
				return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2));
			});
		return it != haystack.end();
	};

	auto equalsCi = [](const char* s1, const char* s2) -> bool {
		if (!s1 || !s2)
			return false;
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
		},
			&searchCtx);

		targetAtomicIdx = searchCtx.foundAIdx;
		targetMatIdx = searchCtx.foundMIdx;
	}

	struct ClumpScanContext {
		int currentAtomicIdx { 0 };
		int targetAtomicIdx { -1 };
		int targetMatIdx { -1 };
		const char* lastPlate { nullptr };
		const char* modelPlate { nullptr };
		const char* targetTexture { nullptr };
		std::vector<PlateMaterialInfo>* pResults { nullptr };
		decltype(containsCi)* pContainsCi { nullptr };
		decltype(equalsCi)* pEqualsCi { nullptr };
	} ctx;

	ctx.targetAtomicIdx = targetAtomicIdx;
	ctx.targetMatIdx = targetMatIdx;
	ctx.lastPlate = lastKnownPlateText;
	ctx.modelPlate = (customModel && customModel->m_szPlateText[0] != '\0') ? customModel->m_szPlateText : nullptr;
	ctx.targetTexture = targetTexture;
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

		std::string fhLower = frameHierarchy;
		std::transform(fhLower.begin(), fhLower.end(), fhLower.begin(), ::tolower);

		// Never match materials on wheels, tyres, suspension, brakes, or rotors as license plates
		if (fhLower.find("wheel") != std::string::npos || fhLower.find("tyre") != std::string::npos || fhLower.find("tire") != std::string::npos || fhLower.find("brake") != std::string::npos || fhLower.find("disc") != std::string::npos || fhLower.find("susp") != std::string::npos) {
			c->currentAtomicIdx++;
			return atomic;
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
					std::string tnLower = tn;
					std::transform(tnLower.begin(), tnLower.end(), tnLower.begin(), ::tolower);

					// Exclude template, wheel, tire, rim, brake textures
					if (tnLower.find("template") != std::string::npos || tnLower.find("wheel") != std::string::npos || tnLower.find("tyre") != std::string::npos || tnLower.find("tire") != std::string::npos || tnLower.find("rim") != std::string::npos || tnLower.find("brake") != std::string::npos || tnLower.find("disc") != std::string::npos) {
						continue;
					}

					if (c->targetTexture && c->targetTexture[0] != '\0' && (*c->pEqualsCi)(texName, c->targetTexture)) {
						isPlate = true;
						isBg = false;
					} else if ((*c->pEqualsCi)(texName, "carplate")) {
						isPlate = true;
						isBg = false;
					} else if ((*c->pEqualsCi)(texName, "carpback") || (*c->pContainsCi)(tn, "plateback")) {
						isPlate = true;
						isBg = true;
					} else if ((*c->pContainsCi)(tn, "carplate") || (*c->pContainsCi)(tn, "numberplate") || (*c->pContainsCi)(tn, "license_plate") || (*c->pContainsCi)(tn, "licence_plate") || (*c->pContainsCi)(tn, "licenseplate") || (*c->pContainsCi)(tn, "licenceplate") || (*c->pContainsCi)(tn, "custom_plate") || (*c->pContainsCi)(tn, "nomer") || tnLower == "plate" || tnLower == "license" || tnLower == "licence") {
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

			if (!isPlate && !fhLower.empty()) {
				if (fhLower.find("bt_plate") != std::string::npos) {
					isPlate = true;
					isBg = false;
				} else if (fhLower.find("carplate") != std::string::npos || fhLower.find("numberplate") != std::string::npos || fhLower.find("license_plate") != std::string::npos || fhLower.find("licence_plate") != std::string::npos || fhLower.find("numplate") != std::string::npos) {
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
	},
		&ctx);

	return results;
}

bool CustomVehicleBindingManager::ApplyPlateToClump(
	RpClump* clump,
	CVehicleModelInfo* customModel,
	const char* plateText,
	const char* lastKnownText,
	uint32_t customModelId,
	const char* targetTexture,
	const CustomVeh::Protocol::PlateMeshConfig* frontPlate,
	const CustomVeh::Protocol::PlateMeshConfig* rearPlate)
{
	if (!clump || !plateText || plateText[0] == '\0')
		return false;

	// Resolve model plate config if customModelId provided and explicit config not passed
	ModelPlateConfig modelCfg;
	bool hasModelCfg = false;
	if (customModelId > 0) {
		std::lock_guard lock(m_mutex);
		auto it = s_modelPlateConfigs.find(customModelId);
		if (it != s_modelPlateConfigs.end()) {
			modelCfg = it->second;
			hasModelCfg = true;
		}
	}

	const char* effTargetTex = (targetTexture && targetTexture[0] != '\0')
		? targetTexture
		: (hasModelCfg && !modelCfg.targetTexture.empty() ? modelCfg.targetTexture.c_str() : nullptr);

	const CustomVeh::Protocol::PlateMeshConfig* effFrontPlate = frontPlate;
	if (!effFrontPlate && hasModelCfg && modelCfg.frontPlate.enabled) {
		effFrontPlate = &modelCfg.frontPlate;
	}

	const CustomVeh::Protocol::PlateMeshConfig* effRearPlate = rearPlate;
	if (!effRearPlate && hasModelCfg && modelCfg.rearPlate.enabled) {
		effRearPlate = &modelCfg.rearPlate;
	}
	const uint8_t plateTextSize = hasModelCfg && modelCfg.plateTextSize >= 1 && modelCfg.plateTextSize <= 16
		? modelCfg.plateTextSize
		: 16;

	// 1. Create or update 3D plate quad atomics if configured
	if (effFrontPlate) {
		CreatePlateQuadAtomic(clump, *effFrontPlate, plateText, false, plateTextSize);
	}
	if (effRearPlate) {
		CreatePlateQuadAtomic(clump, *effRearPlate, plateText, true, plateTextSize);
	}

	// 2. Find plate materials (matching standard names, targetTexture, and attached plate quad atomics)
	auto plateMaterials = FindVehiclePlateMaterials(clump, customModel, lastKnownText, effTargetTex);

	int textPlatesApplied = 0;
	int bgPlatesApplied = 0;

	for (auto& entry : plateMaterials) {
		if (entry.isBackground) {
			CCustomCarPlateMgr::SetupMaterialPlatebackTexture(entry.material, 0);
			bgPlatesApplied++;
		} else {
			CCustomCarPlateMgr::SetupMaterialPlateTexture(entry.material, const_cast<char*>(plateText), 0);
			ApplyPlateTextPixelSize(entry.material, plateTextSize);
			textPlatesApplied++;
		}
	}

	if (textPlatesApplied > 0) {
		ClientLog(LogLevel::Info, std::format("ApplyPlateToClump: Applied '{}' to {} plate text mat(s) and {} background mat(s)", plateText, textPlatesApplied, bgPlatesApplied));
		return true;
	}

	RpMaterial* fallbackMat = CCustomCarPlateMgr::SetupClump(clump, const_cast<char*>(plateText), 0);
	if (fallbackMat) {
		ApplyPlateTextPixelSize(fallbackMat, plateTextSize);
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
			bool gotSamp = GetVehiclePlateText(binding->sampVehicleId, sampPlate, sizeof(sampPlate)) && sampPlate[0] != '\0';
			if (gotSamp && _stricmp(sampPlate, "SAN ANDREAS") != 0) {
				strncpy_s(finalPlate, sizeof(finalPlate), sampPlate, _TRUNCATE);
				hasPlate = true;
			} else {
				std::lock_guard lock(m_mutex);
				auto defIt = s_modelDefaultPlateText.find(binding->customModelId);
				if (defIt != s_modelDefaultPlateText.end() && !defIt->second.empty()) {
					strncpy_s(finalPlate, sizeof(finalPlate), defIt->second.c_str(), _TRUNCATE);
					hasPlate = true;
				} else if (gotSamp) {
					strncpy_s(finalPlate, sizeof(finalPlate), sampPlate, _TRUNCATE);
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
	uint32_t customModelId = 0;
	const char* targetTex = nullptr;
	const CustomVeh::Protocol::PlateMeshConfig* pFrontPlate = nullptr;
	const CustomVeh::Protocol::PlateMeshConfig* pRearPlate = nullptr;

	if (binding) {
		customModel = StreamingExtender::GetCustomModel(binding->customModelId);
		lastKnownText = binding->lastPlateText;
		sampVehicleId = binding->sampVehicleId;
		customModelId = binding->customModelId;
		strncpy_s(binding->lastPlateText, sizeof(binding->lastPlateText), finalPlate, _TRUNCATE);

		if (binding->hasTargetPlateTexture && binding->targetPlateTexture[0] != '\0') {
			targetTex = binding->targetPlateTexture;
		}
		if (binding->hasFrontPlateMesh) {
			pFrontPlate = &binding->frontPlateMesh;
		}
		if (binding->hasRearPlateMesh) {
			pRearPlate = &binding->rearPlateMesh;
		}
	}

	if (sampVehicleId > 0) {
		UpdateSampVehiclePlateText(sampVehicleId, finalPlate);
	}

	ApplyPlateToClump(clump, customModel, finalPlate, lastKnownText, customModelId, targetTex, pFrontPlate, pRearPlate);
	UpdateVehiclePlateVisibility(vehicle);
}

void CustomVehicleBindingManager::UpdateVehiclePlateVisibility(CVehicle* vehicle)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle) || !vehicle->m_pRwObject)
		return;

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	bool isAutomobile = (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE ||
	                     vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK ||
	                     vehicle->m_nVehicleSubClass == VEHICLE_QUAD);
	CAutomobile* car = isAutomobile ? reinterpret_cast<CAutomobile*>(vehicle) : nullptr;

	auto countFrameAtomics = [](RwFrame* root) -> size_t {
		if (!root) return 0;
		size_t count = 0;
		auto countRec = [&](RwFrame* f, auto& self) -> void {
			if (!f) return;
			RwFrameForAllObjects(f, [](RwObject* obj, void* data) -> RwObject* {
				if (obj && RwObjectGetType(obj) == rpATOMIC) {
					*reinterpret_cast<size_t*>(data) += 1;
				}
				return obj;
			}, &count);
			for (RwFrame* child = f->child; child; child = child->next) {
				const char* cname = GetFrameNodeName(child);
				if (cname && (strstr(cname, "bt_platefront") || strstr(cname, "bt_platerear")))
					continue;
				self(child, self);
			}
		};
		countRec(root, countRec);
		return count;
	};

	auto frameHasVisibleAtomics = [](RwFrame* root) -> bool {
		if (!root) return false;
		bool found = false;
		auto searchRec = [&](RwFrame* f, auto& self) -> void {
			if (!f || found) return;
			RwFrameForAllObjects(f, [](RwObject* obj, void* data) -> RwObject* {
				if (obj && RwObjectGetType(obj) == rpATOMIC) {
					RpAtomic* a = reinterpret_cast<RpAtomic*>(obj);
					if (RpAtomicGetFlags(a) & rpATOMICRENDER) {
						*reinterpret_cast<bool*>(data) = true;
					}
				}
				return obj;
			}, &found);
			if (found) return;
			for (RwFrame* child = f->child; child; child = child->next) {
				const char* cname = GetFrameNodeName(child);
				if (cname && (strstr(cname, "bt_platefront") || strstr(cname, "bt_platerear")))
					continue;
				self(child, self);
			}
		};
		searchRec(root, searchRec);
		return found;
	};

	// 1. Front plate visibility
	RwFrame* frontFrame = CClumpModelInfo::GetFrameFromName(clump, "bt_platefront");
	if (frontFrame) {
		bool hideFront = false;
		if (car) {
			unsigned int frontBumpStatus = car->m_damageManager.GetPanelStatus(BUMP_FRONT);
			// Status 3 is PANEL_STATUS_MISSING (bumper detached / popped off)
			if (frontBumpStatus == 3) {
				hideFront = true;
			} else if (car->m_aCarNodes[CAR_BUMP_FRONT]) {
				size_t totalAtomics = countFrameAtomics(car->m_aCarNodes[CAR_BUMP_FRONT]);
				if (totalAtomics > 0 && !frameHasVisibleAtomics(car->m_aCarNodes[CAR_BUMP_FRONT])) {
					hideFront = true;
				}
			} else {
				RwFrame* parent = RwFrameGetParent(frontFrame);
				if (parent) {
					const char* pName = GetFrameNodeName(parent);
					if (pName && !strstr(pName, "chassis") && !strstr(pName, "root")) {
						size_t totalAtomics = countFrameAtomics(parent);
						if (totalAtomics > 0 && !frameHasVisibleAtomics(parent)) {
							hideFront = true;
						}
					}
				}
			}
		}

		RwFrameForAllObjects(frontFrame, [](RwObject* obj, void* data) -> RwObject* {
			if (obj && RwObjectGetType(obj) == rpATOMIC) {
				RpAtomic* a = reinterpret_cast<RpAtomic*>(obj);
				bool hide = *reinterpret_cast<bool*>(data);
				if (hide) {
					RpAtomicSetFlags(a, RpAtomicGetFlags(a) & ~rpATOMICRENDER);
				} else {
					RpAtomicSetFlags(a, RpAtomicGetFlags(a) | rpATOMICRENDER);
				}
			}
			return obj;
		}, &hideFront);
	}

	// 2. Rear plate visibility
	RwFrame* rearFrame = CClumpModelInfo::GetFrameFromName(clump, "bt_platerear");
	if (rearFrame) {
		bool hideRear = false;
		if (car) {
			unsigned int rearBumpStatus = car->m_damageManager.GetPanelStatus(BUMP_REAR);
			RwFrame* parent = RwFrameGetParent(rearFrame);
			const char* parentName = parent ? GetFrameNodeName(parent) : nullptr;
			bool attachedToBoot = parentName && (strstr(parentName, "boot") != nullptr);

			if (attachedToBoot) {
				if (car->m_damageManager.GetDoorStatus(BOOT) == DAMSTATE_NOTPRESENT) {
					hideRear = true;
				} else if (car->m_aCarNodes[CAR_BOOT]) {
					size_t totalAtomics = countFrameAtomics(car->m_aCarNodes[CAR_BOOT]);
					if (totalAtomics > 0 && !frameHasVisibleAtomics(car->m_aCarNodes[CAR_BOOT])) {
						hideRear = true;
					}
				}
			} else {
				if (rearBumpStatus == 3) {
					hideRear = true;
				} else if (car->m_aCarNodes[CAR_BUMP_REAR]) {
					size_t totalAtomics = countFrameAtomics(car->m_aCarNodes[CAR_BUMP_REAR]);
					if (totalAtomics > 0 && !frameHasVisibleAtomics(car->m_aCarNodes[CAR_BUMP_REAR])) {
						hideRear = true;
					}
				} else if (parent && parentName && !strstr(parentName, "chassis") && !strstr(parentName, "root")) {
					size_t totalAtomics = countFrameAtomics(parent);
					if (totalAtomics > 0 && !frameHasVisibleAtomics(parent)) {
						hideRear = true;
					}
				}
			}
		}

		RwFrameForAllObjects(rearFrame, [](RwObject* obj, void* data) -> RwObject* {
			if (obj && RwObjectGetType(obj) == rpATOMIC) {
				RpAtomic* a = reinterpret_cast<RpAtomic*>(obj);
				bool hide = *reinterpret_cast<bool*>(data);
				if (hide) {
					RpAtomicSetFlags(a, RpAtomicGetFlags(a) & ~rpATOMICRENDER);
				} else {
					RpAtomicSetFlags(a, RpAtomicGetFlags(a) | rpATOMICRENDER);
				}
			}
			return obj;
		}, &hideRear);
	}
}

void CustomVehicleBindingManager::ApplyVehicleColors(CVehicle* vehicle, uint8_t prim, uint8_t sec, uint8_t tert, uint8_t quat)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle) || !vehicle->m_pRwObject)
		return;

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	// GTA SA 128-color palette (carcols.dat)
	CRGBA* colorTable = CVehicleModelInfo::ms_vehicleColourTable;
	if (!colorTable)
		return;

	CRGBA primCol = colorTable[prim & 127];
	CRGBA secCol = colorTable[sec & 127];

	struct ColorContext {
		RwRGBA prim;
		RwRGBA sec;
		bool hasTexturedBody {};
		std::vector<RpMaterial*> untexturedOpaqueMaterials;
	} ctx {
		{ primCol.r, primCol.g, primCol.b, 255 },
		{ secCol.r, secCol.g, secCol.b, 255 }
	};

	RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
		auto* c = reinterpret_cast<ColorContext*>(data);
		RpGeometry* geom = RpAtomicGetGeometry(atomic);
		if (!geom)
			return atomic;

		RwFrame* frame = RpAtomicGetFrame(atomic);
		const char* frameName = frame ? GetFrameNodeName(frame) : nullptr;
		std::string fNameLower = frameName ? frameName : "";
		std::transform(fNameLower.begin(), fNameLower.end(), fNameLower.begin(), ::tolower);

		// Skip dynamic license plates and wheels (wheels have their own coloring)
		if (fNameLower.find("bt_plate") != std::string::npos || fNameLower.find("wheel") != std::string::npos)
			return atomic;

		RpGeometryForAllMaterials(geom, [](RpMaterial* mat, void* data) -> RpMaterial* {
			auto* c = reinterpret_cast<ColorContext*>(data);
			RwTexture* tex = RpMaterialGetTexture(mat);
			std::string texLower;
			if (tex) {
				const char* tn = RwTextureGetName(tex);
				if (tn) {
					texLower = tn;
					std::transform(texLower.begin(), texLower.end(), texLower.begin(), ::tolower);
				}
			}

			// Exclude non-body components (glass, lights, tires, brakes, interior, engine, badges)
			if (texLower.find("glass") != std::string::npos || texLower.find("window") != std::string::npos || texLower.find("windscreen") != std::string::npos ||
				texLower.find("light") != std::string::npos || texLower.find("lamp") != std::string::npos || texLower.find("vehiclelights") != std::string::npos ||
				texLower.find("wheel") != std::string::npos || texLower.find("tyre") != std::string::npos || texLower.find("tire") != std::string::npos ||
				texLower.find("brake") != std::string::npos || texLower.find("disc") != std::string::npos || texLower.find("caliper") != std::string::npos ||
				texLower.find("plate") != std::string::npos || texLower.find("nomer") != std::string::npos ||
				texLower.find("interior") != std::string::npos || texLower.find("seat") != std::string::npos || texLower.find("steer") != std::string::npos ||
				texLower.find("engine") != std::string::npos || texLower.find("exhaust") != std::string::npos || texLower.find("handle") != std::string::npos ||
				texLower.find("badge") != std::string::npos || texLower.find("logo") != std::string::npos || texLower.find("shad") != std::string::npos) {
				return mat;
			}

			bool isBody = false;
			bool isSecondary = false;

			if (!texLower.empty()) {
				if (texLower.find("col2") != std::string::npos || texLower.find("secondary") != std::string::npos || texLower.find("stripe") != std::string::npos) {
					isBody = true;
					isSecondary = true;
				} else if (texLower.find("remap") != std::string::npos || texLower.find("body") != std::string::npos || texLower.find("carbody") != std::string::npos || texLower.find("carpaint") != std::string::npos || texLower.find("paint") != std::string::npos || texLower.find("chassis") != std::string::npos || texLower.find("col1") != std::string::npos || texLower.find("primary") != std::string::npos || texLower.find("exterior") != std::string::npos) {
					isBody = true;
				}
			}

			if (isBody) {
				c->hasTexturedBody = true;
				RpMaterialSetColor(mat, isSecondary ? &c->sec : &c->prim);
			} else if (texLower.empty()) {
				const RwRGBA* curCol = RpMaterialGetColor(mat);
				if (curCol && curCol->alpha >= 240) {
					c->untexturedOpaqueMaterials.push_back(mat);
				}
			}

			return mat;
		}, c);

		return atomic;
	}, &ctx);

	if (!ctx.hasTexturedBody) {
		for (RpMaterial* mat : ctx.untexturedOpaqueMaterials) {
			RpMaterialSetColor(mat, &ctx.prim);
		}
	}
}

void CustomVehicleBindingManager::RestoreOriginalMaterials(CVehicle* vehicle)
{
	if (!vehicle || !IsVehiclePointerValid(vehicle) || !vehicle->m_pRwObject)
		return;

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	auto* binding = FindByVehicle(vehicle);
	RwTexture* fallbackBaseBodyTex = nullptr;
	if (binding) {
		auto it = s_modelBaseBodyTextures.find(binding->customModelId);
		if (it != s_modelBaseBodyTextures.end()) {
			fallbackBaseBodyTex = it->second;
		} else {
			CVehicleModelInfo* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
			if (customModel && customModel->m_nTxdIndex != -1) {
				CTxdStore::PushCurrentTxd();
				CTxdStore::SetCurrentTxd(customModel->m_nTxdIndex);
				RwTexDictionary* pDict = RwTexDictionaryGetCurrent();
				if (pDict) {
					fallbackBaseBodyTex = RwTexDictionaryFindNamedTexture(pDict, "body");
					if (!fallbackBaseBodyTex) {
						fallbackBaseBodyTex = RwTexDictionaryFindNamedTexture(pDict, "remapflash92body256");
					}
					if (!fallbackBaseBodyTex) {
						RwTexDictionaryForAllTextures(pDict, [](RwTexture* tex, void* data) -> RwTexture* {
							const char* tn = RwTextureGetName(tex);
							if (tn) {
								std::string tLower = tn;
								std::transform(tLower.begin(), tLower.end(), tLower.begin(), ::tolower);
								if (tLower == "body" || tLower.find("remap") != std::string::npos) {
									*reinterpret_cast<RwTexture**>(data) = tex;
									return nullptr;
								}
							}
							return tex;
						}, &fallbackBaseBodyTex);
					}
				}
				CTxdStore::PopCurrentTxd();
			}
			if (fallbackBaseBodyTex) {
				RwTextureAddRef(fallbackBaseBodyTex);
			}
			s_modelBaseBodyTextures[binding->customModelId] = fallbackBaseBodyTex;
		}
	}

	struct RestoreCtx {
		RwTexture* fallbackTex;
	} rctx { fallbackBaseBodyTex };

	RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
		auto* ctx = reinterpret_cast<RestoreCtx*>(data);
		RpGeometry* geom = RpAtomicGetGeometry(atomic);
		if (geom) {
			RpGeometryForAllMaterials(geom, [](RpMaterial* mat, void* data) -> RpMaterial* {
				auto* ctx = reinterpret_cast<RestoreCtx*>(data);
				auto it = s_originalMaterialTextures.find(mat);
				if (it != s_originalMaterialTextures.end()) {
					RpMaterialSetTexture(mat, it->second);
				} else if (ctx->fallbackTex) {
					RwTexture* curTex = RpMaterialGetTexture(mat);
					if (curTex) {
						const char* texName = RwTextureGetName(curTex);
						if (texName) {
							std::string nameLower = texName;
							std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
							if (nameLower.find("remap") != std::string::npos || nameLower.find("paintjob") != std::string::npos || nameLower.find("livery") != std::string::npos || nameLower.rfind("body", 0) == 0 || nameLower.find("skin") != std::string::npos) {
								RpMaterialSetTexture(mat, ctx->fallbackTex);
							}
						}
					}
				}
				return mat;
			}, ctx);
		}
		return atomic;
	}, &rctx);
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
		binding->resolvedPaintjobIndex = -1;
		binding->paintjobTexture = nullptr;
		RestoreOriginalMaterials(vehicle);
		customModel->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
		customModel->SetEditableMaterials(clump); // non-static: call on model instance
		ApplyVehicleColors(vehicle, vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
		return;
	}

	RwTexture* liveryTex = nullptr;
	if (binding->resolvedPaintjobIndex == paintjobIndex && binding->paintjobTexture) {
		liveryTex = binding->paintjobTexture;
	} else {

	// 1) Search in custom model's own TXD dictionary
	if (customModel->m_nTxdIndex != -1) {
		CTxdStore::PushCurrentTxd();
		CTxdStore::SetCurrentTxd(customModel->m_nTxdIndex);
		RwTexDictionary* pDict = RwTexDictionaryGetCurrent();
		if (pDict) {
			int idx1 = paintjobIndex + 1;
			int idx0 = paintjobIndex;
			char letterUpper = static_cast<char>('A' + paintjobIndex);
			char letterLower = static_cast<char>('a' + paintjobIndex);

			std::vector<std::string> searchNames = {
				std::format("body_{}", letterUpper),
				std::format("body_{}", letterLower),
				std::format("body{}", letterUpper),
				std::format("body{}", letterLower),
				std::format("body{}", idx1),
				std::format("body_{}", idx1),
				std::format("body{}", idx0),
				std::format("body_{}", idx0),
				std::format("remap{}", idx1),
				std::format("remap_{}", idx1),
				std::format("remap{}", idx0),
				std::format("remap_{}", idx0),
				std::format("paintjob{}", idx1),
				std::format("paintjob_{}", idx1),
				std::format("paintjob{}", idx0),
				std::format("paintjob_{}", idx0),
				std::format("paintjob_{}", letterUpper),
				std::format("paintjob_{}", letterLower),
				std::format("livery{}", idx1),
				std::format("livery_{}", idx1),
				std::format("livery{}", idx0),
				std::format("livery_{}", idx0),
				std::format("livery_{}", letterUpper),
				std::format("livery_{}", letterLower),
				std::format("skin{}", idx1),
				std::format("skin_{}", idx1),
				std::format("skin{}", idx0),
				std::format("skin_{}", idx0),
				std::format("skin_{}", letterUpper),
				std::format("skin_{}", letterLower),
				std::format("texture{}", idx1),
				std::format("texture_{}", idx1),
				std::format("texture{}", idx0),
				std::format("texture_{}", idx0),
				std::format("texture_{}", letterUpper),
				std::format("texture_{}", letterLower),
				std::format("camou{}", idx1),
				std::format("camou_{}", idx1),
				std::format("camou{}", idx0),
				std::format("camou_{}", idx0),
				std::format("camouflage{}", idx1),
				std::format("camouflage_{}", idx1),
				std::format("camouflage{}", idx0),
				std::format("camouflage_{}", idx0),
				std::format("stickers{}", idx1),
				std::format("stickers_{}", idx1),
				std::format("stickers{}", idx0),
				std::format("stickers_{}", idx0),
				std::format("pj{}", idx1),
				std::format("pj_{}", idx1),
				std::format("pj{}", idx0),
				std::format("pj_{}", idx0)
			};
			if (paintjobIndex == 0) {
				searchNames.push_back("remap");
				searchNames.push_back("paintjob");
				searchNames.push_back("livery");
				searchNames.push_back("skin");
				searchNames.push_back("texture");
				searchNames.push_back("camou");
				searchNames.push_back("camouflage");
				searchNames.push_back("stickers");
			}

			for (const auto& name : searchNames) {
				liveryTex = RwTexDictionaryFindNamedTexture(pDict, name.c_str());
				if (liveryTex)
					break;
			}

			if (!liveryTex) {
				struct FuzzyTexFind {
					RwTexture* result;
				} fctx { nullptr };

				RwTexDictionaryForAllTextures(pDict, [](RwTexture* tex, void* data) -> RwTexture* {
					auto* fc = reinterpret_cast<FuzzyTexFind*>(data);
					const char* tn = RwTextureGetName(tex);
					if (tn) {
						std::string tLower = tn;
						std::transform(tLower.begin(), tLower.end(), tLower.begin(), ::tolower);
						if (tLower == "body" || tLower == "remapflash92body256" || tLower.find("wheel") != std::string::npos ||
							tLower.find("glass") != std::string::npos || tLower.find("interior") != std::string::npos ||
							tLower.find("plate") != std::string::npos || tLower.find("light") != std::string::npos ||
							tLower.find("shad") != std::string::npos || tLower.find("tire") != std::string::npos ||
							tLower.find("brake") != std::string::npos || tLower.find("engine") != std::string::npos) {
							return tex;
						}
						if (tLower.find("texture") != std::string::npos || tLower.find("skin") != std::string::npos ||
							tLower.find("camou") != std::string::npos || tLower.find("sticker") != std::string::npos ||
							tLower.find("livery") != std::string::npos || tLower.find("remap") != std::string::npos ||
							tLower.find("paintjob") != std::string::npos) {
							fc->result = tex;
							return nullptr;
						}
					}
					return tex;
				}, &fctx);

				liveryTex = fctx.result;
			}
		}
		CTxdStore::PopCurrentTxd();
	}

	// 2) Fall back to engine remap TXDs (ChangeVehiclePaintjob / SetRemap path).
	// Prefer the BASE model remaps (vehicle->m_nModelIndex) — those are the TXDs SA-MP paintjobs use.
	// Stream-load them so SetupRender/SetEditableMaterials can apply vehiclegrunge256 remaps.
	if (!liveryTex && paintjobIndex >= 0 && paintjobIndex < 4) {
		short remapTxd = -1;
		auto* baseModel = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(vehicle->m_nModelIndex));
		if (baseModel)
			remapTxd = baseModel->m_anRemapTxds[paintjobIndex];
		if (remapTxd == -1)
			remapTxd = customModel->m_anRemapTxds[paintjobIndex];

		if (remapTxd != -1) {
			const int txdModelId = remapTxd + 20000; // TXDToModelId
			if (!CStreaming::HasModelLoaded(txdModelId)) {
				CStreaming::RequestModel(txdModelId, 0x16);
				CStreaming::LoadAllRequestedModels(false);
			}

			if (CStreaming::HasModelLoaded(txdModelId)) {
				if (vehicle->m_pRemapTexture && vehicle->m_nPreviousRemapTxd != -1) {
					vehicle->m_pRemapTexture = nullptr;
					CTxdStore::RemoveRef(vehicle->m_nPreviousRemapTxd);
				}
				CTxdStore::AddRef(remapTxd);
				vehicle->m_nPreviousRemapTxd = remapTxd;
				vehicle->m_nRemapTxd = -1;

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

				if (liveryTex) {
					vehicle->m_pRemapTexture = liveryTex;
				}
			} else {
				// Defer to SetupRender — it will stream the TXD next frames
				vehicle->m_nRemapTxd = remapTxd;
			}
		}
	}

		binding->paintjobTexture = liveryTex;
		binding->resolvedPaintjobIndex = paintjobIndex;
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
							if (nameLower.find("remap") != std::string::npos || nameLower.find("paintjob") != std::string::npos || nameLower.find("livery") != std::string::npos || nameLower == "body" || nameLower.rfind("body_", 0) == 0 || nameLower.find("skin") != std::string::npos || nameLower.find("vehiclegrunge") != std::string::npos) {
								auto it = s_originalMaterialTextures.find(mat);
								if (it == s_originalMaterialTextures.end()) {
									s_originalMaterialTextures[mat] = curTex;
									if (curTex) {
										RwTextureAddRef(curTex);
									}
								}
								RpMaterialSetTexture(mat, c->tex);
								RwRGBA whiteCol { 255, 255, 255, 255 };
								RpMaterialSetColor(mat, &whiteCol);
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
		if (cfg.frontWheelScale > 0.01f)
			binding.frontWheelScale = cfg.frontWheelScale;
		if (cfg.rearWheelScale > 0.01f)
			binding.rearWheelScale = cfg.rearWheelScale;
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
		"$vehresetspec", "$vehreset", "$vehhelp", "$customvehhelp", "$vehplate",
		"$vehplatefront", "$vehplaterear", "$vehtargettex"
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
		SendMsg(0xFFFFFF, "{FFFF00}$vehplatefront [model] <X> <Y> <Z> [rotX] [rotY] [rotZ] [scale] {FFFFFF}- Live preview 3D front plate mesh (or 'off')");
		SendMsg(0xFFFFFF, "{FFFF00}$vehplaterear [model] <X> <Y> <Z> [rotX] [rotY] [rotZ] [scale] {FFFFFF}- Live preview 3D rear plate mesh (or 'off')");
		SendMsg(0xFFFFFF, "{FFFF00}$vehtargettex [model] <textureName> {FFFFFF}- Set custom target texture for plate text");
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
		} catch (...) {
		}
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
			if (std::abs(frontC) > 0.5f)
				frontC *= 0.0174532925f;
			if (std::abs(rearC) > 0.5f)
				rearC *= 0.0174532925f;

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

	if (cmd == "$vehplatefront" || cmd == "$vehplaterear") {
		bool isRear = (cmd == "$vehplaterear");
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, std::format("{{FFFF00}}Usage: {{FFFFFF}}{} [modelId] <X> <Y> <Z> [rotX] [rotY] [rotZ] [scale] (or 'off')", cmd).c_str());
			return true;
		}

		CustomVeh::Protocol::PlateMeshConfig cfg {};
		std::string firstArg = args[argIdx];
		std::transform(firstArg.begin(), firstArg.end(), firstArg.begin(), ::tolower);

		if (firstArg == "off" || firstArg == "disable" || firstArg == "none") {
			cfg.enabled = 0;
		} else {
			if (argIdx + 2 >= args.size()) {
				SendMsg(0xFFFF00, std::format("{{FFFF00}}Usage: {{FFFFFF}}{} [modelId] <X> <Y> <Z> [rotX] [rotY] [rotZ] [scale]", cmd).c_str());
				return true;
			}
			try {
				cfg.enabled = 1;
				cfg.offsetX = std::stof(args[argIdx]);
				cfg.offsetY = std::stof(args[argIdx + 1]);
				cfg.offsetZ = std::stof(args[argIdx + 2]);
				cfg.rotX = (argIdx + 3 < args.size()) ? std::stof(args[argIdx + 3]) : 0.0f;
				cfg.rotY = (argIdx + 4 < args.size()) ? std::stof(args[argIdx + 4]) : 0.0f;
				cfg.rotZ = (argIdx + 5 < args.size()) ? std::stof(args[argIdx + 5]) : (isRear ? 180.0f : 0.0f);
				cfg.scale = (argIdx + 6 < args.size()) ? std::stof(args[argIdx + 6]) : 1.0f;
			} catch (...) {
				SendMsg(0xFF6666, "{00FF00}[ExtendedVeh]{FFFFFF} Invalid number format for plate mesh offsets.");
				return true;
			}
		}

		{
			std::lock_guard lock(m_mutex);
			auto& modelCfg = s_modelPlateConfigs[targetModelId];
			modelCfg.hasConfig = true;
			if (isRear) {
				modelCfg.rearPlate = cfg;
			} else {
				modelCfg.frontPlate = cfg;
			}

			for (auto& [vId, b] : m_bindings) {
				if (b.customModelId == targetModelId) {
					if (isRear) {
						b.rearPlateMesh = cfg;
						b.hasRearPlateMesh = true;
					} else {
						b.frontPlateMesh = cfg;
						b.hasFrontPlateMesh = true;
					}

					if (b.appliedGameVehicle && IsVehiclePointerValid(b.appliedGameVehicle)) {
						RpClump* clump = reinterpret_cast<RpClump*>(b.appliedGameVehicle->m_pRwObject);
						if (clump) {
							const char* plateText = b.hasCustomPlateText && b.customPlateText[0] != '\0'
								? b.customPlateText
								: (b.lastPlateText[0] != '\0' ? b.lastPlateText : "SAN ANDREAS");
							uint8_t textSize = 16;
							auto modelCfg = s_modelPlateConfigs.find(targetModelId);
							if (modelCfg != s_modelPlateConfigs.end())
								textSize = modelCfg->second.plateTextSize;
							CreatePlateQuadAtomic(clump, cfg, plateText, isRear, textSize);
						}
					}
				}
			}
		}

		if (cfg.enabled) {
			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} {} Plate: pos=({:.3f}, {:.3f}, {:.3f}), rot=({:.1f}, {:.1f}, {:.1f}), scale={:.2f} (Live Preview)", targetModelId, isRear ? "Rear" : "Front", cfg.offsetX, cfg.offsetY, cfg.offsetZ, cfg.rotX, cfg.rotY, cfg.rotZ, cfg.scale).c_str());
			SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [plate] {0}PlateX={1:.3f} {0}PlateY={2:.3f} {0}PlateZ={3:.3f} {0}PlateRotZ={4:.1f}", isRear ? "rear" : "front", cfg.offsetX, cfg.offsetY, cfg.offsetZ, cfg.rotZ).c_str());
		} else {
			SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} {} Plate disabled.", targetModelId, isRear ? "Rear" : "Front").c_str());
		}
		return true;
	}

	if (cmd == "$vehtargettex") {
		if (argIdx >= args.size()) {
			SendMsg(0xFFFF00, "{FFFF00}Usage: {FFFFFF}$vehtargettex [modelId] <textureName>");
			return true;
		}
		std::string texName = args[argIdx];
		SetModelTargetPlateTexture(targetModelId, texName);

		SendMsg(0x00FF00, std::format("{{00FF00}}[ExtendedVeh]{{FFFFFF}} Model {} Target Plate Texture: '{}' (Live Preview)", targetModelId, texName).c_str());
		SendMsg(0xAAAAAA, std::format("{{AAAAAA}}Server config -> [plate] targetTexture={}", texName).c_str());
		return true;
	}

	return false;
}
