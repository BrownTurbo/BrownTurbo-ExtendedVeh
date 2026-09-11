#pragma once

#include <game_sa/CAEVehicleAudioEntity.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CVehicle.h>
#include <game_sa/CVehicleModelInfo.h>
#include <plugin_sa.h>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "defs.h"

#include "utils.h"

class AudioExtender {
private:
	struct CustomVehicleAudioDefinition {
		int32_t audioModelId = -1;
		int16_t engineOnSoundBankId = -1;
		int16_t engineOffSoundBankId = -1;
		int16_t accelerateSoundBankId = -1;
		int16_t decelerateSoundBankId = -1;
	};
	struct CustomVehicleAudioRuntime {
		CAESound engineSound;
		bool initialized = false;
		bool playing = false;
		bool active = false;
		short bankSlotId = -1;
		short sfxId = -1;
		std::uint16_t vehicleId {};
	};
	struct CustomVehicleAudioState {
		std::uint16_t vehicleId {};
		CustomVehicleAudioRuntime engine;
	};

	static inline std::unordered_map<uint16_t, CustomVehicleAudioState> s_vehicleAudio;
	static inline std::unordered_map<uint32_t, CustomVehicleAudioDefinition> s_customAudioMap;
	static inline std::mutex s_audioMutex;
	// using InitVehicleAudioFn = void(__thiscall*)(CAEVehicleAudioEntity*, void*, CVehicle*);
	// static inline safetyhook::InlineHook s_initVehicleAudioHook;
	static inline bool s_hooksInstalled = false;
	static constexpr std::ptrdiff_t kVehicleAudioIdOffset = 0x2A;

	static int16_t& GetModelAudioId(CVehicleModelInfo* pInfo)
	{
		return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(pInfo) + kVehicleAudioIdOffset);
	}

	// Hook callback when CAEVehicleAudioEntity initializes sound for a created vehicle instance
	// static void __fastcall Hooked_InitialiseVehicleAudio(CAEVehicleAudioEntity* pAudio, void* edx, CVehicle* pVehicle	{
	// 	if (pVehicle) {
	// 		uint32_t modelId = pVehicle->m_nModelIndex;
	// 		int32_t customSoundId = -1;
	// 		{
	// 			std::lock_guard<std::mutex> lock(s_audioMutex);
	// 			auto it = s_customAudioMap.find(modelId);
	// 			if (it != s_customAudioMap.end()) {
	// 				customSoundId = it->second;
	// 			}
	// 		}

	// 		if (customSoundId != -1) {
	// 			CVehicleModelInfo* pInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(modelId));
	// 			if (pInfo) {
	// 				int16_t& soundIdRef = GetModelAudioId(pInfo);
	// 				int16_t originalSoundId = soundIdRef;
	// 				soundIdRef = static_cast<int16_t>(customSoundId);

	// 				s_initAudVehicleioHook.original<InitAudVehicleioFn>()(pAudio, edx, pVehicle);

	// 				soundIdRef = originalSoundId;
	// 				return;
	// 			}
	// 		}
	// 	}

	// 	s_initAudVehicleioHook.original<InitAudVehicleioFn>()(pAudio, edx, pVehicle);
	// }

