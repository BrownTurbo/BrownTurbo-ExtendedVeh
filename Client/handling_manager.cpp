#include "handling_manager.hpp"

#include <sampapi/CChat.h>
#include <sampapi/CLocalPlayer.h>
#include <sampapi/CNetGame.h>
#include <sampapi/CVehiclePool.h>
#include <sampapi/sampapi.h>

#include "utils.h"

#include <game_sa/CAutomobile.h>
#include "CustomVehicleBindingManager.h"
#include "MainThreadQueue.h"
#include "streamingextender.hpp"
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

std::map<uint16_t, std::unique_ptr<tHandlingData>> HandlingManager::m_vehicleHandlings;
std::map<uint16_t, std::unique_ptr<tHandlingData>> HandlingManager::m_modelHandlings;
std::map<CVehicle*, std::unique_ptr<tHandlingData>> HandlingManager::m_customHandlings;
std::map<uint16_t, std::unique_ptr<tHandlingData>> HandlingManager::m_playerHandlings;
std::map<uint16_t, HandlingManager::PlayerAppliedInfo> HandlingManager::m_playerAppliedHandlings;
std::map<uint16_t, std::vector<HandlingManager::HandlingAttribEntry>> HandlingManager::m_pendingVehicleAttribs;
std::unordered_map<uint16_t, uint8_t> HandlingManager::m_vehicleDoorStates;
std::unordered_map<uint16_t, bool> HandlingManager::m_vehicleFlying;
std::unordered_map<CVehicle*, bool> HandlingManager::m_vehicleFlyingByPtr;
std::atomic<bool> HandlingManager::m_isServerAuthorized { false };
std::atomic<bool> HandlingManager::m_initSent { false };
std::recursive_mutex HandlingManager::m_handlingMutex;
std::deque<HandlingManager::PendingCommand> HandlingManager::m_pendingCommands;
std::mutex HandlingManager::m_pendingMutex;
std::unordered_map<CVehicle*, uint16_t> HandlingManager::m_vehicleToSAMPIdCache;
std::unordered_map<uint32_t, int32_t> HandlingManager::m_modelUseCount;
std::mutex HandlingManager::m_cacheMutex;

bool HandlingManager::IsVehicleFlying(uint16_t sampVehicleId)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	auto it = m_vehicleFlying.find(sampVehicleId);
	return (it != m_vehicleFlying.end()) ? it->second : false;
}

bool HandlingManager::IsVehicleFlying(CVehicle* pVehicle)
{
	if (!pVehicle)
		return false;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	auto it = m_vehicleFlyingByPtr.find(pVehicle);
	if (it != m_vehicleFlyingByPtr.end() && it->second)
		return true;
	uint16_t sampId = GetVehicleSAMPId(pVehicle);
	if (sampId != 0xFFFF) {
		auto itSamp = m_vehicleFlying.find(sampId);
		if (itSamp != m_vehicleFlying.end() && itSamp->second)
			return true;
	}
	return false;
}

void HandlingManager::SetVehicleFlyingState(uint16_t sampVehicleId, bool flying, CVehicle* pVehicle)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	if (pVehicle && flying) {
		bool isFlightCapable = (pVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || pVehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || pVehicle->m_nVehicleSubClass == VEHICLE_QUAD || pVehicle->m_nVehicleSubClass == VEHICLE_BIKE || pVehicle->m_nVehicleSubClass == VEHICLE_BMX || pVehicle->m_nVehicleSubClass == VEHICLE_BOAT);
		if (!isFlightCapable) {
			flying = false;
		}
	}
	if (sampVehicleId != 0xFFFF && sampVehicleId != 0) {
		m_vehicleFlying[sampVehicleId] = flying;
	}
	if (pVehicle) {
		m_vehicleFlyingByPtr[pVehicle] = flying;
		if (flying) {
			pVehicle->bIsHandbrakeOn = false;
		}
	}
}

void HandlingManager::QueueCommand(PendingCommand cmd)
{
	std::lock_guard<std::mutex> lock(m_pendingMutex);
	m_pendingCommands.push_back(std::move(cmd));
}

std::vector<HandlingManager::HandlingAttribEntry> HandlingManager::ParseAttribEntries(uint8_t count, RakNet::BitStream* bs)
{
	std::vector<HandlingAttribEntry> entries;
	if (!bs)
		return entries;
	if (count > 64) {
		ClientLog(std::format("[Client] ParseAttribEntries: Rejecting excess count {} (max 64)", count));
		return {};
	}
	entries.reserve(count);

	for (int i = 0; i < count; ++i) {
		if (bs->GetNumberOfUnreadBits() < 16) {
			ClientLog(std::format("[Client] ParseAttribEntries: Truncated bitstream at entry {} of {}", i, count));
			return {};
		}
		CHandlingAttrib attrib;
		if (!bs->Read(attrib)) {
			ClientLog(std::format("[Client] ParseAttribEntries: Failed reading attrib at index {}", i));
			return {};
		}
		uint8_t typeByte;
		if (!bs->Read(typeByte)) {
			ClientLog(std::format("[Client] ParseAttribEntries: Failed reading typeByte for attrib {}", static_cast<int>(attrib)));
			return {};
		}
		CHandlingAttribType expectedType = GetHandlingAttribType(attrib);

		HandlingAttribEntry entry {};
		entry.attrib = attrib;
		entry.type = expectedType;

		switch (expectedType) {
		case TYPE_FLOAT: {
			float v;
			if (!bs->Read(v)) {
				ClientLog(std::format("[Client] ParseAttribEntries: Failed reading float for attrib {}", static_cast<int>(attrib)));
				return {};
			}
			entry.value.f = v;
			ClientLog(std::format("[Client] ParseAttribEntries: Entry {}: attrib={} (FLOAT), val={:.4f}", i, static_cast<int>(attrib), v));
			break;
		}
		case TYPE_UINT:
		case TYPE_FLAG: {
			uint32_t v;
			if (!bs->Read(v)) {
				ClientLog(std::format("[Client] ParseAttribEntries: Failed reading uint for attrib {}", static_cast<int>(attrib)));
				return {};
			}
			entry.value.u = v;
			ClientLog(std::format("[Client] ParseAttribEntries: Entry {}: attrib={} (UINT/FLAG), val={:#x} ({})", i, static_cast<int>(attrib), v, v));
			break;
		}
		case TYPE_BYTE: {
			uint8_t v;
			if (!bs->Read(v)) {
				ClientLog(std::format("[Client] ParseAttribEntries: Failed reading byte for attrib {}", static_cast<int>(attrib)));
				return {};
			}
			entry.value.b = v;
			ClientLog(std::format("[Client] ParseAttribEntries: Entry {}: attrib={} (BYTE), val={}", i, static_cast<int>(attrib), static_cast<int>(v)));
			break;
		}
		default:
			ClientLog(std::format("[Client] ParseAttribEntries: Unknown expectedType for attrib {}", static_cast<int>(attrib)));
			break;
		}

		if (!CanSetHandlingAttrib(attrib)) {
			ClientLog(std::format("[Client] ParseAttribEntries: Attrib {} is read-only, skipping", static_cast<int>(attrib)));
			continue;
		}
		entries.push_back(entry);
	}
	return entries;
}

