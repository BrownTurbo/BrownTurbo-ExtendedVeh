// SDK
#include <plugin_sa.h>

#include <game_sa/CAutomobile.h>
#include <game_sa/CBike.h>
#include <game_sa/CCheat.h>
#include <game_sa/CHandlingDataMgr.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CPad.h>
#include <game_sa/CTimer.h>
#include <game_sa/CTxdStore.h>
#include <game_sa/CVisibilityPlugins.h>
#include <game_sa/CWaterLevel.h>
#include <game_sa/CVehicle.h>
#include <game_sa/CCoronas.h>
#include <game_sa/CShadows.h>
#include <game_sa/rw/rpworld.h>
#include <MinHook.h>
#include <shared/game/CVector.h>
#include <algorithm>
#include <format>
#include <string>

#include "utils.h"

static inline void SetWaterDriveCheatActive(bool active)
{
	CCheat::m_aCheatsActive[CHEAT_CARS_ON_WATER] = active;
	*reinterpret_cast<bool*>(0x969152) = active;
}

// Hook function pointers
static void(__fastcall* g_origUpdateWheelMatrix)(CAutomobile* thisCar, void* edx, int nodeIndex, int flags) = nullptr;

static void __fastcall Hooked_UpdateWheelMatrix(CAutomobile* thisCar, void* edx, int nodeIndex, int flags)
{
	if (!thisCar || !IsVehiclePointerValid(thisCar) || !thisCar->m_pHandlingData) {
		if (g_origUpdateWheelMatrix)
			g_origUpdateWheelMatrix(thisCar, edx, nodeIndex, flags);
		return;
	}

	bool isCapable = (thisCar->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || thisCar->m_nVehicleSubClass == VEHICLE_MTRUCK || thisCar->m_nVehicleSubClass == VEHICLE_QUAD);
	bool isBoat = isCapable && ((thisCar->m_pHandlingData->m_nModelFlags & 0x8000000) != 0);

	if (isBoat) {
		CVector pos = thisCar->GetPosition();
		float waterZ = 0.0f;
		bool inWater = thisCar->bTouchingWater || thisCar->bSubmergedInWater;
		if (!inWater && CWaterLevel::GetWaterLevelNoWaves(pos.x, pos.y, pos.z, &waterZ)) {
			if (pos.z <= (waterZ + 1.2f)) {
				inWater = true;
			}
		}

		bool wasWaterCheat = *reinterpret_cast<bool*>(0x969152);

		if (inWater) {
			*reinterpret_cast<bool*>(0x969152) = true;

			float savedComp[4];
			for (int i = 0; i < 4; ++i) {
				savedComp[i] = thisCar->m_fWheelsSuspensionCompressionPrev[i];
				thisCar->m_fWheelsSuspensionCompressionPrev[i] = 0.5f; // < 1.0f ensures GTA SA (0x6AA74B) rotates wheel into boat mode
			}

			bool wasDrowning = thisCar->bIsDrowning;
			thisCar->bIsDrowning = false;

			auto savedFlags = thisCar->m_pHandlingData->m_nModelFlags;
			thisCar->m_pHandlingData->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
				savedFlags & ~(0x00200000 | 0x00020000) // Clear solid axle flags so all wheels rotate
			);

			g_origUpdateWheelMatrix(thisCar, edx, nodeIndex, flags);

			thisCar->m_pHandlingData->m_nModelFlags = savedFlags;
			thisCar->bIsDrowning = wasDrowning;
			for (int i = 0; i < 4; ++i) {
				thisCar->m_fWheelsSuspensionCompressionPrev[i] = savedComp[i];
			}
			*reinterpret_cast<bool*>(0x969152) = wasWaterCheat;
			return;
		} else {
			// On land, keep wheels vertical even if local player has water drive cheat active for physics
			*reinterpret_cast<bool*>(0x969152) = false;
			g_origUpdateWheelMatrix(thisCar, edx, nodeIndex, flags);
			*reinterpret_cast<bool*>(0x969152) = wasWaterCheat;
			return;
		}
	}

	if (g_origUpdateWheelMatrix)
		g_origUpdateWheelMatrix(thisCar, edx, nodeIndex, flags);
}

// Vehicle Headlights / Taillights Dynamic Scaling Hooks
static bool(__fastcall* g_origDoHeadLightEffect)(CVehicle* thisVehicle, void* edx, int dummyId, CMatrix& vehicleMatrix, unsigned char lightId, unsigned char lightState) = nullptr;
static bool(__fastcall* g_origDoTailLightEffect)(CVehicle* thisVehicle, void* edx, int lightId, CMatrix& matrix, unsigned char arg2, unsigned char arg3, unsigned int arg4, unsigned char arg5) = nullptr;

static void(__cdecl* g_origRegisterCoronaTexture)(
	unsigned int id, CEntity* attachTo, unsigned char red, unsigned char green, unsigned char blue,
	unsigned char alpha, CVector const& posn, float radius, float farClip, RwTexture* texture, eCoronaFlareType flaretype,
	bool enableReflection, bool checkObstacles, int _param_not_used, float angle, bool longDistance, float nearClip,
	unsigned char fadeState, float fadeSpeed, bool onlyFromBelow, bool reflectionDelay) = nullptr;

static void(__cdecl* g_origStoreCarLightShadow)(
	CVehicle* vehicle, int id, RwTexture* texture, CVector* posn,
	float frontX, float frontY, float sideX, float sideY,
	unsigned char red, unsigned char green, unsigned char blue,
	float maxViewAngle) = nullptr;

static CVehicle* s_pCurrentHeadLightVehicle = nullptr;
static CVehicle* s_pCurrentTailLightVehicle = nullptr;

struct LightScaleConfig {
	float coronaScale;
	float shadowFrontScale;
	float shadowSideScale;
};

static inline LightScaleConfig GetVehicleLightScaleConfig(uint8_t lightSize, bool isRear = false)
{
	if (isRear) {
		switch (lightSize) {
		case 1: // LIGHTS_SMALL: tiny pinpoint dot
			return { 0.35f, 0.50f, 0.50f };
		case 0: // LIGHTS_LONG: wide horizontal light bar
			return { 1.30f, 1.00f, 1.80f };
		case 2: // LIGHTS_BIG: massive round glowing red aura
			return { 3.50f, 2.00f, 2.00f };
		case 3: // LIGHTS_TALL: vertically elongated tall light strip
			return { 1.30f, 2.00f, 1.00f };
		default:
			return { 1.0f, 1.0f, 1.0f };
		}
	}

	switch (lightSize) {
	case 1: // LIGHTS_SMALL
		return { 0.40f, 0.45f, 0.45f };
	case 0: // LIGHTS_LONG
		return { 1.0f, 1.0f, 1.0f };
	case 2: // LIGHTS_BIG
		return { 2.40f, 1.80f, 1.80f };
	case 3: // LIGHTS_TALL
		return { 3.20f, 2.40f, 1.25f };
	default:
		return { 1.0f, 1.0f, 1.0f };
	}
}

static bool __fastcall Hooked_DoHeadLightEffect(CVehicle* thisVehicle, void* edx, int dummyId, CMatrix& vehicleMatrix, unsigned char lightId, unsigned char lightState)
{
	s_pCurrentHeadLightVehicle = thisVehicle;
	bool res = g_origDoHeadLightEffect ? g_origDoHeadLightEffect(thisVehicle, edx, dummyId, vehicleMatrix, lightId, lightState) : false;
	s_pCurrentHeadLightVehicle = nullptr;
	return res;
}

static bool __fastcall Hooked_DoTailLightEffect(CVehicle* thisVehicle, void* edx, int lightId, CMatrix& matrix, unsigned char arg2, unsigned char arg3, unsigned int arg4, unsigned char arg5)
{
	s_pCurrentTailLightVehicle = thisVehicle;
	// In GTA SA, arg4 is bSkipCorona (1 skips corona registration for running lights, 0 draws it).
	// arg5 is bCalculateColor (1 calculates red brightness and color, 0 drops it).
	// Setting arg4 = 0 and arg5 = 1 forces taillights to render their red glowing lens coronas at night!
	arg4 = 0;
	arg5 = 1;
	bool res = g_origDoTailLightEffect ? g_origDoTailLightEffect(thisVehicle, edx, lightId, matrix, arg2, arg3, arg4, arg5) : false;
	s_pCurrentTailLightVehicle = nullptr;
	return res;
}

