// SDK
#include <plugin_sa.h>

#include <game_sa/CAudioEngine.h>
#include <game_sa/CAutomobile.h>
#include <game_sa/CBike.h>
#include <game_sa/CCamera.h>
#include <game_sa/CCheat.h>
#include <game_sa/CCoronas.h>
#include <game_sa/CHandlingDataMgr.h>
#include <game_sa/CColModel.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CPad.h>
#include <game_sa/CPlayerPed.h>
#include <game_sa/CShadows.h>
#include <game_sa/CTimer.h>
#include <game_sa/CTxdStore.h>
#include <game_sa/CVehicle.h>
#include <game_sa/CVisibilityPlugins.h>
#include <game_sa/CWaterLevel.h>
#include <game_sa/Fx_c.h>
#include <game_sa/rw/rpworld.h>
#include <MinHook.h>
#include <algorithm>
#include <format>
#include <intrin.h>
#include <shared/game/CVector.h>
#include <string>
#include <unordered_set>

#include "CustomVehicleBindingManager.h"
#include "ModelCache.h"
#include "streamingextender.hpp"
#include "utils.h"

struct DummySwapGuard {
	CVehicleModelInfo* m_baseModel;
	CVehicleModelInfo::CVehicleStructure* m_savedStruct;

	DummySwapGuard(CVehicleModelInfo* baseModel, CVehicleModelInfo* customModel)
		: m_baseModel(baseModel)
		, m_savedStruct(nullptr)
	{
		if (m_baseModel && customModel && customModel->m_pVehicleStruct) {
			m_savedStruct = m_baseModel->m_pVehicleStruct;
			m_baseModel->m_pVehicleStruct = customModel->m_pVehicleStruct;
		}
	}

	~DummySwapGuard()
	{
		if (m_baseModel && m_savedStruct) {
			m_baseModel->m_pVehicleStruct = m_savedStruct;
		}
	}
};

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

	if (nodeIndex >= CAR_WHEEL_RF && nodeIndex <= CAR_WHEEL_LB && thisCar->m_aCarNodes[nodeIndex]) {
		auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisCar);
		if (binding && binding->hasStance) {
			bool isFront = (nodeIndex == CAR_WHEEL_RF || nodeIndex == CAR_WHEEL_LF);
			bool isRight = (nodeIndex == CAR_WHEEL_RF || nodeIndex == CAR_WHEEL_RM || nodeIndex == CAR_WHEEL_RB);
			float scale = isFront ? binding->frontWheelScale : binding->rearWheelScale;
			float camber = isFront ? binding->frontCamber : binding->rearCamber;
			float trackWidth = isFront ? binding->frontTrackWidth : binding->rearTrackWidth;

			RwFrame* wheelFrame = thisCar->m_aCarNodes[nodeIndex];
			if (wheelFrame) {
				bool modified = false;

				if (trackWidth != 0.0f) {
					wheelFrame->modelling.pos.x += isRight ? trackWidth : -trackWidth;
					modified = true;
				}

				if (camber != 0.0f) {
					float angle = isRight ? camber : -camber;
					RwV3d yAxis = { 0.0f, 1.0f, 0.0f };
					RwMatrixRotate(&wheelFrame->modelling, &yAxis, angle, rwCOMBINEPRECONCAT);
					modified = true;
				}

				if (scale != 1.0f && scale > 0.01f) {
					RwV3d scaleVec = { scale, scale, scale };
					RwMatrixScale(&wheelFrame->modelling, &scaleVec, rwCOMBINEPRECONCAT);
					modified = true;
				}

				if (modified) {
					RwFrameUpdateObjects(wheelFrame);
				}
			}
		}
	}
}

// Vehicle Headlights / Taillights Dynamic Scaling Hooks
static bool(__fastcall* g_origDoHeadLightEffect)(CVehicle* thisVehicle, void* edx, int dummyId, CMatrix& vehicleMatrix, unsigned char lightId, unsigned char lightState) = nullptr;
static bool(__fastcall* g_origDoTailLightEffect)(CVehicle* thisVehicle, void* edx, int lightId, CMatrix& matrix, unsigned char arg2, unsigned char arg3, unsigned int arg4, unsigned char arg5) = nullptr;

static void(__cdecl* g_origRegisterCoronaTexture)(
	unsigned int id, CEntity* attachTo, unsigned char red, unsigned char green, unsigned char blue,
	unsigned char alpha, CVector const& posn, float radius, float farClip, RwTexture* texture, eCoronaFlareType flaretype,
	bool enableReflection, bool checkObstacles, int _param_not_used, float angle, bool longDistance, float nearClip,
	unsigned char fadeState, float fadeSpeed, bool onlyFromBelow, bool reflectionDelay)
	= nullptr;

static void(__cdecl* g_origStoreCarLightShadow)(
	CVehicle* vehicle, int id, RwTexture* texture, CVector* posn,
	float frontX, float frontY, float sideX, float sideY,
	unsigned char red, unsigned char green, unsigned char blue,
	float maxViewAngle)
	= nullptr;

static CVehicle* s_pCurrentHeadLightVehicle = nullptr;
static CVehicle* s_pCurrentTailLightVehicle = nullptr;

enum class eVehicleLightingCategory : uint8_t {
	Automobile, // Sedans, coupes, sports cars, station wagons, SUVs
	TwoWheeler, // Motorcycles, dirt bikes, mopeds, bicycles, quad bikes
	HeavyVehicle, // Monster trucks, big rigs, flatbeds, buses, coaches, heavy machinery
	Aircraft, // Airplanes, jets, helicopters
	Boat, // Speedboats, yachts, tugboats, dinghies
	Hovercraft, // Vortex hovercraft, or vehicle in active flight/hover mode
	RCVehicle, // Miniature remote controlled vehicles (RC Bandit, Baron, Raider, Goblin, Tiger, Cam)
	Trailer // Towed trailers (rear markers/taillights only)
};

static inline eVehicleLightingCategory GetVehicleLightingCategory(const CVehicle* pVeh, float* pOutCustomScaleMult = nullptr)
{
	if (pOutCustomScaleMult)
		*pOutCustomScaleMult = 1.0f;

	if (!pVeh)
		return eVehicleLightingCategory::Automobile;

	auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(const_cast<CVehicle*>(pVeh));
	if (binding && binding->hasCustomLighting) {
		if (pOutCustomScaleMult)
			*pOutCustomScaleMult = (binding->customLightScaleMult > 0.05f) ? binding->customLightScaleMult : 1.0f;
		if (binding->customLightingCategory >= 0 && binding->customLightingCategory <= 7) {
			return static_cast<eVehicleLightingCategory>(binding->customLightingCategory);
		}
	}

	int modelIndex = pVeh->m_nModelIndex;
	if (binding && binding->baseModelId > 0) {
		modelIndex = binding->baseModelId;
	}

	// 2. RC Vehicles: native RC model check
	if (IsRcVehicleModel(modelIndex)) {
		return eVehicleLightingCategory::RCVehicle;
	}

	// 3. Custom vehicle model geometry and type inspection
	if (binding && binding->customModelId > 0) {
		auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
		if (customModel) {
			if (customModel->m_nVehicleType == VEHICLE_BIKE || customModel->m_nVehicleType == VEHICLE_BMX || customModel->m_nVehicleType == VEHICLE_QUAD) {
				return eVehicleLightingCategory::TwoWheeler;
			}
			if (customModel->m_nVehicleType == VEHICLE_MTRUCK) {
				return eVehicleLightingCategory::HeavyVehicle;
			}
			if (customModel->m_nVehicleType == VEHICLE_PLANE || customModel->m_nVehicleType == VEHICLE_HELI || customModel->m_nVehicleType == VEHICLE_FPLANE || customModel->m_nVehicleType == VEHICLE_FHELI) {
				return eVehicleLightingCategory::Aircraft;
			}
			if (customModel->m_nVehicleType == VEHICLE_BOAT) {
				return eVehicleLightingCategory::Boat;
			}
			if (customModel->m_nVehicleType == VEHICLE_TRAILER) {
				return eVehicleLightingCategory::Trailer;
			}

			// Dimension inspection: detect miniature custom models (RC) or giant custom models (Heavy)
			auto* col = customModel->m_pColModel;
			if (col) {
				float width = col->m_boundBox.m_vecMax.x - col->m_boundBox.m_vecMin.x;
				float length = col->m_boundBox.m_vecMax.y - col->m_boundBox.m_vecMin.y;
				float height = col->m_boundBox.m_vecMax.z - col->m_boundBox.m_vecMin.z;
				if (width > 0.05f && width < 0.95f && length < 1.65f) {
					return eVehicleLightingCategory::RCVehicle;
				}
				if (width > 2.45f || length > 7.0f || height > 2.8f) {
					return eVehicleLightingCategory::HeavyVehicle;
				}
			}
		}
	}

	// 4. Hovercraft / Flight-capable Vehicles (Vortex 539 or active flight mode)
	if (modelIndex == 539 || IsVehicleInFlightMode(const_cast<CVehicle*>(pVeh))) {
		return eVehicleLightingCategory::Hovercraft;
	}
	if (pVeh->m_pHandlingData && (pVeh->m_pHandlingData->m_nModelFlags & 0x4000000) != 0) {
		return eVehicleLightingCategory::Hovercraft;
	}

	// 5. Native GTA:SA Vehicle SubClass
	switch (pVeh->m_nVehicleSubClass) {
	case VEHICLE_BIKE:
	case VEHICLE_BMX:
	case VEHICLE_QUAD:
		return eVehicleLightingCategory::TwoWheeler;

	case VEHICLE_MTRUCK:
		return eVehicleLightingCategory::HeavyVehicle;

	case VEHICLE_PLANE:
	case VEHICLE_HELI:
	case VEHICLE_FPLANE:
	case VEHICLE_FHELI:
		return eVehicleLightingCategory::Aircraft;

	case VEHICLE_BOAT:
		return eVehicleLightingCategory::Boat;

	case VEHICLE_TRAILER:
		return eVehicleLightingCategory::Trailer;

	case VEHICLE_AUTOMOBILE:
	default:
		if (IsHeavyVehicleModel(modelIndex)) {
			return eVehicleLightingCategory::HeavyVehicle;
		}
		return eVehicleLightingCategory::Automobile;
	}
}

static inline bool IsTwoWheeler(const CVehicle* pVeh)
{
	return GetVehicleLightingCategory(pVeh) == eVehicleLightingCategory::TwoWheeler;
}

struct LightScaleConfig {
	float coronaScale;
	float shadowFrontScale;
	float shadowSideScale;
	float offsetSpacingMult; // Multiplier for clustering width/height spacing
	bool allowClustering; // Whether multi-corona clustering is permitted
};