void HandlingManager::ApplyAttribEntries(tHandlingData* handling, const std::vector<HandlingAttribEntry>& entries, CVehicle* pVehicle)
{
	bool transmissionNeedsInit = false;

	for (const auto& e : entries) {
		switch (e.attrib) {
		case HANDL_TR_FMAXVELOCITY: {
			float maxSpeedKph = e.value.f;
			float speedGameUnits = maxSpeedKph / 180.0f;
			handling->m_transmissionData.m_fMaxGearVelocity = speedGameUnits * 1.2f;
			handling->m_transmissionData.field_5C = speedGameUnits;
			float minGearVel = -speedGameUnits * 0.3f;
			if (minGearVel < -0.2f) {
				minGearVel = -0.2f;
			}
			handling->m_transmissionData.m_fMinGearVelocity = minGearVel;
			transmissionNeedsInit = true;
			break;
		}
		case HANDL_TR_FENGINEACCELERATION: {
			float wheelMult = (handling->m_transmissionData.m_nDriveType == '4' ? 0.25f : 0.5f);
			handling->m_transmissionData.m_fEngineAcceleration = e.value.f * 0.0004f * wheelMult;
			transmissionNeedsInit = true;
			break;
		}
		case HANDL_FBRAKEDECELERATION: {
			handling->m_fBrakeDeceleration = e.value.f * 0.0004f;
			break;
		}
		case HANDL_FCOLLISIONDAMAGEMULT: {
			float mass = (handling->m_fMass > 0.0f) ? handling->m_fMass : 1500.0f;
			handling->m_fCollisionDamageMultiplier = e.value.f * (2000.0f / mass);
			break;
		}
		case HANDL_FMASS: {
			handling->m_fMass = e.value.f;
			if (pVehicle && IsVehiclePointerValid(pVehicle)) {
				pVehicle->m_fMass = handling->m_fMass;
			}
			break;
		}
		case HANDL_FTURNMASS: {
			handling->m_fTurnMass = e.value.f;
			if (pVehicle && IsVehiclePointerValid(pVehicle)) {
				pVehicle->m_fTurnMass = handling->m_fTurnMass;
			}
			break;
		}
		case HANDL_TR_NNUMBEROFGEARS: {
			handling->m_transmissionData.m_nNumberOfGears = e.value.b;
			transmissionNeedsInit = true;
			break;
		}
		case HANDL_TR_NDRIVETYPE: {
			uint8_t oldDriveType = handling->m_transmissionData.m_nDriveType;
			handling->m_transmissionData.m_nDriveType = e.value.b;
			if (oldDriveType != e.value.b) {
				if (e.value.b == '4' && oldDriveType != '4') {
					handling->m_transmissionData.m_fEngineAcceleration *= 0.5f;
				} else if (oldDriveType == '4' && e.value.b != '4') {
					handling->m_transmissionData.m_fEngineAcceleration *= 2.0f;
				}
			}
			transmissionNeedsInit = true;
			break;
		}
		default: {
			void* ptr = ResolveAttributePointer(handling, static_cast<uint8_t>(e.attrib));
			if (!ptr)
				continue;

			switch (e.type) {
			case TYPE_FLOAT:
				*reinterpret_cast<float*>(ptr) = e.value.f;
				break;
			case TYPE_UINT:
			case TYPE_FLAG: {
				uint32_t val = e.value.u;
				if (e.attrib == HANDL_MODELFLAGS) {
					bool wantsFlight = (val & 0x4000000) != 0;
					bool wantsWaterDrive = (val & 0x8000000) != 0;
					if (pVehicle) {
						bool isFlightCapable = (pVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || pVehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || pVehicle->m_nVehicleSubClass == VEHICLE_QUAD || pVehicle->m_nVehicleSubClass == VEHICLE_BIKE || pVehicle->m_nVehicleSubClass == VEHICLE_BMX || pVehicle->m_nVehicleSubClass == VEHICLE_BOAT);
						if (wantsFlight && !isFlightCapable) {
							wantsFlight = false;
						}

						bool isWaterCapable = (pVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || pVehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || pVehicle->m_nVehicleSubClass == VEHICLE_QUAD);
						if (wantsWaterDrive) {
							if (!isWaterCapable) {
								val &= ~0x8000000;
							} else {
								// Clear solid axle flags so all wheels can rotate in boat mode
								val &= ~(0x00200000 | 0x00020000);
							}
						}

						SetVehicleFlyingState(GetVehicleSAMPId(pVehicle), wantsFlight, pVehicle);
						if (pVehicle->m_nVehicleSubClass != VEHICLE_PLANE) {
							// Mask off MFLAG_IS_PLANE (0x4000000) and MFLAG_IS_HELI (0x2000000) for non-planes
							// so GTA SA's CAutomobile does not disable wheel drive and engage landing gear brakes!
							val &= ~(0x4000000 | 0x2000000);
						}
					} else {
						val &= ~(0x4000000 | 0x2000000);
					}
				}
				*reinterpret_cast<uint32_t*>(ptr) = val;
				break;
			}
			case TYPE_BYTE:
				*reinterpret_cast<uint8_t*>(ptr) = e.value.b;
				break;
			default:
				break;
			}
			break;
		}
		}
	}

	if (transmissionNeedsInit) {
		handling->m_transmissionData.InitGearRatios();
	}
}

void HandlingManager::ProcessPendingCommands()
{
	std::deque<PendingCommand> work;
	{
		std::lock_guard<std::mutex> lock(m_pendingMutex);
		if (m_pendingCommands.empty())
			return;
		work.swap(m_pendingCommands);
	}

	for (auto& cmd : work) {
		switch (cmd.type) {
		case PendingCommandType::SetVehicle:
			ProcessVehicleMods(cmd.id, cmd.attribs);
			break;
		case PendingCommandType::SetModel:
			ProcessModelMods(cmd.id, cmd.attribs);
			break;
		case PendingCommandType::SetPlayer:
			ProcessPlayerMods(cmd.id, cmd.attribs);
			break;
		case PendingCommandType::ResetVehicle:
			ResetVehicle(cmd.id);
			break;
		case PendingCommandType::ResetModel:
			ResetModel(cmd.id);
			break;
		case PendingCommandType::ResetPlayer:
			ResetPlayerHandling(cmd.id);
			break;
		}
	}

	// Retry pending vehicle attributes for vehicles that are now available in the pool
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	if (!m_pendingVehicleAttribs.empty()) {
		for (auto it = m_pendingVehicleAttribs.begin(); it != m_pendingVehicleAttribs.end();) {
			uint16_t targetVehId = it->first;
			CVehicle* gtaVeh = GetGameVehicleFromPool(targetVehId);
			if (IsVehiclePointerValid(gtaVeh) && gtaVeh->m_pHandlingData) {
				auto attribs = std::move(it->second);
				it = m_pendingVehicleAttribs.erase(it);
				ClientLog(std::format("[Client] ProcessPendingCommands: Vehicle {} is now available, applying {} queued attribs", targetVehId, attribs.size()));
				ProcessVehicleMods(targetVehId, attribs);
			} else {
				++it;
			}
		}
	}
}

tHandlingData* HandlingManager::ResolveFallbackHandling(uint16_t vehicleId, int modelId)
{
	auto playerIt = m_playerAppliedHandlings.find(vehicleId);
	if (playerIt != m_playerAppliedHandlings.end()) {
		return playerIt->second.handling.get();
	}

	auto modelIt = m_modelHandlings.find(modelId);
	if (modelIt != m_modelHandlings.end()) {
		return modelIt->second.get();
	}

	return static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId))->m_nHandlingId]);
}

tHandlingData* HandlingManager::ResolveFallbackHandling(CVehicle* pVehicle, uint16_t vehicleId, int modelId)
{
	if (IsVehiclePointerValid(pVehicle)) {
		auto customIt = m_customHandlings.find(pVehicle);
		if (customIt != m_customHandlings.end()) {
			return customIt->second.get();
		}
	}
	return ResolveFallbackHandling(vehicleId, modelId);
}

uint16_t HandlingManager::GetVehicleSAMPId(CVehicle* pVehicle)
{
	if (!IsVehiclePointerValid(pVehicle))
		return 0xFFFF;

	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);
		auto it = m_vehicleToSAMPIdCache.find(pVehicle);
		if (it != m_vehicleToSAMPIdCache.end()) {
			return it->second;
		}
	}

	auto pool = GetVehiclesPool();
	if (!std::holds_alternative<std::nullptr_t>(pool)) {
		uint16_t foundId = 0xFFFF;
		std::visit([&](auto&& p) {
			using T = std::decay_t<decltype(p)>;
			if constexpr (!std::is_same_v<T, std::nullptr_t>) {
				if (p) {
					for (uint16_t id = 1; id < MAX_SAMP_VEHICLES; ++id) {
						auto* sampVeh = p->Get(id);
						if (!sampVeh)
							continue;
						if (sampVeh->m_pGameVehicle == pVehicle) {
							foundId = id;
							break;
						}
					}
				}
			}
		},
			pool);
		if (foundId != 0xFFFF) {
			CacheVehicleSAMPId(pVehicle, foundId);
			return foundId;
		}
	}
	return 0xFFFF;
}

void HandlingManager::CacheVehicleSAMPId(CVehicle* vehicle, uint16_t sampId)
{
	if (!IsVehiclePointerValid(vehicle))
		return;
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	auto [it, inserted] = m_vehicleToSAMPIdCache.emplace(vehicle, sampId);
	if (inserted) {
		++m_modelUseCount[static_cast<uint32_t>(vehicle->m_nModelIndex)];
	}
}

void HandlingManager::RemoveVehicleFromCache(CVehicle* vehicle)
{
	if (!IsVehiclePointerValid(vehicle))
		return;
	{
		std::lock_guard<std::recursive_mutex> hlock(m_handlingMutex);
		m_vehicleFlyingByPtr.erase(vehicle);
	}
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	auto it = m_vehicleToSAMPIdCache.find(vehicle);
	if (it == m_vehicleToSAMPIdCache.end())
		return;
	const uint32_t model = static_cast<uint32_t>(vehicle->m_nModelIndex);
	m_vehicleToSAMPIdCache.erase(it);
	auto count = m_modelUseCount.find(model);
	if (count != m_modelUseCount.end()) {
		if (count->second > 1)
			--(count->second);
		else
			m_modelUseCount.erase(count);
	}
}

void HandlingManager::IncrementModelUse(uint32_t modelId)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	m_modelUseCount[modelId]++;
}