static void __cdecl Hooked_RegisterCoronaTexture(
	unsigned int id, CEntity* attachTo, unsigned char red, unsigned char green, unsigned char blue,
	unsigned char alpha, CVector const& posn, float radius, float farClip, RwTexture* texture, eCoronaFlareType flaretype,
	bool enableReflection, bool checkObstacles, int _param_not_used, float angle, bool longDistance, float nearClip,
	unsigned char fadeState, float fadeSpeed, bool onlyFromBelow, bool reflectionDelay)
{
	CVehicle* pVeh = nullptr;
	bool isFront = false;
	bool isRear = false;

	if (s_pCurrentHeadLightVehicle) {
		pVeh = s_pCurrentHeadLightVehicle;
		isFront = true;
	} else if (s_pCurrentTailLightVehicle) {
		pVeh = s_pCurrentTailLightVehicle;
		isRear = true;
	} else if (attachTo) {
		// In GTA SA CEntity, byte offset 0x36 contains m_nType (lower 3 bits). 2 == ENTITY_TYPE_VEHICLE
		uint8_t entityType = (*reinterpret_cast<const uint8_t*>(reinterpret_cast<const char*>(attachTo) + 0x36)) & 0x7;
		if (entityType == 2) {
			pVeh = reinterpret_cast<CVehicle*>(attachTo);
			if (red > 100 && green < 80 && blue < 80) {
				isRear = true;
			} else {
				isFront = true;
			}
		}
	}

	float scale = 1.0f;
	uint8_t lightSize = 0;
	if (pVeh && pVeh->m_pHandlingData) {
		lightSize = isFront
			? static_cast<uint8_t>(pVeh->m_pHandlingData->m_nFrontLights)
			: static_cast<uint8_t>(pVeh->m_pHandlingData->m_nRearLights);
		LightScaleConfig cfg = GetVehicleLightScaleConfig(lightSize, isRear);
		scale = cfg.coronaScale;

		static uint32_t s_lastLog = 0;
		uint32_t now = GetTickCount();
		if (scale != 1.0f && (now - s_lastLog > 2000)) {
			s_lastLog = now;
			ClientLog(std::format("[Client] Corona scaled: isFront={}, isRear={}, size={}, scale={:.2f}, radius={:.2f}->{:.2f}",
				isFront, isRear, static_cast<int>(lightSize), scale, radius, radius * scale));
		}
	}

	radius *= scale;
	if (scale > 1.0f) {
		farClip *= (1.0f + (scale - 1.0f) * 0.5f);
		if (isRear) {
			alpha = static_cast<unsigned char>(std::min(255, static_cast<int>(alpha * 1.6f)));
			if (red < 200) red = 220;
		}
	} else if (scale < 1.0f) {
		farClip *= scale;
	}

	if (g_origRegisterCoronaTexture) {
		if (isRear && pVeh) {
			if (lightSize == 0) { // LIGHTS_LONG: horizontally elongated wide bar
				float barRadius = radius * 0.75f;
				g_origRegisterCoronaTexture(id, attachTo, red, green, blue, alpha, posn, barRadius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);

				// In local coordinates (when attachTo != nullptr), X is vehicle width (Right/Left).
				// In world coordinates (when attachTo == nullptr), use vehicle matrix right vector.
				CVector dirRight(1.0f, 0.0f, 0.0f);
				if (!attachTo && pVeh->m_matrix) {
					dirRight = pVeh->m_matrix->right;
				}

				float offsetDist = radius * 0.55f;
				CVector posL = posn - dirRight * offsetDist;
				CVector posR = posn + dirRight * offsetDist;

				uint32_t subId1 = id ^ 0x24000001;
				uint32_t subId2 = id ^ 0x48000001;

				g_origRegisterCoronaTexture(subId1, attachTo, red, green, blue, alpha, posL, barRadius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
				g_origRegisterCoronaTexture(subId2, attachTo, red, green, blue, alpha, posR, barRadius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
			} else if (lightSize == 3) { // LIGHTS_TALL: vertically elongated tall column
				float barRadius = radius * 0.75f;
				g_origRegisterCoronaTexture(id, attachTo, red, green, blue, alpha, posn, barRadius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);

				// In local coordinates (when attachTo != nullptr), Z is vehicle height (Up/Down).
				// In world coordinates (when attachTo == nullptr), use vehicle matrix at (up) vector.
				CVector dirUp(0.0f, 0.0f, 1.0f);
				if (!attachTo && pVeh->m_matrix) {
					dirUp = pVeh->m_matrix->at;
				}

				float offsetDist = radius * 0.55f;
				CVector posUp = posn + dirUp * offsetDist;
				CVector posDn = posn - dirUp * offsetDist;

				uint32_t subId1 = id ^ 0x24000001;
				uint32_t subId2 = id ^ 0x48000001;

				g_origRegisterCoronaTexture(subId1, attachTo, red, green, blue, alpha, posUp, barRadius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
				g_origRegisterCoronaTexture(subId2, attachTo, red, green, blue, alpha, posDn, barRadius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
			} else {
				// LIGHTS_SMALL (1), LIGHTS_BIG (2), or default:
				g_origRegisterCoronaTexture(id, attachTo, red, green, blue, alpha, posn, radius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
			}
		} else {
			g_origRegisterCoronaTexture(id, attachTo, red, green, blue, alpha, posn, radius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
		}
	}
}

static void __cdecl Hooked_StoreCarLightShadow(
	CVehicle* vehicle, int id, RwTexture* texture, CVector* posn,
	float frontX, float frontY, float sideX, float sideY,
	unsigned char red, unsigned char green, unsigned char blue,
	float maxViewAngle)
{
	CVehicle* pVeh = vehicle;
	if (!pVeh) {
		if (s_pCurrentHeadLightVehicle) pVeh = s_pCurrentHeadLightVehicle;
		else if (s_pCurrentTailLightVehicle) pVeh = s_pCurrentTailLightVehicle;
	}

	CVector modifiedPosn = posn ? *posn : CVector(0.0f, 0.0f, 0.0f);

	if (pVeh && pVeh->m_pHandlingData) {
		bool isFront = (s_pCurrentHeadLightVehicle != nullptr) || !(red > 100 && green < 50 && blue < 50);
		uint8_t lightSize = isFront
			? static_cast<uint8_t>(pVeh->m_pHandlingData->m_nFrontLights)
			: static_cast<uint8_t>(pVeh->m_pHandlingData->m_nRearLights);

		LightScaleConfig cfg = GetVehicleLightScaleConfig(lightSize, !isFront);

		// Because posn is the center of the shadow quad, scaling frontX/frontY expands the shadow symmetrically
		// both forward and backward. To prevent the light from spilling backward under the vehicle chassis,
		// shift the center forward by front * (scale - 1.0f). This keeps the back edge firmly pinned at the bumper!
		float shiftFactor = cfg.shadowFrontScale - 1.0f;
		if (posn) {
			modifiedPosn.x += frontX * shiftFactor;
			modifiedPosn.y += frontY * shiftFactor;
		}

		frontX *= cfg.shadowFrontScale;
		frontY *= cfg.shadowFrontScale;
		sideX *= cfg.shadowSideScale;
		sideY *= cfg.shadowSideScale;

		static uint32_t s_lastShadowLog = 0;
		uint32_t now = GetTickCount();
		if ((cfg.shadowFrontScale != 1.0f || cfg.shadowSideScale != 1.0f) && (now - s_lastShadowLog > 2000)) {
			s_lastShadowLog = now;
			ClientLog(std::format("[Client] CarLightShadow scaled: isFront={}, size={}, frontScale={:.2f}, sideScale={:.2f}",
				isFront, static_cast<int>(lightSize), cfg.shadowFrontScale, cfg.shadowSideScale));
		}
	}

	if (g_origStoreCarLightShadow) {
		g_origStoreCarLightShadow(vehicle, id, texture, posn ? &modifiedPosn : nullptr, frontX, frontY, sideX, sideY, red, green, blue, maxViewAngle);
	}
}

#include <windows.h>
#include <iostream>

#include <sampapi/CChat.h>
#include <sampapi/CNetGame.h>
#include <sampapi/CVehiclePool.h>
#include <sampapi/sampapi.h>

#include "utils.h"

#include <RakHook/rakhook.hpp>
#include <RakHook/samp.hpp>
#include <RakNet/BitStream.h>
#include <RakNet/PacketEnumerations.h>
#include <RakNet/StringCompressor.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "defs.h"
#include "handling_manager.hpp"

#include "audioextender.hpp"
#include "binaryrwparser.hpp"
#include "streamingextender.hpp"

#include "../Shared/CustomVehicleProtocol.hpp"
#include "CollisionLoader.h"
#include "CryptoUtility.h"
#include "CustomVehicleBindingManager.h"
#include "ImGuiOverlay.h"
#include "MainThreadQueue.h"
#include "ModelCache.h"

namespace fs = std::filesystem;
using namespace plugin;

ExtendedVeh::Collision::CollisionLoader* colLoader = &ExtendedVeh::Collision::CollisionLoader::Instance();

class CustomVehiclesASI {
private:
	enum class AssetState {
		Pending,
		Ready,
		Failed
	};

	struct PendingCustomVehicle {
		CustomVeh::Protocol::VehicleDefinition def;
		CVehicleModelInfo* modelInfo = nullptr;
		std::vector<uint8_t> txd;
		std::vector<uint8_t> dff;
		std::vector<uint8_t> col;
		fs::path dffPath;
		AssetState dffState = AssetState::Pending;
		fs::path txdPath;
		AssetState txdState = AssetState::Pending;
		fs::path colPath;
		AssetState colState = AssetState::Pending;
		bool queuedForFinalize = false;
		mutable std::mutex assetMutex;
	};

	std::queue<std::shared_ptr<PendingCustomVehicle>> m_completedQueue;
	std::mutex m_queueMutex;
	std::mutex m_pendingDefMutex;
	std::queue<std::shared_ptr<PendingCustomVehicle>> m_pendingDefQueue;
	std::atomic<bool> m_pendingClearAll { false };
	std::queue<uint32_t> m_destructionQueue;
	std::mutex m_destructionMutex;
	bool m_runtimeInitialized = false;

public:
	bool ReadAssetDescriptor(RakNet::BitStream& bs, CustomVeh::Protocol::AssetDescriptor& asset)
	{
		if (!bs.Read(asset.type))
			return false;

		if (!bs.Read(asset.size))
			return false;

		if (!bs.Read(asset.compressedSize))
			return false;

		if (!bs.Read(asset.chunkSize))
			return false;

		if (!bs.Read(asset.chunkCount))
			return false;

		if (!bs.Read(asset.sha256, CustomVeh::Protocol::SHA256_BUFFER_SIZE))
			return false;
		asset.sha256[CustomVeh::Protocol::SHA256_BUFFER_SIZE - 1] = '\0';

		if (!bs.Read(asset.filename, CustomVeh::Protocol::FILENAME_SIZE))
			return false;
		asset.filename[CustomVeh::Protocol::FILENAME_SIZE - 1] = '\0';

		return true;
	}

	bool ReadVehicleDefinition(RakNet::BitStream& bs, CustomVeh::Protocol::VehicleDefinition& def)
	{
		if (!bs.Read(def.customModelId))
			return false;
		if (!bs.Read(def.visualBaseModel))
			return false;
		if (!bs.Read(def.handlingBaseModel))
			return false;
		if (!bs.Read(def.audioBaseModel))
			return false;
		if (!bs.Read(def.engineSoundId.OnSound))
			return false;
		if (!bs.Read(def.engineSoundId.OffSound))
			return false;
		if (!bs.Read(def.celerateSoundId.accelerateSound))
			return false;
		if (!bs.Read(def.celerateSoundId.decelerateSound))
			return false;
		if (!bs.Read(def.flags))
			return false;
		if (!ReadAssetDescriptor(bs, def.dff))
			return false;
		if (!ReadAssetDescriptor(bs, def.txd))
			return false;
		if (!ReadAssetDescriptor(bs, def.col))
			return false;

		return true;
	}

private:
	void CreateModelAndBeginTransfers(std::shared_ptr<PendingCustomVehicle> pending)
	{
		pending->modelInfo = StreamingExtender::CreateCustomModel(pending->def);
		if (!pending->modelInfo) {
			return;
		}

		auto pushToQueue = [this, pending]() {
			if (pending->queuedForFinalize)
				return;
			pending->queuedForFinalize = true;

			std::lock_guard lock(m_queueMutex);
			m_completedQueue.push(pending);
		};

		auto beginCol = [this, pending, pushToQueue]() {
			if (pending->def.col.filename[0] == '\0') {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->colState = AssetState::Ready;
				}
				pushToQueue();
				return;
			}
			ModelTransferClient::Instance().RequestFile(
				pending->def.customModelId, ModelFileKind::Col, pending->def.col.sha256,
				[pending, pushToQueue](bool ok, const fs::path& path) {
					{
						std::lock_guard<std::mutex> lock(pending->assetMutex);
						if (ok) {
							pending->colPath = path;
							pending->colState = AssetState::Ready;
						} else {
							pending->colState = AssetState::Failed;
						}
					}
					pushToQueue();
				});
		};

		auto beginDff = [this, pending, beginCol, pushToQueue](bool ok, const fs::path& path) {
			if (!ok) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
				pushToQueue();
				return;
			}

			{
				std::lock_guard<std::mutex> lock(pending->assetMutex);
				pending->dffPath = path;
			}

			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
				pushToQueue();
				return;
			}
			const auto size = file.tellg();
			if (size <= 0) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
				pushToQueue();
				return;
			}

			file.seekg(0, std::ios::beg);

			std::vector<std::uint8_t> raw(static_cast<std::size_t>(size));

			if (!file.read(reinterpret_cast<char*>(raw.data()), size)) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
				pushToQueue();
				return;
			}

			pending->dff = BinaryRwParser::ExtractClump(raw);
			if (pending->dff.empty()) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
				pushToQueue();
				return;
			}
			{
				std::lock_guard<std::mutex> lock(pending->assetMutex);
				pending->dffState = AssetState::Ready;
			}

			beginCol();
		};

		ModelTransferClient::Instance().RequestFile(
			pending->def.customModelId, ModelFileKind::Txd, pending->def.txd.sha256,
			[this, pending, beginDff, pushToQueue](bool ok, const fs::path& path) {
				if (!ok) {
					{
						std::lock_guard<std::mutex> lock(pending->assetMutex);
						pending->txdState = AssetState::Failed;
						pending->dffState = AssetState::Failed;
						pending->colState = AssetState::Failed;
					}
					pushToQueue();
					return;
				}

				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->txdPath = path;
					pending->txdState = AssetState::Ready;
				}

				ModelTransferClient::Instance().RequestFile(pending->def.customModelId, ModelFileKind::Dff, pending->def.dff.sha256, beginDff);
			});
	}

	void FinalizeCustomVehicle(std::shared_ptr<PendingCustomVehicle> pending)
	{
		if (pending->dffState != AssetState::Ready || pending->txdState != AssetState::Ready) {
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("[CustomVeh] Failed to load essential assets for model {}", pending->def.customModelId).c_str());
			return;
		}

		CVehicleModelInfo* newModel = pending->modelInfo;
		if (!newModel)
			return;

		if (pending->txd.empty() && !pending->txdPath.empty()) {
			std::ifstream file(pending->txdPath, std::ios::binary | std::ios::ate);
			if (file) {
				const auto size = file.tellg();
				if (size > 0) {
					file.seekg(0, std::ios::beg);
					pending->txd.resize(static_cast<std::size_t>(size));
					file.read(reinterpret_cast<char*>(pending->txd.data()), size);
				}
			}
		}

		if (pending->txd.empty()) {
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("[CustomVeh] TXD file empty or unreadable for model {}", pending->def.customModelId).c_str());
			return;
		}

		int txdSlot = CTxdStore::AddTxdSlot(std::format("custom_veh_{}", pending->def.customModelId).c_str());

		RwMemory txdMem { pending->txd.data(), static_cast<RwUInt32>(pending->txd.size()) };
		RwStream* txdStream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &txdMem);

		if (!txdStream) {
			CTxdStore::RemoveTxdSlot(txdSlot);
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("[CustomVeh] Failed to open TXD stream for model {}", pending->def.customModelId).c_str());
			return;
		}

		if (!CTxdStore::LoadTxd(txdSlot, txdStream)) {
			RwStreamClose(txdStream, nullptr);
			CTxdStore::RemoveTxdSlot(txdSlot);
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("[CustomVeh] Failed to load TXD for model {}", pending->def.customModelId).c_str());
			return;
		}

		RwStreamClose(txdStream, nullptr);

		CTxdStore::AddRef(txdSlot);
		newModel->m_nTxdIndex = txdSlot;

		CTxdStore::PushCurrentTxd();
		CTxdStore::SetCurrentTxd(txdSlot);

		RwMemory dffMem { pending->dff.data(), static_cast<RwUInt32>(pending->dff.size()) };
		RwStream* dffStream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &dffMem);
		if (dffStream != nullptr) {
			RpClump* pClump = RpClumpStreamRead(dffStream);
			if (pClump) {
				if (!StreamingExtender::FinalizeClump(newModel, pClump)) {
					CTxdStore::RemoveTxdSlot(txdSlot);
					StreamingExtender::DestroyCustomModel(pending->def.customModelId);
					SendMsg(0xFF0000, std::format("[CustomVeh] Failed to finalize clump for model {}", pending->def.customModelId).c_str());
				}
				if (pending->colState == AssetState::Ready && (pending->def.flags & CustomVeh::Protocol::HasCol) != 0) {
					if (pending->col.empty() && !pending->colPath.empty()) {
						std::ifstream colFile(pending->colPath, std::ios::binary | std::ios::ate);
						if (colFile) {
							const auto cSize = colFile.tellg();
							if (cSize > 0) {
								colFile.seekg(0, std::ios::beg);
								pending->col.resize(static_cast<std::size_t>(cSize));
								colFile.read(reinterpret_cast<char*>(pending->col.data()), cSize);
							}
						}
					}
					if (!pending->col.empty()) {
						ExtendedVeh::Collision::CollisionLoader* colLoader = &ExtendedVeh::Collision::CollisionLoader::Instance();
						if (!colLoader->LoadCollisionFromMemory(pending->col.data(), pending->col.size(), newModel)) {
							SendMsg(0xFF8800, std::format("[CustomVeh] Warning: Failed to parse COL for model {}", pending->def.customModelId).c_str());
						}
					}
				}
			} else {
				CTxdStore::RemoveTxdSlot(txdSlot);
				StreamingExtender::DestroyCustomModel(pending->def.customModelId);
				SendMsg(0xFF0000, std::format("[CustomVeh] Failed to parse DFF for model {}", pending->def.customModelId).c_str());
			}
			RwStreamClose(dffStream, nullptr);
		} else {
			CTxdStore::RemoveTxdSlot(txdSlot);
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("[CustomVeh] Failed to open DFF stream for model {}", pending->def.customModelId).c_str());
		}

		CTxdStore::PopCurrentTxd();
	}

	bool IsCustomModelCurrentlyInUse(uint32_t customModelId)
	{
		return HandlingManager::GetModelUseCount(customModelId) > 0;
	}