static inline LightScaleConfig GetVehicleLightScaleConfig(
	uint8_t lightSize,
	bool isRear = false,
	eVehicleLightingCategory category = eVehicleLightingCategory::Automobile,
	float customMult = 1.0f)
{
	LightScaleConfig cfg;

	switch (category) {
	case eVehicleLightingCategory::RCVehicle: {
		// Miniature RC Models (RC Bandit, Baron, Raider, Goblin, Tiger, Cam)
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.15f, 0.35f, 0.35f, 0.20f, false };
				break; // LIGHTS_SMALL: tiny micro LED dot
			case 0:
				cfg = { 0.22f, 0.45f, 0.55f, 0.20f, false };
				break; // LIGHTS_LONG: micro horizontal LED bar
			case 2:
				cfg = { 0.30f, 0.60f, 0.60f, 0.25f, false };
				break; // LIGHTS_BIG: bright RC beacon
			case 3:
				cfg = { 0.22f, 0.50f, 0.45f, 0.20f, false };
				break; // LIGHTS_TALL: micro vertical tail strip
			default:
				cfg = { 0.20f, 0.40f, 0.40f, 0.20f, false };
				break;
			}
		} else {
			switch (lightSize) {
			case 1:
				cfg = { 0.18f, 0.35f, 0.30f, 0.20f, false };
				break; // LIGHTS_SMALL: micro flashlight projector
			case 0:
				cfg = { 0.28f, 0.40f, 0.50f, 0.20f, false };
				break; // LIGHTS_LONG: miniature horizontal headlight bar
			case 2:
				cfg = { 0.45f, 0.55f, 0.50f, 0.25f, false };
				break; // LIGHTS_BIG: high-intensity RC crawler pod
			case 3:
				cfg = { 0.30f, 0.50f, 0.35f, 0.20f, false };
				break; // LIGHTS_TALL: micro vertical projector
			default:
				cfg = { 0.25f, 0.40f, 0.35f, 0.20f, false };
				break;
			}
		}
		break;
	}

	case eVehicleLightingCategory::Hovercraft: {
		// Hovercraft (Vortex 539) & Flight-capable hovering vehicles
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.80f, 1.00f, 1.00f, 0.80f, false };
				break; // LIGHTS_SMALL: compact shroud LED
			case 0:
				cfg = { 0.95f, 1.10f, 1.25f, 0.85f, true };
				break; // LIGHTS_LONG: wide rear skirt glow
			case 2:
				cfg = { 1.25f, 1.25f, 1.20f, 0.90f, false };
				break; // LIGHTS_BIG: fan housing lamp
			case 3:
				cfg = { 0.95f, 1.20f, 1.00f, 0.85f, false };
				break; // LIGHTS_TALL: rudder beacon
			default:
				cfg = { 1.00f, 1.00f, 1.10f, 0.85f, false };
				break;
			}
		} else {
			switch (lightSize) {
			case 1:
				cfg = { 0.75f, 1.00f, 0.85f, 0.70f, false };
				break; // LIGHTS_SMALL: laser surface projector
			case 0:
				cfg = { 1.10f, 1.10f, 1.35f, 0.90f, true };
				break; // LIGHTS_LONG: skirt searchlight bar
			case 2:
				cfg = { 1.35f, 1.25f, 1.25f, 0.90f, false };
				break; // LIGHTS_BIG: marine floodlight
			case 3:
				cfg = { 1.05f, 1.25f, 0.95f, 0.70f, false };
				break; // LIGHTS_TALL: cockpit forward beacon
			default:
				cfg = { 1.00f, 1.05f, 1.05f, 0.70f, false };
				break;
			}
		}
		break;
	}

	case eVehicleLightingCategory::TwoWheeler: {
		// Motorcycles, Dirt Bikes, BMX, Quads
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.65f, 0.90f, 0.85f, 0.50f, false };
				break; // LIGHTS_SMALL: compact sharp LED tail dot
			case 0:
				cfg = { 0.75f, 1.00f, 1.05f, 0.50f, false };
				break; // LIGHTS_LONG: sleek contained tail glow
			case 2:
				cfg = { 1.05f, 1.15f, 1.10f, 0.50f, false };
				break; // LIGHTS_BIG: classic round cafe/chopper tail lamp
			case 3:
				cfg = { 0.75f, 1.15f, 0.90f, 0.50f, false };
				break; // LIGHTS_TALL: vertical fender LED strip
			default:
				cfg = { 0.85f, 1.00f, 1.00f, 0.50f, false };
				break;
			}
		} else {
			switch (lightSize) {
			case 1:
				cfg = { 0.70f, 0.90f, 0.80f, 0.50f, false };
				break; // LIGHTS_SMALL: pinpoint projector LED dot
			case 0:
				cfg = { 0.90f, 1.05f, 1.10f, 0.50f, false };
				break; // LIGHTS_LONG: sports bike horizontal slit
			case 2:
				cfg = { 1.20f, 1.20f, 1.10f, 0.50f, false };
				break; // LIGHTS_BIG: classic 7" chopper headlight
			case 3:
				cfg = { 0.95f, 1.20f, 0.85f, 0.50f, false };
				break; // LIGHTS_TALL: vertical streetfighter projector
			default:
				cfg = { 0.85f, 1.00f, 0.85f, 0.50f, false };
				break;
			}
		}
		break;
	}

	case eVehicleLightingCategory::HeavyVehicle: {
		// Monster Trucks, Semi Trucks, Buses, Heavy Machinery
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.85f, 1.05f, 1.05f, 0.80f, false };
				break; // LIGHTS_SMALL: commercial LED clearance dot
			case 0:
				cfg = { 1.00f, 1.15f, 1.30f, 0.90f, true };
				break; // LIGHTS_LONG: wide heavy-duty bumper bar
			case 2:
				cfg = { 1.35f, 1.30f, 1.25f, 1.00f, false };
				break; // LIGHTS_BIG: heavy-duty rear lamp
			case 3:
				cfg = { 1.00f, 1.30f, 1.10f, 0.90f, true };
				break; // LIGHTS_TALL: vertical corner clearance pillars
			default:
				cfg = { 1.10f, 1.10f, 1.10f, 1.00f, false };
				break;
			}
		} else {
			switch (lightSize) {
			case 1:
				cfg = { 0.80f, 0.95f, 0.85f, 0.70f, false };
				break; // LIGHTS_SMALL: heavy cab clearance projector
			case 0:
				cfg = { 1.15f, 1.15f, 1.35f, 0.90f, true };
				break; // LIGHTS_LONG: cross-grille light bar
			case 2:
				cfg = { 1.40f, 1.35f, 1.30f, 0.90f, false };
				break; // LIGHTS_BIG: 24V industrial floodlights
			case 3:
				cfg = { 1.15f, 1.30f, 1.00f, 0.90f, true };
				break; // LIGHTS_TALL: vertical heavy-truck grille columns
			default:
				cfg = { 1.10f, 1.10f, 1.10f, 0.90f, false };
				break;
			}
		}
		break;
	}

	case eVehicleLightingCategory::Aircraft: {
		// Airplanes, Jets, Helicopters
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.80f, 1.00f, 1.00f, 0.80f, false };
				break; // LIGHTS_SMALL: wingtip position light
			case 0:
				cfg = { 0.95f, 1.10f, 1.25f, 0.80f, false };
				break; // LIGHTS_LONG: wing trailing edge glow
			case 2:
				cfg = { 1.30f, 1.25f, 1.20f, 0.80f, false };
				break; // LIGHTS_BIG: anti-collision tail strobe
			case 3:
				cfg = { 0.95f, 1.25f, 1.00f, 0.80f, false };
				break; // LIGHTS_TALL: vertical rudder beacon
			default:
				cfg = { 1.00f, 1.00f, 1.00f, 0.80f, false };
				break;
			}
		} else {
			switch (lightSize) {
			case 1:
				cfg = { 0.80f, 1.15f, 0.85f, 0.70f, false };
				break; // LIGHTS_SMALL: nosegear taxi projector
			case 0:
				cfg = { 1.15f, 1.20f, 1.35f, 0.70f, false };
				break; // LIGHTS_LONG: runway turnoff lights
			case 2:
				cfg = { 1.50f, 1.50f, 1.35f, 0.70f, false };
				break; // LIGHTS_BIG: runway landing lights
			case 3:
				cfg = { 1.15f, 1.40f, 0.95f, 0.70f, false };
				break; // LIGHTS_TALL: searchlight
			default:
				cfg = { 1.10f, 1.15f, 1.10f, 0.70f, false };
				break;
			}
		}
		break;
	}

	case eVehicleLightingCategory::Boat: {
		// Boats, Speedboats, Yachts
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.75f, 0.90f, 0.90f, 0.80f, false };
				break; // LIGHTS_SMALL: compact transom light
			case 0:
				cfg = { 0.90f, 1.00f, 1.20f, 0.80f, false };
				break; // LIGHTS_LONG: swim platform LED bar
			case 2:
				cfg = { 1.20f, 1.15f, 1.15f, 0.80f, false };
				break; // LIGHTS_BIG: stern floodlight
			case 3:
				cfg = { 0.90f, 1.15f, 0.95f, 0.80f, false };
				break; // LIGHTS_TALL: masthead white anchor light
			default:
				cfg = { 0.95f, 0.95f, 0.95f, 0.80f, false };
				break;
			}
		} else {
			switch (lightSize) {
			case 1:
				cfg = { 0.75f, 1.00f, 0.80f, 0.70f, false };
				break; // LIGHTS_SMALL: bow navigation light
			case 0:
				cfg = { 1.10f, 1.15f, 1.30f, 0.70f, false };
				break; // LIGHTS_LONG: bow docking floodlight bar
			case 2:
				cfg = { 1.30f, 1.25f, 1.20f, 0.70f, false };
				break; // LIGHTS_BIG: marine bow searchlight
			case 3:
				cfg = { 1.05f, 1.25f, 0.95f, 0.70f, false };
				break; // LIGHTS_TALL: radar arch floodlight
			default:
				cfg = { 1.00f, 1.05f, 1.05f, 0.70f, false };
				break;
			}
		}
		break;
	}

	case eVehicleLightingCategory::Trailer: {
		// Trailers & Semi-trailers
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.80f, 1.00f, 1.00f, 0.85f, false };
				break; // LIGHTS_SMALL: LED clearance marker dot
			case 0:
				cfg = { 0.95f, 1.10f, 1.25f, 0.90f, true };
				break; // LIGHTS_LONG: full-width trailer tail bar
			case 2:
				cfg = { 1.25f, 1.25f, 1.20f, 0.90f, false };
				break; // LIGHTS_BIG: trailer tail lamp
			case 3:
				cfg = { 0.95f, 1.25f, 1.05f, 0.90f, true };
				break; // LIGHTS_TALL: vertical rear corner clearance lights
			default:
				cfg = { 1.00f, 1.00f, 1.00f, 0.90f, false };
				break;
			}
		} else {
			cfg = { 0.75f, 0.75f, 0.75f, 0.70f, false }; // Front amber corner markers
		}
		break;
	}

	case eVehicleLightingCategory::Automobile:
	default: {
		// Standard Automobiles (Sedans, Coupes, Sports, Wagons, SUVs)
		if (isRear) {
			switch (lightSize) {
			case 1:
				cfg = { 0.75f, 1.00f, 1.00f, 0.70f, false };
				break; // LIGHTS_SMALL: compact crisp LED tail dot
			case 0:
				cfg = { 0.82f, 1.10f, 1.25f, 0.85f, true };
				break; // LIGHTS_LONG: sleek horizontal light bar
			case 2:
				cfg = { 1.25f, 1.25f, 1.20f, 1.00f, false };
				break; // LIGHTS_BIG: bold circular tail lamp
			case 3:
				cfg = { 0.82f, 1.25f, 1.05f, 0.85f, true };
				break; // LIGHTS_TALL: vertical pillar tail strip
			default:
				cfg = { 1.00f, 1.00f, 1.00f, 1.00f, false };
				break;
			}
		} else {
			switch (lightSize) {
			case 1:
				cfg = { 0.75f, 0.90f, 0.80f, 0.60f, false };
				break; // LIGHTS_SMALL: compact crisp LED projector dot
			case 0:
				cfg = { 1.05f, 1.05f, 1.30f, 0.85f, true };
				break; // LIGHTS_LONG: wide horizontal headlight bar
			case 2:
				cfg = { 1.28f, 1.25f, 1.20f, 1.00f, false };
				break; // LIGHTS_BIG: bold round headlight
			case 3:
				cfg = { 1.05f, 1.25f, 0.90f, 0.85f, true };
				break; // LIGHTS_TALL: vertical projector column
			default:
				cfg = { 1.00f, 1.00f, 1.00f, 1.00f, false };
				break;
			}
		}
		break;
	}
	}

	if (customMult > 0.05f && customMult != 1.0f) {
		cfg.coronaScale *= customMult;
		cfg.shadowFrontScale *= customMult;
		cfg.shadowSideScale *= customMult;
	}

	return cfg;
}

static bool __fastcall Hooked_DoHeadLightEffect(CVehicle* thisVehicle, void* edx, int dummyId, CMatrix& vehicleMatrix, unsigned char lightId, unsigned char lightState)
{
	s_pCurrentHeadLightVehicle = thisVehicle;
	bool res = false;

	if (thisVehicle && IsVehiclePointerValid(thisVehicle)) {
		auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisVehicle);
		if (binding) {
			auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
			auto* baseModel = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(thisVehicle->m_nModelIndex));
			DummySwapGuard guard(baseModel, customModel);
			if (g_origDoHeadLightEffect) {
				res = g_origDoHeadLightEffect(thisVehicle, edx, dummyId, vehicleMatrix, lightId, lightState);
			}
			s_pCurrentHeadLightVehicle = nullptr;
			return res;
		}
	}

	if (g_origDoHeadLightEffect) {
		res = g_origDoHeadLightEffect(thisVehicle, edx, dummyId, vehicleMatrix, lightId, lightState);
	}
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
	bool res = false;

	if (thisVehicle && IsVehiclePointerValid(thisVehicle)) {
		auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisVehicle);
		if (binding) {
			auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
			auto* baseModel = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(thisVehicle->m_nModelIndex));
			DummySwapGuard guard(baseModel, customModel);
			if (g_origDoTailLightEffect) {
				res = g_origDoTailLightEffect(thisVehicle, edx, lightId, matrix, arg2, arg3, arg4, arg5);
			}
			s_pCurrentTailLightVehicle = nullptr;
			return res;
		}
	}

	if (g_origDoTailLightEffect) {
		res = g_origDoTailLightEffect(thisVehicle, edx, lightId, matrix, arg2, arg3, arg4, arg5);
	}
	s_pCurrentTailLightVehicle = nullptr;
	return res;
}

