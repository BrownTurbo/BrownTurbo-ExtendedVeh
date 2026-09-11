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

class StreamingExtender {
private:
	static inline std::unordered_map<uint32_t, CVehicleModelInfo*> s_customModels;
	inline static void (*s_destructionRequeueCallback)(uint32_t) = nullptr;

public:
	static CVehicleModelInfo* CreateCustomModel(const CustomVeh::Protocol::VehicleDefinition& def)
	{
		auto it = s_customModels.find(def.customModelId);
		if (it != s_customModels.end() && it->second != nullptr) {
			return it->second;
		}

		CBaseModelInfo* visualBase = CModelInfo::GetModelInfo(def.visualBaseModel);
		if (!visualBase)
			return nullptr;

		auto* vBaseInfo = reinterpret_cast<CVehicleModelInfo*>(visualBase);

		CVehicleModelInfo* newModel = new CVehicleModelInfo();
		if (!newModel)
			return nullptr;
		memcpy(newModel, vBaseInfo, sizeof(CVehicleModelInfo));
		newModel->m_pRwClump = nullptr;
		newModel->m_pRwObject = nullptr;
		newModel->SetIsLod(0);

		CBaseModelInfo* handlingBase = CModelInfo::GetModelInfo(def.handlingBaseModel);
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
			RpClumpDestroy(pInfo->m_pRwClump);
			pInfo->m_pRwClump = nullptr;
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
			if (pInfo->m_pRwClump) {
				RpClumpDestroy(pInfo->m_pRwClump);
				pInfo->m_pRwClump = nullptr;
			}
			if (pInfo->m_pColModel) {
				pInfo->m_pColModel = nullptr;
			}
			if (pInfo->m_nTxdIndex != -1) {
				CTxdStore::RemoveTxdSlot(pInfo->m_nTxdIndex);
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
				if (pInfo->m_pRwClump) {
					RpClumpDestroy(pInfo->m_pRwClump);
					pInfo->m_pRwClump = nullptr;
				}
				pInfo->m_pColModel = nullptr;
				if (pInfo->m_nTxdIndex != -1) {
					CTxdStore::RemoveTxdSlot(pInfo->m_nTxdIndex);
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