void HandlingManager::DecrementModelUse(uint32_t modelId)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	auto it = m_modelUseCount.find(modelId);
	if (it != m_modelUseCount.end()) {
		if (--it->second == 0) {
			m_modelUseCount.erase(it);
		}
	}
}

int32_t HandlingManager::GetModelUseCount(uint32_t modelId)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	auto it = m_modelUseCount.find(modelId);
	return (it != m_modelUseCount.end()) ? it->second : 0;
}

bool HandlingManager::CanSetHandlingAttrib(CHandlingAttrib attribute)
{
	switch (attribute) {
	case HANDL_UIDENTIFIER:
	case HANDL_ANIMGROUP:
	case HANDL_UIMONETARYVALUE:
		return false;
	}
	return true;
}

CHandlingAttribType HandlingManager::GetHandlingAttribType(CHandlingAttrib attribute)
{
	switch (attribute) {
	case HANDL_FMASS:
	case HANDL_FTURNMASS:
	case HANDL_FDRAGMULTIPLIER:
	case HANDL_CENTREOFMASS_X:
	case HANDL_CENTREOFMASS_Y:
	case HANDL_CENTREOFMASS_Z:
	case HANDL_FTRACTIONMULTIPLIER:
	case HANDL_FTRACTIONLOSS:
	case HANDL_FTRACTIONBIAS:
	case HANDL_TR_FMAXVELOCITY:
	case HANDL_TR_FENGINEACCELERATION:
	case HANDL_TR_FENGINEINERTIA:
	case HANDL_FBRAKEDECELERATION:
	case HANDL_FBRAKEBIAS:
	case HANDL_FSTEERINGLOCK:
	case HANDL_FSUSPENSIONFORCELEVEL:
	case HANDL_FSUSPENSIONDAMPINGLEVEL:
	case HANDL_FSUSPENSIONHIGHSPDCOMDAMP:
	case HANDL_FSUSPENSIONUPPERLIMIT:
	case HANDL_FSUSPENSIONLOWERLIMIT:
	case HANDL_FSUSPENSIONBIAS:
	case HANDL_FSUSPENSIONANTIDIVEMULT:
	case HANDL_FSEATOFFSETDISTANCE:
	case HANDL_FCOLLISIONDAMAGEMULT:
		return TYPE_FLOAT;

	case HANDL_NPERCENTSUBMERGED:
	case HANDL_ANIMGROUP:
	case HANDL_FRONTLIGHTS:
	case HANDL_REARLIGHTS:
	case HANDL_TR_NDRIVETYPE:
	case HANDL_TR_NENGINETYPE:
	case HANDL_TR_NNUMBEROFGEARS:
	case HANDL_BABS:
		return TYPE_BYTE;

	case HANDL_UIDENTIFIER:
	case HANDL_UIMONETARYVALUE:
		return TYPE_UINT;

	case HANDL_HANDLINGFLAGS:
	case HANDL_MODELFLAGS:
		return TYPE_FLAG;
	}
	return TYPE_NONE;
}

void* HandlingManager::ResolveAttributePointer(tHandlingData* handling, uint8_t attribId)
{
	switch (static_cast<CHandlingAttrib>(attribId)) {
	case HANDL_FMASS:
		return &handling->m_fMass;
	case HANDL_FTURNMASS:
		return &handling->m_fTurnMass;
	case HANDL_FDRAGMULTIPLIER:
		return &handling->m_fDragMult;
	case HANDL_CENTREOFMASS_X:
		return &handling->m_vecCentreOfMass.x;
	case HANDL_CENTREOFMASS_Y:
		return &handling->m_vecCentreOfMass.y;
	case HANDL_CENTREOFMASS_Z:
		return &handling->m_vecCentreOfMass.z;
	case HANDL_FTRACTIONMULTIPLIER:
		return &handling->m_fTractionMultiplier;
	case HANDL_FTRACTIONLOSS:
		return &handling->m_fTractionLoss;
	case HANDL_FTRACTIONBIAS:
		return &handling->m_fTractionBias;
	case HANDL_TR_FMAXVELOCITY:
		return &handling->m_transmissionData.m_fMaxGearVelocity;
	case HANDL_TR_FENGINEACCELERATION:
		return &handling->m_transmissionData.m_fEngineAcceleration;
	case HANDL_TR_FENGINEINERTIA:
		return &handling->m_transmissionData.m_fEngineInertia;
	case HANDL_FBRAKEDECELERATION:
		return &handling->m_fBrakeDeceleration;
	case HANDL_FBRAKEBIAS:
		return &handling->m_fBrakeBias;
	case HANDL_FSTEERINGLOCK:
		return &handling->m_fSteeringLock;
	case HANDL_FSUSPENSIONFORCELEVEL:
		return &handling->m_fSuspensionForceLevel;
	case HANDL_FSUSPENSIONDAMPINGLEVEL:
		return &handling->m_fSuspensionDampingLevel;
	case HANDL_FSUSPENSIONHIGHSPDCOMDAMP:
		return &handling->m_fSuspensionHighSpdComDamp;
	case HANDL_FSUSPENSIONUPPERLIMIT:
		return &handling->m_fSuspensionUpperLimit;
	case HANDL_FSUSPENSIONLOWERLIMIT:
		return &handling->m_fSuspensionLowerLimit;
	case HANDL_FSUSPENSIONBIAS:
		return &handling->m_fSuspensionBiasBetweenFrontAndRear;
	case HANDL_FSUSPENSIONANTIDIVEMULT:
		return &handling->m_fSuspensionAntiDiveMultiplier;
	case HANDL_FSEATOFFSETDISTANCE:
		return &handling->m_fSeatOffsetDistance;
	case HANDL_FCOLLISIONDAMAGEMULT:
		return &handling->m_fCollisionDamageMultiplier;

	case HANDL_NPERCENTSUBMERGED:
		return &handling->m_nPercentSubmerged;
	case HANDL_ANIMGROUP:
		return &handling->m_nAnimGroup;
	case HANDL_FRONTLIGHTS:
		return &handling->m_nFrontLights;
	case HANDL_REARLIGHTS:
		return &handling->m_nRearLights;
	case HANDL_TR_NDRIVETYPE:
		return &handling->m_transmissionData.m_nDriveType;
	case HANDL_TR_NENGINETYPE:
		return &handling->m_transmissionData.m_nEngineType;
	case HANDL_TR_NNUMBEROFGEARS:
		return &handling->m_transmissionData.m_nNumberOfGears;
	case HANDL_BABS:
		return &handling->m_bABS;

	case HANDL_UIDENTIFIER:
		return &handling->m_nVehicleId;
	case HANDL_UIMONETARYVALUE:
		return &handling->m_nMonetaryValue;

	case HANDL_HANDLINGFLAGS:
		return &handling->m_nHandlingFlags;
	case HANDL_MODELFLAGS:
		return &handling->m_nModelFlags;
	default:
		return nullptr;
	}
}

void HandlingManager::RecalculateDerivedHandling(tHandlingData* handling, CVehicle* pVehicle)
{
	if (!handling)
		return;

	handling->m_transmissionData.m_nHandlingFlags = handling->m_nHandlingFlags;

	if (handling->m_nPercentSubmerged > 0) {
		handling->m_fBuoyancyConstant = 0.0080000004f * handling->m_fMass * 100.0f / static_cast<float>(handling->m_nPercentSubmerged);
	}

	if (pVehicle && IsVehiclePointerValid(pVehicle)) {
		bool isWaterCapable = (pVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || pVehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || pVehicle->m_nVehicleSubClass == VEHICLE_QUAD);
		if (!isWaterCapable) {
			handling->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
				handling->m_nModelFlags & ~0x8000000);
		}

		bool isFlightCapable = (pVehicle->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || pVehicle->m_nVehicleSubClass == VEHICLE_MTRUCK || pVehicle->m_nVehicleSubClass == VEHICLE_QUAD || pVehicle->m_nVehicleSubClass == VEHICLE_BIKE || pVehicle->m_nVehicleSubClass == VEHICLE_BMX || pVehicle->m_nVehicleSubClass == VEHICLE_BOAT);
		if ((handling->m_nModelFlags & 0x4000000) != 0) {
			if (isFlightCapable) {
				SetVehicleFlyingState(GetVehicleSAMPId(pVehicle), true, pVehicle);
			}
			if (pVehicle->m_nVehicleSubClass != VEHICLE_PLANE) {
				handling->m_nModelFlags = static_cast<eVehicleHandlingModelFlags>(
					handling->m_nModelFlags & ~(0x4000000 | 0x2000000));
			}
		}

		pVehicle->m_pHandlingData = handling;
		pVehicle->m_fTurnMass = handling->m_fTurnMass;
		pVehicle->m_fMass = handling->m_fMass;
		pVehicle->m_nHandlingFlagsIntValue = handling->m_nHandlingFlags;
		pVehicle->m_vecCentreOfMass = handling->m_vecCentreOfMass;
		pVehicle->m_fBuoyancyConstant = handling->m_fBuoyancyConstant;

		pVehicle->bIsVan = (handling->m_nModelFlags & VEHICLE_HANDLING_MODEL_IS_VAN) != 0;
		pVehicle->bIsBus = (handling->m_nModelFlags & VEHICLE_HANDLING_MODEL_IS_BUS) != 0;
		pVehicle->bLowVehicle = (handling->m_nModelFlags & VEHICLE_HANDLING_MODEL_IS_LOW) != 0;
		pVehicle->bIsBig = (handling->m_nModelFlags & VEHICLE_HANDLING_MODEL_IS_BIG) != 0;

		if (IsVehicleFlying(pVehicle)) {
			pVehicle->bIsHandbrakeOn = false;
		}
	}
}

