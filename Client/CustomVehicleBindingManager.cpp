#include "CustomVehicleBindingManager.h"
#include "CollisionLoader.h"
#include "streamingextender.hpp"
#include "handling_manager.hpp"
#include "utils.h"

#include <game_sa/CAutomobile.h>
#include <game_sa/CBike.h>
#include <game_sa/CBoat.h>
#include <game_sa/CCustomCarPlateMgr.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CStreaming.h>
#include <game_sa/CTxdStore.h>
#include <game_sa/CVehicleModelInfo.h>
#include <game_sa/rw/rpworld.h>
#include <game_sa/rw/rwcore.h>
#include <algorithm>

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

	for (int i = 1; i <= 6; ++i) {
		std::string extraName = std::format("extra{}", i);
		RwFrame* frame = CClumpModelInfo::GetFrameFromName(clump, extraName.c_str());
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
		}, &ctx);
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

void CustomVehicleBindingManager::Bind(uint16_t vehicleId, uint32_t customModelId)
{
	uint32_t baseModelId = 0;
	bool hasBaseModelId = false;

	{
		std::lock_guard baseLock(s_baseModelMutex);

		const auto it = s_baseModelIds.find(customModelId);
		if (it != s_baseModelIds.end()) {
			baseModelId = it->second;
			hasBaseModelId = true;
		}
	}

	std::lock_guard lock(m_mutex);

	auto existing = m_bindings.find(vehicleId);
	if (existing != m_bindings.end() && existing->second.customModelId == customModelId) {
		existing->second.baseModelId = baseModelId;
		existing->second.hasBaseModelId = hasBaseModelId;
		return;
	}
	if (existing != m_bindings.end()) {
		HandlingManager::DecrementModelUse(existing->second.customModelId);
		m_bindings.erase(existing);
	}

	Binding binding;
	binding.sampVehicleId = vehicleId;
	binding.customModelId = customModelId;
	binding.gtaModelId = customModelId;
	binding.originalModelId = -1;
	binding.appliedGameVehicle = nullptr;
	binding.modelApplied = false;

	binding.baseModelId = baseModelId;
	binding.hasBaseModelId = hasBaseModelId;

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
				RpClump* origClump = CloneClumpPreservingOrder(origModel->m_pRwClump);
				if (origClump) {
					CVisibilityPlugins::SetupVehicleVariables(origClump);
					origModel->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
					origModel->SetEditableMaterials(origClump);   // non-static: must call on model instance

					char plateText[32] = {};
					if (GetVehiclePlateText(vehicleId, plateText, sizeof(plateText))) {
						CCustomCarPlateMgr::SetupClump(origClump, plateText, 0);
					}

					vehicle->AttachToRwObject(reinterpret_cast<RwObject*>(origClump), true);
					if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
						reinterpret_cast<CAutomobile*>(vehicle)->SetupModelNodes();
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
						reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
					} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
						reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
					}
				}
				// CVehicle doesn't expose m_pColModel directly; use CEntity::GetColModel().
				// Restoring the original model's collision: GTA:SA looks up the col model
				// via the model info when it next needs it — resetting the vehicle's model
				// index (which was never changed for custom vehicles) keeps it correct.
				// Nothing extra to do here.
			}
			HandlingManager::OnVehicleStreamIn(vehicle, static_cast<uint16_t>(vehicleId));
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

