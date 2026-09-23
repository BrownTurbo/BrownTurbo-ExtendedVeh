#pragma once

#include <cstring>
#include <unordered_map>

#include "game_sa/CHandlingDataMgr.h"
#include "game_sa/CModelInfo.h"
#include "game_sa/CStreaming.h"
#include "game_sa/CTxdStore.h"
#include "game_sa/CVehicleModelInfo.h"
#include "game_sa/CVisibilityPlugins.h"
#include "game_sa/NodeName.h"
#include "game_sa/common.h"
#include "plugin.h"

#include "CVisibilityPlugins.h"
#include "audioextender.hpp"
#include "defs.h"
#include "handling_manager.hpp"
#include "rpworld.h"
#include "utils.h"

class StreamingExtender;
inline CBaseModelInfo* GetEngineModelInfo(int modelId);

class StreamingExtender {
public:
	static constexpr int TOTAL_GTA_MODEL_COUNT = 20000;
	static inline CBaseModelInfo** GetModelInfoTable()
	{
		// GTA SA 1.0 US: 0x00A9B0C8 is the array CBaseModelInfo*[20000]
		// *(0x403DA4 + 3) reads the immediate operand of "mov eax, ds:00A9B0C8h[eax*4]" at CModelInfo::GetModelInfo.
		// Verified against MTA:SA (ARRAY_ModelInfo) and samp-custom-vehicles.
		// NOTE: Plugin-SDK's CModelInfo::ms_modelInfoPtrs reads from 0x40CD67 which is machine code
		// in vanilla GTA SA without fastman92, writing to which causes heap corruption (0xC0000374)!
		static CBaseModelInfo** const s_table = *reinterpret_cast<CBaseModelInfo***>(0x00403DA4 + 3);
		return s_table;
	}

private:
	struct CustomModelEntry {
		int gtaModelSlot = -1;
		CVehicleModelInfo* modelInfo = nullptr;
	};
	static inline std::unordered_map<uint32_t, CustomModelEntry*> s_customModels;
	static constexpr int CUSTOM_GTA_MODEL_BASE = 19000;

	static int AllocateGtaModelSlot()
	{
		auto** table = GetModelInfoTable();
		for (int slot = CUSTOM_GTA_MODEL_BASE;
			slot < TOTAL_GTA_MODEL_COUNT;
			++slot) {
			if (table[slot] != nullptr)
				continue;
			bool alreadyClaimed = false;
			for (auto& [id, e] : s_customModels) {
				if (e && e->gtaModelSlot == slot) {
					alreadyClaimed = true;
					break;
				}
			}
			if (!alreadyClaimed)
				return slot;
		}

		return -1;
	}

	static bool IsValidGtaModelSlot(int slot)
	{
		return slot >= 0 && slot < TOTAL_GTA_MODEL_COUNT;
	}

	inline static void (*s_destructionRequeueCallback)(uint32_t) = nullptr;

	using RemoveTxdSlotFn = void(__cdecl*)(int);
	static inline safetyhook::InlineHook s_removeTxdSlotHook;
	static inline bool s_hooksInstalled = false;

	static void __cdecl Hooked_RemoveTxdSlot(int index)
	{
		if (index < 0)
			return;
		auto* pool = CTxdStore::ms_pTxdPool;
		if (!pool || index >= pool->m_nSize || pool->IsFreeSlotAtIndex(index) || pool->GetAt(index) == nullptr) {
			return;
		}
		s_removeTxdSlotHook.original<RemoveTxdSlotFn>()(index);
	}

public:
	static void InstallHooks()
	{
		if (s_hooksInstalled)
			return;

		void* addr = reinterpret_cast<void*>(0x731CD0);
		s_removeTxdSlotHook = safetyhook::create_inline(addr, reinterpret_cast<void*>(&Hooked_RemoveTxdSlot));
		if (s_removeTxdSlotHook) {
			s_hooksInstalled = true;
		}
	}