static void(__fastcall* g_origAddExhaustParticles)(CVehicle* thisVehicle, void* edx) = nullptr;

static void __fastcall Hooked_AddExhaustParticles(CVehicle* thisVehicle, void* edx)
{
	if (!thisVehicle || !IsVehiclePointerValid(thisVehicle)) {
		if (g_origAddExhaustParticles)
			g_origAddExhaustParticles(thisVehicle, edx);
		return;
	}

	auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisVehicle);
	if (binding) {
		auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
		auto* baseModel = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(thisVehicle->m_nModelIndex));
		DummySwapGuard guard(baseModel, customModel);

		if (g_origAddExhaustParticles)
			g_origAddExhaustParticles(thisVehicle, edx);

		if (binding->backfireEnabled && thisVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE) {
			auto* car = reinterpret_cast<CAutomobile*>(thisVehicle);
			float speed = car->m_vecMoveSpeed.Magnitude();
			if (speed > 0.15f && car->m_fGasPedal < 0.05f && car->m_nCurrentGear > 1) {
				uint32_t now = GetTickCount();
				if (now - binding->lastBackfireTick > (400u + (static_cast<uint32_t>(thisVehicle->m_nRandomSeed) % 500u))) {
					binding->lastBackfireTick = now;

					CVector exhaustLocal(-0.6f, -2.0f, 0.0f);
					if (customModel && customModel->m_pVehicleStruct) {
						// EXHAUST is VehicleDummies::EXHAUST = index 6 (byte offset 0x48).
						// Index 2 would be LIGHT_FRONT_SECONDARY, causing backfire at headlight pos!
						// Verified: CMultiplayerSA_VehicleDummies.cpp line 116: "EXHAUST is at index 6, offset = 6*12 = 0x48"
						exhaustLocal = customModel->m_pVehicleStruct->m_avDummyPos[6];
					}
					// CMatrix has no TransformPoint() - use operator*(CMatrix, CVector) from CMatrix.h:95
					// which performs: pos + right*v.x + forward*v.y + up*v.z (i.e. local-to-world transform)
					CVector exhaustWorld = thisVehicle->GetMatrix() * exhaustLocal;
					CVector backwardDir = -thisVehicle->GetMatrix().GetForward();

					CCoronas::RegisterCorona(
						reinterpret_cast<uintptr_t>(thisVehicle) + 0x20,
						nullptr,
						255, 140, 40, 255,
						exhaustWorld,
						0.45f,
						35.0f,
						CORONATYPE_SHINYSTAR,
						FLARETYPE_NONE,
						false, false, 0, 0.0f, false, 0.05f, 0, 15.0f, false, false);

					CVector rightDir = thisVehicle->GetMatrix().GetRight();
					g_fx.AddSparks(exhaustWorld, backwardDir, 3.5f, 15, rightDir, 0, 0.25f, 0.2f);

					AudioEngine.ReportMissionAudioEvent(static_cast<eAudioEvents>(1131), &exhaustWorld);
				}
			}
		}
		return;
	}

	if (g_origAddExhaustParticles)
		g_origAddExhaustParticles(thisVehicle, edx);
}

// Hooked_SetRemap: Intercepts CVehicle::SetRemap (0x6D0C00) to support SA-MP ChangeVehiclePaintjob on custom vehicles
static void(__fastcall* g_origSetRemap)(CVehicle* thisVehicle, void* edx, int remapIndex) = nullptr;

static void __fastcall Hooked_SetRemap(CVehicle* thisVehicle, void* edx, int remapIndex)
{
	if (!thisVehicle || !IsVehiclePointerValid(thisVehicle)) {
		if (g_origSetRemap)
			g_origSetRemap(thisVehicle, edx, remapIndex);
		return;
	}

	auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisVehicle);
	if (binding) {
		CustomVehicleBindingManager::Instance().SetVehiclePaintjob(binding->sampVehicleId, remapIndex);
		return;
	}

	if (g_origSetRemap)
		g_origSetRemap(thisVehicle, edx, remapIndex);
}

// Hooked_GetFrameFromId: Intercepts CClumpModelInfo::GetFrameFromId (0x4C53C0)
// Prevents crash when attaching upgrades to custom vehicle models that lack certain dummy/component frames (MTA:SA parity CMultiplayerSA_CrashFixHacks.cpp:2430)
static RwFrame*(__cdecl* g_origGetFrameFromId)(RpClump* clump, int id) = nullptr;

static RwFrame* __cdecl Hooked_GetFrameFromId(RpClump* clump, int id)
{
	if (!clump)
		return nullptr;

	RwFrame* frame = g_origGetFrameFromId ? g_origGetFrameFromId(clump, id) : nullptr;
	if (frame)
		return frame;

	// Ignore callers that legitimately expect and handle NULL (e.g., optional window frames)
	uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
	if (caller == 0x6D308F // CVehicle::SetWindowOpenFlag
		|| caller == 0x6D30BF // CVehicle::ClearWindowOpenFlag
		|| caller == 0x4C7DDE // CVehicleModelInfo::GetOriginalCompPosition
		|| caller == 0x4C96BD) // CVehicleModelInfo::CreateInstance
	{
		return nullptr;
	}

	// For custom models with missing dummy/component frames during upgrade installation
	// (0x6DFA61 CVehicle::AddUpgrade, 0x6D3847 CVehicle::AddReplacementUpgrade, etc.):
	// Search for nearest valid frame ID to prevent immediate NULL-pointer crash in RwFrameAddChild
	for (int i = 2; i < 40; ++i) {
		int newId = id + (i / 2) * ((i & 1) ? -1 : 1);
		if (newId >= 0 && g_origGetFrameFromId) {
			RwFrame* fallbackFrame = g_origGetFrameFromId(clump, newId);
			if (fallbackFrame)
				return fallbackFrame;
		}
	}

	// Root frame as safe ultimate fallback
	if (clump->object.parent) {
		return reinterpret_cast<RwFrame*>(clump->object.parent);
	}

	return nullptr;
}

// Hooked_AddUpgrade: Intercepts CVehicle::AddUpgrade (0x6DFA20) to support SA-MP AddVehicleComponent with custom vehicle dummy positions
static void(__fastcall* g_origAddUpgrade)(CVehicle* thisVehicle, void* edx, int modelIndex, int upgradeIndex) = nullptr;

static void __fastcall Hooked_AddUpgrade(CVehicle* thisVehicle, void* edx, int modelIndex, int upgradeIndex)
{
	if (!thisVehicle || !IsVehiclePointerValid(thisVehicle) || !thisVehicle->m_pRwClump) {
		return;
	}

	auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisVehicle);
	if (binding) {
		auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
		auto* baseModel = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(thisVehicle->m_nModelIndex));
		DummySwapGuard guard(baseModel, customModel);
		if (g_origAddUpgrade)
			g_origAddUpgrade(thisVehicle, edx, modelIndex, upgradeIndex);
		if (binding->hasWheelColor && modelIndex >= 1025 && modelIndex <= 1098) {
			CustomVehicleBindingManager::Instance().ApplyWheelColorToVehicle(thisVehicle, binding->wheelColorR, binding->wheelColorG, binding->wheelColorB);
		}
		return;
	}

	if (g_origAddUpgrade)
		g_origAddUpgrade(thisVehicle, edx, modelIndex, upgradeIndex);
}

// Hooked_RemoveUpgrade: Intercepts CVehicle::RemoveUpgrade (0x6D3630) to support SA-MP RemoveVehicleComponent
static void(__fastcall* g_origRemoveUpgrade)(CVehicle* thisVehicle, void* edx, int upgradeIndex) = nullptr;

static void __fastcall Hooked_RemoveUpgrade(CVehicle* thisVehicle, void* edx, int upgradeIndex)
{
	if (!thisVehicle || !IsVehiclePointerValid(thisVehicle) || !thisVehicle->m_pRwClump) {
		return;
	}

	auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisVehicle);
	if (binding) {
		auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
		auto* baseModel = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(thisVehicle->m_nModelIndex));
		DummySwapGuard guard(baseModel, customModel);
		if (g_origRemoveUpgrade)
			g_origRemoveUpgrade(thisVehicle, edx, upgradeIndex);
		return;
	}

	if (g_origRemoveUpgrade)
		g_origRemoveUpgrade(thisVehicle, edx, upgradeIndex);
}

// Hooked_DoesVehicleUseSiren: Intercepts CVehicle::DoesVehicleUseSiren (0x6D8470) to support emergency sirens on custom vehicles
static bool(__fastcall* g_origDoesVehicleUseSiren)(CVehicle* thisVehicle, void* edx) = nullptr;

static bool __fastcall Hooked_DoesVehicleUseSiren(CVehicle* thisVehicle, void* edx)
{
	if (!thisVehicle || !IsVehiclePointerValid(thisVehicle)) {
		if (g_origDoesVehicleUseSiren)
			return g_origDoesVehicleUseSiren(thisVehicle, edx);
		return false;
	}

	auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(thisVehicle);
	if (binding && binding->hasCustomSiren) {
		return true; // Siren capability is present on this custom vehicle
	}

	uint32_t modelIdForAudio = (binding && binding->customModelId > 0) ? binding->customModelId : static_cast<uint32_t>(thisVehicle->m_nModelIndex);
	auto audioDef = AudioExtender::GetVehicleAudio(modelIdForAudio);
	if (audioDef && audioDef->sirenType >= 0) {
		return (audioDef->sirenType > 0);
	}

	if (g_origDoesVehicleUseSiren)
		return g_origDoesVehicleUseSiren(thisVehicle, edx);

	return false;
}

// Hooked_GetVehicleSirenType: Hooks CAEVehicleAudioEntity::GetVehicleSirenType at 0x4F62A0
// Controls whether siren sound plays (*pSirenActive) and whether it's wail vs police (*pSirenType).
typedef void(__fastcall* GetVehicleSirenType_t)(CAEVehicleAudioEntity* thisEntity, void* edx, bool* pSirenActive, bool* pSirenType, cVehicleParams* pParams);
static GetVehicleSirenType_t g_origGetVehicleSirenType = nullptr;

static void __fastcall Hooked_GetVehicleSirenType(CAEVehicleAudioEntity* thisEntity, void* edx, bool* pSirenActive, bool* pSirenType, cVehicleParams* pParams)
{
	if (!thisEntity || !pParams || !pParams->m_pVehicle) {
		if (pSirenActive)
			*pSirenActive = false;
		if (pSirenType)
			*pSirenType = false;
		return;
	}

	CVehicle* pVehicle = pParams->m_pVehicle;

	if (thisEntity->m_bSoundsStopped) {
		if (pSirenActive)
			*pSirenActive = false;
		return;
	}

	auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(pVehicle);
	if (binding && binding->hasCustomSiren) {
		if (!binding->sirenEnabled || binding->sirenType == 0) {
			if (pSirenActive)
				*pSirenActive = false;
			return;
		}
		if (pSirenActive)
			*pSirenActive = true;
		if (pSirenType)
			*pSirenType = (binding->sirenType == 2);
		return;
	}

	uint32_t modelIdForAudio = (binding && binding->customModelId > 0) ? binding->customModelId : static_cast<uint32_t>(pVehicle->m_nModelIndex);
	auto audioDef = AudioExtender::GetVehicleAudio(modelIdForAudio);
	if (audioDef && audioDef->sirenType >= 0) {
		if (audioDef->sirenType == 0 || !pVehicle->bSirenOrAlarm) {
			if (pSirenActive)
				*pSirenActive = false;
			return;
		}
		if (pSirenActive)
			*pSirenActive = true;
		if (pSirenType)
			*pSirenType = (audioDef->sirenType == 2);
		return;
	}

	if (g_origGetVehicleSirenType) {
		g_origGetVehicleSirenType(thisEntity, edx, pSirenActive, pSirenType, pParams);
	}
}