CustomVehicleBindingManager::Binding* CustomVehicleBindingManager::FindByVehicle(CVehicle* vehicle)
{
	if (!vehicle)
		return nullptr;
	std::lock_guard lock(m_mutex);
	for (auto& [id, b] : m_bindings) {
		if (b.appliedGameVehicle == vehicle && b.modelApplied)
			return &b;
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
		if (!model || !model->m_pRwClump)
			continue;

		auto* vehicle = GetGameVehicleFromPool(vehicleId);
		if (!vehicle || !IsVehiclePointerValid(vehicle)) {
			binding.appliedGameVehicle = nullptr;
			binding.modelApplied = false;
			binding.originalModelId = -1;
			continue;
		}

		if (binding.originalModelId < 0) {
			binding.originalModelId = vehicle->m_nModelIndex;
		}

		if (!binding.modelApplied || binding.appliedGameVehicle != vehicle) {
			RpClump* newClump = CloneClumpPreservingOrder(model->m_pRwClump);
			if (newClump) {
				CVisibilityPlugins::SetupVehicleVariables(newClump);
				model->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
				model->SetEditableMaterials(newClump);   // non-static: must call on model instance

				char plateText[32] = {};
				if (GetVehiclePlateText(vehicleId, plateText, sizeof(plateText))) {
					CCustomCarPlateMgr::SetupClump(newClump, plateText, 0);
				}

				if (binding.hasExtras) {
					ApplyExtrasToClump(newClump, binding.extrasMask);
				}

				vehicle->DeleteRwObject();
				vehicle->AttachToRwObject(reinterpret_cast<RwObject*>(newClump), true);

				if (vehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || vehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || vehicle->m_nVehicleSubClass == VEHICLE_QUAD) {
					reinterpret_cast<CAutomobile*>(vehicle)->SetupModelNodes();
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX) {
					reinterpret_cast<CBike*>(vehicle)->SetupModelNodes();
				} else if (vehicle->m_nVehicleSubClass == VEHICLE_BOAT) {
					reinterpret_cast<CBoat*>(vehicle)->SetupModelNodes();
				}

				// CVehicle doesn't expose m_pColModel directly (CEntity::GetColModel() is the API).
				// Custom model collision is managed via model info — no manual vehicle pointer update needed.

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
			// Model is already applied — check if vehicle colors changed (e.g. ChangeVehicleColor or Respray)
			if (vehicle->m_nPrimaryColor != binding.lastPrimaryColor ||
				vehicle->m_nSecondaryColor != binding.lastSecondaryColor ||
				vehicle->m_nTertiaryColor != binding.lastTertiaryColor ||
				vehicle->m_nQuaternaryColor != binding.lastQuaternaryColor)
			{
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

	if (binding->hasCustomHorn) {
		pAudio->m_settings.m_bHornTon = static_cast<char>(binding->hornSoundId);
		pAudio->m_settings.m_fHornHigh = binding->hornPitch;
	}

	if (binding->hasCustomSiren) {
		pAudio->m_bModelWithSiren = true;
		vehicle->bSirenOrAlarm = binding->sirenEnabled;
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
	if (!customModel)
		return;

	RpClump* clump = reinterpret_cast<RpClump*>(vehicle->m_pRwObject);
	if (!clump)
		return;

	if (paintjobIndex < 0) {
		CVehicleModelInfo::ms_pRemapTexture = nullptr;
		customModel->SetEditableMaterials(clump);   // non-static: call on model instance
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
					}, &liveryTex);
				}
			}
			CTxdStore::PopCurrentTxd();
		}
	}

	if (liveryTex) {
		CVehicleModelInfo::ms_pRemapTexture = liveryTex;
		customModel->SetEditableMaterials(clump);   // non-static: call on model instance

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
							if (nameLower.find("remap") != std::string::npos ||
								nameLower.find("paintjob") != std::string::npos ||
								nameLower.find("livery") != std::string::npos) {
								RpMaterialSetTexture(mat, c->tex);
							}
						}
					}
					return mat;
				}, c);
			}
			return atomic;
		}, &ctx);

		customModel->SetVehicleColour(vehicle->m_nPrimaryColor, vehicle->m_nSecondaryColor, vehicle->m_nTertiaryColor, vehicle->m_nQuaternaryColor);
		customModel->SetEditableMaterials(clump);   // non-static: call on model instance
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
					if (nameLower.find("glass") != std::string::npos ||
						nameLower.find("window") != std::string::npos ||
						nameLower.find("windscreen") != std::string::npos ||
						nameLower.find("lightson") != std::string::npos) {
						isWindow = true;
					}
				}
			}

			const RwRGBA* curCol = RpMaterialGetColor(mat);
			if (curCol && curCol->alpha < 240) {
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
		}, c);

		return atomic;
	}, &ctx);
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
		"wheel_front", "wheel_rear"
	};

	std::vector<RwFrame*> targetFrames;
	for (const char* name : s_wheelFrames) {
		RwFrame* f = CClumpModelInfo::GetFrameFromName(clump, name);
		if (f)
			targetFrames.push_back(f);
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
					if (nameLower.find("tyre") != std::string::npos ||
						nameLower.find("tire") != std::string::npos ||
						nameLower.find("rubber") != std::string::npos) {
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
		}, c);

		return atomic;
	}, &ctx);
}