	static void RestoreHooks()
	{
		if (!s_hooksInstalled)
			return;

		s_removeTxdSlotHook.reset();
		s_hooksInstalled = false;
	}
	static CVehicleModelInfo* CreateCustomModel(const CustomVeh::Protocol::VehicleDefinition& def)
	{
		auto it = s_customModels.find(def.customModelId);
		if (it != s_customModels.end() && it->second->modelInfo != nullptr) {
			return it->second->modelInfo;
		}

		CBaseModelInfo* visualBase = GetEngineModelInfo(static_cast<int>(def.visualBaseModel));
		if (!visualBase) {
			ClientLog(LogLevel::Error, std::format(
				"Streaming ERROR: visual base model {} not found.",
				def.visualBaseModel));
			return nullptr;
		}

		if (visualBase->GetModelType() != MODEL_INFO_VEHICLE) {
			ClientLog(LogLevel::Error, std::format(
				"[Streaming] ERROR: visual base model {} is not a vehicle model.",
				def.visualBaseModel));
			return nullptr;
		}

		auto* vBaseInfo = reinterpret_cast<CVehicleModelInfo*>(visualBase);

		const int gtaSlot = AllocateGtaModelSlot();
		if (!IsValidGtaModelSlot(gtaSlot)) {
			ClientLog(LogLevel::Error, std::format(
				"[Streaming] ERROR: no free GTA model-info slot available for custom model {}.",
				def.customModelId));
			return nullptr;
		}

		CustomModelEntry* entry = new CustomModelEntry();
		entry->gtaModelSlot = gtaSlot;
		entry->modelInfo = nullptr;
		s_customModels[def.customModelId] = entry;

		ClientLog(LogLevel::Info, std::format("[Streaming] Creating custom model {} using GTA model slot {} (base={}).", def.customModelId, gtaSlot, def.visualBaseModel));

		CVehicleModelInfo* newModel = new CVehicleModelInfo();
		if (!newModel) {
			ClientLog(LogLevel::Error, std::format(
				"[Streaming] ERROR: failed to allocate CVehicleModelInfo for custom model {}.",
				def.customModelId));
			s_customModels.erase(def.customModelId);
			delete entry;
			return nullptr;
		}

		memcpy(newModel, vBaseInfo, sizeof(CVehicleModelInfo));
		newModel->m_pRwClump = nullptr;
		newModel->m_pRwObject = nullptr;
		newModel->m_pVehicleStruct = nullptr;
		newModel->m_nTxdIndex = -1;
		newModel->m_pColModel = nullptr;
		newModel->m_nRefCount = 0;
		newModel->SetIsLod(0);
		newModel->bDoWeOwnTheColModel = 0;

		CBaseModelInfo* handlingBase = GetEngineModelInfo(static_cast<int>(def.handlingBaseModel));
		if (handlingBase && handlingBase->GetModelType() == MODEL_INFO_VEHICLE) {
			newModel->m_nHandlingId = reinterpret_cast<CVehicleModelInfo*>(handlingBase)->m_nHandlingId;
		}

		if (def.modelInfo.wheelScaleFront > 0.001f) {
			newModel->m_fWheelSizeFront = def.modelInfo.wheelScaleFront;
		}
		if (def.modelInfo.wheelScaleRear > 0.001f) {
			newModel->m_fWheelSizeRear = def.modelInfo.wheelScaleRear;
		}
		if (def.modelInfo.wheelModelId > 0) {
			newModel->m_nWheelModelIndex = def.modelInfo.wheelModelId;
		}
		if (def.modelInfo.vehicleClass != 0xFF) {
			newModel->m_nVehicleClass = def.modelInfo.vehicleClass;
		}
		if (def.modelInfo.frequency > 0) {
			newModel->m_nFrq = def.modelInfo.frequency;
		}
		if (def.modelInfo.wheelUpgradeClass != 0xFF && def.modelInfo.wheelUpgradeClass != 0) {
			newModel->m_nWheelUpgradeClass = def.modelInfo.wheelUpgradeClass;
		}
		if (def.modelInfo.comprate != 0) {
			newModel->m_nCompRules = def.modelInfo.comprate;
		}

		GetModelInfoTable()[gtaSlot] = newModel;

		entry->modelInfo = newModel;

		ClientLog(LogLevel::Info, std::format("[Streaming] Custom model {} registered: gtaSlot={} modelInfo=0x{:08X}", def.customModelId, gtaSlot, reinterpret_cast<std::uintptr_t>(newModel)));
		return newModel;
	}