static void __cdecl Hooked_RegisterCoronaTexture(
	unsigned int id, CEntity* attachTo, unsigned char red, unsigned char green, unsigned char blue,
	unsigned char alpha, CVector const& posn, float radius, float farClip, RwTexture* texture, eCoronaFlareType flaretype,
	bool enableReflection, bool checkObstacles, int _param_not_used, float angle, bool longDistance, float nearClip,
	unsigned char fadeState, float fadeSpeed, bool onlyFromBelow, bool reflectionDelay)
{
	static thread_local bool s_bInRegisterCoronaHook = false;
	if (s_bInRegisterCoronaHook) {
		if (g_origRegisterCoronaTexture) {
			g_origRegisterCoronaTexture(id, attachTo, red, green, blue, alpha, posn, radius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
		}
		return;
	}
	struct CoronaHookScopeGuard {
		bool& flag;
		CoronaHookScopeGuard(bool& f)
			: flag(f)
		{
			flag = true;
		}
		~CoronaHookScopeGuard() { flag = false; }
	} guard(s_bInRegisterCoronaHook);

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
	float customMult = 1.0f;
	eVehicleLightingCategory category = GetVehicleLightingCategory(pVeh, &customMult);
	LightScaleConfig cfg {};

	if (pVeh && pVeh->m_pHandlingData) {
		lightSize = isFront
			? static_cast<uint8_t>(pVeh->m_pHandlingData->m_nFrontLights)
			: static_cast<uint8_t>(pVeh->m_pHandlingData->m_nRearLights);
		cfg = GetVehicleLightScaleConfig(lightSize, isRear, category, customMult);
		scale = cfg.coronaScale;

		static uint32_t s_lastLog = 0;
		uint32_t now = GetTickCount();
		if (scale != 1.0f && (now - s_lastLog > 2000)) {
			s_lastLog = now;
			ClientLog(LogLevel::Debug, std::format("Corona scaled: cat={}, isFront={}, isRear={}, size={}, scale={:.2f}, radius={:.2f}->{:.2f}", static_cast<int>(category), isFront, isRear, static_cast<int>(lightSize), scale, radius, radius * scale));
		}
	}

	radius *= scale;
	if (isRear) {
		if (category == eVehicleLightingCategory::RCVehicle) {
			radius = std::clamp(radius, 0.03f, 0.08f);
		} else if (category == eVehicleLightingCategory::TwoWheeler) {
			radius = std::clamp(radius, 0.14f, 0.28f);
		} else {
			radius = std::clamp(radius, 0.18f, 0.38f);
		}

		// Directional camera-facing check (MTA:SA approach):
		// Taillights face backwards. When camera is in front of the vehicle or perpendicular,
		// the taillights are occluded by the vehicle chassis/doors and must not bleed through.
		if (pVeh) {
			CVector dirFwd(0.0f, 1.0f, 0.0f);
			if (pVeh->m_matrix) {
				dirFwd = pVeh->m_matrix->up;
			} else {
				dirFwd = pVeh->GetForward();
			}
			CVector vehBack = -dirFwd;

			CVector worldPos = posn;
			if (attachTo) {
				if (attachTo->m_matrix) {
					worldPos = *attachTo->m_matrix * posn;
				} else {
					worldPos = attachTo->GetPosition() + posn;
				}
			}

			CVector toCam = TheCamera.GetPosition() - worldPos;
			float camDist = toCam.Magnitude();
			if (camDist > 0.001f) {
				toCam /= camDist;
				float dot = vehBack.x * toCam.x + vehBack.y * toCam.y + vehBack.z * toCam.z;
				if (dot <= 0.02f) {
					// Camera is in front or directly to the side: light hidden behind car body
					return;
				}
				// Smooth directional Fresnel falloff
				float viewFactor = std::clamp((dot - 0.02f) / 0.65f, 0.0f, 1.0f);
				alpha = static_cast<unsigned char>(alpha * (0.40f + 0.60f * viewFactor));
			}
		}

		// Calibrated automotive taillight alpha:
		// Running lights (night driving): crisp, elegant, translucent red lens glow (~70 - 110).
		// Brake lights: rich, intense stopping signal (~160 - 225).
		if (alpha < 130) {
			alpha = static_cast<unsigned char>(std::clamp(static_cast<int>(alpha * 1.15f), 70, 110));
		} else {
			alpha = static_cast<unsigned char>(std::clamp(static_cast<int>(alpha * 1.05f), 160, 225));
		}

		// Deep, vibrant crimson red color
		if (red < 200)
			red = 230;
		if (green > 35)
			green = 25;
		if (blue > 35)
			blue = 25;

		farClip *= 1.15f;
	} else if (isFront) {
		if (scale > 1.0f) {
			farClip *= (1.0f + (scale - 1.0f) * 0.35f);
			if (lightSize == 2) { // LIGHTS_BIG: slightly more luminous
				float frontBoost = (category == eVehicleLightingCategory::Aircraft ? 1.25f : 1.12f);
				alpha = static_cast<unsigned char>(std::min(255, static_cast<int>(alpha * frontBoost)));
			}
		} else if (scale < 1.0f) {
			farClip *= (0.5f + scale * 0.5f);
		}
	}

	if (g_origRegisterCoronaTexture) {
		if (pVeh && (isFront || isRear) && cfg.allowClustering) {
			// Explicit front vs rear geometry:
			// Headlights have larger lens assemblies (6cm - 11cm offset).
			// Taillights have compact, sleek lens housings (5.5cm - 9.5cm offset).
			float barRadius = isFront ? (radius * 0.52f) : (radius * 0.50f);
			unsigned char subAlpha = isFront ? static_cast<unsigned char>(alpha * 0.32f) : static_cast<unsigned char>(alpha * 0.40f);
			float offsetDist = isFront
				? (std::clamp(radius * 0.16f, 0.06f, 0.11f) * cfg.offsetSpacingMult)
				: (std::clamp(radius * 0.32f, 0.055f, 0.095f) * cfg.offsetSpacingMult);

			if (lightSize == 0) { // LIGHTS_LONG: horizontally elongated sleek bar
				g_origRegisterCoronaTexture(id, attachTo, red, green, blue, alpha, posn, radius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);

				// In local coordinates (when attachTo != nullptr), X is vehicle width (Right/Left).
				// In world coordinates (when attachTo == nullptr), use vehicle matrix right vector.
				CVector dirRight(1.0f, 0.0f, 0.0f);
				if (!attachTo) {
					if (pVeh->m_matrix) {
						dirRight = pVeh->m_matrix->right;
					} else {
						dirRight = pVeh->GetRightDirection();
					}
				}

				CVector posL = posn - dirRight * offsetDist;
				CVector posR = posn + dirRight * offsetDist;

				uint32_t subId1 = id ^ 0x24000001;
				uint32_t subId2 = id ^ 0x48000001;
				eCoronaFlareType subFlare = FLARETYPE_NONE;

				g_origRegisterCoronaTexture(subId1, attachTo, red, green, blue, subAlpha, posL, barRadius, farClip, texture, subFlare, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
				g_origRegisterCoronaTexture(subId2, attachTo, red, green, blue, subAlpha, posR, barRadius, farClip, texture, subFlare, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
			} else if (lightSize == 3) { // LIGHTS_TALL: vertically elongated blade / pillar strip
				g_origRegisterCoronaTexture(id, attachTo, red, green, blue, alpha, posn, radius, farClip, texture, flaretype, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);

				// In local coordinates (when attachTo != nullptr), Z is vehicle height (Up/Down).
				// In world coordinates (when attachTo == nullptr), use vehicle matrix at (up) vector.
				CVector dirUp(0.0f, 0.0f, 1.0f);
				if (!attachTo) {
					if (pVeh->m_matrix) {
						dirUp = pVeh->m_matrix->at;
					} else {
						dirUp = pVeh->GetTopDirection();
					}
				}

				// Symmetrical vertical expansion centered on lamp dummy keeps lights inside the housing
				CVector pos1 = posn + dirUp * offsetDist;
				CVector pos2 = posn - dirUp * offsetDist;

				uint32_t subId1 = id ^ 0x24000001;
				uint32_t subId2 = id ^ 0x48000001;
				eCoronaFlareType subFlare = FLARETYPE_NONE;

				g_origRegisterCoronaTexture(subId1, attachTo, red, green, blue, subAlpha, pos1, barRadius, farClip, texture, subFlare, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
				g_origRegisterCoronaTexture(subId2, attachTo, red, green, blue, subAlpha, pos2, barRadius, farClip, texture, subFlare, enableReflection, checkObstacles, _param_not_used, angle, longDistance, nearClip, fadeState, fadeSpeed, onlyFromBelow, reflectionDelay);
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
		if (s_pCurrentHeadLightVehicle)
			pVeh = s_pCurrentHeadLightVehicle;
		else if (s_pCurrentTailLightVehicle)
			pVeh = s_pCurrentTailLightVehicle;
	}

	CVector modifiedPosn = posn ? *posn : CVector(0.0f, 0.0f, 0.0f);

	if (pVeh && pVeh->m_pHandlingData) {
		bool isFront = (s_pCurrentHeadLightVehicle != nullptr) || !(red > 100 && green < 50 && blue < 50);
		float customMult = 1.0f;
		eVehicleLightingCategory category = GetVehicleLightingCategory(pVeh, &customMult);
		uint8_t lightSize = isFront
			? static_cast<uint8_t>(pVeh->m_pHandlingData->m_nFrontLights)
			: static_cast<uint8_t>(pVeh->m_pHandlingData->m_nRearLights);

		LightScaleConfig cfg = GetVehicleLightScaleConfig(lightSize, !isFront, category, customMult);

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
			ClientLog(LogLevel::Debug, std::format("CarLightShadow scaled: cat={}, isFront={}, size={}, frontScale={:.2f}, sideScale={:.2f}", static_cast<int>(category), isFront, static_cast<int>(lightSize), cfg.shadowFrontScale, cfg.shadowSideScale));
		}
	}

	if (g_origStoreCarLightShadow) {
		CVehicle* passVeh = vehicle ? vehicle : pVeh;
		g_origStoreCarLightShadow(passVeh, id, texture, posn ? &modifiedPosn : nullptr, frontX, frontY, sideX, sideY, red, green, blue, maxViewAngle);
	}
}

#include <game_sa/CColLine.h>
#include <game_sa/CCollisionData.h>
#include <game_sa/CColModel.h>

using GetColModelFn = CColModel*(__thiscall*)(CEntity*);
static GetColModelFn g_origGetColModel = nullptr;

static CColModel* __fastcall Hooked_GetColModel(CEntity* thisPtr, void* /*edx*/)
{
	if (thisPtr && thisPtr->m_nType == ENTITY_TYPE_VEHICLE) {
		CColModel* customCol = CustomVehicleBindingManager::GetCollisionForVehicle(reinterpret_cast<CVehicle*>(thisPtr));
		if (customCol && customCol->m_pColData && customCol->m_pColData->m_pLines && customCol->m_pColData->m_nNumLines >= 4) {
			return customCol;
		}
	}
	return g_origGetColModel ? g_origGetColModel(thisPtr) : nullptr;
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

static RpClump* SafeRpClumpStreamRead(RwStream* stream, uint32_t* outExceptionCode)
{
	__try {
		return RpClumpStreamRead(stream);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		if (outExceptionCode) {
			*outExceptionCode = GetExceptionCode();
		}
		return nullptr;
	}
}

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
		fs::path audioEnginePath;
		fs::path audioAccelPath;
		fs::path audioDecelPath;
		fs::path audioBrakePath;
		fs::path audioCrashPath;
		bool queuedForFinalize = false;
		mutable std::mutex assetMutex;
	};

	std::queue<std::shared_ptr<PendingCustomVehicle>> m_completedQueue;
	std::mutex m_queueMutex;
	std::mutex m_pendingDefMutex;
	std::queue<std::shared_ptr<PendingCustomVehicle>> m_pendingDefQueue; // raw defs awaiting CreateModelAndBeginTransfers
	std::unordered_set<uint32_t> m_activeModelIds; // model IDs with active/completed transfer; guards against duplicate defs
	std::mutex m_pendingFinalizeMutex;
	std::queue<std::shared_ptr<PendingCustomVehicle>> m_pendingFinalizeQueue; // downloaded, awaiting player-spawn to finalize
	std::atomic<bool> m_pendingClearAll { false };
	std::queue<uint32_t> m_destructionQueue;
	std::mutex m_destructionMutex;
	bool m_runtimeInitialized = false;
	std::atomic<bool> g_localPlayerSpawned { false };

public:
	bool IsServerAuthorized() const
	{
		return HandlingManager::m_isServerAuthorized.load(std::memory_order_acquire);
	}

	bool IsLocalPlayerSpawned() const
	{
		// Primary: set by RPC_Spawn. Fallback: GTA actually has a local player ped in world.
		// This handles servers that skip RPC_Spawn (e.g. direct SetPlayerPos spawns).
		if (g_localPlayerSpawned.load(std::memory_order_acquire))
			return true;
		CPlayerPed* ped = FindPlayerPed(-1);
		return ped != nullptr;
	}

	void SetLocalPlayerSpawned(bool spawned)
	{
		g_localPlayerSpawned.store(spawned, std::memory_order_release);

		ClientLog(LogLevel::Info, std::format("Local player spawned={}", spawned));
	}

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

		if (bs.GetNumberOfUnreadBits() >= 8) {
			bs.Read(def.modelInfo.vehicleClass);
			bs.Read(def.modelInfo.wheelModelId);
			bs.Read(def.modelInfo.wheelScaleFront);
			bs.Read(def.modelInfo.wheelScaleRear);
			bs.Read(def.modelInfo.frequency);
			bs.Read(def.modelInfo.level);
			bs.Read(def.modelInfo.comprate);
			bs.Read(def.modelInfo.numExtras);
			if (bs.GetNumberOfUnreadBits() >= 8) {
				bs.Read(def.modelInfo.wheelUpgradeClass);
			}

			if (bs.GetNumberOfUnreadBits() >= sizeof(CustomVeh::Protocol::LightingOffsets) * 8) {
				bs.Read(def.lighting.headlightOffsetX);
				bs.Read(def.lighting.headlightOffsetY);
				bs.Read(def.lighting.headlightOffsetZ);
				bs.Read(def.lighting.taillightOffsetX);
				bs.Read(def.lighting.taillightOffsetY);
				bs.Read(def.lighting.taillightOffsetZ);
				bs.Read(def.lighting.headlightCustomX);
				bs.Read(def.lighting.headlightCustomY);
				bs.Read(def.lighting.headlightCustomZ);
				bs.Read(def.lighting.taillightCustomX);
				bs.Read(def.lighting.taillightCustomY);
				bs.Read(def.lighting.taillightCustomZ);
			}
		}

		if (def.flags & CustomVeh::Protocol::HasAnyAudio) {
			if (!bs.Read(def.customAudio.volume))
				return false;
			if (!bs.Read(def.customAudio.minDistance))
				return false;
			if (!bs.Read(def.customAudio.maxDistance))
				return false;
			if (!bs.Read(def.customAudio.pitchMultiplier))
				return false;
			if (!bs.Read(def.customAudio.accelPitchFactor))
				return false;
			if (!bs.Read(def.customAudio.muteNative))
				return false;

			if (def.flags & CustomVeh::Protocol::HasAudioEngine) {
				if (!ReadAssetDescriptor(bs, def.audioEngine))
					return false;
			}
			if (def.flags & CustomVeh::Protocol::HasAudioAccel) {
				if (!ReadAssetDescriptor(bs, def.audioAccel))
					return false;
			}
			if (def.flags & CustomVeh::Protocol::HasAudioDecel) {
				if (!ReadAssetDescriptor(bs, def.audioDecel))
					return false;
			}
			if (def.flags & CustomVeh::Protocol::HasAudioBrake) {
				if (!ReadAssetDescriptor(bs, def.audioBrake))
					return false;
			}
			if (def.flags & CustomVeh::Protocol::HasAudioCrash) {
				if (!ReadAssetDescriptor(bs, def.audioCrash))
					return false;
			}
		}

		return true;
	}

private:
	void CreateModelAndBeginTransfers(std::shared_ptr<PendingCustomVehicle> pending)
	{
		if (!pending) {
			return;
		}

		ClientLog(LogLevel::Info, std::format("Starting asset transfer for model {}", pending->def.customModelId));

		auto pushToQueue = [this, pending]() {
			if (pending->queuedForFinalize)
				return;
			pending->queuedForFinalize = true;

			std::lock_guard lock(m_queueMutex);
			m_completedQueue.push(pending);
		};

		auto beginAudio = [this, pending, pushToQueue]() {
			if (!(pending->def.flags & CustomVeh::Protocol::HasAnyAudio)) {
				pushToQueue();
				return;
			}

			struct AudioReq {
				ModelFileKind kind;
				const char* sha;
				fs::path* outPath;
			};
			std::vector<AudioReq> reqs;
			if ((pending->def.flags & CustomVeh::Protocol::HasAudioEngine) && pending->def.audioEngine.filename[0] != '\0')
				reqs.push_back({ ModelFileKind::AudioEngine, pending->def.audioEngine.sha256, &pending->audioEnginePath });
			if ((pending->def.flags & CustomVeh::Protocol::HasAudioAccel) && pending->def.audioAccel.filename[0] != '\0')
				reqs.push_back({ ModelFileKind::AudioAccel, pending->def.audioAccel.sha256, &pending->audioAccelPath });
			if ((pending->def.flags & CustomVeh::Protocol::HasAudioDecel) && pending->def.audioDecel.filename[0] != '\0')
				reqs.push_back({ ModelFileKind::AudioDecel, pending->def.audioDecel.sha256, &pending->audioDecelPath });
			if ((pending->def.flags & CustomVeh::Protocol::HasAudioBrake) && pending->def.audioBrake.filename[0] != '\0')
				reqs.push_back({ ModelFileKind::AudioBrake, pending->def.audioBrake.sha256, &pending->audioBrakePath });
			if ((pending->def.flags & CustomVeh::Protocol::HasAudioCrash) && pending->def.audioCrash.filename[0] != '\0')
				reqs.push_back({ ModelFileKind::AudioCrash, pending->def.audioCrash.sha256, &pending->audioCrashPath });

			if (reqs.empty()) {
				pushToQueue();
				return;
			}

			auto remaining = std::make_shared<std::atomic<size_t>>(reqs.size());
			for (const auto& r : reqs) {
				ModelTransferClient::Instance().RequestFile(
					pending->def.customModelId, r.kind, r.sha,
					[pending, pushToQueue, outPath = r.outPath, remaining](bool ok, const fs::path& path) {
						if (ok) {
							std::lock_guard<std::mutex> lock(pending->assetMutex);
							*outPath = path;
						}
						if (--(*remaining) == 0) {
							pushToQueue();
						}
					});
			}
		};

		auto beginCol = [this, pending, beginAudio]() {
			if (pending->def.col.filename[0] == '\0') {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->colState = AssetState::Ready;
				}
				beginAudio();
				return;
			}
			ModelTransferClient::Instance().RequestFile(
				pending->def.customModelId, ModelFileKind::Col, pending->def.col.sha256,
				[pending, beginAudio](bool ok, const fs::path& path) {
					{
						std::lock_guard<std::mutex> lock(pending->assetMutex);
						if (ok) {
							pending->colPath = path;
							pending->colState = AssetState::Ready;
						} else {
							pending->colState = AssetState::Failed;
						}
					}
					beginAudio();
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
				ClientLog(LogLevel::Error, std::format("Corrupt DFF for model {}: ExtractClump failed. Invalidating cache.", pending->def.customModelId));
				ModelCache::Instance().Invalidate(pending->def.customModelId, static_cast<uint8_t>(ModelFileKind::Dff));
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
		if (!pending)
			return;

		if (!IsLocalPlayerSpawned()) {
			ClientLog(LogLevel::Debug, std::format("Deferring finalization of model {} because local player has not spawned.", pending->def.customModelId));

			std::lock_guard<std::mutex> lock(m_pendingFinalizeMutex);
			m_pendingFinalizeQueue.push(std::move(pending));
			return;
		}

		ClientLog(LogLevel::Debug, std::format("FinalizeCustomVehicle: model={} dffState={} txdState={} txdPath='{}' dffBytes={} txdBytes={}", pending->def.customModelId, static_cast<int>(pending->dffState), static_cast<int>(pending->txdState), pending->txdPath.string(), pending->dff.size(), pending->txd.size()));

		if (pending->dffState != AssetState::Ready || pending->txdState != AssetState::Ready) {
			ClientLog(LogLevel::Error, std::format("FinalizeCustomVehicle: ABORT model={} - assets not ready (dff={} txd={})", pending->def.customModelId, (int)pending->dffState, (int)pending->txdState));
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("Failed to load essential assets for model {}", pending->def.customModelId).c_str());
			return;
		}

		if (!pending->modelInfo) {
			pending->modelInfo = StreamingExtender::CreateCustomModel(pending->def);
			if (!pending->modelInfo) {
				SendMsg(0xFF0000, std::format("Failed to create GTA model for custom model {}", pending->def.customModelId).c_str());
				return;
			}
		}

		CVehicleModelInfo* newModel = pending->modelInfo;
		if (!newModel)
			return;

		ClientLog(LogLevel::Debug, std::format("FinalizeCustomVehicle: model={} modelInfo=0x{:X} txdPath='{}' txdBytes={}", pending->def.customModelId, reinterpret_cast<uintptr_t>(newModel), pending->txdPath.string(), pending->txd.size()));

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
		ClientLog(LogLevel::Debug, std::format("TXD buffer ready: model={} bytes={} path='{}'", pending->def.customModelId, pending->txd.size(), pending->txdPath.string()));

		if (pending->txd.empty()) {
			ClientLog(LogLevel::Error, std::format("FinalizeCustomVehicle: ABORT model={} - txd empty after read attempt (txdPath='{}')", pending->def.customModelId, pending->txdPath.string()));
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("TXD file empty or unreadable for model {}", pending->def.customModelId).c_str());
			return;
		}

		std::string txdName = std::format("custom_veh_{}", pending->def.customModelId);
		int txdSlot = CTxdStore::FindTxdSlot(txdName.c_str());
		if (txdSlot < 0) {
			txdSlot = CTxdStore::AddTxdSlot(txdName.c_str());
		} else {
			CTxdStore::RemoveTxd(txdSlot);
		}

		if (txdSlot < 0) {
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("Failed to allocate TXD slot for model {}", pending->def.customModelId).c_str());
			return;
		}

		newModel->m_nTxdIndex = txdSlot;

		RwMemory txdMem { pending->txd.data(), static_cast<RwUInt32>(pending->txd.size()) };
		RwStream* txdStream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &txdMem);

		if (!txdStream) {
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			SendMsg(0xFF0000, std::format("Failed to open TXD stream for model {}", pending->def.customModelId).c_str());
			return;
		}

		if (!CTxdStore::LoadTxd(txdSlot, txdStream)) {
			RwStreamClose(txdStream, nullptr);
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			ModelCache::Instance().Invalidate(pending->def.customModelId, static_cast<uint8_t>(ModelFileKind::Txd));
			SendMsg(0xFF0000, std::format("Failed to load TXD for model {}", pending->def.customModelId).c_str());
			return;
		}
		ClientLog(LogLevel::Info, std::format("TXD loaded: model={} slot={} name='{}'", pending->def.customModelId, txdSlot, txdName));

		RwStreamClose(txdStream, nullptr);

		CTxdStore::AddRef(txdSlot);

		CTxdStore::PushCurrentTxd();
		CTxdStore::SetCurrentTxd(txdSlot);

		RwMemory dffMem { pending->dff.data(), static_cast<RwUInt32>(pending->dff.size()) };
		RwStream* dffStream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &dffMem);
		if (dffStream != nullptr) {
			if (RwStreamFindChunk(dffStream, rwID_CLUMP, nullptr, nullptr)) {
				ClientLog(LogLevel::Debug, std::format("Loading DFF for model {}: bytes={}, txdSlot={}", pending->def.customModelId, pending->dff.size(), txdSlot));

				// CRITICAL FIX: GTA SA's collision plugin reader (0x41B2BD) executes during
				// RpClumpStreamRead if the DFF has an embedded collision chunk. It accesses
				// ds:[0x009689E0] (set via 0x0041A750) to attach the collision model to the
				// model info and flag ownership (0x41B2C7: or byte ptr [eax+13h], 8).
				// If ds:[0x009689E0] is null, it crashes at 0x4C4BD2 (inside SetColModel)
				// or at 0x41B2C7. Setting it here mirrors GTA SA CStreaming::ConvertModelToLoaded (0x536798).
				auto SetCollisionModel = reinterpret_cast<void(__cdecl*)(CBaseModelInfo*)>(0x0041B350);
				SetCollisionModel(newModel);
				*reinterpret_cast<CBaseModelInfo**>(0x009689E0) = newModel;
				auto UseCommonVehicleTexDictionary = reinterpret_cast<void(__cdecl*)()>(0x004C75A0);
				auto StopUsingCommonVehicleTexDictionary = reinterpret_cast<void(__cdecl*)()>(0x004C75C0);
				UseCommonVehicleTexDictionary();

				uint32_t clumpException = 0;
				RpClump* pClump = SafeRpClumpStreamRead(dffStream, &clumpException);

				SetCollisionModel(nullptr);
				*reinterpret_cast<CBaseModelInfo**>(0x009689E0) = nullptr;
				StopUsingCommonVehicleTexDictionary();

				ClientLog(LogLevel::Debug, std::format("RpClumpStreamRead model {} -> clump=0x{:08X} (exc=0x{:08X})", pending->def.customModelId, reinterpret_cast<std::uintptr_t>(pClump), clumpException));
				RwStreamClose(dffStream, nullptr);
				if (pClump) {
					auto* baseModelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(static_cast<int>(pending->def.visualBaseModel)));
					bool FinalizeRet = StreamingExtender::FinalizeClump(newModel, pClump, baseModelInfo);
					ClientLog(LogLevel::Debug, std::format("StreamingExtender::FinalizeClump returned model={} result={} m_pRwClump=0x{:08X} m_pVehicleStruct=0x{:08X}", pending->def.customModelId, FinalizeRet, reinterpret_cast<std::uintptr_t>(newModel->m_pRwClump), reinterpret_cast<std::uintptr_t>(newModel->m_pVehicleStruct)));
					if (!FinalizeRet) {
						CTxdStore::PopCurrentTxd();
						StreamingExtender::DestroyCustomModel(pending->def.customModelId);
						SendMsg(0xFF0000, std::format("Failed to finalize clump for model {}", pending->def.customModelId).c_str());
						return;
					}

					if (newModel && newModel->m_pVehicleStruct) {
						const auto& ltg = pending->def.lighting;
						// Headlight dummy (slot 0):
						if (ltg.headlightCustomX != 0.0f || ltg.headlightCustomY != 0.0f || ltg.headlightCustomZ != 0.0f) {
							if (ltg.headlightCustomX != 0.0f)
								newModel->m_pVehicleStruct->m_avDummyPos[0].x = fabsf(ltg.headlightCustomX);
							if (ltg.headlightCustomY != 0.0f)
								newModel->m_pVehicleStruct->m_avDummyPos[0].y = ltg.headlightCustomY;
							if (ltg.headlightCustomZ != 0.0f)
								newModel->m_pVehicleStruct->m_avDummyPos[0].z = ltg.headlightCustomZ;
						}
						newModel->m_pVehicleStruct->m_avDummyPos[0].x += ltg.headlightOffsetX;
						newModel->m_pVehicleStruct->m_avDummyPos[0].y += ltg.headlightOffsetY;
						newModel->m_pVehicleStruct->m_avDummyPos[0].z += ltg.headlightOffsetZ;

						// Taillight dummy (slot 1):
						if (ltg.taillightCustomX != 0.0f || ltg.taillightCustomY != 0.0f || ltg.taillightCustomZ != 0.0f) {
							if (ltg.taillightCustomX != 0.0f)
								newModel->m_pVehicleStruct->m_avDummyPos[1].x = fabsf(ltg.taillightCustomX);
							if (ltg.taillightCustomY != 0.0f)
								newModel->m_pVehicleStruct->m_avDummyPos[1].y = ltg.taillightCustomY;
							if (ltg.taillightCustomZ != 0.0f)
								newModel->m_pVehicleStruct->m_avDummyPos[1].z = ltg.taillightCustomZ;
						}
						newModel->m_pVehicleStruct->m_avDummyPos[1].x += ltg.taillightOffsetX;
						newModel->m_pVehicleStruct->m_avDummyPos[1].y += ltg.taillightOffsetY;
						newModel->m_pVehicleStruct->m_avDummyPos[1].z += ltg.taillightOffsetZ;

						ClientLog(LogLevel::Info, std::format("Applied lighting dummy config for model {}: headlights=({:.3f}, {:.3f}, {:.3f}), taillights=({:.3f}, {:.3f}, {:.3f})", pending->def.customModelId, newModel->m_pVehicleStruct->m_avDummyPos[0].x, newModel->m_pVehicleStruct->m_avDummyPos[0].y, newModel->m_pVehicleStruct->m_avDummyPos[0].z, newModel->m_pVehicleStruct->m_avDummyPos[1].x, newModel->m_pVehicleStruct->m_avDummyPos[1].y, newModel->m_pVehicleStruct->m_avDummyPos[1].z));
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
								ModelCache::Instance().Invalidate(pending->def.customModelId, static_cast<uint8_t>(ModelFileKind::Col));
								SendMsg(0xFF8800, std::format("Warning: Failed to parse COL for model {}", pending->def.customModelId).c_str());
							}
						}
					}
					// Fallback & Adaptation: Ensure the vehicle model info has a valid collision model with suspension lines
					CBaseModelInfo* visualBase = GetEngineModelInfo(static_cast<int>(pending->def.visualBaseModel));
					if (!newModel->m_pColModel) {
						if (visualBase && visualBase->m_pColModel) {
							newModel->m_pColModel = visualBase->m_pColModel;
							newModel->bDoWeOwnTheColModel = 0;
							*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(newModel) + 0x12) &= ~0x80;
							*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(newModel) + 0x13) &= ~0x08;
							ClientLog(LogLevel::Info, std::format("Model {} using base collision model from base {}", pending->def.customModelId, pending->def.visualBaseModel));
						}
					} else {
						// If custom collision exists (external COL) but lacks wheel suspension lines,
						// inject the base model's 4 suspension lines into the collision data.
						bool hasValidLines = (newModel->m_pColModel->m_pColData && newModel->m_pColModel->m_pColData->m_pLines && newModel->m_pColModel->m_pColData->m_nNumLines >= 4);
						if (!hasValidLines && newModel->m_pColModel->m_pColData && visualBase && visualBase->m_pColModel && visualBase->m_pColModel->m_pColData && visualBase->m_pColModel->m_pColData->m_pLines && visualBase->m_pColModel->m_pColData->m_nNumLines >= 4) {
							auto pMalloc = reinterpret_cast<void*(__cdecl*)(size_t)>(0x72F420);
							void* lineMem = pMalloc(sizeof(CColLine) * 4);
							if (lineMem) {
								memcpy(lineMem, visualBase->m_pColModel->m_pColData->m_pLines, sizeof(CColLine) * 4);
								newModel->m_pColModel->m_pColData->m_pLines = reinterpret_cast<CColLine*>(lineMem);
								newModel->m_pColModel->m_pColData->m_nNumLines = 4;
								hasValidLines = true;
								ClientLog(LogLevel::Info, std::format("Model {}: Injected 4 suspension lines from base model {}", pending->def.customModelId, pending->def.visualBaseModel));
							}
						}
						if (!hasValidLines) {
							// If custom collision cannot provide valid suspension lines, safely delete it and revert to base vehicle's collision
							if (newModel->m_pColModel) {
								reinterpret_cast<void(__thiscall*)(CBaseModelInfo*)>(0x4C4C40)(newModel);
							}
							if (visualBase && visualBase->m_pColModel) {
								newModel->m_pColModel = visualBase->m_pColModel;
								newModel->bDoWeOwnTheColModel = 0;
								*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(newModel) + 0x12) &= ~0x80;
								*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(newModel) + 0x13) &= ~0x08;
								ClientLog(LogLevel::Warning, std::format("Model {} reverted to base collision model {} due to missing suspension lines", pending->def.customModelId, pending->def.visualBaseModel));
							}
						}
					}
				} else {
					CTxdStore::PopCurrentTxd();
					StreamingExtender::DestroyCustomModel(pending->def.customModelId);
					ModelCache::Instance().Invalidate(pending->def.customModelId, static_cast<uint8_t>(ModelFileKind::Dff));
					SendMsg(0xFF0000, std::format("Failed to parse DFF for model {}", pending->def.customModelId).c_str());
					return;
				}
			} else {
				RwStreamClose(dffStream, nullptr);
				CTxdStore::PopCurrentTxd();
				StreamingExtender::DestroyCustomModel(pending->def.customModelId);
				ModelCache::Instance().Invalidate(pending->def.customModelId, static_cast<uint8_t>(ModelFileKind::Dff));
				SendMsg(0xFF0000, std::format("[CustomVeh] No CLUMP chunk in DFF for model {}", pending->def.customModelId).c_str());
				return;
			}
		} else {
			CTxdStore::PopCurrentTxd();
			StreamingExtender::DestroyCustomModel(pending->def.customModelId);
			ModelCache::Instance().Invalidate(pending->def.customModelId, static_cast<uint8_t>(ModelFileKind::Dff));
			SendMsg(0xFF0000, std::format("Failed to open DFF stream for model {}", pending->def.customModelId).c_str());
			return;
		}

		CTxdStore::PopCurrentTxd();
		ClientLog(LogLevel::Info, std::format("Model {} finalized successfully. modelInfo=0x{:X}, rwClump=0x{:X}, txdSlot={}", pending->def.customModelId, reinterpret_cast<std::uintptr_t>(newModel), reinterpret_cast<std::uintptr_t>(newModel->m_pRwClump), newModel->m_nTxdIndex));

		if (pending->def.flags & CustomVeh::Protocol::HasAnyAudio) {
			AudioExtender::RegisterCustomAudio(
				pending->def.customModelId,
				pending->def.customAudio,
				pending->audioEnginePath,
				pending->audioAccelPath,
				pending->audioDecelPath,
				pending->audioBrakePath,
				pending->audioCrashPath);
		}
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
				StreamingExtender::InstallHooks();
				TransferConfig::Instance().Load();
				ModelCache::Instance().Sweep();
				m_runtimeInitialized = true;
			}
		});

		Events::shutdownRwEvent.Add([this]() {
			ModelTransferClient::Instance().CancelAll("client shutdown");
			ModelTransferClient::Instance().Shutdown();
			AudioExtender::RestoreHooks();
			StreamingExtender::RestoreHooks();
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
		ClientLog(LogLevel::Info, std::format("Received definition for model {} (DFF='{}', TXD='{}', COL='{}').", def.customModelId, def.dff.filename, def.txd.filename, def.col.filename));
		CustomVehicleBindingManager::SetBaseModelId(def.customModelId, def.visualBaseModel);
		AudioExtender::RegisterVehicleAudio(def.customModelId, def.audioBaseModel, def.engineSoundId.OnSound, def.engineSoundId.OffSound, def.celerateSoundId.accelerateSound, def.celerateSoundId.decelerateSound);

		// Guard: if we already started a transfer for this model ID, ignore the duplicate.
		{
			std::lock_guard<std::mutex> lock(m_pendingDefMutex);
			if (m_activeModelIds.count(def.customModelId)) {
				ClientLog(LogLevel::Warning, std::format("Ignoring duplicate VehicleDefinition for model {}.", def.customModelId));
				return;
			}
			m_activeModelIds.insert(def.customModelId);
		}

		auto pending = std::make_shared<PendingCustomVehicle>();
		pending->def = def;

		// Defer asset download until local player spawns
		if (!IsLocalPlayerSpawned()) {
			std::lock_guard<std::mutex> lock(m_pendingDefMutex);
			m_pendingDefQueue.push(pending);
			ClientLog(LogLevel::Debug, std::format("Player not spawned yet; queued asset download for model {} until spawn.", def.customModelId));
			return;
		}

		// Dispatch to the main game thread - CreateModelAndBeginTransfers sends RakNet packets
		// and must not run on the network receive thread.
		MainThreadQueue::Instance().Push([this, pending]() {
			ClientLog(LogLevel::Info, std::format("Starting asset transfer for model {} (dispatched to main thread).", pending->def.customModelId));
			CreateModelAndBeginTransfers(pending);
		});
	}

	void PushDestructionCommand(uint32_t modelId)
	{
		std::lock_guard<std::mutex> lock(m_destructionMutex);
		m_destructionQueue.push(modelId);
	}

	void ProcessPendingDefinitions()
	{
		if (!IsLocalPlayerSpawned())
			return;

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
			if (StreamingExtender::IsCustomModel(pending->def.customModelId)) {
				ClientLog(LogLevel::Warning, std::format("Custom model {} already exists; skipping creation.", pending->def.customModelId));
				continue;
			}
			ClientLog(LogLevel::Info, std::format("Activating custom model {} after local player spawn (remaining queue={}).", pending->def.customModelId, localQueue.size()));
			AudioExtender::RegisterVehicleAudio(pending->def.customModelId, pending->def.audioBaseModel, pending->def.engineSoundId.OnSound, pending->def.engineSoundId.OffSound, pending->def.celerateSoundId.accelerateSound, pending->def.celerateSoundId.decelerateSound);
			CreateModelAndBeginTransfers(pending);
		}
	}

	// Process models that finished downloading while the player was not yet spawned.
	// These go directly to FinalizeCustomVehicle - no re-download needed.
	void ProcessPendingFinalizations()
	{
		if (!IsLocalPlayerSpawned())
			return;

		std::queue<std::shared_ptr<PendingCustomVehicle>> localQueue;
		{
			std::lock_guard<std::mutex> lock(m_pendingFinalizeMutex);
			if (m_pendingFinalizeQueue.empty())
				return;
			localQueue.swap(m_pendingFinalizeQueue);
		}

		ClientLog(LogLevel::Debug, std::format("ProcessPendingFinalizations: processing {} deferred finalizations", localQueue.size()));

		while (!localQueue.empty()) {
			auto pending = localQueue.front();
			localQueue.pop();
			ClientLog(LogLevel::Info, std::format("Player spawned - finalizing deferred model {}.", pending->def.customModelId));
			FinalizeCustomVehicle(pending);
		}
	}

	void ProcessPendingDestructions()
	{
		if (!IsLocalPlayerSpawned())
			return;

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
			// Drop all queued model work before destroying streaming data
			{
				std::lock_guard<std::mutex> lock(m_pendingDefMutex);
				while (!m_pendingDefQueue.empty())
					m_pendingDefQueue.pop();
				m_activeModelIds.clear(); // allow fresh VehicleDefinitions after reconnect
			}
			{
				std::lock_guard<std::mutex> lock(m_pendingFinalizeMutex);
				while (!m_pendingFinalizeQueue.empty())
					m_pendingFinalizeQueue.pop();
			}
			{
				std::lock_guard<std::mutex> lock(m_queueMutex);
				while (!m_completedQueue.empty())
					m_completedQueue.pop();
			}
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
	ClientLog(LogLevel::Info, "InitializeHooks thread started, waiting for samp.dll...");
	while (GetModuleHandleA("samp.dll") == nullptr) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	ClientLog(LogLevel::Info, std::format("samp.dll loaded at 0x{:X}", rakhook::samp_addr()));

	while (!ASIinitialized) {
		if (rakhook::samp_addr() && rakhook::samp_version() != rakhook::samp_ver::unknown) {
			if (IsGameInitialized()) {
				if (rakhook::initialize()) {
					ASIinitialized = true;
					ClientLog(LogLevel::Info, std::format("rakhook initialized successfully (samp_version={})", static_cast<int>(rakhook::samp_version())));
					break;
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	rakhook::on_receive_rpc += [](unsigned char& id, RakNet::BitStream* bs) -> bool {
		if (id == RPC_InitGame) {
			ClientLog(LogLevel::Info, "Received RPC_InitGame (139), sending init packet...");
			_customVehInstance.SetLocalPlayerSpawned(false);
			g_windowVisible.store(false, std::memory_order_relaxed);
			ModelTransferClient::Instance().ClearHistory();
			_customVehInstance.RequestClearAllCustomModels();
			if (!HandlingManager::ProcessAction(ACTION_RESET_ALL, nullptr)) {
				ClientLog(LogLevel::Error, "HandlingManager::ProcessAction failed to process ACTION_RESET_ALL.");
			}
			if (!_customVehInstance.IsServerAuthorized()) {
				HandlingManager::ResetInitState();
				HandlingManager::SendInitPacket();
				ClientLog(LogLevel::Info, "Game session initialized; handshake sent.");
			} else {
				ClientLog(LogLevel::Debug, "localPlayer seems to be already authorized...");
			}
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
		} else if (id == RPC_Spawn) {
			ClientLog(LogLevel::Info, "RPC_Spawn received.");
			_customVehInstance.SetLocalPlayerSpawned(true);
			_customVehInstance.ProcessPendingDefinitions();
			_customVehInstance.ProcessPendingFinalizations();
			CustomVehicleBindingManager::Instance().Process();
			return true;
		} else if (id == RPC_GameModeRestart) {
			ClientLog(LogLevel::Info, "Received RPC_GameModeRestart (142), resetting session state and re-sending init packet...");
			_customVehInstance.SetLocalPlayerSpawned(false);
			g_windowVisible.store(false, std::memory_order_relaxed);
			HandlingManager::m_isServerAuthorized.store(false, std::memory_order_release);
			ModelTransferClient::Instance().CancelAll("gamemode restart");
			ModelTransferClient::Instance().ClearHistory();
			_customVehInstance.RequestClearAllCustomModels();
			if (!HandlingManager::ProcessAction(ACTION_RESET_ALL, nullptr)) {
				ClientLog(LogLevel::Error, "HandlingManager::ProcessAction failed to process ACTION_RESET_ALL.");
			}
			HandlingManager::ResetInitState();
			HandlingManager::SendInitPacket();
			ClientLog(LogLevel::Info, "GameModeRestart processed; handshake re-sent.");
			return true;
		} else if (id == RPC_ServerQuit) {
			ClientLog(LogLevel::Info, "Received RPC_ServerQuit (166).");
			_customVehInstance.SetLocalPlayerSpawned(false);
			g_windowVisible.store(false, std::memory_order_relaxed);
			HandlingManager::m_isServerAuthorized.store(false, std::memory_order_release);
			return true;
		}
		return true;
	};
	rakhook::on_send_rpc += [](int& id, RakNet::BitStream* bs, PacketPriority& priority, PacketReliability& reliability, char& ord_channel, bool& sh_timestamp) -> bool {
		if (id == RPC_RequestClass) {
			ClientLog(LogLevel::Debug, "RPC_RequestClass sent.");
			_customVehInstance.SetLocalPlayerSpawned(false);
			if (!_customVehInstance.IsServerAuthorized() && rakhook::orig && rakhook::orig->IsConnected()) {
				HandlingManager::ResetInitState();
				HandlingManager::SendInitPacket();
				ClientLog(LogLevel::Info, "Handshake sent on RPC_RequestClass fallback.");
			}
			return true;
		} else if (id == RPC_RequestSpawn) {
			ClientLog(LogLevel::Debug, "RPC_RequestSpawn sent.");
			_customVehInstance.SetLocalPlayerSpawned(false);
			if (!_customVehInstance.IsServerAuthorized() && rakhook::orig && rakhook::orig->IsConnected()) {
				HandlingManager::ResetInitState();
				HandlingManager::SendInitPacket();
				ClientLog(LogLevel::Info, "Handshake sent on RPC_RequestSpawn fallback.");
			}
			return true;
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
					SendMsg(0xFF0000, "Failed to read vehicle definition from packet");
					return false;
				}
				_customVehInstance.HandleCustomVehicleDef(def);
				return false;
			} else if (actionID == CustomVehAction::CustomVehicleDestroy) {
				uint32_t customModelId;
				if (!bs.Read(customModelId)) {
					SendMsg(0xFF0000, "Failed to read vehicle modelId from packet");
					return false;
				}
				ModelTransferClient::Instance().CancelModelTransfers(customModelId);
				_customVehInstance.PushDestructionCommand(customModelId);
				return false;
			}
			return HandlingManager::ProcessAction(actionID, &bs);
		} else if (packetId == ID_CONNECTION_REQUEST_ACCEPTED) {
			ClientLog(LogLevel::Info, "Received ID_CONNECTION_REQUEST_ACCEPTED, sending init packet...");
			ModelCache::Instance().ReloadManifest();
			_customVehInstance.SetLocalPlayerSpawned(false);
			g_windowVisible.store(false, std::memory_order_relaxed);
			ModelTransferClient::Instance().ClearHistory();
			_customVehInstance.RequestClearAllCustomModels();
			if (!HandlingManager::ProcessAction(ACTION_RESET_ALL, nullptr)) {
				ClientLog(LogLevel::Error, "HandlingManager::ProcessAction failed to process ACTION_RESET_ALL.");
			}
			if (!_customVehInstance.IsServerAuthorized() && rakhook::orig && rakhook::orig->IsConnected()) {
				HandlingManager::ResetInitState();
				HandlingManager::SendInitPacket();
				ClientLog(LogLevel::Info, "Connection Request Accepted; handshake sent.");
			}
		} else if (packetId == ID_DISCONNECTION_NOTIFICATION || packetId == ID_CONNECTION_LOST || packetId == ID_CONNECTION_BANNED) {
			ClientLog(LogLevel::Info, "Disconnected from server");
			ModelCache::Instance().SaveManifest();
			_customVehInstance.SetLocalPlayerSpawned(false);
			// Hide download window on disconnect
			g_windowVisible.store(false, std::memory_order_relaxed);
			HandlingManager::m_isServerAuthorized.store(false, std::memory_order_release);
			ModelTransferClient::Instance().CancelAll("server connection lost");
			ModelTransferClient::Instance().ClearHistory();
			_customVehInstance.RequestClearAllCustomModels();
			HandlingManager::ProcessAction(ACTION_RESET_ALL, nullptr);
		}
		return true;
	};
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
			ClientLog(LogLevel::Error, std::format("Exception in ProcessPendingCommands: {}", e.what()));
		} catch (...) {
			ClientLog(LogLevel::Error, "Unknown exception in ProcessPendingCommands");
		}

		if (!ASIinitialized) {
			return;
		}

		static uint32_t s_lastAuthRetryTick = 0;
		if (!_customVehInstance.IsServerAuthorized() && rakhook::orig && rakhook::orig->IsConnected()) {
			uint32_t now = GetTickCount();
			if (now - s_lastAuthRetryTick > 3000) {
				s_lastAuthRetryTick = now;
				HandlingManager::ResetInitState();
				HandlingManager::SendInitPacket();
				ClientLog(LogLevel::Debug, "Periodic handshake sent (awaiting authorization)...");
			}
		}

		_customVehInstance.ProcessPendingDefinitions();
		_customVehInstance.ProcessCompletedDownloads();
		_customVehInstance.ProcessPendingFinalizations();
		CustomVehicleBindingManager::Instance().Process();
		AudioExtender::ProcessVehicleAudio();

		// Render visual effects (neon underglow and emergency roof strobes) for custom vehicles
		CustomVehicleBindingManager::Instance().ForEachBinding([](uint16_t vehId, const CustomVehicleBindingManager::Binding& b) {
			if (!b.modelApplied || !b.appliedGameVehicle || !IsVehiclePointerValid(b.appliedGameVehicle))
				return;

			CVehicle* pVeh = b.appliedGameVehicle;
			// 'bIsVisible' is a bitfield in CEntity - no m_ prefix (CEntity.h:36)
			if (!pVeh->m_pRwObject || !pVeh->bIsVisible)
				return;

			CMatrixLink& mat = pVeh->GetMatrix();

			// 1. Render chassis neon underglow if enabled
			if (b.hasNeon && b.neonEnabled) {
				// CMatrix has no TransformPoint(); use operator*(CMatrix, CVector) from CMatrix.h:95
				// which performs: mat.pos + mat.right*v.x + mat.forward*v.y + mat.up*v.z
				CVector worldLeft = mat * CVector(-0.85f, 0.0f, -0.35f);
				CVector worldRight = mat * CVector(0.85f, 0.0f, -0.35f);

				// Left chassis neon corona
				CCoronas::RegisterCorona(
					reinterpret_cast<uintptr_t>(pVeh) + 0x10,
					nullptr,
					b.neonR, b.neonG, b.neonB, 255,
					worldLeft,
					b.neonSize,
					45.0f,
					CORONATYPE_SHINYSTAR,
					FLARETYPE_NONE,
					false, false, 0, 0.0f, false, 0.15f, 0, 15.0f, false, false);

				// Right chassis neon corona
				CCoronas::RegisterCorona(
					reinterpret_cast<uintptr_t>(pVeh) + 0x11,
					nullptr,
					b.neonR, b.neonG, b.neonB, 255,
					worldRight,
					b.neonSize,
					45.0f,
					CORONATYPE_SHINYSTAR,
					FLARETYPE_NONE,
					false, false, 0, 0.0f, false, 0.15f, 0, 15.0f, false, false);

				// Chassis ambient ground shadow
				if (gpShadowCarTex) {
					CVector chassisBottom = mat * CVector(0.0f, 0.0f, -0.45f); // operator*(CMatrix, CVector) - local-to-world
					CVector forward = mat.GetForward();
					CVector right = mat.GetRight();
					CShadows::StoreCarLightShadow(
						pVeh,
						static_cast<int>(reinterpret_cast<uintptr_t>(pVeh) + 0x12),
						gpShadowCarTex,
						&chassisBottom,
						forward.x * 2.2f, forward.y * 2.2f,
						right.x * 1.3f, right.y * 1.3f,
						b.neonR, b.neonG, b.neonB,
						5.0f);
				}
			}

			// 2. Render visual emergency strobe lights if siren is enabled
			if (b.hasCustomSiren && b.sirenEnabled) {
				int modelId = pVeh->m_nModelIndex;
				bool nativeSirenModel = (modelId == 596 || modelId == 597 || modelId == 598 || modelId == 599 || modelId == 490 || modelId == 601 || modelId == 528 || modelId == 407 || modelId == 416 || modelId == 427 || modelId == 544 || modelId == 523);

				// For models without native GTA:SA siren coronas, render alternating Red/Blue roof strobes
				if (!nativeSirenModel) {
					uint32_t phase = (GetTickCount() / 160) % 2;

					float roofZ = 0.75f;
					auto* col = pVeh->GetColModel();
					if (col) {
						roofZ = col->m_boundBox.m_vecMax.z + 0.08f;
					}

					CVector roofLeft = mat * CVector(-0.35f, 0.0f, roofZ);
					CVector roofRight = mat * CVector(0.35f, 0.0f, roofZ);

					if (phase == 0) {
						CCoronas::RegisterCorona(
							reinterpret_cast<uintptr_t>(pVeh) + 0x30,
							nullptr,
							255, 20, 20, 255,
							roofLeft,
							0.75f,
							65.0f,
							CORONATYPE_SHINYSTAR,
							FLARETYPE_NONE,
							false, false, 0, 0.0f, false, 0.15f, 0, 15.0f, false, false);
					} else {
						CCoronas::RegisterCorona(
							reinterpret_cast<uintptr_t>(pVeh) + 0x31,
							nullptr,
							20, 80, 255, 255,
							roofRight,
							0.75f,
							65.0f,
							CORONATYPE_SHINYSTAR,
							FLARETYPE_NONE,
							false, false, 0, 0.0f, false, 0.15f, 0, 15.0f, false, false);
					}
				}
			}
		});

		_customVehInstance.ProcessPendingDestructions();
		_customVehInstance.ProcessPendingClearAll();

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

				uint32_t audioModelId = static_cast<uint32_t>(cur.gameVeh->m_nModelIndex);
				auto* curBinding = CustomVehicleBindingManager::Instance().FindByVehicle(cur.gameVeh);
				if (curBinding && curBinding->customModelId > 0) {
					audioModelId = curBinding->customModelId;
				}

				if (AudioExtender::GetVehicleAudio(audioModelId).has_value()) {
					std::optional<AudioExtender::CustomVehicleAudioState*> audioState = AudioExtender::GetOrCreateAudioState(*cur.gameVeh);
					if (audioState) {
						if (!AudioExtender::InitialiseCustomVehicleAudio(audioState.value()->engine, *cur.gameVeh, audioModelId)) {
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
				AudioExtender::RemoveVehicleBassAudio(old.gtaRef);
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
	} catch (const std::exception& e) {
		ClientLog(LogLevel::Error, std::format("Exception in OnGameProcess: {}", e.what()));
	} catch (...) {
		ClientLog(LogLevel::Error, "Unknown exception in OnGameProcess");
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

		CModelInfo::ms_modelInfoPtrs = StreamingExtender::GetModelInfoTable();

		MH_Initialize();
		MH_STATUS mhStatus = MH_CreateHook(reinterpret_cast<void*>(0x53BEE0), reinterpret_cast<void*>(&hooked_game_loop), reinterpret_cast<void**>(&orig_game_loop));
		if (mhStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x53BEE0));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CGame::Process (0x53BEE0) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CGame::Process hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CGame::Process (0x53BEE0): {}", MH_StatusToString(mhStatus)));
		}

		MH_STATUS whlStatus = MH_CreateHook(reinterpret_cast<void*>(0x6AA290), reinterpret_cast<void*>(&Hooked_UpdateWheelMatrix), reinterpret_cast<void**>(&g_origUpdateWheelMatrix));
		if (whlStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6AA290));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CAutomobile::UpdateWheelMatrix (0x6AA290) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CAutomobile::UpdateWheelMatrix hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CAutomobile::UpdateWheelMatrix (0x6AA290): {}", MH_StatusToString(whlStatus)));
		}

		MH_STATUS hlStatus = MH_CreateHook(reinterpret_cast<void*>(0x6E0A50), reinterpret_cast<void*>(&Hooked_DoHeadLightEffect), reinterpret_cast<void**>(&g_origDoHeadLightEffect));
		if (hlStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6E0A50));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CVehicle::DoHeadLightEffect (0x6E0A50) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CVehicle::DoHeadLightEffect hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CVehicle::DoHeadLightEffect (0x6E0A50): {}", MH_StatusToString(hlStatus)));
		}

		MH_STATUS tlStatus = MH_CreateHook(reinterpret_cast<void*>(0x6E1780), reinterpret_cast<void*>(&Hooked_DoTailLightEffect), reinterpret_cast<void**>(&g_origDoTailLightEffect));
		if (tlStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6E1780));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CVehicle::DoTailLightEffect (0x6E1780) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CVehicle::DoTailLightEffect hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CVehicle::DoTailLightEffect (0x6E1780): {}", MH_StatusToString(tlStatus)));
		}

		MH_STATUS exhStatus = MH_CreateHook(reinterpret_cast<void*>(0x6DE240), reinterpret_cast<void*>(&Hooked_AddExhaustParticles), reinterpret_cast<void**>(&g_origAddExhaustParticles));
		if (exhStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6DE240));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CVehicle::AddExhaustParticles (0x6DE240) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CVehicle::AddExhaustParticles hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CVehicle::AddExhaustParticles (0x6DE240): {}", MH_StatusToString(exhStatus)));
		}

		MH_STATUS rcTexStatus = MH_CreateHook(reinterpret_cast<void*>(0x6FC180), reinterpret_cast<void*>(&Hooked_RegisterCoronaTexture), reinterpret_cast<void**>(&g_origRegisterCoronaTexture));
		if (rcTexStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6FC180));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CCoronas::RegisterCorona[Texture] (0x6FC180) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CCoronas::RegisterCorona[Texture] hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CCoronas::RegisterCorona[Texture] (0x6FC180): {}", MH_StatusToString(rcTexStatus)));
		}

		MH_STATUS clsStatus = MH_CreateHook(reinterpret_cast<void*>(0x70C500), reinterpret_cast<void*>(&Hooked_StoreCarLightShadow), reinterpret_cast<void**>(&g_origStoreCarLightShadow));
		if (clsStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x70C500));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CShadows::StoreCarLightShadow (0x70C500) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CShadows::StoreCarLightShadow hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CShadows::StoreCarLightShadow (0x70C500): {}", MH_StatusToString(clsStatus)));
		}

		MH_STATUS remapStatus = MH_CreateHook(reinterpret_cast<void*>(0x6D0C00), reinterpret_cast<void*>(&Hooked_SetRemap), reinterpret_cast<void**>(&g_origSetRemap));
		if (remapStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6D0C00));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CVehicle::SetRemap (0x6D0C00) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CVehicle::SetRemap hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CVehicle::SetRemap (0x6D0C00): {}", MH_StatusToString(remapStatus)));
		}

		MH_STATUS addUpgStatus = MH_CreateHook(reinterpret_cast<void*>(0x6DFA20), reinterpret_cast<void*>(&Hooked_AddUpgrade), reinterpret_cast<void**>(&g_origAddUpgrade));
		if (addUpgStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6DFA20));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CVehicle::AddUpgrade (0x6DFA20) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CVehicle::AddUpgrade hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CVehicle::AddUpgrade (0x6DFA20): {}", MH_StatusToString(addUpgStatus)));
		}

		MH_STATUS remUpgStatus = MH_CreateHook(reinterpret_cast<void*>(0x6D3630), reinterpret_cast<void*>(&Hooked_RemoveUpgrade), reinterpret_cast<void**>(&g_origRemoveUpgrade));
		if (remUpgStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6D3630));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CVehicle::RemoveUpgrade (0x6D3630) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CVehicle::RemoveUpgrade hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CVehicle::RemoveUpgrade (0x6D3630): {}", MH_StatusToString(remUpgStatus)));
		}

		MH_STATUS sirenStatus = MH_CreateHook(reinterpret_cast<void*>(0x6D8470), reinterpret_cast<void*>(&Hooked_DoesVehicleUseSiren), reinterpret_cast<void**>(&g_origDoesVehicleUseSiren));
		if (sirenStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x6D8470));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CVehicle::DoesVehicleUseSiren (0x6D8470) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CVehicle::DoesVehicleUseSiren hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CVehicle::DoesVehicleUseSiren (0x6D8470): {}", MH_StatusToString(sirenStatus)));
		}

		MH_STATUS sirenAudioStatus = MH_CreateHook(reinterpret_cast<void*>(0x4F62A0), reinterpret_cast<void*>(&Hooked_GetVehicleSirenType), reinterpret_cast<void**>(&g_origGetVehicleSirenType));
		if (sirenAudioStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x4F62A0));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CAEVehicleAudioEntity::GetVehicleSirenType (0x4F62A0) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CAEVehicleAudioEntity::GetVehicleSirenType hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CAEVehicleAudioEntity::GetVehicleSirenType (0x4F62A0): {}", MH_StatusToString(sirenAudioStatus)));
		}

		MH_STATUS frameIdStatus = MH_CreateHook(reinterpret_cast<void*>(0x4C53C0), reinterpret_cast<void*>(&Hooked_GetFrameFromId), reinterpret_cast<void**>(&g_origGetFrameFromId));
		if (frameIdStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x4C53C0));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CClumpModelInfo::GetFrameFromId (0x4C53C0) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CClumpModelInfo::GetFrameFromId hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CClumpModelInfo::GetFrameFromId (0x4C53C0): {}", MH_StatusToString(frameIdStatus)));
		}

		MH_STATUS colStatus = MH_CreateHook(reinterpret_cast<void*>(0x535300), reinterpret_cast<void*>(&Hooked_GetColModel), reinterpret_cast<void**>(&g_origGetColModel));
		if (colStatus == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(reinterpret_cast<void*>(0x535300));
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "CEntity::GetColModel (0x535300) hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable CEntity::GetColModel hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook CEntity::GetColModel (0x535300): {}", MH_StatusToString(colStatus)));
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

		// OnGameProcess() is already called by hooked_game_loop (0x53BEE0) once per frame.
		// Do NOT add it to drawingEvent or gameProcessEvent: both of those also fire on
		// every game loop iteration, causing 3x execution per frame (triple TurnSpeed decay,
		// triple pool processing, triple MainThreadQueue drain).

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

		if (g_origAddExhaustParticles) {
			MH_DisableHook(reinterpret_cast<void*>(0x6DE240));
			MH_RemoveHook(reinterpret_cast<void*>(0x6DE240));
			g_origAddExhaustParticles = nullptr;
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

		if (g_origSetRemap) {
			MH_DisableHook(reinterpret_cast<void*>(0x6D0C00));
			MH_RemoveHook(reinterpret_cast<void*>(0x6D0C00));
			g_origSetRemap = nullptr;
		}

		if (g_origAddUpgrade) {
			MH_DisableHook(reinterpret_cast<void*>(0x6DFA20));
			MH_RemoveHook(reinterpret_cast<void*>(0x6DFA20));
			g_origAddUpgrade = nullptr;
		}

		if (g_origRemoveUpgrade) {
			MH_DisableHook(reinterpret_cast<void*>(0x6D3630));
			MH_RemoveHook(reinterpret_cast<void*>(0x6D3630));
			g_origRemoveUpgrade = nullptr;
		}

		if (g_origDoesVehicleUseSiren) {
			MH_DisableHook(reinterpret_cast<void*>(0x6D8470));
			MH_RemoveHook(reinterpret_cast<void*>(0x6D8470));
			g_origDoesVehicleUseSiren = nullptr;
		}

		if (g_origGetVehicleSirenType) {
			MH_DisableHook(reinterpret_cast<void*>(0x4F62A0));
			MH_RemoveHook(reinterpret_cast<void*>(0x4F62A0));
			g_origGetVehicleSirenType = nullptr;
		}

		if (g_origGetFrameFromId) {
			MH_DisableHook(reinterpret_cast<void*>(0x4C53C0));
			MH_RemoveHook(reinterpret_cast<void*>(0x4C53C0));
			g_origGetFrameFromId = nullptr;
		}

		if (g_origGetColModel) {
			MH_DisableHook(reinterpret_cast<void*>(0x535300));
			MH_RemoveHook(reinterpret_cast<void*>(0x535300));
			g_origGetColModel = nullptr;
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
