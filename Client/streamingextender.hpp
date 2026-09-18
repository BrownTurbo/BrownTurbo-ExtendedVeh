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

class StreamingExtender;
inline CBaseModelInfo* GetEngineModelInfo(int modelId);

class StreamingExtender {
private:
	static inline std::unordered_map<uint32_t, CVehicleModelInfo*> s_customModels;
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
		if (it != s_customModels.end() && it->second != nullptr) {
			return it->second;
		}

		CBaseModelInfo* visualBase = GetEngineModelInfo(def.visualBaseModel);
		if (!visualBase)
			return nullptr;

		auto* vBaseInfo = reinterpret_cast<CVehicleModelInfo*>(visualBase);

		CVehicleModelInfo* newModel = new CVehicleModelInfo();
		if (!newModel)
			return nullptr;
		memcpy(newModel, vBaseInfo, sizeof(CVehicleModelInfo));
		newModel->m_pRwClump = nullptr;
		newModel->m_pRwObject = nullptr;
		newModel->m_pVehicleStruct = nullptr;
		newModel->m_nTxdIndex = -1;
		newModel->m_pColModel = nullptr;
		newModel->m_nRefCount = 0;
		newModel->SetIsLod(0);
		newModel->bDoWeOwnTheColModel = 0;

		CBaseModelInfo* handlingBase = GetEngineModelInfo(def.handlingBaseModel);
		if (handlingBase) {
			newModel->m_nHandlingId = reinterpret_cast<CVehicleModelInfo*>(handlingBase)->m_nHandlingId;
		}

		s_customModels[def.customModelId] = newModel;
		return newModel;
	}

	static bool FinalizeClump(CVehicleModelInfo* pInfo, RpClump* pClump)
	{
		if (!pInfo || !pClump)
			return false;
		if (pInfo->m_pRwClump) {
			pInfo->DeleteRwObject();
		}
		CVisibilityPlugins::SetupVehicleVariables(pClump);
		pInfo->SetClump(pClump);
		pInfo->SetAtomicRenderCallbacks();
		return pInfo->m_pRwClump == pClump;
	}

	static void RegisterModel(uint32_t customId, CVehicleModelInfo* pInfo)
	{
		s_customModels[customId] = pInfo;
	}

	static CVehicleModelInfo* GetCustomModel(uint32_t id)
	{
		auto it = s_customModels.find(id);
		return (it != s_customModels.end()) ? it->second : nullptr;
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

		CVehicleModelInfo* pInfo = it->second;
		if (pInfo) {
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
		s_customModels.erase(it);
		AudioExtender::UnregisterVehicleAudio(customId);
	}

	static void ClearAllCustomModels()
	{
		for (auto& [id, pInfo] : s_customModels) {
			if (pInfo) {
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
	if (modelId >= 0 && modelId < CModelInfo::ms_modelInfoCount) {
		return CModelInfo::ms_modelInfoPtrs[modelId];
	}
	return nullptr;
}