	static void ExtractDummiesFromClump(RpClump* pClump, CVehicleModelInfo::CVehicleStructure* pStruct)
	{
		if (!pClump || !pStruct)
			return;

		// GTA:SA CVehicleStructure::m_avDummyPos[] indices, verified against:
		//   - VehicleDummies::Enum in MTA:SA Shared/sdk/enums/VehicleDummies.h
		//   - CMultiplayerSA_VehicleDummies.cpp byte offsets (EXHAUST=0x48 => idx 6, ENGINE=0x54 => idx 7, etc.)
		//   - MTA:SA CLuaFunctionParseHelpers.cpp ADD_ENUM table
		// This function is a FALLBACK ONLY: it only writes a slot if SetClump's
		// PreprocessHierarchy (0x4C8E60) left it as (0,0,0), so we never corrupt
		// data that GTA:SA already filled correctly.
		struct DummyMapping {
			int dummyIndex;
			std::vector<const char*> names;
		};

		// Index | VehicleDummies enum      | byte offset in m_avDummyPos
		//   0   | LIGHT_FRONT_MAIN         | 0x00  (headlights)
		//   1   | LIGHT_REAR_MAIN          | 0x0C  (taillights)
		//   2   | LIGHT_FRONT_SECONDARY    | 0x18
		//   3   | LIGHT_REAR_SECONDARY     | 0x24
		//   4   | SEAT_FRONT               | 0x30  (driver seat!)
		//   5   | SEAT_REAR                | 0x3C
		//   6   | EXHAUST                  | 0x48  (exhaust smoke & backfire)
		//   7   | ENGINE                   | 0x54  (engine fire/damage particles)
		//   8   | GAS_CAP / petrolcap      | 0x60
		//   9   | TRAILER_ATTACH           | 0x6C
		//  10   | HAND_REST                | 0x78
		//  11   | EXHAUST_SECONDARY        | 0x84
		static const DummyMapping s_dummyMap[] = {
			{ 0,  { "headlights",  "headlights_dummy",  "headlight"  } },   // LIGHT_FRONT_MAIN
			{ 1,  { "taillights",  "taillights_dummy",  "taillight"  } },   // LIGHT_REAR_MAIN
			{ 4,  { "seat_f",      "seat_front"                       } },   // SEAT_FRONT (driver!)
			{ 5,  { "seat_r",      "seat_rear"                        } },   // SEAT_REAR
			{ 6,  { "exhaust",     "exhaust_dummy"                    } },   // EXHAUST
			{ 7,  { "engine",      "engine_dummy"                     } },   // ENGINE
			{ 8,  { "petrolcap",   "petrolcap_dummy",   "gascap"      } },   // GAS_CAP
			{ 9,  { "trailer_attach", "trailer"                       } },   // TRAILER_ATTACH
			{ 10, { "handgrip",    "hand_rest",         "handrest"    } },   // HAND_REST
			{ 11, { "exhaust_2",   "exhaust2",          "secexhaust",
			        "exhaust_secondary"                                } },   // EXHAUST_SECONDARY
		};

		RwFrame* rootFrame = RpClumpGetFrame(pClump);
		if (!rootFrame)
			return;

		RwFrameUpdateObjects(rootFrame);

		for (const auto& mapping : s_dummyMap) {
			RwFrame* frame = nullptr;
			for (const char* name : mapping.names) {
				frame = CClumpModelInfo::GetFrameFromName(pClump, name);
				if (frame)
					break;
			}
			if (frame) {
				RwMatrix* ltm = RwFrameGetLTM(frame);
				if (ltm) {
					CVector& slot = pStruct->m_avDummyPos[mapping.dummyIndex];
					// Only write if PreprocessHierarchy left this slot as (0,0,0).
					// This prevents overwriting positions GTA:SA filled correctly.
					if (slot.x == 0.0f && slot.y == 0.0f && slot.z == 0.0f) {
						slot = CVector(ltm->pos.x, ltm->pos.y, ltm->pos.z);
						if (mapping.dummyIndex == 0 || mapping.dummyIndex == 1) {
							// Headlights/taillights: GTA:SA stores abs(X), right side positive
							slot.x = fabsf(slot.x);
						}
					}
				}
			}
		}
	}