public:
	CustomVehiclesASI()
	{
		Events::initRwEvent.Add([this]() {
			if (!m_runtimeInitialized) {
				fs::create_directories(GetSampCacheRoot());
				AudioExtender::InstallHooks();
				TransferConfig::Instance().Load();
				ModelCache::Instance().Sweep();
				m_runtimeInitialized = true;
			}
		});

		Events::shutdownRwEvent.Add([this]() {
			ModelTransferClient::Instance().CancelAll("client shutdown");
			ModelTransferClient::Instance().Shutdown();
			AudioExtender::RestoreHooks();
			StreamingExtender::ClearAllCustomModels();
			m_runtimeInitialized = false;
		});

		StreamingExtender::SetDestructionCallback([](uint32_t modelId) {
			_customVehInstance.PushDestructionCommand(modelId);
		});
	}

	void ProcessCompletedDownloads()
	{
		std::queue<std::shared_ptr<PendingCustomVehicle>> localQueue;
		{
			std::lock_guard<std::mutex> lock(m_queueMutex);
			if (m_completedQueue.empty())
				return;
			localQueue.swap(m_completedQueue);
		}

		while (!localQueue.empty()) {
			FinalizeCustomVehicle(localQueue.front());
			localQueue.pop();
		}
	}

	void HandleCustomVehicleDef(const CustomVeh::Protocol::VehicleDefinition& def)
	{
		auto pending = std::make_shared<PendingCustomVehicle>();
		pending->def = def;

		std::lock_guard<std::mutex> lock(m_pendingDefMutex);
		m_pendingDefQueue.push(pending);
	}

	void PushDestructionCommand(uint32_t modelId)
	{
		std::lock_guard<std::mutex> lock(m_destructionMutex);
		m_destructionQueue.push(modelId);
	}

	void ProcessPendingDefinitions()
	{
		std::queue<std::shared_ptr<PendingCustomVehicle>> localQueue;
		{
			std::lock_guard<std::mutex> lock(m_pendingDefMutex);
			if (m_pendingDefQueue.empty())
				return;
			localQueue.swap(m_pendingDefQueue);
		}

		while (!localQueue.empty()) {
			auto pending = localQueue.front();
			localQueue.pop();
			AudioExtender::RegisterVehicleAudio(pending->def.customModelId, pending->def.audioBaseModel, pending->def.engineSoundId.OnSound, pending->def.engineSoundId.OffSound, pending->def.celerateSoundId.accelerateSound, pending->def.celerateSoundId.decelerateSound);
			CreateModelAndBeginTransfers(pending);
		}
	}

	void ProcessPendingDestructions()
	{
		std::queue<uint32_t> localQueue;
		{
			std::lock_guard<std::mutex> lock(m_destructionMutex);
			if (m_destructionQueue.empty())
				return;
			localQueue.swap(m_destructionQueue);
		}

		while (!localQueue.empty()) {
			uint32_t modelIdToDestroy = localQueue.front();
			localQueue.pop();

			if (!IsCustomModelCurrentlyInUse(modelIdToDestroy)) {
				StreamingExtender::DestroyCustomModel(modelIdToDestroy);
			} else {
				PushDestructionCommand(modelIdToDestroy);
			}
		}
	}

	void RequestClearAllCustomModels()
	{
		m_pendingClearAll.store(true, std::memory_order_relaxed);
	}

	void ProcessPendingClearAll()
	{
		if (m_pendingClearAll.exchange(false, std::memory_order_relaxed)) {
			StreamingExtender::ClearAllCustomModels();
		}
	}

	void onPlayerStreamIn(uint16_t playerId)
	{
		MainThreadQueue::Instance().Push([playerId]() {
			//
		});
	}

	void onPlayerStreamOut(uint16_t playerId)
	{
		MainThreadQueue::Instance().Push([playerId]() {
			HandlingManager::RemovePlayerHandling(playerId, nullptr);
		});
	}

	~CustomVehiclesASI() = default;
} _customVehInstance;