void HandlingManager::OnVehicleStreamIn(CVehicle* pVehicle, uint16_t sampId)
{
	if (!IsVehiclePointerValid(pVehicle) || !pVehicle->m_pHandlingData)
		return;

	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	// Apply custom door states if configured for this vehicle
	auto doorIt = m_vehicleDoorStates.find(sampId);
	if (doorIt != m_vehicleDoorStates.end() && doorIt->second != 0) {
		uint8_t appliedCount = 0;
		for (uint8_t d = 0; d < 6; ++d) {
			if (doorIt->second & (1 << d)) {
				ApplyDoorState(pVehicle, d, true);
				appliedCount++;
			}
		}
		ClientLog(std::format("[Client] OnVehicleStreamIn: Applied {} cached missing door(s) for vehicle {}", appliedCount, sampId));
	}

	// 1. Check if there are pending attributes for this vehicle
	auto pendingIt = m_pendingVehicleAttribs.find(sampId);
	if (pendingIt != m_pendingVehicleAttribs.end()) {
		auto it = m_vehicleHandlings.find(sampId);
		if (it == m_vehicleHandlings.end()) {
			auto newHandling = std::make_unique<tHandlingData>();
			std::memcpy(newHandling.get(), pVehicle->m_pHandlingData, sizeof(tHandlingData));
			auto [insertedIt, _] = m_vehicleHandlings.emplace(sampId, std::move(newHandling));
			it = insertedIt;
		}
		tHandlingData* handling = it->second.get();
		ApplyAttribEntries(handling, pendingIt->second, pVehicle);
		handling->m_transmissionData.InitGearRatios();
		RecalculateDerivedHandling(handling, pVehicle);
		m_pendingVehicleAttribs.erase(pendingIt);
		ClientLog(std::format("[Client] OnVehicleStreamIn: Applied pending handling for vehicle {}", sampId));
		return;
	}

	// 2. Check if vehicle already has custom vehicle handling
	auto vehIt = m_vehicleHandlings.find(sampId);
	if (vehIt != m_vehicleHandlings.end()) {
		RecalculateDerivedHandling(vehIt->second.get(), pVehicle);
		ClientLog(std::format("[Client] OnVehicleStreamIn: Reapplied custom handling for vehicle {}", sampId));
		return;
	}

	// 3. Check if vehicle has model handling override
	uint16_t modelId = static_cast<uint16_t>(pVehicle->m_nModelIndex);
	auto* binding = CustomVehicleBindingManager::Instance().Find(sampId);
	if (binding && binding->customModelId > 0) {
		modelId = static_cast<uint16_t>(binding->customModelId);
	}
	auto modelIt = m_modelHandlings.find(modelId);
	if (modelIt != m_modelHandlings.end()) {
		RecalculateDerivedHandling(modelIt->second.get(), pVehicle);
		ClientLog(std::format("[Client] OnVehicleStreamIn: Reapplied model handling for vehicle {} (model {})", sampId, modelId));
		return;
	} else if (binding && binding->customModelId > 0) {
		auto* customModel = StreamingExtender::GetCustomModel(binding->customModelId);
		if (customModel) {
			tHandlingData* customBaseHandling = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[customModel->m_nHandlingId]);
			RecalculateDerivedHandling(customBaseHandling, pVehicle);
			ClientLog(std::format("[Client] OnVehicleStreamIn: Reapplied custom base handling for vehicle {} (custom model {})", sampId, modelId));
			return;
		}
	}
}

void HandlingManager::IsolateVehicleHandling(CVehicle* pVehicle)
{
	if (!IsVehiclePointerValid(pVehicle) || !pVehicle->m_pHandlingData)
		return;

	if (m_customHandlings.find(pVehicle) == m_customHandlings.end()) {
		auto customHandling = std::make_unique<tHandlingData>();

		std::memcpy(customHandling.get(), pVehicle->m_pHandlingData, sizeof(tHandlingData));

		pVehicle->m_pHandlingData = customHandling.get();
		m_customHandlings[pVehicle] = std::move(customHandling);
	}
}

void HandlingManager::ModifyMass(CVehicle* pVehicle, float mass)
{
	if (!IsVehiclePointerValid(pVehicle) || !pVehicle->m_pHandlingData)
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	IsolateVehicleHandling(pVehicle);

	tHandlingData* handling = m_customHandlings[pVehicle].get();
	handling->m_fMass = mass;
	handling->m_fTurnMass = mass * 1.5f;
	handling->m_fDragMult = (mass / 5.0f) * 0.002f;
	RecalculateDerivedHandling(handling, pVehicle);
}

void HandlingManager::ModifyTransmission(CVehicle* pVehicle, float maxSpeed, float acceleration, int gears)
{
	if (!IsVehiclePointerValid(pVehicle) || !pVehicle->m_pHandlingData)
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	IsolateVehicleHandling(pVehicle);

	tHandlingData* handling = m_customHandlings[pVehicle].get();
	handling->m_transmissionData.m_fMaxGearVelocity = maxSpeed / 180.0f;
	handling->m_transmissionData.m_fEngineAcceleration = acceleration;
	handling->m_transmissionData.m_nNumberOfGears = gears;
	handling->m_transmissionData.InitGearRatios();
}

void HandlingManager::ResetVehicleHandling(CVehicle* pVehicle)
{
	if (!IsVehiclePointerValid(pVehicle))
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	auto it = m_customHandlings.find(pVehicle);
	if (it != m_customHandlings.end()) {
		uint16_t vehicleId = GetVehicleSAMPId(pVehicle);
		uint32_t modelIndex = pVehicle->m_nModelIndex;
		auto* binding = CustomVehicleBindingManager::Instance().Find(vehicleId);
		if (binding && binding->customModelId > 0) {
			modelIndex = binding->customModelId;
		}

		auto* vehicleModelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelIndex));
		if (vehicleModelInfo) {
			unsigned int handlingId = vehicleModelInfo->m_nHandlingId;
			pVehicle->m_pHandlingData = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[handlingId]);
		}

		m_customHandlings.erase(it);
	}
}

void HandlingManager::ApplyModelToVehicles(uint16_t modelId, tHandlingData* handling)
{
	auto pool = GetVehiclesPool();
	if (std::holds_alternative<std::nullptr_t>(pool))
		return;

	if (!handling) {
		auto modelIt = m_modelHandlings.find(modelId);
		if (modelIt != m_modelHandlings.end()) {
			handling = modelIt->second.get();
		} else {
			auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
			if (modelInfo) {
				handling = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);
			}
		}
	}
	if (!handling)
		return;

	std::visit([&](auto&& p) {
		using T = std::decay_t<decltype(p)>;
		if constexpr (!std::is_same_v<T, std::nullptr_t>) {
			if (p) {
				for (uint16_t id = 1; id < MAX_SAMP_VEHICLES; ++id) {
					auto* sampVeh = p->Get(id);
					if (!sampVeh || !sampVeh->m_pGameVehicle)
						continue;
					CVehicle* gtaVeh = sampVeh->m_pGameVehicle;
					if (!IsVehiclePointerValid(gtaVeh))
						continue;

					uint32_t currentVehModel = gtaVeh->m_nModelIndex;
					auto* binding = CustomVehicleBindingManager::Instance().Find(id);
					if (binding && binding->customModelId > 0) {
						currentVehModel = binding->customModelId;
					}
					if (currentVehModel != modelId)
						continue;

					if (m_vehicleHandlings.find(id) == m_vehicleHandlings.end() && m_playerAppliedHandlings.find(id) == m_playerAppliedHandlings.end() && m_customHandlings.find(gtaVeh) == m_customHandlings.end()) {
						RecalculateDerivedHandling(handling, gtaVeh);
					}
				}
			}
		}
	},
		pool);
}