public:
	// static void InstallHooks()
	// {
	// 	if (s_hooksInstalled)
	// 		return;

	// 	void* addrVehicleAudio = GtaAddress(0x4EFA10);
	// 	if (!addrVehicleAudio) {
	// 		return;
	// 	}
	// 	if (!IsExecutableAddress(reinterpret_cast<uintptr_t>(addrVehicleAudio)) || !IsInsideMainModule(reinterpret_cast<uintptr_t>(addrVehicleAudio)) || !LooLooksLikeFunctionEntryinterpret_cast<uintptr_t>(addrVehicleAudio))) {
	// 		return;
	// 	}
	// 	auto removeHook = safetyhook::create_inline(addrVehicleAudio, reinterpret_cast<void*>(&Hooked_InitialiseAudVehicleio));

	// 	if (!removeHook) {
	// 		s_initAudVehicleioHook.reset();
	// 		return;
	// 	}

	// 	s_initAudVehicleioHook = std::move(removeHook);

	// 	s_hooksInstalled = true;
	// }

	// static void RestoreHooks()
	// {
	// 	if (!s_hooksInstalled)
	// 		return;

	// 	s_initAudVehicleioHook.reset();
	// 	s_hooksInstalled = false;
	// }

	static CAEVehicleAudioEntity* GetVehicleAudioEntity(CVehicle* vehicle)
	{
		if (!IsVehiclePointerValid(vehicle))
			return nullptr;

		return &vehicle->m_vehicleAudio;
	}

	static std::optional<CustomVehicleAudioState*> GetOrCreateAudioState(CVehicle& vehicle)
	{
		if (!IsVehiclePointerValid(&vehicle))
			return std::nullopt;
		std::lock_guard<std::mutex> lock(s_audioMutex);
		const auto vehicleRef = static_cast<uint16_t>(CPools::GetVehicleRef(&vehicle));
		auto [it, inserted] = s_vehicleAudio.try_emplace(vehicleRef);
		if (inserted) {
			it->second.vehicleId = vehicleRef;
			it->second.engine.vehicleId = vehicleRef;
		}
		return std::optional<CustomVehicleAudioState*>(&it->second);
	}

	static std::optional<AudioExtender::CustomVehicleAudioState*> GetAudioState(CVehicle& vehicle)
	{
		if (!IsVehiclePointerValid(&vehicle))
			return std::nullopt;
		std::lock_guard<std::mutex> lock(s_audioMutex);
		const auto vehicleRef = static_cast<uint16_t>(CPools::GetVehicleRef(&vehicle));
		const auto it = s_vehicleAudio.find(vehicleRef);
		return std::optional<AudioExtender::CustomVehicleAudioState*>((it != s_vehicleAudio.end()) ? &it->second : nullptr);
	}

	static void RemoveVehicleAudioState(uint16_t vehicleRef)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		s_vehicleAudio.erase(vehicleRef);
	}

	static void ApplyCustomVehicleAudio(
		CVehicle& vehicle,
		const CustomVehicleAudioDefinition&
			definition)
	{
		if (!IsVehiclePointerValid(&vehicle))
			return;
		auto& audio = vehicle.m_vehicleAudio;

		if (definition.accelerateSoundBankId >= 0) {
			audio.m_nEngineAccelerateSoundBankId = definition.accelerateSoundBankId;
		}
		if (definition.decelerateSoundBankId >= 0) {
			audio.m_nEngineDecelerateSoundBankId = definition.decelerateSoundBankId;
		}
		if (definition.engineOnSoundBankId >= 0) {
			audio.m_settings.m_nEngineOnSoundBankId = definition.engineOnSoundBankId;
		}
		if (definition.engineOffSoundBankId >= 0) {
			audio.m_settings.m_nEngineOffSoundBankId = definition.engineOffSoundBankId;
		}
	}

	static bool InitialiseCustomVehicleAudio(CustomVehicleAudioRuntime& state, CVehicle& vehicle)
	{
		if (!IsVehiclePointerValid(&vehicle))
			return false;
		const auto modelId = static_cast<std::uint32_t>(vehicle.m_nModelIndex);
		const auto definition = GetVehicleAudio(modelId);
		if (!definition) {
			return false;
		}

		auto* audio = GetVehicleAudioEntity(&vehicle);
		if (!audio) {
			return false;
		}

		if (state.initialized)
			return true;

		state.bankSlotId = definition->engineOnSoundBankId;
		state.sfxId = 0;
		if (state.bankSlotId < 0)
			return false;

		state.vehicleId = static_cast<uint16_t>(CPools::GetVehicleRef(&vehicle));

		state.engineSound.Initialise(
			state.bankSlotId,
			state.sfxId,
			audio,
			vehicle.GetPosition(),
			// as for now these values are hardcoded ... will replace them in the near future.
			1.0f, // volume
			80.0f, // max distance
			1.0f, // speed
			1.0f, // timeScale
			0, // arg9
			0, // environmentFlags
			1.0f, // arg11
			0); // current play position

		state.initialized = true;
		state.playing = true;
		state.active = true;

		ApplyCustomVehicleAudio(vehicle, *definition);
		return true;
	}

	static void RegisterVehicleAudio(uint32_t customModelId, int32_t audioBaseModelId, int16_t engineOnSoundId, int16_t engineOffSoundId, int16_t accelerateSoundId, int16_t decelerateSoundId)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		if (engineOnSoundId >= 0) {
			s_customAudioMap[customModelId].audioModelId = audioBaseModelId;
			s_customAudioMap[customModelId].engineOnSoundBankId = engineOnSoundId;
			s_customAudioMap[customModelId].accelerateSoundBankId = accelerateSoundId;
			s_customAudioMap[customModelId].engineOffSoundBankId = engineOffSoundId;
			s_customAudioMap[customModelId].decelerateSoundBankId = decelerateSoundId;
		} else if (audioBaseModelId >= 400 && audioBaseModelId <= 611) {
			CVehicleModelInfo* pBaseInfo = reinterpret_cast<CVehicleModelInfo*>(GetEngineModelInfo(audioBaseModelId));
			if (pBaseInfo) {
				s_customAudioMap[customModelId].audioModelId = audioBaseModelId;
				s_customAudioMap[customModelId].engineOnSoundBankId = GetModelAudioId(pBaseInfo);
				s_customAudioMap[customModelId].accelerateSoundBankId = -1;
				s_customAudioMap[customModelId].engineOffSoundBankId = -1;
				s_customAudioMap[customModelId].decelerateSoundBankId = -1;
			}
		}
	}

	static void RegisterVehicleAudio(uint32_t customModelId, int16_t engineOnSoundId, int16_t engineOffSoundId, int16_t accelerateSoundId, int16_t decelerateSoundId)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		s_customAudioMap[customModelId].audioModelId = -1;
		s_customAudioMap[customModelId].engineOnSoundBankId = engineOnSoundId;
		s_customAudioMap[customModelId].accelerateSoundBankId = accelerateSoundId;
		s_customAudioMap[customModelId].engineOffSoundBankId = engineOffSoundId;
		s_customAudioMap[customModelId].decelerateSoundBankId = decelerateSoundId;
	}

	static std::optional<AudioExtender::CustomVehicleAudioDefinition> GetVehicleAudio(uint32_t customModelId)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		auto it = s_customAudioMap.find(customModelId);
		return (it != s_customAudioMap.end()) ? std::optional<AudioExtender::CustomVehicleAudioDefinition>(it->second) : std::nullopt;
	}

	static std::optional<AudioExtender::CustomVehicleAudioDefinition> GetAudioIdForModel(uint32_t modelId)
	{
		return GetVehicleAudio(modelId);
	}

	static void UnregisterVehicleAudio(uint32_t customModelId)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		s_customAudioMap.erase(customModelId);
	}
};