	static bool FinalizeClump(CVehicleModelInfo* pInfo, RpClump* pClump, CVehicleModelInfo* pBaseInfo = nullptr)
	{
		if (!pInfo || !pClump)
			return false;

		ClientLog(LogLevel::Debug, std::format(" FinalizeClump -> pInfo=0x{:08X} pClump=0x{:08X}", reinterpret_cast<std::uintptr_t>(pInfo), reinterpret_cast<std::uintptr_t>(pClump)));

		if (pInfo->m_pRwClump) {
			ClientLog(LogLevel::Debug, " FinalizeClump -> deleting old RW clump");
			pInfo->DeleteRwObject();
			ClientLog(LogLevel::Debug, " FinalizeClump -> old RW clump deleted");
		}
		// CRITICAL: If m_pVehicleStruct was set (e.g. by a previous SetClump call),
		// release it from GTA:SA's CPool<CVehicleStructure> BEFORE calling SetClump.
		// SetClump (0x4C95C0) calls PreprocessHierarchy which always allocates a NEW
		// CVehicleStructure from the pool. Failing to release the old one first
		// causes CPool depletion (at most 70 entries on SA 1.0 US).
		// Verified: MTA:SA CRenderWareSA.cpp:406-417 uses destructor 0x4C7410 + release 0x4C9580.
		if (pInfo->m_pVehicleStruct) {
			ClientLog(LogLevel::Debug, std::format(" FinalizeClump -> releasing vehicle struct=0x{:08X}", reinterpret_cast<std::uintptr_t>(pInfo->m_pVehicleStruct)));
			auto CVehicleStructure_Destructor = reinterpret_cast<void(__thiscall*)(CVehicleModelInfo::CVehicleStructure*)>(0x4C7410);
			auto CVehicleStructure_Release    = reinterpret_cast<void(__cdecl*)(CVehicleModelInfo::CVehicleStructure*)>(0x4C9580);
			CVehicleStructure_Destructor(pInfo->m_pVehicleStruct);
			CVehicleStructure_Release(pInfo->m_pVehicleStruct);
			pInfo->m_pVehicleStruct = nullptr;
			ClientLog(LogLevel::Debug, " FinalizeClump -> vehicle struct released");
		}
		ClientLog(LogLevel::Debug, " FinalizeClump -> BEFORE SetupVehicleVariables");
		CVisibilityPlugins::SetupVehicleVariables(pClump);
		ClientLog(LogLevel::Debug, " FinalizeClump -> AFTER SetupVehicleVariables");

		ClientLog(LogLevel::Debug, " FinalizeClump -> BEFORE SetClump");
		pInfo->SetClump(pClump);   // SetClump allocates m_pVehicleStruct from pool + fills dummies
		ClientLog(LogLevel::Debug, " FinalizeClump -> AFTER SetClump");

		ClientLog(LogLevel::Debug, " FinalizeClump -> BEFORE SetAtomicRenderCallbacks");
		pInfo->SetAtomicRenderCallbacks();
		ClientLog(LogLevel::Debug, " FinalizeClump -> AFTER SetAtomicRenderCallbacks");

		// ExtractDummiesFromClump is a fallback only: it fills any dummy slot that
		// PreprocessHierarchy left as (0,0,0), using the RpClump frame hierarchy.
		if (pInfo->m_pVehicleStruct) {
			ExtractDummiesFromClump(pClump, pInfo->m_pVehicleStruct);

			if (pBaseInfo && pBaseInfo->m_pVehicleStruct) {
				// Fallback any (0,0,0) dummy positions from base vehicle
				for (int i = 0; i < 15; ++i) {
					if (pInfo->m_pVehicleStruct->m_avDummyPos[i].Magnitude() < 0.001f) {
						pInfo->m_pVehicleStruct->m_avDummyPos[i] = pBaseInfo->m_pVehicleStruct->m_avDummyPos[i];
					}
				}
				// Fallback any (0,0,0) upgrade attachment positions from base vehicle
				for (int i = 0; i < 18; ++i) {
					if (pInfo->m_pVehicleStruct->m_aUpgrades[i].m_vPosition.Magnitude() < 0.001f) {
						pInfo->m_pVehicleStruct->m_aUpgrades[i] = pBaseInfo->m_pVehicleStruct->m_aUpgrades[i];
					}
				}
			}
		}
		return pInfo->m_pRwClump == pClump;
	}

	static void RegisterModel(uint32_t customId, CustomModelEntry* pInfo)
	{
		s_customModels[customId]->modelInfo = pInfo->modelInfo;
	}

	static CVehicleModelInfo* GetCustomModel(uint32_t id)
	{
		auto it = s_customModels.find(id);
		return (it != s_customModels.end()) ? it->second->modelInfo : nullptr;
	}

	static bool IsCustomModel(uint32_t id)
	{
		return s_customModels.contains(id);
	}