void HandlingManager::RevertModelToOriginal(uint16_t modelId)
{
	auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
	if (!modelInfo)
		return;
	tHandlingData* original = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);

	auto pool = GetVehiclesPool();
	if (std::holds_alternative<std::nullptr_t>(pool))
		return;

	std::visit([&](auto&& p) {
		using T = std::decay_t<decltype(p)>;
		if constexpr (!std::is_same_v<T, std::nullptr_t>) {
			if (p) {
				for (uint16_t id = 1; id < MAX_SAMP_VEHICLES; ++id) {
					auto* sampVeh = p->Get(id);
					if (!sampVeh || !sampVeh->m_pGameVehicle)
						continue;
					CVehicle* gtaVeh = sampVeh->m_pGameVehicle;
					if (!IsVehiclePointerValid(gtaVeh))
						continue;

					uint32_t currentVehModel = gtaVeh->m_nModelIndex;
					auto* binding = CustomVehicleBindingManager::Instance().Find(id);
					if (binding && binding->customModelId > 0) {
						currentVehModel = binding->customModelId;
					}
					if (currentVehModel != modelId)
						continue;

					if (m_vehicleHandlings.find(id) == m_vehicleHandlings.end() && m_playerAppliedHandlings.find(id) == m_playerAppliedHandlings.end() && m_customHandlings.find(gtaVeh) == m_customHandlings.end()) {
						RecalculateDerivedHandling(original, gtaVeh);
					}
				}
			}
		}
	},
		pool);
}

void HandlingManager::ApplyPlayerHandling(uint16_t playerId, CVehicle* pVehicle)
{
	if (!IsVehiclePointerValid(pVehicle) || !pVehicle->m_pHandlingData)
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	uint16_t vehicleId = GetVehicleSAMPId(pVehicle);
	if (vehicleId == 0xFFFF)
		return;

	if (m_vehicleHandlings.find(vehicleId) != m_vehicleHandlings.end() || m_playerAppliedHandlings.find(vehicleId) != m_playerAppliedHandlings.end()) {
		return;
	}

	auto it = m_playerHandlings.find(playerId);
	if (it == m_playerHandlings.end())
		return;

	auto newHandling = std::make_unique<tHandlingData>();
	std::memcpy(newHandling.get(), it->second.get(), sizeof(tHandlingData));
	RecalculateDerivedHandling(newHandling.get(), pVehicle);

	PlayerAppliedInfo info;
	info.playerId = playerId;
	info.handling = std::move(newHandling);
	m_playerAppliedHandlings[vehicleId] = std::move(info);
}

void HandlingManager::RemovePlayerHandling(uint16_t playerId, CVehicle* pVehicle)
{
	if (!IsVehiclePointerValid(pVehicle))
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	uint16_t vehicleId = GetVehicleSAMPId(pVehicle);
	if (vehicleId == 0xFFFF)
		return;

	auto it = m_playerAppliedHandlings.find(vehicleId);
	if (it == m_playerAppliedHandlings.end() || it->second.playerId != playerId)
		return;

	int modelId = pVehicle->m_nModelIndex;
	auto vehIt = m_vehicleHandlings.find(vehicleId);
	if (vehIt != m_vehicleHandlings.end()) {
		RecalculateDerivedHandling(vehIt->second.get(), pVehicle);
	} else {
		auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
		if (modelInfo) {
			unsigned int handlingId = modelInfo->m_nHandlingId;
			tHandlingData* fallback = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[handlingId]);
			auto modelIt = m_modelHandlings.find(modelId);
			if (modelIt != m_modelHandlings.end()) {
				fallback = modelIt->second.get();
			}
			RecalculateDerivedHandling(fallback, pVehicle);
		}
	}
	m_playerAppliedHandlings.erase(it);
}

void HandlingManager::ProcessPlayerMods(uint16_t playerId, const std::vector<HandlingAttribEntry>& entries)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	auto it = m_playerHandlings.find(playerId);
	if (it == m_playerHandlings.end()) {
		auto newHandling = std::make_unique<tHandlingData>();
		auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(400));
		if (modelInfo) {
			tHandlingData* base = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);
			std::memcpy(newHandling.get(), base, sizeof(tHandlingData));
		} else {
			memset(newHandling.get(), 0, sizeof(tHandlingData));
		}
		auto [insertedIt, _] = m_playerHandlings.emplace(playerId, std::move(newHandling));
		it = insertedIt;
	}

	tHandlingData* handling = it->second.get();
	ApplyAttribEntries(handling, entries);
	handling->m_transmissionData.InitGearRatios();

	auto* localPlayerPed = FindPlayerPed();
	if (localPlayerPed && localPlayerPed->m_pVehicle) {
		if (MatchPlayerId(playerId)) {
			ApplyPlayerHandling(playerId, localPlayerPed->m_pVehicle);
		}
	}
}

void HandlingManager::ResetPlayerHandling(uint16_t playerId)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	auto it = m_playerHandlings.find(playerId);
	if (it == m_playerHandlings.end())
		return;

	for (auto mapIt = m_playerAppliedHandlings.begin(); mapIt != m_playerAppliedHandlings.end();) {
		if (mapIt->second.playerId == playerId) {
			uint16_t vehicleId = mapIt->first;
			CVehicle* gtaVehicle = GetGameVehicleFromPool(vehicleId);
			if (IsVehiclePointerValid(gtaVehicle)) {
				int modelId = gtaVehicle->m_nModelIndex;
				auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
				if (modelInfo) {
					auto modelIt = m_modelHandlings.find(modelId);
					tHandlingData* fallback = (modelIt != m_modelHandlings.end()
							? modelIt->second.get()
							: static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]));
					gtaVehicle->m_pHandlingData = fallback;
					gtaVehicle->m_fMass = fallback->m_fMass;
					gtaVehicle->m_fTurnMass = fallback->m_fTurnMass;
				}
			}
			mapIt = m_playerAppliedHandlings.erase(mapIt);
		} else {
			++mapIt;
		}
	}

	m_playerHandlings.erase(it);
}

void HandlingManager::ProcessVehicleMods(uint16_t sampVehicleId, const std::vector<HandlingAttribEntry>& entries)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	CVehicle* gtaVehicle = GetGameVehicleFromPool(sampVehicleId);
	if (!IsVehiclePointerValid(gtaVehicle) || !gtaVehicle->m_pHandlingData) {
		auto& pending = m_pendingVehicleAttribs[sampVehicleId];
		pending.insert(pending.end(), entries.begin(), entries.end());
		ClientLog(std::format("[Client] ProcessVehicleMods: Vehicle {} not in pool, queued {} attribs", sampVehicleId, entries.size()));
		return;
	}

	auto customIt = m_customHandlings.find(gtaVehicle);
	if (customIt != m_customHandlings.end()) {
		m_customHandlings.erase(customIt);
	}

	auto it = m_vehicleHandlings.find(sampVehicleId);
	if (it == m_vehicleHandlings.end()) {
		auto newHandling = std::make_unique<tHandlingData>();
		std::memcpy(newHandling.get(), gtaVehicle->m_pHandlingData, sizeof(tHandlingData));
		auto [insertedIt, _] = m_vehicleHandlings.emplace(sampVehicleId, std::move(newHandling));
		it = insertedIt;
	}

	tHandlingData* handling = it->second.get();
	ApplyAttribEntries(handling, entries, gtaVehicle);
	handling->m_transmissionData.InitGearRatios();

	RecalculateDerivedHandling(handling, gtaVehicle);
	ClientLog(std::format("[Client] ProcessVehicleMods: Applied {} attribs to vehicle {}. Mass={:.1f}, MaxVel={:.1f} km/h (game={:.4f}), Accel={:.5f}, Gears={}, Submerged={}, Buoyancy={:.4f}, ModelFlags={:#x}",
		entries.size(), sampVehicleId, handling->m_fMass,
		(handling->m_transmissionData.m_fMaxGearVelocity / 1.2f) * 180.0f,
		handling->m_transmissionData.m_fMaxGearVelocity,
		handling->m_transmissionData.m_fEngineAcceleration,
		static_cast<int>(handling->m_transmissionData.m_nNumberOfGears),
		static_cast<int>(handling->m_nPercentSubmerged),
		handling->m_fBuoyancyConstant,
		static_cast<uint32_t>(handling->m_nModelFlags)));
}

void HandlingManager::ProcessVehicleDoorState(uint16_t sampVehicleId, uint8_t doorId, bool missing)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	if (doorId == 0xFF) {
		m_vehicleDoorStates[sampVehicleId] = missing ? 0x3F : 0x00;
	} else if (doorId <= 5) {
		if (missing) {
			m_vehicleDoorStates[sampVehicleId] |= (1 << doorId);
		} else {
			m_vehicleDoorStates[sampVehicleId] &= ~(1 << doorId);
		}
	}

	CVehicle* gtaVehicle = GetGameVehicleFromPool(sampVehicleId);
	if (!gtaVehicle) {
		std::lock_guard<std::mutex> clock(m_cacheMutex);
		for (const auto& [veh, id] : m_vehicleToSAMPIdCache) {
			if (id == sampVehicleId && IsVehiclePointerValid(veh)) {
				gtaVehicle = veh;
				break;
			}
		}
	}

	if (IsVehiclePointerValid(gtaVehicle)) {
		ApplyDoorState(gtaVehicle, doorId, missing);
		ClientLog(std::format("[Client] ProcessVehicleDoorState: Applied doorId {} (missing={}) to vehicleId {}", doorId, missing, sampVehicleId));
	} else {
		ClientLog(std::format("[Client] ProcessVehicleDoorState: vehicleId {} not in pool, cached doorId {} (missing={})", sampVehicleId, doorId, missing));
	}
}

