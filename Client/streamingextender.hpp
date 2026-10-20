#pragma once

#include <cstring>
#include <unordered_map>

#include "game_sa/CAutomobile.h"
#include "game_sa/CColModel.h"
#include "game_sa/CCustomCarPlateMgr.h"
#include "game_sa/CHandlingDataMgr.h"
#include "game_sa/CModelInfo.h"
#include "game_sa/CPools.h"
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
	static constexpr int CUSTOM_GTA_MODEL_PREFERRED_BASE = 11000;

	static int AllocateGtaModelSlot()
	{
		auto** table = GetModelInfoTable();

		auto tryRange = [&](int rangeStart, int rangeEnd) -> int {
			for (int slot = rangeStart; slot < rangeEnd; ++slot) {
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
		};

		int slot = tryRange(CUSTOM_GTA_MODEL_PREFERRED_BASE, TOTAL_GTA_MODEL_COUNT);
		if (slot >= 0)
			return slot;
		slot = tryRange(0, CUSTOM_GTA_MODEL_PREFERRED_BASE);
		if (slot >= 0)
			return slot;

		ClientLog(LogLevel::Error, std::format("AllocateGtaModelSlot: exhausted entire table [0, {}). s_customModels has {} entries.", TOTAL_GTA_MODEL_COUNT, static_cast<int>(s_customModels.size())));
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
			ClientLog(LogLevel::Error, std::format("Streaming ERROR: visual base model {} not found.", def.visualBaseModel));
			return nullptr;
		}

		if (visualBase->GetModelType() != MODEL_INFO_VEHICLE) {
			ClientLog(LogLevel::Error, std::format("visual base model {} is not a vehicle model.", def.visualBaseModel));
			return nullptr;
		}

		auto* vBaseInfo = reinterpret_cast<CVehicleModelInfo*>(visualBase);

		const int gtaSlot = AllocateGtaModelSlot();
		if (!IsValidGtaModelSlot(gtaSlot)) {
			ClientLog(LogLevel::Error, std::format("no free GTA model-info slot available for custom model {}.", def.customModelId));
			return nullptr;
		}

		CustomModelEntry* entry = new CustomModelEntry();
		entry->gtaModelSlot = gtaSlot;
		entry->modelInfo = nullptr;
		s_customModels[def.customModelId] = entry;

		ClientLog(LogLevel::Info, std::format("Creating custom model {} using GTA model slot {} (base={}).", def.customModelId, gtaSlot, def.visualBaseModel));

		CVehicleModelInfo* newModel = new CVehicleModelInfo();
		if (!newModel) {
			ClientLog(LogLevel::Error, std::format("failed to allocate CVehicleModelInfo for custom model {}.", def.customModelId));
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
		newModel->m_pPlateMaterial = nullptr;
		std::memset(newModel->m_szPlateText, 0, sizeof(newModel->m_szPlateText));
		std::memset(newModel->m_apDirtMaterials, 0, sizeof(newModel->m_apDirtMaterials));
		newModel->m_pAnimBlock = nullptr;

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
		newModel->m_nWheelModelIndex = def.modelInfo.wheelModelId;
		if (def.modelInfo.vehicleClass != 0xFF) {
			newModel->m_nVehicleClass = def.modelInfo.vehicleClass;
		}
		if (def.modelInfo.frequency > 0) {
			newModel->m_nFrq = def.modelInfo.frequency;
		}
		if (def.modelInfo.wheelUpgradeClass != 0xFF) {
			newModel->m_nWheelUpgradeClass = def.modelInfo.wheelUpgradeClass;
		}
		if (def.modelInfo.comprate != 0) {
			newModel->m_nCompRules = def.modelInfo.comprate;
		}

		GetModelInfoTable()[gtaSlot] = newModel;

		entry->modelInfo = newModel;

		ClientLog(LogLevel::Info, std::format("Custom model {} registered: gtaSlot={} modelInfo=0x{:08X}", def.customModelId, gtaSlot, reinterpret_cast<std::uintptr_t>(newModel)));
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
			{ 0, { "headlights", "headlights_dummy", "headlight" } }, // LIGHT_FRONT_MAIN
			{ 1, { "taillights", "taillights_dummy", "taillight" } }, // LIGHT_REAR_MAIN
			{ 2, { "headlights2", "headlights2_dummy", "headlight2", "headlights_2", "headlight_2" } }, // LIGHT_FRONT_SECONDARY
			{ 3, { "taillights2", "taillights2_dummy", "taillight2", "taillights_2", "taillight_2" } }, // LIGHT_REAR_SECONDARY
			{ 4, { "ped_frontseat", "seat_f", "seat_front" } }, // SEAT_FRONT (driver!)
			{ 5, { "ped_backseat", "seat_r", "seat_rear" } }, // SEAT_REAR
			{ 6, { "exhaust", "exhaust_dummy" } }, // EXHAUST
			{ 7, { "engine", "engine_dummy" } }, // ENGINE
			{ 8, { "petrolcap", "petrolcap_dummy", "gascap" } }, // GAS_CAP
			{ 9, { "trailer_attach", "trailer" } }, // TRAILER_ATTACH
			{ 10, { "handgrip", "hand_rest", "handrest" } }, // HAND_REST
			{ 11, { "exhaust_2", "exhaust2", "secexhaust", "exhaust_secondary" } }, // EXHAUST_SECONDARY
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
						if (mapping.dummyIndex >= 0 && mapping.dummyIndex <= 3) {
							// Headlights/taillights (primary & secondary): GTA:SA stores abs(X), right side positive
							slot.x = fabsf(slot.x);
						}
					}
				}
			}
		}
	}

	static void NormalizeFrameNamesRecursive(RwFrame* frame)
	{
		if (!frame)
			return;

		char* name = GetFrameNodeName(frame);
		if (name && *name) {
			std::string s(name);
			bool modified = false;
			for (char& c : s) {
				if (c == ' ') {
					c = '_';
					modified = true;
				}
			}
			if (modified) {
				SetFrameNodeName(frame, s.c_str());
				ClientLog(LogLevel::Debug, std::format("Normalized frame name: '{}' -> '{}'", name, s));
			}
		}

		for (RwFrame* child = frame->child; child != nullptr; child = child->next) {
			NormalizeFrameNamesRecursive(child);
		}
	}

	static bool FinalizeClump(CVehicleModelInfo* pInfo, RpClump* pClump, CVehicleModelInfo* pBaseInfo = nullptr)
	{
		if (!pInfo || !pClump)
			return false;

		ClientLog(LogLevel::Debug, std::format("FinalizeClump -> pInfo=0x{:08X} pClump=0x{:08X}", reinterpret_cast<std::uintptr_t>(pInfo), reinterpret_cast<std::uintptr_t>(pClump)));

		if (pInfo->m_pRwClump) {
			ClientLog(LogLevel::Debug, "FinalizeClump -> deleting old RW clump");
			pInfo->DeleteRwObject();
			ClientLog(LogLevel::Debug, "FinalizeClump -> old RW clump deleted");
		}
		// CRITICAL: If m_pVehicleStruct was set (e.g. by a previous SetClump call),
		// release it from GTA:SA's CPool<CVehicleStructure> BEFORE calling SetClump.
		// SetClump (0x4C95C0) calls PreprocessHierarchy which always allocates a NEW
		// CVehicleStructure from the pool. Failing to release the old one first
		// causes CPool depletion (at most 70 entries on SA 1.0 US).
		// Verified: MTA:SA CRenderWareSA.cpp:406-417 uses destructor 0x4C7410 + release 0x4C9580.
		if (pInfo->m_pVehicleStruct) {
			ClientLog(LogLevel::Debug, std::format("FinalizeClump -> releasing vehicle struct=0x{:08X}", reinterpret_cast<std::uintptr_t>(pInfo->m_pVehicleStruct)));
			auto CVehicleStructure_Destructor = reinterpret_cast<void(__thiscall*)(CVehicleModelInfo::CVehicleStructure*)>(0x4C7410);
			auto CVehicleStructure_Release = reinterpret_cast<void(__cdecl*)(CVehicleModelInfo::CVehicleStructure*)>(0x4C9580);
			CVehicleStructure_Destructor(pInfo->m_pVehicleStruct);
			CVehicleStructure_Release(pInfo->m_pVehicleStruct);
			pInfo->m_pVehicleStruct = nullptr;
			ClientLog(LogLevel::Debug, "FinalizeClump -> vehicle struct released");
		}

		RwFrame* rootFrame = RpClumpGetFrame(pClump);
		if (rootFrame) {
			NormalizeFrameNamesRecursive(rootFrame);

			// Frame sanitation for custom vehicle models:
			// 1. Ensure a chassis dummy exists.
			// GTA SA requires "chassis_dummy" or "chassis" for CAR_CHASSIS (m_aCarNodes[CAR_CHASSIS]).
			// If missing, find a primary body frame (e.g. "body", "carbody") and rename it to "chassis_dummy".
			RwFrame* chassis = CClumpModelInfo::GetFrameFromName(pClump, "chassis_dummy");
			if (!chassis) {
				chassis = CClumpModelInfo::GetFrameFromName(pClump, "chassis");
			}
			if (!chassis) {
				const char* bodyNames[] = { "body", "carbody", "bodyshell", "shell" };
				for (const char* bname : bodyNames) {
					RwFrame* bf = CClumpModelInfo::GetFrameFromName(pClump, bname);
					if (bf) {
						SetFrameNodeName(bf, "chassis_dummy");
						ClientLog(LogLevel::Info, std::format("Renamed frame '{}' to 'chassis_dummy'", bname));
						chassis = bf;
						break;
					}
				}
			}

			// 2. Re-parent wheel dummies to clump root.
			// In standard GTA SA vehicles, wheel dummies are direct children of the clump root.
			// If a custom model placed wheels as children of "body" / "chassis", re-parent them to rootFrame
			// and adjust their local modelling matrices so they remain in root coordinate space.
			RwFrameUpdateObjects(rootFrame);
			const char* wheelDummyNames[] = {
				"wheel_rf_dummy", "wheel_lf_dummy", "wheel_rb_dummy", "wheel_lb_dummy",
				"wheel_rf", "wheel_lf", "wheel_rb", "wheel_lb"
			};
			for (const char* wname : wheelDummyNames) {
				RwFrame* wf = CClumpModelInfo::GetFrameFromName(pClump, wname);
				RwFrame* wfParent = wf ? RwFrameGetParent(wf) : nullptr;
				if (wf && wfParent && wfParent != rootFrame) {
					RwMatrix* ltmWheel = RwFrameGetLTM(wf);
					RwMatrix* ltmRoot = RwFrameGetLTM(rootFrame);
					RwMatrix newLocal;
					RwMatrix invRoot;
					if (ltmWheel && ltmRoot && RwMatrixInvert(&invRoot, ltmRoot)) {
						RwMatrixMultiply(&newLocal, ltmWheel, &invRoot);
					} else if (ltmWheel) {
						newLocal = *ltmWheel;
					}
					RwFrameRemoveChild(wf);
					RwFrameAddChild(rootFrame, wf);
					std::memcpy(&wf->modelling, &newLocal, sizeof(RwMatrix));
					RwFrameUpdateObjects(rootFrame);
					ClientLog(LogLevel::Info, std::format("Re-parented wheel dummy '{}' to clump root (pos: {:.3f}, {:.3f}, {:.3f})", wname, newLocal.pos.x, newLocal.pos.y, newLocal.pos.z));
				}
			}

			// 3. Normalize wheel dummy frame names to end with '_dummy'.
			// GTA SA's PreprocessHierarchy strictly looks for "wheel_rf_dummy", "wheel_lf_dummy", etc.
			const char* wheelRenamePairs[][2] = {
				{ "wheel_rf", "wheel_rf_dummy" },
				{ "wheel_lf", "wheel_lf_dummy" },
				{ "wheel_rb", "wheel_rb_dummy" },
				{ "wheel_lb", "wheel_lb_dummy" },
				{ "wheel_rm", "wheel_rm_dummy" },
				{ "wheel_lm", "wheel_lm_dummy" }
			};
			for (auto& pair : wheelRenamePairs) {
				RwFrame* targetDummy = CClumpModelInfo::GetFrameFromName(pClump, pair[1]);
				if (!targetDummy) {
					RwFrame* baseFrame = CClumpModelInfo::GetFrameFromName(pClump, pair[0]);
					if (baseFrame) {
						SetFrameNodeName(baseFrame, pair[1]);
						ClientLog(LogLevel::Info, std::format("Renamed wheel frame '{}' to '{}'", pair[0], pair[1]));
					}
				}
			}

			// 4. Custom wheel atomic discovery and promotion.
			// GTA SA's CVehicleModelInfo::SetClump -> PreprocessHierarchy expects the master wheel atomic
			// to be directly attached to "wheel_rf_dummy". It then sets the wheel render callback on it,
			// and automatically clones it to wheel_lf_dummy, wheel_rb_dummy, wheel_lb_dummy.
			// Modders almost universally export models where wheel_rf_dummy is an empty dummy frame,
			// and the actual wheel mesh atomic is attached to a child frame (e.g. "WheelFrRight", "wheel", "wheel_shiny").
			// Because GTA SA only inspects direct objects on wheel_rf_dummy, it finds no atomic, leaves
			// the master wheel atomic null, and skips cloning to the other 3 wheels, leaving the car with no wheels
			// or crashing when looking sideways (Q/E).
			auto getFirstDirectAtomic = [](RwFrame* frame) -> RpAtomic* {
				if (!frame) return nullptr;
				RpAtomic* found = nullptr;
				RwFrameForAllObjects(frame, [](RwObject* obj, void* data) -> RwObject* {
					if (obj && obj->type == rpATOMIC) {
						*reinterpret_cast<RpAtomic**>(data) = reinterpret_cast<RpAtomic*>(obj);
						return nullptr;
					}
					return obj;
				}, &found);
				return found;
			};

			std::function<RpAtomic*(RwFrame*)> findAtomicTree = [&](RwFrame* frame) -> RpAtomic* {
				if (!frame) return nullptr;
				RpAtomic* at = getFirstDirectAtomic(frame);
				if (at) return at;
				for (RwFrame* child = frame->child; child != nullptr; child = child->next) {
					at = findAtomicTree(child);
					if (at) return at;
				}
				return nullptr;
			};

			RwFrame* wheelRf = CClumpModelInfo::GetFrameFromName(pClump, "wheel_rf_dummy");
			RpAtomic* masterWheelAtomic = wheelRf ? getFirstDirectAtomic(wheelRf) : nullptr;

			if (wheelRf && !masterWheelAtomic) {
				// Search child frames under wheel_rf_dummy (e.g. WheelFrRight, wheel, wheel_shiny)
				for (RwFrame* child = wheelRf->child; child != nullptr; child = child->next) {
					masterWheelAtomic = findAtomicTree(child);
					if (masterWheelAtomic) {
						RwFrame* origFrame = RpAtomicGetFrame(masterWheelAtomic);
						const char* origName = origFrame ? GetFrameNodeName(origFrame) : nullptr;
						RpAtomicSetFrame(masterWheelAtomic, wheelRf);
						ClientLog(LogLevel::Info, std::format("Promoted custom wheel atomic from child frame '{}' to 'wheel_rf_dummy'", origName ? origName : "unnamed"));
						break;
					}
				}
			}

			// If still no atomic on wheel_rf_dummy, check other wheel dummies (e.g. modder put atomic on wheel_lf_dummy)
			if (wheelRf && !masterWheelAtomic) {
				const char* otherDummies[] = { "wheel_lf_dummy", "wheel_rb_dummy", "wheel_lb_dummy" };
				for (const char* od : otherDummies) {
					RwFrame* otherF = CClumpModelInfo::GetFrameFromName(pClump, od);
					if (otherF) {
						RpAtomic* otherAt = findAtomicTree(otherF);
						if (otherAt) {
							RpAtomic* cloned = RpAtomicClone(otherAt);
							if (cloned) {
								RpClumpAddAtomic(pClump, cloned);
								RpAtomicSetFrame(cloned, wheelRf);
								masterWheelAtomic = cloned;
								ClientLog(LogLevel::Info, std::format("Cloned wheel atomic from '{}' to 'wheel_rf_dummy'", od));
								break;
							}
						}
					}
				}
			}

			// If still no atomic, search for any frame whose name contains "wheel" (excluding steeringwheel and dummy)
			if (wheelRf && !masterWheelAtomic) {
				struct WheelSearchCtx {
					RpAtomic* found { nullptr };
				} wsc;
				RpClumpForAllAtomics(pClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
					auto* ctx = reinterpret_cast<WheelSearchCtx*>(data);
					RwFrame* f = RpAtomicGetFrame(atomic);
					if (f) {
						const char* fn = GetFrameNodeName(f);
						if (fn) {
							std::string nameLower = fn;
							std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
							if (nameLower.find("wheel") != std::string::npos && nameLower.find("steering") == std::string::npos && nameLower.find("dummy") == std::string::npos) {
								ctx->found = atomic;
								return nullptr; // stop search
							}
						}
					}
					return atomic;
				}, &wsc);

				if (wsc.found) {
					RpAtomicSetFrame(wsc.found, wheelRf);
					masterWheelAtomic = wsc.found;
					ClientLog(LogLevel::Info, "Assigned wheel atomic from clump to 'wheel_rf_dummy'");
				}
			}

			// Clean up duplicate pre-existing atomics on other wheel dummy children,
			// because SetClump is about to clone masterWheelAtomic to wheel_lf, wheel_rb, wheel_lb.
			if (masterWheelAtomic) {
				const char* slaveDummies[] = { "wheel_lf_dummy", "wheel_rb_dummy", "wheel_lb_dummy", "wheel_rm_dummy", "wheel_lm_dummy" };
				for (const char* sd : slaveDummies) {
					RwFrame* sf = CClumpModelInfo::GetFrameFromName(pClump, sd);
					if (sf) {
						for (RwFrame* child = sf->child; child != nullptr; child = child->next) {
							RpAtomic* extraAt = findAtomicTree(child);
							while (extraAt) {
								RpClumpRemoveAtomic(pClump, extraAt);
								RpAtomicDestroy(extraAt);
								extraAt = findAtomicTree(child);
							}
						}
					}
				}
			}

			// Fallback ONLY when the DFF genuinely has NO wheel geometry anywhere and wheelModelId <= 0:
			if (!masterWheelAtomic && pInfo->m_nWheelModelIndex <= 0) {
				int fallbackWheel = 1080;
				if (pBaseInfo && pBaseInfo->m_nWheelModelIndex > 0) {
					fallbackWheel = pBaseInfo->m_nWheelModelIndex;
				}
				pInfo->m_nWheelModelIndex = static_cast<int16_t>(fallbackWheel);
				ClientLog(LogLevel::Warning, std::format("Model has NO wheel geometry in DFF and wheelModelId <= 0. Falling back to wheelModelId={}", fallbackWheel));
			}
		}

		ClientLog(LogLevel::Debug, "FinalizeClump -> BEFORE SetupVehicleVariables");
		CVisibilityPlugins::SetupVehicleVariables(pClump);
		ClientLog(LogLevel::Debug, "FinalizeClump -> AFTER SetupVehicleVariables");

		ClientLog(LogLevel::Debug, "FinalizeClump -> BEFORE SetClump");
		pInfo->SetClump(pClump); // SetClump allocates m_pVehicleStruct from pool + fills dummies
		ClientLog(LogLevel::Debug, "FinalizeClump -> AFTER SetClump");

		// Ensure all vehicle doors and panels are marked as damagable in m_nMaskComponentsDamagable.
		// If a model lacks separate _dam meshes, GTA SA's SetClump does not set these bits.
		// When damaged, GTA SA (CAutomobile::SetDoorDamage at 0x6B16B3) checks m_nMaskComponentsDamagable:
		// if the bit is missing, it sets damage state to 4 (DAMSTATE_NOTPRESENT), knocks off the door as a flying
		// component, and hides the door, causing CJ to open an invisible door in thin air.
		// Setting these bits ensures GTA SA sets damage state to 2 (DAMAGED) instead of 4 (NOTPRESENT).
		if (pInfo->m_pVehicleStruct) {
			pInfo->m_pVehicleStruct->m_nMaskComponentsDamagable |= (1 << CAR_DOOR_RF) | (1 << CAR_DOOR_RR) | (1 << CAR_DOOR_LF) | (1 << CAR_DOOR_LR) | (1 << CAR_BONNET) | (1 << CAR_BOOT) | (1 << CAR_BUMP_FRONT) | (1 << CAR_BUMP_REAR) | (1 << CAR_WING_RF) | (1 << CAR_WING_LF) | (1 << CAR_WINDSCREEN);
		}

		// Copy tuning upgrades and remap TXD descriptions from base vehicle model info.
		// SetClump allocates a fresh CVehicleStructure from CPool<CVehicleStructure>, which leaves
		// m_aUpgrades (UpgradePosnDesc[18]), m_anUpgrades[18], and m_anRemapTxds[4] uninitialized/zeroed.
		// Without copying them, CVehicle::AddUpgrade (0x6DFA20) reads parentComponentId=0 (CAR_NODE_NONE),
		// causing CClumpModelInfo::GetFrameFromId to fail and tuning mods (spoilers, hoods, scoops, nitro)
		// to attach at (0,0,0) or fail completely.
		if (pBaseInfo && pBaseInfo->m_pVehicleStruct && pInfo->m_pVehicleStruct) {
			memcpy(pInfo->m_pVehicleStruct->m_aUpgrades, pBaseInfo->m_pVehicleStruct->m_aUpgrades, sizeof(pInfo->m_pVehicleStruct->m_aUpgrades));
			memcpy(pInfo->m_anUpgrades, pBaseInfo->m_anUpgrades, sizeof(pInfo->m_anUpgrades));
			memcpy(pInfo->m_anRemapTxds, pBaseInfo->m_anRemapTxds, sizeof(pInfo->m_anRemapTxds));
		}

		ClientLog(LogLevel::Debug, "FinalizeClump -> BEFORE SetAtomicRenderCallbacks");
		pInfo->SetAtomicRenderCallbacks();
		ClientLog(LogLevel::Debug, "FinalizeClump -> AFTER SetAtomicRenderCallbacks");

		// Auto-detect wheel sizes if model has wheel atomics under wheel dummies
		if (pInfo->m_fWheelSizeFront < 0.1f || (pBaseInfo && fabsf(pInfo->m_fWheelSizeFront - pBaseInfo->m_fWheelSizeFront) < 0.001f) || fabsf(pInfo->m_fWheelSizeFront - 0.7f) < 0.01f) {
			const char* testWheelDummies[] = { "wheel_rf_dummy", "wheel_lf_dummy", "wheel_rb_dummy", "wheel_lb_dummy" };
			for (const char* wd : testWheelDummies) {
				RwFrame* wf = CClumpModelInfo::GetFrameFromName(pClump, wd);
				if (wf) {
					struct WheelSizeFinder {
						float maxRadius { 0.0f };
					} wsf;

					auto checkAtomic = [](RwObject* obj, void* data) -> RwObject* {
						if (obj && obj->type == rpATOMIC) {
							RpAtomic* atomic = reinterpret_cast<RpAtomic*>(obj);
							RpGeometry* geom = RpAtomicGetGeometry(atomic);
							if (geom) {
								float r = 0.0f;
								const RwSphere* sph = RpAtomicGetBoundingSphere(atomic);
								if (sph && sph->radius > 0.1f && sph->radius < 1.5f) {
									r = sph->radius;
								} else if (geom->morphTarget && geom->morphTarget[0].verts && geom->numVertices > 0) {
									float maxRSq = 0.0f;
									for (int v = 0; v < geom->numVertices; ++v) {
										const RwV3d& pt = geom->morphTarget[0].verts[v];
										float rSq = pt.x * pt.x + pt.y * pt.y + pt.z * pt.z;
										if (rSq > maxRSq) maxRSq = rSq;
									}
									r = std::sqrt(maxRSq);
								}
								if (r > 0.15f && r < 1.5f) {
									auto* ctx = reinterpret_cast<WheelSizeFinder*>(data);
									ctx->maxRadius = std::max(ctx->maxRadius, r);
								}
							}
						}
						return obj;
					};

					RwFrameForAllObjects(wf, checkAtomic, &wsf);
					for (RwFrame* child = wf->child; child != nullptr; child = child->next) {
						RwFrameForAllObjects(child, checkAtomic, &wsf);
					}

					if (wsf.maxRadius > 0.15f) {
						pInfo->m_fWheelSizeFront = wsf.maxRadius * 2.0f;
						pInfo->m_fWheelSizeRear = wsf.maxRadius * 2.0f;
						ClientLog(LogLevel::Info, std::format("Auto-detected wheel size: {:.3f} (radius: {:.3f}) from '{}'", static_cast<float>(pInfo->m_fWheelSizeFront), static_cast<float>(wsf.maxRadius), wd));
						break;
					}
				}
			}
		}

		if (!pInfo->m_pPlateMaterial) {
			struct PlateFinderCtx {
				RpMaterial* foundMat { nullptr };
			} pfc;
			RpClumpForAllAtomics(pClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
				auto* ctx = reinterpret_cast<PlateFinderCtx*>(data);
				RpGeometry* geom = RpAtomicGetGeometry(atomic);
				if (!geom || !geom->matList.materials)
					return atomic;

				RwFrame* frame = RpAtomicGetFrame(atomic);
				std::string frameHierarchy;
				for (RwFrame* f = frame; f != nullptr; f = RwFrameGetParent(f)) {
					const char* n = GetFrameNodeName(f);
					if (n && *n) {
						frameHierarchy += " ";
						frameHierarchy += n;
					}
				}
				std::transform(frameHierarchy.begin(), frameHierarchy.end(), frameHierarchy.begin(), ::tolower);

				if (frameHierarchy.find("wheel") != std::string::npos || frameHierarchy.find("tyre") != std::string::npos || frameHierarchy.find("tire") != std::string::npos || frameHierarchy.find("brake") != std::string::npos || frameHierarchy.find("disc") != std::string::npos || frameHierarchy.find("susp") != std::string::npos) {
					return atomic;
				}

				for (int m = 0; m < geom->matList.numMaterials; ++m) {
					RpMaterial* mat = geom->matList.materials[m];
					if (!mat)
						continue;

					bool isPlate = false;
					if (mat->texture && mat->texture->name && mat->texture->name[0]) {
						std::string tn = mat->texture->name;
						std::transform(tn.begin(), tn.end(), tn.begin(), ::tolower);
						if (tn.find("template") == std::string::npos && tn.find("wheel") == std::string::npos && tn.find("tyre") == std::string::npos && tn.find("tire") == std::string::npos && tn.find("rim") == std::string::npos && tn.find("brake") == std::string::npos && tn.find("disc") == std::string::npos) {
							if (tn == "carplate" || tn == "numberplate" || tn == "number_plate" || tn.find("carplate") != std::string::npos || tn.find("numberplate") != std::string::npos || tn.find("license_plate") != std::string::npos || tn.find("licence_plate") != std::string::npos || tn.find("licenseplate") != std::string::npos || tn.find("licenceplate") != std::string::npos || tn.find("custom_plate") != std::string::npos || tn.find("nomer") != std::string::npos || tn == "plate" || tn == "license" || tn == "licence") {
								if (tn.find("carpback") == std::string::npos && tn.find("plateback") == std::string::npos) {
									isPlate = true;
								}
							}
						}
					}
					if (!isPlate && (frameHierarchy.find("bt_plate") != std::string::npos || frameHierarchy.find("carplate") != std::string::npos || frameHierarchy.find("numplate") != std::string::npos)) {
						isPlate = true;
					}

					if (isPlate) {
						ctx->foundMat = mat;
						return atomic;
					}
				}
				return atomic;
			},
				&pfc);

			if (pfc.foundMat) {
				pInfo->m_pPlateMaterial = pfc.foundMat;
				if (pInfo->m_szPlateText[0] != '\0') {
					CCustomCarPlateMgr::SetupMaterialPlateTexture(pfc.foundMat, pInfo->m_szPlateText, 0);
				}
				ClientLog(LogLevel::Info, std::format("FinalizeClump: modelInfo=0x{:X} plate material detected via fallback scanner (mat=0x{:X})", reinterpret_cast<std::uintptr_t>(pInfo), reinterpret_cast<std::uintptr_t>(pfc.foundMat)));
			}
		}

		// If the DFF contains an unmanaged "tuning" frame containing optional bodykit variations
		// (e.g. bumper_f0 vs bumper_f1, spoiler1, spoiler2, interior0 vs interior1), GTA:SA's visibility plugins
		// do not manage them, causing all variations to render simultaneously on top of each other.
		// Retain baseline factory parts (*0: bumper_f0, bumper_r0, interior0, splitter0, trunk_badge0)
		// and hide duplicate/aftermarket variations (*1, *2, *3, spoiler*, fenders*).
		RwFrame* tuningFrame = CClumpModelInfo::GetFrameFromName(pClump, "tuning");
		if (tuningFrame) {
			struct TuningFilterContext {
				RwFrame* root;
			} ctx { tuningFrame };
			RpClumpForAllAtomics(pClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
				auto* c = reinterpret_cast<TuningFilterContext*>(data);
				RwFrame* atomicFrame = RpAtomicGetFrame(atomic);
				for (RwFrame* f = atomicFrame; f != nullptr; f = RwFrameGetParent(f)) {
					if (f == c->root) {
						const char* nodeName = GetFrameNodeName(atomicFrame);
						std::string name = nodeName ? nodeName : "";
						std::transform(name.begin(), name.end(), name.begin(), ::tolower);

						// Hide aftermarket variations (*1, *2, *3, spoiler*, fenders*)
						// Keep baseline factory parts (*0: bumper_f0, bumper_r0, interior0, splitter0, trunk_badge0)
						bool hide = false;
						if (name.rfind("fenders", 0) == 0 || name.rfind("spoiler", 0) == 0) {
							hide = true;
						} else if (!name.empty()) {
							char lastChar = name.back();
							if (lastChar >= '1' && lastChar <= '9') {
								hide = true;
							}
						}

						if (hide) {
							RpAtomicSetFlags(atomic, 0);
						} else {
							RpAtomicSetFlags(atomic, rpATOMICRENDER);
						}
						break;
					}
				}
				return atomic;
			},
				&ctx);
		}

		// ExtractDummiesFromClump is a fallback only: it fills any dummy slot that
		// PreprocessHierarchy left as (0,0,0), using the RpClump frame hierarchy.
		if (pInfo->m_pVehicleStruct) {
			ExtractDummiesFromClump(pClump, pInfo->m_pVehicleStruct);

			// Explicitly zero secondary headlights (slot 2) and secondary taillights (slot 3)
			// if the custom model DFF does not contain headlights2 or taillights2 frames.
			if (CClumpModelInfo::GetFrameFromName(pClump, "headlights2") == nullptr) {
				pInfo->m_pVehicleStruct->m_avDummyPos[2] = CVector(0.0f, 0.0f, 0.0f);
			}
			if (CClumpModelInfo::GetFrameFromName(pClump, "taillights2") == nullptr) {
				pInfo->m_pVehicleStruct->m_avDummyPos[3] = CVector(0.0f, 0.0f, 0.0f);
			}

			if (pBaseInfo && pBaseInfo->m_pVehicleStruct) {
				// Fallback any (0,0,0) dummy positions from base vehicle
				for (int i = 0; i < 15; ++i) {
					// DO NOT fallback auxiliary/secondary lights (slot 2: LIGHT_FRONT_SECONDARY, slot 3: LIGHT_REAR_SECONDARY).
					// If the custom model does not have secondary lights in its DFF, inheriting them from the base vehicle
					// causes phantom/duplicate headlights (e.g. Turismo's secondary driving lights) to render floating!
					if (i == 2 || i == 3)
						continue;

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
		if (it != s_customModels.end()) {
			return it->second->modelInfo;
		}
		return nullptr;
	}

	static bool IsCustomModel(uint32_t id)
	{
		return s_customModels.contains(id);
	}

	static int GetGtaModelSlot(uint32_t customId)
	{
		auto it = s_customModels.find(customId);
		if (it != s_customModels.end() && it->second) {
			return it->second->gtaModelSlot;
		}
		return -1;
	}

	static bool IsGtaSlotUsedByVehicle(int gtaSlot)
	{
		auto* pool = CPools::ms_pVehiclePool;
		if (!pool || gtaSlot < 0)
			return false;
		for (int i = 0; i < pool->m_nSize; ++i) {
			CVehicle* veh = pool->GetAt(i);
			if (veh && veh->m_nModelIndex == gtaSlot)
				return true;
		}
		return false;
	}

	static void FreeCustomModelInfo(CVehicleModelInfo* pInfo)
	{
		if (pInfo) {
			// NOTE: Do NOT call `delete pInfo`.
			// `newModel` was allocated on the plugin's CRT heap, but `memcpy` from `vBaseInfo`
			// copied GTA SA's `CVehicleModelInfo` vtable. Polymorphic `delete pInfo` invokes
			// GTA's virtual scalar deleting destructor, which calls GTA's internal allocator
			// to free memory from the plugin's heap -> APPLICATION_VERIFIER_HEAPS_SWITCHED_HEAP_HANDLE.
			// All GTA resources (RW clump, col model, TXD) are already cleaned up before this call,
			// so we directly release the raw memory back to the plugin's CRT heap without dispatching
			// the virtual destructor.
			::operator delete(static_cast<void*>(pInfo));
		}
	}

	static void SafeFreeCustomColModel(CColModel* col)
	{
		if (!col)
			return;
		if (col->m_pColData) {
			col->m_nFlags = 0; // Clear bit 2 (0x04) so ~CColModel() won't attempt to free sub-arrays or link list!
			auto pFree = reinterpret_cast<void(__cdecl*)(void*)>(0x72F430);
			pFree(col->m_pColData);
			col->m_pColData = nullptr;
		}
		reinterpret_cast<void(__cdecl*)(CColModel*)>(0x40fc40)(col);
	}

	static void DestroyCustomModel(uint32_t customId)
	{
		auto it = s_customModels.find(customId);
		if (it == s_customModels.end())
			return;

		CustomModelEntry* modelEntry = it->second;
		if (!modelEntry) {
			s_customModels.erase(it);
			return;
		}

		// Check if the model is still in use by SA-MP or GTA vehicle pool
		if (HandlingManager::GetModelUseCount(customId) > 0 ||
			(modelEntry->gtaModelSlot >= 0 && IsGtaSlotUsedByVehicle(modelEntry->gtaModelSlot))) {
			// Re-queue the destruction command
			if (s_destructionRequeueCallback) {
				s_destructionRequeueCallback(customId);
			}
			return;
		}

		// Proceed with destruction...
		CVehicleModelInfo* pInfo = modelEntry->modelInfo;
		if (pInfo) {
			if (modelEntry->gtaModelSlot >= 0 && modelEntry->gtaModelSlot < TOTAL_GTA_MODEL_COUNT)
				GetModelInfoTable()[modelEntry->gtaModelSlot] = nullptr;

			pInfo->DeleteRwObject();
			if (pInfo->bDoWeOwnTheColModel && pInfo->m_pColModel) {
				SafeFreeCustomColModel(pInfo->m_pColModel);
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
			FreeCustomModelInfo(pInfo);
		}
		delete modelEntry;
		s_customModels.erase(it);
		AudioExtender::UnregisterVehicleAudio(customId);
	}

	static void ClearAllCustomModels()
	{
		// Move-steal the map so s_customModels is empty *before* teardown begins.
		// If DeleteRwObject or any other cleanup step throws, AllocateGtaModelSlot
		// will never see stale entries claiming slots that are null in the GTA table.
		auto models = std::move(s_customModels); // s_customModels is now empty
		for (auto& [id, entry] : models) {
			if (!entry)
				continue;

			// If a vehicle in GTA's world is still using this model slot (e.g. on kick/disconnect
			// before SA-MP removes vehicles), do NOT free the model info or RW clump yet.
			// Freeing it while a vehicle still exists causes an immediate crash on the next frame's render.
			if (entry->gtaModelSlot >= 0 && IsGtaSlotUsedByVehicle(entry->gtaModelSlot)) {
				s_customModels[id] = entry;
				continue;
			}

			CVehicleModelInfo* pInfo = entry->modelInfo;
			if (pInfo) {
				if (entry->gtaModelSlot >= 0 && entry->gtaModelSlot < TOTAL_GTA_MODEL_COUNT)
					GetModelInfoTable()[entry->gtaModelSlot] = nullptr;

				pInfo->DeleteRwObject();
				if (pInfo->bDoWeOwnTheColModel && pInfo->m_pColModel) {
					SafeFreeCustomColModel(pInfo->m_pColModel);
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
				FreeCustomModelInfo(pInfo);
			}
			delete entry;
			AudioExtender::UnregisterVehicleAudio(id);
		}
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
