#include "handling_manager.hpp"

#include <sampapi/CChat.h>
#include <sampapi/CLocalPlayer.h>
#include <sampapi/CNetGame.h>
#include <sampapi/CVehiclePool.h>
#include <sampapi/sampapi.h>

#include "utils.h"

#include "CustomVehicleBindingManager.h"
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
bool HandlingManager::m_isServerAuthorized = false;
std::recursive_mutex HandlingManager::m_handlingMutex;
std::deque<HandlingManager::PendingCommand> HandlingManager::m_pendingCommands;
std::mutex HandlingManager::m_pendingMutex;
std::unordered_map<CVehicle*, uint16_t> HandlingManager::m_vehicleToSAMPIdCache;
std::unordered_map<uint32_t, int32_t> HandlingManager::m_modelUseCount;
std::mutex HandlingManager::m_cacheMutex;

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
	entries.reserve(count);

	for (int i = 0; i < count; ++i) {
		CHandlingAttrib attrib;
		if (!bs->Read(attrib))
			return {};
		CHandlingAttribType expectedType = GetHandlingAttribType(attrib);

		HandlingAttribEntry entry {};
		entry.attrib = attrib;
		entry.type = expectedType;

		switch (expectedType) {
		case TYPE_FLOAT: {
			float v;
			if (!bs->Read(v))
				return {};
			entry.value.f = v;
			break;
		}
		case TYPE_UINT:
		case TYPE_FLAG: {
			uint32_t v;
			if (!bs->Read(v))
				return {};
			entry.value.u = v;
			break;
		}
		case TYPE_BYTE: {
			uint8_t v;
			if (!bs->Read(v))
				return {};
			entry.value.b = v;
			break;
		}
		default:
			break;
		}

		if (!CanSetHandlingAttrib(attrib))
			continue;
		entries.push_back(entry);
	}
	return entries;
}