void HandlingManager::ApplyDoorState(CVehicle* pVehicle, uint8_t doorId, bool missing)
{
	if (!IsVehiclePointerValid(pVehicle))
		return;

	// In GTA:SA, CAutomobile has subclass 0, MTRUCK is 1, QUAD is 2
	if (pVehicle->m_nVehicleSubClass != 0 && pVehicle->m_nVehicleSubClass != 1 && pVehicle->m_nVehicleSubClass != 2)
		return;

	CAutomobile* autoVeh = reinterpret_cast<CAutomobile*>(pVehicle);

	struct DoorInfo {
		int nodeIdx;
		eDoors gtaDoor;
	};

	static constexpr DoorInfo kDoors[6] = {
		{ CAR_BONNET, BONNET }, // 0: Bonnet / Hood
		{ CAR_BOOT, BOOT }, // 1: Boot / Trunk
		{ CAR_DOOR_LF, DOOR_FRONT_LEFT }, // 2: Front Left Door
		{ CAR_DOOR_RF, DOOR_FRONT_RIGHT }, // 3: Front Right Door
		{ CAR_DOOR_LR, DOOR_REAR_LEFT }, // 4: Rear Left Door
		{ CAR_DOOR_RR, DOOR_REAR_RIGHT } // 5: Rear Right Door
	};

	auto applySingleDoor = [&](uint8_t index) {
		if (index > 5)
			return;

		const auto& [nodeIdx, gtaDoor] = kDoors[index];

		if (missing) {
			autoVeh->PopDoor(nodeIdx, gtaDoor, false);
			autoVeh->m_damageManager.SetDoorStatus(gtaDoor, DAMSTATE_NOTPRESENT);
			if (nodeIdx >= 0 && nodeIdx < CAR_NUM_NODES && autoVeh->m_aCarNodes[nodeIdx]) {
				pVehicle->SetComponentVisibility(autoVeh->m_aCarNodes[nodeIdx], 0);
			}
		} else {
			autoVeh->FixDoor(nodeIdx, gtaDoor);
			autoVeh->m_damageManager.SetDoorStatus(gtaDoor, DAMSTATE_OK);
			autoVeh->m_doors[gtaDoor].m_fAngle = autoVeh->m_doors[gtaDoor].m_fClosedAngle;
			autoVeh->m_doors[gtaDoor].m_fPrevAngle = autoVeh->m_doors[gtaDoor].m_fClosedAngle;
			autoVeh->m_doors[gtaDoor].m_fAngVel = 0.0f;
			autoVeh->m_doors[gtaDoor].m_nDoorState = DOOR_NOTHING;
			if (nodeIdx >= 0 && nodeIdx < CAR_NUM_NODES && autoVeh->m_aCarNodes[nodeIdx]) {
				pVehicle->SetComponentVisibility(autoVeh->m_aCarNodes[nodeIdx], 1);
			}
		}
	};

	if (doorId == 0xFF) {
		for (uint8_t i = 0; i < 6; ++i) {
			applySingleDoor(i);
		}
	} else if (doorId <= 5) {
		applySingleDoor(doorId);
	}
}

void HandlingManager::ResetVehicle(uint16_t sampVehicleId)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	m_pendingVehicleAttribs.erase(sampVehicleId);
	m_vehicleFlying.erase(sampVehicleId);

	auto it = m_vehicleHandlings.find(sampVehicleId);
	if (it == m_vehicleHandlings.end())
		return;

	CVehicle* gtaVehicle = GetGameVehicleFromPool(sampVehicleId);
	if (IsVehiclePointerValid(gtaVehicle)) {
		int modelId = gtaVehicle->m_nModelIndex;

		tHandlingData* fallback = ResolveFallbackHandling(gtaVehicle, sampVehicleId, modelId);
		if (fallback) {
			RecalculateDerivedHandling(fallback, gtaVehicle);
		}
	}
	m_vehicleHandlings.erase(it);
}

void HandlingManager::ProcessModelMods(uint16_t modelId, const std::vector<HandlingAttribEntry>& entries)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
	if (!modelInfo)
		return;
	tHandlingData* original = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);

	auto it = m_modelHandlings.find(modelId);
	if (it == m_modelHandlings.end()) {
		auto newHandling = std::make_unique<tHandlingData>();
		std::memcpy(newHandling.get(), original, sizeof(tHandlingData));
		auto [insertedIt, _] = m_modelHandlings.emplace(modelId, std::move(newHandling));
		it = insertedIt;
	}

	tHandlingData* handling = it->second.get();
	ApplyAttribEntries(handling, entries);
	handling->m_transmissionData.InitGearRatios();
	RecalculateDerivedHandling(handling);

	ApplyModelToVehicles(modelId, handling);
	ClientLog(std::format("[Client] ProcessModelMods: Applied {} attribs to model {}", entries.size(), modelId));
}

void HandlingManager::ResetModel(uint16_t modelId)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	auto it = m_modelHandlings.find(modelId);
	if (it == m_modelHandlings.end())
		return;
	RevertModelToOriginal(modelId);
	m_modelHandlings.erase(it);
}

void HandlingManager::OnVehicleDestructor(CVehicle* pVehicle)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	if (!pVehicle) {
		for (auto it = m_customHandlings.begin(); it != m_customHandlings.end();) {
			if (GetVehicleSAMPId(it->first) == 0xFFFF) {
				if (it->first && it->first->m_pHandlingData == it->second.get()) {
					uint32_t modelIdx = it->first->m_nModelIndex;
					auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelIdx));
					if (modelInfo) {
						it->first->m_pHandlingData = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);
					}
				}
				it = m_customHandlings.erase(it);
			} else {
				++it;
			}
		}
		return;
	}

	if (!IsVehiclePointerValid(pVehicle))
		return;

	uint16_t vehicleId = GetVehicleSAMPId(pVehicle);
	auto cushndleit = m_customHandlings.find(pVehicle);
	auto plrhndleit = m_playerAppliedHandlings.find(vehicleId);
	auto vehhndleit = m_vehicleHandlings.find(vehicleId);
	auto cacheit = m_vehicleToSAMPIdCache.find(pVehicle);

	if (pVehicle->m_pHandlingData) {
		auto customIt = m_customHandlings.find(pVehicle);
		if (customIt != m_customHandlings.end() && pVehicle->m_pHandlingData == customIt->second.get()) {
			uint32_t modelIdx = pVehicle->m_nModelIndex;
			auto* binding = CustomVehicleBindingManager::Instance().Find(vehicleId);
			if (binding && binding->customModelId > 0) {
				modelIdx = binding->customModelId;
			}
			auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelIdx));
			if (modelInfo) {
				pVehicle->m_pHandlingData = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);
			} else {
				pVehicle->m_pHandlingData = nullptr;
			}
		}
	}

	if (cushndleit != m_customHandlings.end()) {
		m_customHandlings.erase(cushndleit);
	}
	if (cacheit != m_vehicleToSAMPIdCache.end()) {
		m_vehicleToSAMPIdCache.erase(cacheit);
	}
}

void HandlingManager::SendHandlingPacket(CustomVehAction action, RakNet::BitStream* bs)
{
	RakNet::BitStream packet;
	packet.Write((uint8_t)PKT_EXTVEH);
	packet.Write((uint8_t)action);
	if (bs) {
		packet.Write(reinterpret_cast<const char*>(bs->GetData()), bs->GetNumberOfBytesUsed());
	}
	bool sent = rakhook::send(&packet, HIGH_PRIORITY, RELIABLE_ORDERED, 0);
	ClientLog(std::format("[Client] SendHandlingPacket: action={}, bytes={}, sent={}", static_cast<int>(action), packet.GetNumberOfBytesUsed(), sent));
}

void HandlingManager::ResetInitState()
{
	HandlingManager::m_initSent.store(false, std::memory_order_release);
	HandlingManager::m_isServerAuthorized.store(false, std::memory_order_release);
}

void HandlingManager::SendInitPacket()
{
	if (m_initSent.exchange(true, std::memory_order_acq_rel)) {
		ClientLog("[Client] ACTION_INIT already sent for current session.");
		return;
	}

	ClientLog(std::format("[Client] Sending ACTION_INIT packet (compat_ver=0x{:X})...", EXTENDEDVEH_COMPAT_VERSION));
	RakNet::BitStream bs;
	bs.Write(static_cast<uint32_t>(EXTENDEDVEH_COMPAT_VERSION));
	SendHandlingPacket(ACTION_INIT, &bs);
}