bool ASIinitialized = false;
void InitializeHooks()
{
	ClientLog("[Client] InitializeHooks thread started, waiting for samp.dll...");
	while (GetModuleHandleA("samp.dll") == nullptr) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	ClientLog(std::format("[Client] samp.dll loaded at 0x{:X}", rakhook::samp_addr()));

	while (!ASIinitialized) {
		if (rakhook::samp_addr() && rakhook::samp_version() != rakhook::samp_ver::unknown) {
			if (IsGameInitialized()) {
				if (rakhook::initialize()) {
					ASIinitialized = true;
					ClientLog(std::format("[Client] rakhook initialized successfully (samp_version={})", static_cast<int>(rakhook::samp_version())));
					break;
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	rakhook::on_receive_rpc += [](unsigned char& id, RakNet::BitStream* bs) -> bool {
		if (id == RPC_InitGame) {
			ClientLog("[Client] Received RPC_InitGame (139), sending init packet...");
			HandlingManager::SendInitPacket();
		} else if (id == RPC_WorldPlayerAdd) {
			size_t originalOffset = bs->GetReadOffset();
			uint16_t playerId = 0;
			bs->Read(playerId);
			bs->SetReadOffset(originalOffset);
			_customVehInstance.onPlayerStreamIn(playerId);
		} else if (id == RPC_WorldPlayerRemove) {
			size_t originalOffset = bs->GetReadOffset();
			uint16_t playerId = 0;
			bs->Read(playerId);
			bs->SetReadOffset(originalOffset);
			_customVehInstance.onPlayerStreamOut(playerId);
		}
		return true;
	};

	rakhook::on_receive_packet += [](auto* packet) -> bool {
		uint8_t packetId = packet->data[0];
		if (packetId == PKT_EXTVEH) {
			RakNet::BitStream bs(packet->data, packet->length, false);
			bs.IgnoreBits(8);

			CustomVehAction actionID;
			bs.Read(actionID);
			if (actionID == CustomVehAction::CustomVehicleDefine) {
				CustomVeh::Protocol::VehicleDefinition def;
				if (!_customVehInstance.ReadVehicleDefinition(bs, def)) {
					SendMsg(0xFF0000, "[CustomVeh] Failed to read vehicle definition from packet");
					return false;
				}
				_customVehInstance.HandleCustomVehicleDef(def);
				return false;
			} else if (actionID == CustomVehAction::CustomVehicleDestroy) {
				uint32_t customModelId;
				if (!bs.Read(customModelId)) {
					SendMsg(0xFF0000, "[CustomVeh] Failed to read vehicle modelId from packet");
					return false;
				}
				ModelTransferClient::Instance().CancelModelTransfers(customModelId);
				_customVehInstance.PushDestructionCommand(customModelId);
				return false;
			}
			return HandlingManager::ProcessAction(actionID, &bs);
		} else if (packetId == ID_CONNECTION_REQUEST_ACCEPTED) {
			ClientLog("[Client] Received ID_CONNECTION_REQUEST_ACCEPTED, sending init packet...");
			HandlingManager::SendInitPacket();
		} else if (packetId == ID_DISCONNECTION_NOTIFICATION || packetId == ID_CONNECTION_LOST || packetId == ID_CONNECTION_BANNED) {
			ClientLog("[Client] Disconnected from server");
			HandlingManager::m_isServerAuthorized = false;
			_customVehInstance.RequestClearAllCustomModels();
			HandlingManager::ProcessAction(ACTION_RESET_ALL, nullptr);
			ModelTransferClient::Instance().CancelAll("server connection lost");
		}
		return true;
	};

	if (rakhook::orig && rakhook::orig->IsConnected()) {
		ClientLog("[Client] Already connected upon hook init, sending init packet...");
		HandlingManager::SendInitPacket();
	}
}

std::unique_ptr<c_plugin> Plugn;

using game_loop_t = void(__cdecl*)();
static game_loop_t orig_game_loop = nullptr;

static void OnGameProcess()
{
	static uint32_t s_lastFrame = 0xFFFFFFFF;
	if (CTimer::m_FrameCounter == s_lastFrame)
		return;
	s_lastFrame = CTimer::m_FrameCounter;

	try {
		MainThreadQueue::Instance().DrainOnMainThread();

		try {
			HandlingManager::ProcessPendingCommands();
		} catch (const std::exception& e) {
			ClientLog(std::format("[Client] Exception in ProcessPendingCommands: {}", e.what()));
		} catch (...) {
			ClientLog("[Client] Unknown exception in ProcessPendingCommands");
		}

		if (!ASIinitialized) {
			return;
		}

		_customVehInstance.ProcessPendingDefinitions();
		_customVehInstance.ProcessCompletedDownloads();
		CustomVehicleBindingManager::Instance().Process();
		_customVehInstance.ProcessPendingDestructions();
		_customVehInstance.ProcessPendingClearAll();

		static auto lastInitTry = std::chrono::steady_clock::now();
		if (!HandlingManager::m_isServerAuthorized && rakhook::orig && rakhook::orig->IsConnected()) {
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastInitTry).count() >= 1500) {
				lastInitTry = now;
				HandlingManager::SendInitPacket();
			}
		}

		// Update vehicle cache and model use counts
		struct VehicleEntry {
			CVehicle* gameVeh;
			uint16_t sampId;
			uint16_t gtaRef;
		};
		static std::vector<VehicleEntry> previousVehicles;
		std::vector<VehicleEntry> currentVehicles;
		currentVehicles.reserve(128);

		auto pool = GetVehiclesPool();
		if (!std::holds_alternative<std::nullptr_t>(pool)) {
			std::visit([&](auto&& p) {
				using T = std::decay_t<decltype(p)>;
				if constexpr (!std::is_same_v<T, std::nullptr_t>) {
					if (p) {
						for (uint16_t id = 1; id < MAX_SAMP_VEHICLES; ++id) {
							auto* sampVeh = p->Get(id);
							if (sampVeh) {
								if (sampVeh->m_pGameVehicle && IsVehiclePointerValid(sampVeh->m_pGameVehicle) && IsVehicleStreamedForLocalPlayer(sampVeh->m_pGameVehicle)) {
									uint16_t gtaRef = static_cast<uint16_t>(CPools::GetVehicleRef(sampVeh->m_pGameVehicle));
									currentVehicles.push_back({ sampVeh->m_pGameVehicle, id, gtaRef });
								}
							}
						}
					}
				}
			},
				pool);
		}

		CPed* localPed = FindPlayerPed();
		bool anyBoatVehicleNeedsWaterDrive = false;

		// Detect new vehicles and synchronize state for all streamed vehicles
		for (const auto& cur : currentVehicles) {
			bool found = false;
			for (const auto& old : previousVehicles) {
				if (old.gameVeh == cur.gameVeh) {
					found = true;
					break;
				}
			}
			if (!found) {
				HandlingManager::CacheVehicleSAMPId(cur.gameVeh, cur.sampId);
				HandlingManager::OnVehicleStreamIn(cur.gameVeh, cur.sampId);
			}

			if (IsVehiclePointerValid(cur.gameVeh)) {
				// 1. Amphibious water driving for vehicles with MFLAG_IS_BOAT (0x8000000)
				bool isWaterCapable = (cur.gameVeh->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || cur.gameVeh->m_nVehicleSubClass == VEHICLE_MTRUCK || cur.gameVeh->m_nVehicleSubClass == VEHICLE_QUAD);
				bool isBoat = isWaterCapable && cur.gameVeh->m_pHandlingData && ((cur.gameVeh->m_pHandlingData->m_nModelFlags & 0x8000000) != 0);

				if (isBoat) {
					cur.gameVeh->m_pHandlingData->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
						cur.gameVeh->m_pHandlingData->m_nModelFlags & ~(0x00200000 | 0x00020000));

					CVector pos = cur.gameVeh->GetPosition();
					float waterZ = 0.0f;
					bool inWater = cur.gameVeh->bTouchingWater || cur.gameVeh->bSubmergedInWater;
					if (!inWater && CWaterLevel::GetWaterLevelNoWaves(pos.x, pos.y, pos.z, &waterZ)) {
						if (pos.z <= (waterZ + 1.2f)) {
							inWater = true;
						}
					}

					if (inWater) {
						anyBoatVehicleNeedsWaterDrive = true;
						cur.gameVeh->bEngineOn = true;
						cur.gameVeh->bIsDrowning = false;
						cur.gameVeh->bSubmergedInWater = false;
						if (cur.gameVeh->m_pDriver != localPed) {
							cur.gameVeh->bIsHandbrakeOn = false;
						}
						if (cur.gameVeh->m_pHandlingData->m_fBuoyancyConstant > 0.0f) {
							cur.gameVeh->m_fBuoyancyConstant = cur.gameVeh->m_pHandlingData->m_fBuoyancyConstant;
						}
					}
				}

				// 2. Vehicle flight sync for streamed vehicles
				bool isFlightCapable = (cur.gameVeh->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || cur.gameVeh->m_nVehicleSubClass == VEHICLE_MTRUCK || cur.gameVeh->m_nVehicleSubClass == VEHICLE_QUAD || cur.gameVeh->m_nVehicleSubClass == VEHICLE_BIKE || cur.gameVeh->m_nVehicleSubClass == VEHICLE_BMX || cur.gameVeh->m_nVehicleSubClass == VEHICLE_BOAT);

				if (cur.gameVeh->m_pHandlingData && (cur.gameVeh->m_pHandlingData->m_nModelFlags & 0x4000000) != 0) {
					if (isFlightCapable) {
						HandlingManager::SetVehicleFlyingState(cur.sampId, true, cur.gameVeh);
						if (cur.gameVeh->m_nVehicleSubClass != VEHICLE_PLANE) {
							cur.gameVeh->m_pHandlingData->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
								cur.gameVeh->m_pHandlingData->m_nModelFlags & ~(0x4000000 | 0x2000000));
						}
					} else {
						cur.gameVeh->m_pHandlingData->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
							cur.gameVeh->m_pHandlingData->m_nModelFlags & ~(0x4000000 | 0x2000000));
					}
				}

				if (HandlingManager::IsVehicleFlying(cur.gameVeh)) {
					cur.gameVeh->bIsHandbrakeOn = false;
				}

				if (AudioExtender::GetVehicleAudio(static_cast<uint32_t>(cur.gameVeh->m_nModelIndex)).has_value()) {
					std::optional<AudioExtender::CustomVehicleAudioState*> audioState = AudioExtender::GetOrCreateAudioState(*cur.gameVeh);
					if (audioState) {
						if (!AudioExtender::InitialiseCustomVehicleAudio(audioState.value()->engine, *cur.gameVeh)) {
							AudioExtender::RemoveVehicleAudioState(cur.gtaRef);
						}
					}
				}
			}
		}

		// Detect vehicles that disappeared
		for (const auto& old : previousVehicles) {
			bool stillExists = false;
			for (const auto& cur : currentVehicles) {
				if (cur.gameVeh == old.gameVeh) {
					stillExists = true;
					break;
				}
			}
			if (!stillExists) {
				HandlingManager::RemoveVehicleFromCache(old.gameVeh);
				AudioExtender::RemoveVehicleAudioState(old.gtaRef);
			}
		}

		// Update previousVehicles for next frame
		previousVehicles = std::move(currentVehicles);

		// Vehicle destructor cleanup
		HandlingManager::OnVehicleDestructor(nullptr);

		// Vehicle enter / exit for local player
		static CVehicle* s_prevLocalVehicle = nullptr;
		static bool s_pluginEnabledWaterDrive = false;

		if (!localPed) {
			s_prevLocalVehicle = nullptr;
			if (s_pluginEnabledWaterDrive) {
				SetWaterDriveCheatActive(false);
				s_pluginEnabledWaterDrive = false;
			}
			return;
		}
		CVehicle* curVehicle = localPed->m_pVehicle;
		if (!curVehicle) {
			localPed->CantBeKnockedOffBike = 0;
		}

		if (curVehicle != s_prevLocalVehicle) {
			uint16_t localId = GetLocalPlayerId();
			if (s_prevLocalVehicle && IsVehiclePointerValid(s_prevLocalVehicle))
				HandlingManager::RemovePlayerHandling(localId, s_prevLocalVehicle);
			if (curVehicle && IsVehiclePointerValid(curVehicle))
				HandlingManager::ApplyPlayerHandling(localId, curVehicle);
			s_prevLocalVehicle = curVehicle;
		}

		if (curVehicle && IsVehiclePointerValid(curVehicle) && curVehicle->m_pHandlingData) {
			bool isWaterCapable = (curVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || curVehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || curVehicle->m_nVehicleSubClass == VEHICLE_QUAD);
			bool isBoat = isWaterCapable && ((curVehicle->m_pHandlingData->m_nModelFlags & 0x8000000) != 0);

			if (isBoat) {
				anyBoatVehicleNeedsWaterDrive = true;
				if (curVehicle->bTouchingWater || curVehicle->bSubmergedInWater) {
					curVehicle->bEngineOn = true;
					curVehicle->bIsDrowning = false;
					curVehicle->bSubmergedInWater = false;
				}

				if (curVehicle->m_pDriver == localPed) {
					// Release forced water handbrake unless player is actively pressing handbrake key
					CPad* pad = CPad::GetPad(0);
					bool playerHandbrake = pad && (pad->GetHandBrake() > 0);
					curVehicle->bIsHandbrakeOn = playerHandbrake;
				} else {
					// Passenger should not lock handbrake on the driver's vehicle
					curVehicle->bIsHandbrakeOn = false;
				}

				// Clear solid axle flags so GTA does not exclude rear wheels from boat mode rotation
				curVehicle->m_pHandlingData->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
					curVehicle->m_pHandlingData->m_nModelFlags & ~(0x00200000 | 0x00020000));
			}

			// Vehicle flight (cars, boats, bikes) with auto-leveling & anti-inversion
			bool isFlightCapable = (curVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || curVehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || curVehicle->m_nVehicleSubClass == VEHICLE_QUAD || curVehicle->m_nVehicleSubClass == VEHICLE_BIKE || curVehicle->m_nVehicleSubClass == VEHICLE_BMX || curVehicle->m_nVehicleSubClass == VEHICLE_BOAT);

			bool isPlane = isFlightCapable && HandlingManager::IsVehicleFlying(curVehicle);
			if (curVehicle->m_pHandlingData && (curVehicle->m_pHandlingData->m_nModelFlags & 0x4000000) != 0) {
				if (isFlightCapable) {
					isPlane = true;
					if (curVehicle->m_nVehicleSubClass != VEHICLE_PLANE) {
						curVehicle->m_pHandlingData->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
							curVehicle->m_pHandlingData->m_nModelFlags & ~(0x4000000 | 0x2000000));
						HandlingManager::SetVehicleFlyingState(HandlingManager::GetVehicleSAMPId(curVehicle), true, curVehicle);
					}
				} else {
					curVehicle->m_pHandlingData->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
						curVehicle->m_pHandlingData->m_nModelFlags & ~(0x4000000 | 0x2000000));
				}
			}

			// Ensure legacy GTA cheats are disabled so they do not fight our custom physics or flip cars upside down
			CCheat::m_aCheatsActive[CHEAT_CARS_FLY] = false;
			CCheat::m_aCheatsActive[CHEAT_BOATS_FLY] = false;

			static unsigned int s_lastFlightFrame = 0;
			static float s_smoothSteerLR = 0.0f;
			if (isPlane && curVehicle->m_matrix && curVehicle->m_pDriver == localPed) {
				curVehicle->bIsHandbrakeOn = false;

				if (CTimer::m_FrameCounter != s_lastFlightFrame) {
					s_lastFlightFrame = CTimer::m_FrameCounter;

					CVector forward = curVehicle->GetForward();
					CVector up = curVehicle->GetUp();
					CVector right = curVehicle->GetRight();
					float timeStep = CTimer::ms_fTimeStep;
					float fwdSpeed = curVehicle->m_vecMoveSpeed.Dot(forward);
					bool onGround = (curVehicle->GetNumContactWheels() > 0);

					CPad* pad = CPad::GetPad(0);
					short steerUD = pad ? pad->GetSteeringUpDown() : 0;
					if (steerUD == 0 && pad)
						steerUD = pad->GetPedWalkUpDown();
					short steerLR = pad ? pad->GetSteeringLeftRight() : 0;
					if (steerLR == 0 && pad)
						steerLR = pad->GetPedWalkLeftRight();

					bool isClimbing = (steerUD < 0);
					bool isDescending = (steerUD > 0) || (pad && pad->GetBrake() > 0 && !onGround);
					bool isAccelerating = (pad && pad->GetAccelerate() > 0);

					// 1. Aerodynamic lift & descent management
					if (fwdSpeed > 0.06f && !onGround) {
						float liftRatio = std::min(1.2f, fwdSpeed / 0.20f);
						if (isDescending) {
							// Active descent: reduce vertical speed so vehicle descends freely to land
							curVehicle->m_vecMoveSpeed.z -= 0.0055f * timeStep;
						} else if (isAccelerating) {
							// Active acceleration: full cruise lift + forward pitch climb
							float cruiseLift = 0.0085f * timeStep * liftRatio;
							curVehicle->m_vecMoveSpeed.z += cruiseLift;
							if (forward.z > 0.0f) {
								curVehicle->m_vecMoveSpeed.z += forward.z * fwdSpeed * 0.08f * timeStep;
							}
						} else {
							// Gliding / coasting (no gas): gentle descent slope towards ground
							curVehicle->m_vecMoveSpeed.z += 0.0055f * timeStep * liftRatio;
						}
					} else if (onGround && fwdSpeed > 0.12f && (isClimbing || isAccelerating)) {
						// Ground takeoff lift
						curVehicle->m_vecMoveSpeed.z += 0.0065f * timeStep;
					}

					// 2. Flight input controls
					if (pad) {
						// Air thrust on Accelerate (W)
						if (isAccelerating) {
							if (fwdSpeed < 1.0f) {
								curVehicle->m_vecMoveSpeed += forward * (0.0045f * timeStep);
							}
						}

						// Air brake & descent on Brake / Reverse (S)
						if (pad->GetBrake() > 0) {
							if (fwdSpeed > 0.02f) {
								curVehicle->m_vecMoveSpeed -= forward * (0.0040f * timeStep);
							}
							if (!onGround) {
								curVehicle->m_vecMoveSpeed.z -= 0.0045f * timeStep;
							}
						}

						// Ascend / Climb booster on Handbrake (Spacebar)
						if (pad->GetHandBrake() > 0) {
							curVehicle->m_vecMoveSpeed.z += 0.0075f * timeStep;
							if (forward.z < 0.35f) {
								curVehicle->m_vecTurnSpeed += right * (0.008f * timeStep);
							}
						}

						// Steering / Yaw & banking (A / D or Left / Right) with progressive input smoothing
						float targetSteer = (steerLR != 0) ? (steerLR / 128.0f) : 0.0f;
						float steerLerp = std::min(0.35f, 0.20f * timeStep);
						s_smoothSteerLR = s_smoothSteerLR * (1.0f - steerLerp) + targetSteer * steerLerp;

						if (fabs(s_smoothSteerLR) > 0.01f) {
							// Smooth, comfortable yaw turning
							curVehicle->m_vecTurnSpeed -= up * (s_smoothSteerLR * 0.018f * timeStep);
							// Gentle aerodynamic banking into turn (capped to ~14 deg)
							if (fabs(right.z) < 0.24f) {
								curVehicle->m_vecTurnSpeed += forward * (s_smoothSteerLR * 0.004f * timeStep);
							}
							// Subtle lateral turning force to smoothly guide trajectory
							curVehicle->m_vecMoveSpeed -= right * (s_smoothSteerLR * 0.0015f * timeStep);
						}

						// Pitch control (Arrow Up = Pitch UP & Climb, Arrow Down = Pitch DOWN & Descend)
						if (steerUD != 0) {
							if (steerUD < 0) {
								// Arrow Up: PITCH NOSE UP & CLIMB into the sky!
								// Clamped to max +35 deg (forward.z < 0.55f)
								if (forward.z < 0.55f) {
									float factor = (-steerUD / 128.0f) * timeStep;
									curVehicle->m_vecTurnSpeed += right * (0.018f * factor);
									curVehicle->m_vecMoveSpeed.z += 0.0070f * factor;
								}
							} else if (steerUD > 0) {
								// Arrow Down: PITCH NOSE DOWN & DESCEND to runway/ground!
								// Allows comfortable descent up to -25 deg (forward.z > -0.42f)
								if (forward.z > -0.42f) {
									float factor = (steerUD / 128.0f) * timeStep;
									curVehicle->m_vecTurnSpeed -= right * (0.016f * factor);
									curVehicle->m_vecMoveSpeed.z -= 0.0070f * factor;
								}
							}
						}
					}

					// 3. Aerodynamic heading alignment: redirect horizontal velocity along the car's heading
					// Smoothly curves the flight trajectory without sharp snaps
					float horizSpeed = sqrtf(curVehicle->m_vecMoveSpeed.x * curVehicle->m_vecMoveSpeed.x + curVehicle->m_vecMoveSpeed.y * curVehicle->m_vecMoveSpeed.y);
					if (horizSpeed > 0.04f) {
						CVector fwdHoriz(forward.x, forward.y, 0.0f);
						float fwdHorizLen = sqrtf(fwdHoriz.x * fwdHoriz.x + fwdHoriz.y * fwdHoriz.y);
						if (fwdHorizLen > 0.001f) {
							fwdHoriz /= fwdHorizLen;
							CVector targetHorizVel = fwdHoriz * horizSpeed;
							float alignRate = 0.035f * timeStep;
							curVehicle->m_vecMoveSpeed.x = curVehicle->m_vecMoveSpeed.x * (1.0f - alignRate) + targetHorizVel.x * alignRate;
							curVehicle->m_vecMoveSpeed.y = curVehicle->m_vecMoveSpeed.y * (1.0f - alignRate) + targetHorizVel.y * alignRate;
						}
					}

					// 4. Auto-leveling & roll stabilization (Does NOT fight player's pitch input!)
					CVector worldUp(0.0f, 0.0f, 1.0f);
					if (up.z < 0.70f) {
						// Emergency righting if vehicle is tilted past 45 degrees
						CVector biasedUp = up + right * 0.15f;
						CVector righting = biasedUp.Cross(worldUp);
						curVehicle->m_vecTurnSpeed += righting * (0.080f * timeStep);
					} else {
						// Roll auto-leveling: actively restores wings to level horizon (correct positive feedback prevention)
						curVehicle->m_vecTurnSpeed += forward * (right.z * 0.075f * timeStep);

						// Pitch centering ONLY when player is not actively pressing pitch keys
						if (steerUD == 0) {
							curVehicle->m_vecTurnSpeed -= right * (forward.z * 0.020f * timeStep);
						}
					}

					// 5. Hard angle clamping (Impossible to flip, loop backwards, or barrel roll)
					float pitchVelocity = curVehicle->m_vecTurnSpeed.Dot(right);
					if (forward.z >= 0.55f && pitchVelocity > 0.0f) {
						curVehicle->m_vecTurnSpeed -= right * pitchVelocity; // Clamp max climb (+35 deg)
					}
					if (forward.z <= -0.42f && pitchVelocity < 0.0f) {
						curVehicle->m_vecTurnSpeed -= right * pitchVelocity; // Clamp max dive (-25 deg)
					}

					float rollVelocity = curVehicle->m_vecTurnSpeed.Dot(forward);
					if (right.z >= 0.28f && rollVelocity < 0.0f) {
						curVehicle->m_vecTurnSpeed -= forward * rollVelocity; // Clamp left bank (~16 deg)
					}
					if (right.z <= -0.28f && rollVelocity > 0.0f) {
						curVehicle->m_vecTurnSpeed -= forward * rollVelocity; // Clamp right bank (~16 deg)
					}

					// 6. Angular damping to eliminate tumbling / wobble and stabilize flight
					curVehicle->m_vecTurnSpeed.x *= 0.88f;
					curVehicle->m_vecTurnSpeed.y *= 0.88f;
					curVehicle->m_vecTurnSpeed.z *= 0.88f;

					// 7. Velocity limiter to prevent physics explosions
					float speedSq = curVehicle->m_vecMoveSpeed.MagnitudeSqr();
					if (speedSq > 1.30f * 1.30f) {
						curVehicle->m_vecMoveSpeed *= (1.30f / sqrtf(speedSq));
					}
				}
			} else {
				s_smoothSteerLR = 0.0f;
			}
		}

		// Activate water drive cheat if ANY boat vehicle is in water, or local player is in an amphibious vehicle
		if (anyBoatVehicleNeedsWaterDrive) {
			SetWaterDriveCheatActive(true);
			s_pluginEnabledWaterDrive = true;
		} else {
			if (s_pluginEnabledWaterDrive) {
				SetWaterDriveCheatActive(false);
				s_pluginEnabledWaterDrive = false;
			}
		}
	} catch (...) {
		// Prevent unhandled exception termination
	}
}