void HandlingManager::ApplyAttribEntries(tHandlingData* handling, const std::vector<HandlingAttribEntry>& entries)
{
	for (const auto& e : entries) {
		void* ptr = ResolveAttributePointer(handling, static_cast<uint8_t>(e.attrib));
		if (!ptr)
			continue;

		switch (e.type) {
		case TYPE_FLOAT:
			*reinterpret_cast<float*>(ptr) = e.value.f;
			break;
		case TYPE_UINT:
		case TYPE_FLAG:
			*reinterpret_cast<uint32_t*>(ptr) = e.value.u;
			break;
		case TYPE_BYTE:
			*reinterpret_cast<uint8_t*>(ptr) = e.value.b;
			break;
		default:
			break;
		}
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
	if (pVehicle) {
		auto customIt = m_customHandlings.find(pVehicle);
		if (customIt != m_customHandlings.end()) {
			return customIt->second.get();
		}
	}
	return ResolveFallbackHandling(vehicleId, modelId);
}

uint16_t HandlingManager::GetVehicleSAMPId(CVehicle* pVehicle)
{
	if (!pVehicle)
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
					for (uint16_t id = 0; id < (std::min)(static_cast<uint16_t>(p->m_nCount), static_cast<uint16_t>(MAX_SAMP_VEHICLES)); ++id) {
						auto* sampVeh = p->Get(id);
						if (sampVeh && sampVeh->m_pGameVehicle == pVehicle) {
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
	if (!vehicle)
		return;
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	auto [it, inserted] = m_vehicleToSAMPIdCache.emplace(vehicle, sampId);
	if (inserted) {
		++m_modelUseCount[static_cast<uint32_t>(vehicle->m_nModelIndex)];
	}
}

void HandlingManager::RemoveVehicleFromCache(CVehicle* vehicle)
{
	if (!vehicle)
		return;
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

bool HandlingManager::IsValidVehicle(CVehicle* pVehicle)
{
	return pVehicle != nullptr && pVehicle->m_pHandlingData != nullptr;
}

void HandlingManager::IsolateVehicleHandling(CVehicle* pVehicle)
{
	if (!IsValidVehicle(pVehicle))
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
	if (!IsValidVehicle(pVehicle))
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);
	IsolateVehicleHandling(pVehicle);

	tHandlingData* handling = m_customHandlings[pVehicle].get();
	handling->m_fMass = mass;
	handling->m_fTurnMass = mass * 1.5f;
	handling->m_fDragMult = (mass / 5.0f) * 0.002f;
}

void HandlingManager::ModifyTransmission(CVehicle* pVehicle, float maxSpeed, float acceleration, int gears)
{
	if (!IsValidVehicle(pVehicle))
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
	if (pVehicle == nullptr)
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	auto it = m_customHandlings.find(pVehicle);
	if (it != m_customHandlings.end()) {
		unsigned int modelIndex = pVehicle->m_nModelIndex;

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

	std::visit([&](auto&& p) {
		using T = std::decay_t<decltype(p)>;
		if constexpr (!std::is_same_v<T, std::nullptr_t>) {
			if (p) {
				for (uint16_t id = 0; id < (std::min)(static_cast<uint16_t>(p->m_nCount), static_cast<uint16_t>(MAX_SAMP_VEHICLES)); ++id) {
					auto* sampVeh = p->Get(id);
					if (!sampVeh || !sampVeh->m_pGameVehicle)
						continue;
					CVehicle* gtaVeh = sampVeh->m_pGameVehicle;
					if (gtaVeh->m_nModelIndex != modelId)
						continue;

					if (m_vehicleHandlings.find(id) == m_vehicleHandlings.end() && m_playerAppliedHandlings.find(id) == m_playerAppliedHandlings.end() && m_customHandlings.find(gtaVeh) == m_customHandlings.end()) {
						gtaVeh->m_pHandlingData = handling;
						gtaVeh->m_fTurnMass = handling->m_fTurnMass;
						gtaVeh->m_fMass = handling->m_fMass;
						gtaVeh->m_nHandlingFlagsIntValue = handling->m_nHandlingFlags;
						gtaVeh->m_vecCentreOfMass = handling->m_vecCentreOfMass;
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
				for (uint16_t id = 0; id < (std::min)(static_cast<uint16_t>(p->m_nCount), static_cast<uint16_t>(MAX_SAMP_VEHICLES)); ++id) {
					auto* sampVeh = p->Get(id);
					if (!sampVeh || !sampVeh->m_pGameVehicle)
						continue;
					CVehicle* gtaVeh = sampVeh->m_pGameVehicle;
					if (gtaVeh->m_nModelIndex != modelId)
						continue;

					if (m_vehicleHandlings.find(id) == m_vehicleHandlings.end() && m_playerAppliedHandlings.find(id) == m_playerAppliedHandlings.end() && m_customHandlings.find(gtaVeh) == m_customHandlings.end()) {
						gtaVeh->m_pHandlingData = original;
						gtaVeh->m_fMass = original->m_fMass;
						gtaVeh->m_fTurnMass = original->m_fTurnMass;
					}
				}
			}
		}
	},
		pool);
}

void HandlingManager::ApplyPlayerHandling(uint16_t playerId, CVehicle* pVehicle)
{
	if (!pVehicle || !pVehicle->m_pHandlingData)
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
	pVehicle->m_pHandlingData = newHandling.get();
	pVehicle->m_fTurnMass = newHandling->m_fTurnMass;
	pVehicle->m_fMass = newHandling->m_fMass;
	pVehicle->m_nHandlingFlagsIntValue = newHandling->m_nHandlingFlags;
	pVehicle->m_vecCentreOfMass = newHandling->m_vecCentreOfMass;

	PlayerAppliedInfo info;
	info.playerId = playerId;
	info.handling = std::move(newHandling);
	m_playerAppliedHandlings[vehicleId] = std::move(info);
}

void HandlingManager::RemovePlayerHandling(uint16_t playerId, CVehicle* pVehicle)
{
	if (!pVehicle)
		return;
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	uint16_t vehicleId = GetVehicleSAMPId(pVehicle);
	if (vehicleId == 0xFFFF)
		return;

	auto it = m_playerAppliedHandlings.find(vehicleId);
	if (it == m_playerAppliedHandlings.end() || it->second.playerId != playerId)
		return;

	int modelId = pVehicle->m_nModelIndex;
	auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
	if (modelInfo) {
		unsigned int handlingId = modelInfo->m_nHandlingId;
		tHandlingData* fallback = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[handlingId]);
		auto modelIt = m_modelHandlings.find(modelId);
		if (modelIt != m_modelHandlings.end()) {
			fallback = modelIt->second.get();
		}
		pVehicle->m_pHandlingData = fallback;
		pVehicle->m_fMass = fallback->m_fMass;
		pVehicle->m_fTurnMass = fallback->m_fTurnMass;
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
			if (gtaVehicle) {
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
	if (!gtaVehicle || !gtaVehicle->m_pHandlingData)
		return;

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
	ApplyAttribEntries(handling, entries);
	handling->m_transmissionData.InitGearRatios();

	gtaVehicle->m_pHandlingData = handling;
	gtaVehicle->m_fTurnMass = handling->m_fTurnMass;
	gtaVehicle->m_fMass = handling->m_fMass;
	gtaVehicle->m_nHandlingFlagsIntValue = handling->m_nHandlingFlags;
	gtaVehicle->m_vecCentreOfMass = handling->m_vecCentreOfMass;
}

void HandlingManager::ResetVehicle(uint16_t sampVehicleId)
{
	std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

	auto it = m_vehicleHandlings.find(sampVehicleId);
	if (it == m_vehicleHandlings.end())
		return;

	CVehicle* gtaVehicle = GetGameVehicleFromPool(sampVehicleId);
	if (gtaVehicle) {
		int modelId = gtaVehicle->m_nModelIndex;

		tHandlingData* fallback = ResolveFallbackHandling(gtaVehicle, sampVehicleId, modelId);
		if (fallback) {
			gtaVehicle->m_pHandlingData = fallback;
			gtaVehicle->m_fMass = fallback->m_fMass;
			gtaVehicle->m_fTurnMass = fallback->m_fTurnMass;
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

	ApplyModelToVehicles(modelId, handling);
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

	uint16_t vehicleId = GetVehicleSAMPId(pVehicle);
	auto cushndleit = m_customHandlings.find(pVehicle);
	auto plrhndleit = m_playerAppliedHandlings.find(vehicleId);
	auto vehhndleit = m_vehicleHandlings.find(vehicleId);
	auto cacheit = m_vehicleToSAMPIdCache.find(pVehicle);

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

	if (pVehicle->m_pHandlingData) {
		auto customIt = m_customHandlings.find(pVehicle);
		if (customIt != m_customHandlings.end() && pVehicle->m_pHandlingData == customIt->second.get()) {
			uint32_t modelIdx = pVehicle->m_nModelIndex;
			auto* modelInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelIdx));
			if (modelInfo) {
				pVehicle->m_pHandlingData = static_cast<tHandlingData*>(&gHandlingDataMgr.m_aVehicleHandling[modelInfo->m_nHandlingId]);
			} else {
				pVehicle->m_pHandlingData = nullptr;
			}
		}
	}

	if (vehicleId != 0xFFFF) {
		if (vehhndleit != m_vehicleHandlings.end()) {
			m_vehicleHandlings.erase(vehhndleit);
		}
		if (plrhndleit != m_playerAppliedHandlings.end()) {
			m_playerAppliedHandlings.erase(plrhndleit);
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
	packet.Write((uint8_t)PKT_CHANDLING);
	packet.Write((uint8_t)action);
	if (bs) {
		packet.Write(reinterpret_cast<const char*>(bs->GetData()), bs->GetNumberOfBytesUsed());
	}
	rakhook::send(&packet, HIGH_PRIORITY, RELIABLE_ORDERED, 0);
}

bool HandlingManager::ProcessAction(CustomVehAction action, RakNet::BitStream* bs)
{
	if (action != ACTION_RESET_ALL && bs == nullptr)
		return false;

	switch (action) {
	case ACTION_INIT_RESPONSE: {
		uint32_t compat_ver;
		bool allowed;
		if (!bs->Read(compat_ver) || !bs->Read(allowed))
			return false;

		if (allowed && compat_ver == EXTENDEDVEH_COMPAT_VERSION) {
			m_isServerAuthorized = true;
			SendMsg(-1, "{00FF00}[ModernCHandling]{FFFFFF} Server authorized handling modifications.");
		} else {
			SendMsg(-1, "{FF0000}[ModernCHandling] Version mismatch with server.");
		}
		return false;
	}

	case ACTION_SET_VEHICLE_HANDLING: {
		if (!m_isServerAuthorized)
			return false;

		uint16_t vehicleId;
		uint8_t count;
		if (!bs->Read(vehicleId) || !bs->Read(count))
			return false;

		QueueCommand({ PendingCommandType::SetVehicle, vehicleId, ParseAttribEntries(count, bs) });
		return false;
	}

	case ACTION_RESET_VEHICLE: {
		uint16_t vehicleId;
		if (!bs->Read(vehicleId))
			return false;
		QueueCommand({ PendingCommandType::ResetVehicle, vehicleId, {} });
		return false;
	}

	case ACTION_SET_MODEL_HANDLING: {
		if (!m_isServerAuthorized)
			return false;
		uint16_t modelId;
		uint8_t count;
		if (!bs->Read(modelId) || !bs->Read(count))
			return false;
		QueueCommand({ PendingCommandType::SetModel, modelId, ParseAttribEntries(count, bs) });
		return false;
	}

	case ACTION_RESET_MODEL: {
		uint16_t modelId;
		if (!bs->Read(modelId))
			return false;
		QueueCommand({ PendingCommandType::ResetModel, modelId, {} });
		return false;
	}

	case ACTION_SET_PLAYER_HANDLING: {
		if (!m_isServerAuthorized)
			return false;
		uint16_t playerId;
		uint8_t count;
		if (!bs->Read(playerId) || !bs->Read(count))
			return false;
		QueueCommand({ PendingCommandType::SetPlayer, playerId, ParseAttribEntries(count, bs) });
		return false;
	}

	case ACTION_RESET_PLAYER_HANDLING: {
		uint16_t playerId;
		if (!bs->Read(playerId))
			return false;
		if (!m_isServerAuthorized)
			return false;
		QueueCommand({ PendingCommandType::ResetPlayer, playerId, {} });
		return false;
	}

	case ACTION_GET_VEHICLE_HANDLING: {
		uint16_t vehicleId;
		if (!bs->Read(vehicleId))
			return false;
		tHandlingData* data = nullptr;
		auto it = m_vehicleHandlings.find(vehicleId);
		if (it != m_vehicleHandlings.end()) {
			data = it->second.get();
		} else {
			CVehicle* gtaVeh = GetGameVehicleFromPool(vehicleId);
			if (gtaVeh) {
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
		return false;
	}

	case ACTION_GET_MODEL_HANDLING: {
		uint16_t modelId;
		if (!bs->Read(modelId))
			return false;
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
		return false;
	}

	case ACTION_GET_PLAYER_HANDLING: {
		uint16_t playerId;
		if (!bs->Read(playerId))
			return false;
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
		return false;
	}

	case ACTION_RESET_ALL: {
		// Reset all vehicles, models, and players
		std::lock_guard<std::recursive_mutex> lock(m_handlingMutex);

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
		return false;
	}
	case ACTION_ASSET_BEGIN: {
		ModelTransferClient::Instance().OnTransferBegin(bs);
		return false;
	}
	case ACTION_ASSET_CHUNK: {
		ModelTransferClient::Instance().OnTransferChunk(bs);
		return false;
	}
	case ACTION_ASSET_END: {
		ModelTransferClient::Instance().OnTransferEnd(bs);
		return false;
	}
	case ACTION_ASSET_CANCEL: {
		ModelTransferClient::Instance().OnTransferCancel(bs);
		return false;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::CustomVehicleBind): {
		CustomVeh::Protocol::VehicleBinding binding {};
		if (bs->Read(reinterpret_cast<char*>(&binding), sizeof(binding))) {
			CustomVehicleBindingManager::Instance().Bind(
				binding.sampVehicleId, binding.customModelId);
		}
		return false;
	}
	case static_cast<CustomVehAction>(CustomVeh::Protocol::Action::CustomVehicleUnbind): {
		CustomVeh::Protocol::VehicleUnbinding unbinding {};
		if (bs->Read(reinterpret_cast<char*>(&unbinding), sizeof(unbinding))) {
			CustomVehicleBindingManager::Instance().Unbind(unbinding.sampVehicleId);
		}
		return false;
	}
	default:
		break;
	}
	return true;
}