	static void DestroyCustomModel(uint32_t customId)
	{
		// Check if the model is still in use
		if (HandlingManager::GetModelUseCount(customId) > 0) {
			// Re-queue the destruction command
			if (s_destructionRequeueCallback) {
				s_destructionRequeueCallback(customId);
			}
			return;
		}

		// Proceed with destruction...
		auto it = s_customModels.find(customId);
		if (it == s_customModels.end())
			return;

		CustomModelEntry* modelEntry = it->second;
		CVehicleModelInfo* pInfo = modelEntry ? modelEntry->modelInfo : nullptr;
		if (pInfo) {
			if (modelEntry->gtaModelSlot >= 0 && modelEntry->gtaModelSlot < TOTAL_GTA_MODEL_COUNT)
				GetModelInfoTable()[modelEntry->gtaModelSlot] = nullptr;

			pInfo->DeleteRwObject();
			if (pInfo->bDoWeOwnTheColModel && pInfo->m_pColModel) {
				delete pInfo->m_pColModel;
				pInfo->m_pColModel = nullptr;
				pInfo->bDoWeOwnTheColModel = 0;
			}
			pInfo->m_pColModel = nullptr;
			if (pInfo->m_nTxdIndex >= 0) {
				auto* pool = CTxdStore::ms_pTxdPool;
				if (pool && pInfo->m_nTxdIndex < pool->m_nSize && !pool->IsFreeSlotAtIndex(pInfo->m_nTxdIndex) && pool->GetAt(pInfo->m_nTxdIndex) != nullptr) {
					CTxdStore::RemoveTxdSlot(pInfo->m_nTxdIndex);
				}
				pInfo->m_nTxdIndex = -1;
			}
			// NOTE: Do NOT delete pInfo->m_pVehicleStruct here.
			// DeleteRwObject() (0x4C8040) already calls the CVehicleStructure destructor
			// (0x4C7410) and releases it back to CPool<CVehicleStructure> (0x4C9580),
			// then zeroes m_pVehicleStruct. Calling CRT delete on a pool pointer is
			// heap corruption / double-free. Verified: MTA:SA CRenderWareSA.cpp:406-417.
			delete pInfo;
		}
		delete modelEntry;
		s_customModels.erase(it);
		AudioExtender::UnregisterVehicleAudio(customId);
	}

	static void ClearAllCustomModels()
	{
		for (auto& [id, entry] : s_customModels) {
			if (!entry) continue;
			CVehicleModelInfo* pInfo = entry->modelInfo;
			if (pInfo) {
				if (entry->gtaModelSlot >= 0 && entry->gtaModelSlot < TOTAL_GTA_MODEL_COUNT)
					GetModelInfoTable()[entry->gtaModelSlot] = nullptr;

				pInfo->DeleteRwObject();
				if (pInfo->bDoWeOwnTheColModel && pInfo->m_pColModel) {
					delete pInfo->m_pColModel;
					pInfo->m_pColModel = nullptr;
					pInfo->bDoWeOwnTheColModel = 0;
				}
				pInfo->m_pColModel = nullptr;
				if (pInfo->m_nTxdIndex >= 0) {
					auto* pool = CTxdStore::ms_pTxdPool;
					if (pool && pInfo->m_nTxdIndex < pool->m_nSize && !pool->IsFreeSlotAtIndex(pInfo->m_nTxdIndex) && pool->GetAt(pInfo->m_nTxdIndex) != nullptr) {
						CTxdStore::RemoveTxdSlot(pInfo->m_nTxdIndex);
					}
					pInfo->m_nTxdIndex = -1;
				}
				delete pInfo;
			}
			delete entry;
			AudioExtender::UnregisterVehicleAudio(id);
		}
		s_customModels.clear();
	}

	static void SetDestructionCallback(void (*callback)(uint32_t))
	{
		s_destructionRequeueCallback = callback;
	}
};

inline CBaseModelInfo* GetEngineModelInfo(int modelId)
{
	if (modelId >= static_cast<int>(CUSTOM_MODEL_BASE_ID)) {
		return StreamingExtender::GetCustomModel(static_cast<uint32_t>(modelId));
	}
	if (modelId >= 0 && modelId < StreamingExtender::TOTAL_GTA_MODEL_COUNT) {
		return StreamingExtender::GetModelInfoTable()[modelId];
	}
	return nullptr;
}