static void __cdecl hooked_game_loop()
{
	if (orig_game_loop)
		orig_game_loop();

	OnGameProcess();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason) {
	case DLL_PROCESS_ATTACH: {
		DisableThreadLibraryCalls(hModule);

		MH_Initialize();
		MH_STATUS mhStatus = MH_CreateHook(reinterpret_cast<void*>(0x53BEE0), reinterpret_cast<void*>(&hooked_game_loop), reinterpret_cast<void**>(&orig_game_loop));
		if (mhStatus == MH_OK) {
			MH_EnableHook(reinterpret_cast<void*>(0x53BEE0));
			ClientLog("[Client] CGame::Process (0x53BEE0) hooked successfully via MinHook");
		} else {
			ClientLog(std::format("[Client] Failed to hook CGame::Process (0x53BEE0): {}", MH_StatusToString(mhStatus)));
		}

		MH_STATUS whlStatus = MH_CreateHook(reinterpret_cast<void*>(0x6AA290), reinterpret_cast<void*>(&Hooked_UpdateWheelMatrix), reinterpret_cast<void**>(&g_origUpdateWheelMatrix));
		if (whlStatus == MH_OK) {
			MH_EnableHook(reinterpret_cast<void*>(0x6AA290));
			ClientLog("[Client] CAutomobile::UpdateWheelMatrix (0x6AA290) hooked successfully via MinHook");
		} else {
			ClientLog(std::format("[Client] Failed to hook CAutomobile::UpdateWheelMatrix (0x6AA290): {}", MH_StatusToString(whlStatus)));
		}

		MH_STATUS hlStatus = MH_CreateHook(reinterpret_cast<void*>(0x6E0A50), reinterpret_cast<void*>(&Hooked_DoHeadLightEffect), reinterpret_cast<void**>(&g_origDoHeadLightEffect));
		if (hlStatus == MH_OK) {
			MH_EnableHook(reinterpret_cast<void*>(0x6E0A50));
			ClientLog("[Client] CVehicle::DoHeadLightEffect (0x6E0A50) hooked successfully via MinHook");
		} else {
			ClientLog(std::format("[Client] Failed to hook CVehicle::DoHeadLightEffect (0x6E0A50): {}", MH_StatusToString(hlStatus)));
		}

		MH_STATUS tlStatus = MH_CreateHook(reinterpret_cast<void*>(0x6E1780), reinterpret_cast<void*>(&Hooked_DoTailLightEffect), reinterpret_cast<void**>(&g_origDoTailLightEffect));
		if (tlStatus == MH_OK) {
			MH_EnableHook(reinterpret_cast<void*>(0x6E1780));
			ClientLog("[Client] CVehicle::DoTailLightEffect (0x6E1780) hooked successfully via MinHook");
		} else {
			ClientLog(std::format("[Client] Failed to hook CVehicle::DoTailLightEffect (0x6E1780): {}", MH_StatusToString(tlStatus)));
		}

		MH_STATUS rcTexStatus = MH_CreateHook(reinterpret_cast<void*>(0x6FC180), reinterpret_cast<void*>(&Hooked_RegisterCoronaTexture), reinterpret_cast<void**>(&g_origRegisterCoronaTexture));
		if (rcTexStatus == MH_OK) {
			MH_EnableHook(reinterpret_cast<void*>(0x6FC180));
			ClientLog("[Client] CCoronas::RegisterCorona[Texture] (0x6FC180) hooked successfully via MinHook");
		} else {
			ClientLog(std::format("[Client] Failed to hook CCoronas::RegisterCorona[Texture] (0x6FC180): {}", MH_StatusToString(rcTexStatus)));
		}

		MH_STATUS clsStatus = MH_CreateHook(reinterpret_cast<void*>(0x70C500), reinterpret_cast<void*>(&Hooked_StoreCarLightShadow), reinterpret_cast<void**>(&g_origStoreCarLightShadow));
		if (clsStatus == MH_OK) {
			MH_EnableHook(reinterpret_cast<void*>(0x70C500));
			ClientLog("[Client] CShadows::StoreCarLightShadow (0x70C500) hooked successfully via MinHook");
		} else {
			ClientLog(std::format("[Client] Failed to hook CShadows::StoreCarLightShadow (0x70C500): {}", MH_StatusToString(clsStatus)));
		}

		Plugn = std::make_unique<c_plugin>(hModule);
		static bool threadSpawned = false;
		if (!threadSpawned) {
			threadSpawned = true;
			std::thread(InitializeHooks).detach();
		}
		Events::initGameEvent += []() {
			colLoader->Initialize();
		};
		Events::drawingEvent += []() {
			OnGameProcess();
		};
		Events::gameProcessEvent += []() {
			OnGameProcess();
		};
		break;
	}
	case DLL_PROCESS_DETACH: {
		SetWaterDriveCheatActive(false);
		CCheat::m_aCheatsActive[CHEAT_CARS_FLY] = false;
		CCheat::m_aCheatsActive[CHEAT_BOATS_FLY] = false;

		if (orig_game_loop) {
			MH_DisableHook(reinterpret_cast<void*>(0x53BEE0));
			MH_RemoveHook(reinterpret_cast<void*>(0x53BEE0));
			orig_game_loop = nullptr;
		}

		if (g_origUpdateWheelMatrix) {
			MH_DisableHook(reinterpret_cast<void*>(0x6AA290));
			MH_RemoveHook(reinterpret_cast<void*>(0x6AA290));
			g_origUpdateWheelMatrix = nullptr;
		}

		if (g_origDoHeadLightEffect) {
			MH_DisableHook(reinterpret_cast<void*>(0x6E0A50));
			MH_RemoveHook(reinterpret_cast<void*>(0x6E0A50));
			g_origDoHeadLightEffect = nullptr;
		}

		if (g_origDoTailLightEffect) {
			MH_DisableHook(reinterpret_cast<void*>(0x6E1780));
			MH_RemoveHook(reinterpret_cast<void*>(0x6E1780));
			g_origDoTailLightEffect = nullptr;
		}

		if (g_origRegisterCoronaTexture) {
			MH_DisableHook(reinterpret_cast<void*>(0x6FC180));
			MH_RemoveHook(reinterpret_cast<void*>(0x6FC180));
			g_origRegisterCoronaTexture = nullptr;
		}

		if (g_origStoreCarLightShadow) {
			MH_DisableHook(reinterpret_cast<void*>(0x70C500));
			MH_RemoveHook(reinterpret_cast<void*>(0x70C500));
			g_origStoreCarLightShadow = nullptr;
		}

		rakhook::on_receive_rpc.clear();
		rakhook::on_send_rpc.clear();
		rakhook::on_receive_packet.clear();
		rakhook::on_send_packet.clear();

		if (GetModuleHandleA("samp.dll") != nullptr) {
			rakhook::destroy();
		}

		Plugn.reset();
		break;
	}
	}
	return true;
}