bool HandlingManager::ProcessAction(CustomVehAction action, RakNet::BitStream* bs)
{
	if (action != ACTION_RESET_ALL && bs == nullptr)
		return false;

	ClientLog(std::format("[Client] ProcessAction: action={}", static_cast<int>(action)));

	switch (action) {
	case ACTION_INIT_RESPONSE: {
		uint32_t compat_ver;
		bool allowed;
		if (!bs->Read(compat_ver) || !bs->Read(allowed)) {
			ClientLog("[Client] ACTION_INIT_RESPONSE: Failed to read packet data");
			return false;
		}

		ClientLog(std::format("[Client] ACTION_INIT_RESPONSE: allowed={}, server_compat_ver=0x{:X}", allowed, compat_ver));
		if (allowed && compat_ver == EXTENDEDVEH_COMPAT_VERSION) {
			m_isServerAuthorized.store(true, std::memory_order_release);
			ClientLog("[Client] authorization established.");
			SendMsg(-1, "{00FF00}[ExtendedVeh]{FFFFFF} Server authorized handling modifications.");
		} else {
			SendMsg(-1, "{FF0000}[ExtendedVeh] Version mismatch with server.");
			return false;
		}
		return true;
	}

	case ACTION_SET_VEHICLE_HANDLING: {
		if (!m_isServerAuthorized.load(std::memory_order_acquire)) {
			ClientLog("[Client] ACTION_SET_VEHICLE_HANDLING: rejected (server not authorized)");
			return false;
		}

		uint16_t vehicleId;
		uint8_t count;
		if (!bs->Read(vehicleId) || !bs->Read(count)) {
			ClientLog("[Client] ACTION_SET_VEHICLE_HANDLING: Failed to read vehicleId/count");
			return false;
		}

		ClientLog(std::format("[Client] ACTION_SET_VEHICLE_HANDLING: vehicleId={}, count={}", vehicleId, count));
		auto entries = ParseAttribEntries(count, bs);
		ProcessVehicleMods(vehicleId, entries);
		return true;
	}

	case ACTION_SET_VEHICLE_DOOR_STATE: {
		uint16_t vehicleId;
		uint8_t doorId;
		bool missing;
		if (!bs->Read(vehicleId) || !bs->Read(doorId) || !bs->Read(missing)) {
			ClientLog("[Client] ACTION_SET_VEHICLE_DOOR_STATE: Failed to read vehicleId/doorId/missing");
			return false;
		}

		ClientLog(std::format("[Client] ACTION_SET_VEHICLE_DOOR_STATE: vehicleId={}, doorId={}, missing={}", vehicleId, doorId, missing));
		MainThreadQueue::Instance().Push([vehicleId, doorId, missing]() {
			HandlingManager::ProcessVehicleDoorState(vehicleId, doorId, missing);
		});
		return true;
	}

	case ACTION_RESET_VEHICLE: {
		uint16_t vehicleId;
		if (!bs->Read(vehicleId)) {
			ClientLog("[Client] ACTION_RESET_VEHICLE: Failed to read vehicleId");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_RESET_VEHICLE: vehicleId={}", vehicleId));
		ResetVehicle(vehicleId);
		return true;
	}

	case ACTION_SET_MODEL_HANDLING: {
		if (!m_isServerAuthorized.load(std::memory_order_acquire)) {
			ClientLog("[Client] ACTION_SET_MODEL_HANDLING: rejected (server not authorized)");
			return false;
		}
		uint16_t modelId;
		uint8_t count;
		if (!bs->Read(modelId) || !bs->Read(count)) {
			ClientLog("[Client] ACTION_SET_MODEL_HANDLING: Failed to read modelId/count");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_SET_MODEL_HANDLING: modelId={}, count={}", modelId, count));
		auto entries = ParseAttribEntries(count, bs);
		ProcessModelMods(modelId, entries);
		return true;
	}

	case ACTION_RESET_MODEL: {
		uint16_t modelId;
		if (!bs->Read(modelId)) {
			ClientLog("[Client] ACTION_RESET_MODEL: Failed to read modelId");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_RESET_MODEL: modelId={}", modelId));
		ResetModel(modelId);
		return true;
	}

	case ACTION_SET_PLAYER_HANDLING: {
		if (!m_isServerAuthorized.load(std::memory_order_acquire)) {
			ClientLog("[Client] ACTION_SET_PLAYER_HANDLING: rejected (server not authorized)");
			return false;
		}
		uint16_t playerId;
		uint8_t count;
		if (!bs->Read(playerId) || !bs->Read(count)) {
			ClientLog("[Client] ACTION_SET_PLAYER_HANDLING: Failed to read playerId/count");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_SET_PLAYER_HANDLING: playerId={}, count={}", playerId, count));
		auto entries = ParseAttribEntries(count, bs);
		ProcessPlayerMods(playerId, entries);
		return true;
	}

	case ACTION_RESET_PLAYER_HANDLING: {
		uint16_t playerId;
		if (!bs->Read(playerId)) {
			ClientLog("[Client] ACTION_RESET_PLAYER_HANDLING: Failed to read playerId");
			return false;
		}
		if (!m_isServerAuthorized.load(std::memory_order_acquire)) {
			ClientLog("[Client] ACTION_RESET_PLAYER_HANDLING: rejected (server not authorized)");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_RESET_PLAYER_HANDLING: playerId={}", playerId));
		ResetPlayerHandling(playerId);
		return true;
	}

	case ACTION_GET_VEHICLE_HANDLING: {
		uint16_t vehicleId;
		if (!bs->Read(vehicleId)) {
			ClientLog("[Client] ACTION_GET_VEHICLE_HANDLING: Failed to read vehicleId");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_GET_VEHICLE_HANDLING: vehicleId={}", vehicleId));
		tHandlingData* data = nullptr;
		auto it = m_vehicleHandlings.find(vehicleId);
		if (it != m_vehicleHandlings.end()) {
			data = it->second.get();
		} else {
			CVehicle* gtaVeh = GetGameVehicleFromPool(vehicleId);
			if (IsVehiclePointerValid(gtaVeh)) {
				int modelId = gtaVeh->m_nModelIndex;
				data = ResolveFallbackHandling(gtaVeh, vehicleId, modelId);
			}
		}
		if (data) {
			RakNet::BitStream response;
			response.Write(vehicleId);
			response.Write(reinterpret_cast<const char*>(data), sizeof(tHandlingData));
			SendHandlingPacket(ACTION_SET_VEHICLE_HANDLING, &response);
		}
		return true;
	}

	case ACTION_GET_MODEL_HANDLING: {
		uint16_t modelId;
		if (!bs->Read(modelId)) {
			ClientLog("[Client] ACTION_GET_MODEL_HANDLING: Failed to read modelId");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_GET_MODEL_HANDLING: modelId={}", modelId));
		tHandlingData* data = nullptr;
		auto it = m_modelHandlings.find(modelId);
		if (it != m_modelHandlings.end()) {
			data = it->second.get();
		} else {
			// Fallback: original handling for that model
			auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
			if (modelInfo) {
				data = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);
			}
		}
		if (data) {
			RakNet::BitStream response;
			response.Write(modelId);
			response.Write(reinterpret_cast<const char*>(data), sizeof(tHandlingData));
			SendHandlingPacket(ACTION_SET_MODEL_HANDLING, &response);
		}
		return true;
	}

	case ACTION_GET_PLAYER_HANDLING: {
		uint16_t playerId;
		if (!bs->Read(playerId)) {
			ClientLog("[Client] ACTION_GET_PLAYER_HANDLING: Failed to read playerId");
			return false;
		}
		ClientLog(std::format("[Client] ACTION_GET_PLAYER_HANDLING: playerId={}", playerId));
		tHandlingData* data = nullptr;
		auto it = m_playerHandlings.find(playerId);
		if (it != m_playerHandlings.end()) {
			data = it->second.get();
		}
		if (data) {
			RakNet::BitStream response;
			response.Write(playerId);
			response.Write(reinterpret_cast<const char*>(data), sizeof(tHandlingData));
			SendHandlingPacket(ACTION_SET_PLAYER_HANDLING, &response);
		}
		return true;
	}

	case ACTION_RESET_ALL: {
		ClientLog("[Client] ACTION_RESET_ALL: Resetting all vehicles, models, and players");
		// Reset all vehicles, models, and players
		std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
		m_pendingVehicleAttribs.clear();

		// Revert all vehicles to their original handling (or model override)
		while (!m_vehicleHandlings.empty())
			ResetVehicle(m_vehicleHandlings.begin()->first);

		// Revert all models
		while (!m_modelHandlings.empty())
			ResetModel(m_modelHandlings.begin()->first);

		// Reset all players
		while (!m_playerHandlings.empty())
			ResetPlayerHandling(m_playerHandlings.begin()->first);

		// Also clear custom handlings (CVehicle* map) - they are per‑vehicle overrides
		while (!m_customHandlings.empty()) {
			ResetVehicleHandling(m_customHandlings.begin()->first);
			RemoveVehicleFromCache(m_customHandlings.begin()->first);
		}

		// Restore door states and clear cache
		for (const auto& [vehId, mask] : m_vehicleDoorStates) {
			CVehicle* gtaVehicle = GetGameVehicleFromPool(vehId);
			if (IsVehiclePointerValid(gtaVehicle)) {
				ApplyDoorState(gtaVehicle, 0xFF, false);
			}
		}
		m_vehicleDoorStates.clear();
		m_vehicleFlying.clear();
		m_vehicleFlyingByPtr.clear();

		CustomVehicleBindingManager::Instance().Clear();

		return true;
	}
	case ACTION_ASSET_BEGIN: {
		ClientLog("[Client] ACTION_ASSET_BEGIN");
		ModelTransferClient::Instance().OnTransferBegin(bs);
		return true;
	}
	case ACTION_ASSET_CHUNK: {
		ModelTransferClient::Instance().OnTransferChunk(bs);
		return true;
	}
	case ACTION_ASSET_END: {
		ClientLog("[Client] ACTION_ASSET_END");
		ModelTransferClient::Instance().OnTransferEnd(bs);
		return true;
	}
	case ACTION_ASSET_CANCEL:
	case ACTION_ASSET_REJECTED: {
		ClientLog(std::format("[Client] ACTION_ASSET_CANCEL / REJECTED: action={}", static_cast<int>(action)));
		ModelTransferClient::Instance().OnTransferCancel(bs);
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::CustomVehicleBind): {
		CustomVeh::Protocol::VehicleBinding binding {};
		if (bs->Read(reinterpret_cast<char*>(&binding), sizeof(binding))) {
			ClientLog(std::format("[Client] CustomVehicleBind: vehId={}, customModelId={}", binding.sampVehicleId, binding.customModelId));
			CustomVehicleBindingManager::Instance().Bind(
				binding.sampVehicleId, binding.customModelId);
		} else {
			ClientLog("[Client] CustomVehicleBind: Failed to read binding");
			return false;
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::CustomVehicleUnbind): {
		CustomVeh::Protocol::VehicleUnbinding unbinding {};
		if (bs->Read(reinterpret_cast<char*>(&unbinding), sizeof(unbinding))) {
			ClientLog(std::format("[Client] CustomVehicleUnbind: vehId={}", unbinding.sampVehicleId));
			uint16_t vehicleId = unbinding.sampVehicleId;
			MainThreadQueue::Instance().Push([vehicleId]() {
				CustomVehicleBindingManager::Instance().Unbind(vehicleId);
			});
		} else {
			ClientLog("[Client] CustomVehicleUnbind: Failed to read unbinding");
			return false;
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleStance): {
		CustomVeh::Protocol::VehicleStancePacket stance {};
		if (bs->Read(reinterpret_cast<char*>(&stance), sizeof(stance))) {
			ClientLog(std::format("[Client] SetVehicleStance: vehId={}, frontScale={:.2f}, rearScale={:.2f}, frontCamber={:.2f}, rearCamber={:.2f}, frontTrack={:.2f}, rearTrack={:.2f}",
				stance.sampVehicleId, stance.frontWheelScale, stance.rearWheelScale, stance.frontCamber, stance.rearCamber, stance.frontTrackWidth, stance.rearTrackWidth));
			CustomVehicleBindingManager::Instance().SetVehicleStance(
				stance.sampVehicleId, stance.frontWheelScale, stance.rearWheelScale,
				stance.frontCamber, stance.rearCamber, stance.frontTrackWidth, stance.rearTrackWidth);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleExtras): {
		CustomVeh::Protocol::VehicleExtrasPacket extras {};
		if (bs->Read(reinterpret_cast<char*>(&extras), sizeof(extras))) {
			ClientLog(std::format("[Client] SetVehicleExtras: vehId={}, mask=0x{:X}", extras.sampVehicleId, extras.extrasMask));
			CustomVehicleBindingManager::Instance().SetVehicleExtras(extras.sampVehicleId, extras.extrasMask);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehiclePaintjob): {
		CustomVeh::Protocol::VehiclePaintjobPacket pj {};
		if (bs->Read(reinterpret_cast<char*>(&pj), sizeof(pj))) {
			ClientLog(std::format("[Client] SetVehiclePaintjob: vehId={}, paintjob={}", pj.sampVehicleId, static_cast<int>(pj.paintjobIndex)));
			CustomVehicleBindingManager::Instance().SetVehiclePaintjob(pj.sampVehicleId, pj.paintjobIndex);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleNeon): {
		CustomVeh::Protocol::VehicleNeonPacket neon {};
		if (bs->Read(reinterpret_cast<char*>(&neon), sizeof(neon))) {
			ClientLog(std::format("[Client] SetVehicleNeon: vehId={}, enabled={}, r={}, g={}, b={}, size={:.2f}",
				neon.sampVehicleId, neon.enabled != 0, neon.r, neon.g, neon.b, neon.size));
			CustomVehicleBindingManager::Instance().SetVehicleNeon(neon.sampVehicleId, neon.enabled != 0, neon.r, neon.g, neon.b, neon.size);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleWindowTint): {
		CustomVeh::Protocol::VehicleWindowTintPacket tint {};
		if (bs->Read(reinterpret_cast<char*>(&tint), sizeof(tint))) {
			ClientLog(std::format("[Client] SetVehicleWindowTint: vehId={}, alpha={}, r={}, g={}, b={}",
				tint.sampVehicleId, tint.alpha, tint.r, tint.g, tint.b));
			CustomVehicleBindingManager::Instance().SetVehicleWindowTint(tint.sampVehicleId, tint.alpha, tint.r, tint.g, tint.b);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleWheelColor): {
		CustomVeh::Protocol::VehicleWheelColorPacket wc {};
		if (bs->Read(reinterpret_cast<char*>(&wc), sizeof(wc))) {
			ClientLog(std::format("[Client] SetVehicleWheelColor: vehId={}, r={}, g={}, b={}",
				wc.sampVehicleId, wc.r, wc.g, wc.b));
			CustomVehicleBindingManager::Instance().SetVehicleWheelColor(wc.sampVehicleId, wc.r, wc.g, wc.b);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleBackfire): {
		CustomVeh::Protocol::VehicleBackfirePacket bf {};
		if (bs->Read(reinterpret_cast<char*>(&bf), sizeof(bf))) {
			ClientLog(std::format("[Client] SetVehicleBackfire: vehId={}, enabled={}",
				bf.sampVehicleId, bf.enabled != 0));
			CustomVehicleBindingManager::Instance().SetVehicleBackfire(bf.sampVehicleId, bf.enabled != 0);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleHorn): {
		CustomVeh::Protocol::VehicleHornPacket horn {};
		if (bs->Read(reinterpret_cast<char*>(&horn), sizeof(horn))) {
			ClientLog(std::format("[Client] SetVehicleHorn: vehId={}, soundId={}, pitch={:.2f}",
				horn.sampVehicleId, horn.hornSoundId, horn.hornPitch));
			CustomVehicleBindingManager::Instance().SetVehicleHorn(horn.sampVehicleId, horn.hornSoundId, horn.hornPitch);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleSiren): {
		CustomVeh::Protocol::VehicleSirenPacket siren {};
		if (bs->Read(reinterpret_cast<char*>(&siren), sizeof(siren))) {
			ClientLog(std::format("[Client] SetVehicleSiren: vehId={}, enabled={}, type={}",
				siren.sampVehicleId, siren.enabled != 0, siren.sirenType));
			CustomVehicleBindingManager::Instance().SetVehicleSiren(siren.sampVehicleId, siren.enabled != 0, siren.sirenType);
		}
		return true;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::SetVehicleLights): {
		CustomVeh::Protocol::VehicleLightsPacket lights {};
		if (bs->Read(reinterpret_cast<char*>(&lights), sizeof(lights))) {
			ClientLog(std::format("[Client] SetVehicleLights: vehId={}, category={}, scale={:.2f}",
				lights.sampVehicleId, lights.lightingCategory, lights.lightScaleMult));
			CustomVehicleBindingManager::Instance().SetVehicleLights(lights.sampVehicleId, lights.lightingCategory, lights.lightScaleMult);
		}
		return true;
	}
	default:
		ClientLog(std::format("[Client] ProcessAction: Unknown or unhandled action {}", static_cast<int>(action)));
		break;
	}
	return false;
}

bool IsVehicleInFlightMode(CVehicle* pVehicle)
{
	return HandlingManager::IsVehicleFlying(pVehicle);
}
