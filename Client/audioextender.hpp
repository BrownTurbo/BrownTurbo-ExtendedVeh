#pragma once

#include <game_sa/CAEVehicleAudioEntity.h>
#include <game_sa/CCamera.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CPools.h>
#include <game_sa/CVehicle.h>
#include <game_sa/CVehicleModelInfo.h>
#include <plugin_sa.h>
#include <bass.h>
#pragma comment(lib, "bass.lib")
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Shared/CustomVehicleProtocol.hpp"
#include "defs.h"
#include "CustomVehicleBindingManager.h"
#include "utils.h"

namespace fs = std::filesystem;

class AudioExtender {
public:
	struct CustomAudioFiles {
		std::string engineFile;
		std::string accelFile;
		std::string decelFile;
		std::string brakeFile;
		std::string crashFile;
	};

	struct CustomVehicleAudioDefinition {
		int32_t audioModelId = -1;
		int16_t engineOnSoundBankId = -1;
		int16_t engineOffSoundBankId = -1;
		int16_t accelerateSoundBankId = -1;
		int16_t decelerateSoundBankId = -1;
		int16_t hornSoundId = -1;
		float hornPitch = 1.0f;
		int8_t sirenType = -1; // -1: default, 0: disabled, 1: wail (ambulance/fire), 2: police switching
		bool hasCustomAudioStreams = false;
		CustomVeh::Protocol::CustomAudioInstructions customAudio = {};
		CustomAudioFiles audioFiles = {};
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

	struct VehicleBassRuntime {
		uint16_t vehicleRef = 0;
		uint32_t customModelId = 0;
		HSTREAM hEngine = 0;
		HSTREAM hAccel = 0;
		HSTREAM hDecel = 0;
		HSTREAM hBrake = 0;
		HSTREAM hCrash = 0;
		float baseFreqEngine = 44100.0f;
		float baseFreqAccel = 44100.0f;
		float baseFreqDecel = 44100.0f;
		float baseFreqBrake = 44100.0f;
		float baseFreqCrash = 44100.0f;
		float lastHealth = 1000.0f;
		float lastSpeed = 0.0f;
		float currentRpm = 1.0f;
	};

private:
	static inline std::unordered_map<uint16_t, CustomVehicleAudioState> s_vehicleAudio;
	static inline std::unordered_map<uint32_t, CustomVehicleAudioDefinition> s_customAudioMap;
	static inline std::unordered_map<uint16_t, VehicleBassRuntime> s_vehicleBassRuntimes;
	static inline std::mutex s_audioMutex;
	using InitVehicleAudioFn = void(__thiscall*)(CAEVehicleAudioEntity*, CVehicle*);
	static inline safetyhook::InlineHook s_initVehicleAudioHook;
	static inline bool s_hooksInstalled = false;
	static constexpr std::ptrdiff_t kVehicleAudioIdOffset = 0x2A;

	static void __fastcall Hooked_InitialiseVehicleAudio(CAEVehicleAudioEntity* pAudio, void* edx, CVehicle* pVehicle)
	{
		if (pVehicle && IsVehiclePointerValid(pVehicle)) {
			std::uint32_t modelId = static_cast<std::uint32_t>(pVehicle->m_nModelIndex);
			auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(pVehicle);
			if (binding && binding->customModelId > 0) {
				modelId = binding->customModelId;
			}
			const int origModelIndex = pVehicle->m_nModelIndex;
			bool swappedModel = false;
			AudioExtender::CustomVehicleAudioDefinition def;
			bool hasCustomAudio = false;
			{
				std::lock_guard<std::mutex> lock(s_audioMutex);
				const auto it = s_customAudioMap.find(modelId);
				if (it != s_customAudioMap.end()) {
					def = it->second;
					hasCustomAudio = true;
					if (def.audioModelId >= 400 && def.audioModelId <= 611) {
						pVehicle->m_nModelIndex = def.audioModelId;
						swappedModel = true;
					}
				}
			}
			if (!swappedModel && (pVehicle->m_nModelIndex < 400 || pVehicle->m_nModelIndex > 611)) {
				pVehicle->m_nModelIndex = 400;
				swappedModel = true;
			}
			s_initVehicleAudioHook.original<InitVehicleAudioFn>()(pAudio, pVehicle);
			if (swappedModel) {
				pVehicle->m_nModelIndex = origModelIndex;
			}
			if (hasCustomAudio) {
				ApplyCustomVehicleAudio(*pVehicle, def);
			}
		} else {
			s_initVehicleAudioHook.original<InitVehicleAudioFn>()(pAudio, pVehicle);
		}
	}

public:
	static void InstallHooks()
	{
		if (s_hooksInstalled)
			return;

		void* addrVehicleAudio = GtaAddress(0x4F7670);
		if (addrVehicleAudio && IsExecutableAddress(reinterpret_cast<uintptr_t>(addrVehicleAudio)) && LooksLikeFunctionEntry(reinterpret_cast<uintptr_t>(addrVehicleAudio))) {
			auto removeHook = safetyhook::create_inline(addrVehicleAudio, reinterpret_cast<void*>(&Hooked_InitialiseVehicleAudio));
			if (removeHook) {
				s_initVehicleAudioHook = std::move(removeHook);
			}
		}

		s_hooksInstalled = true;
	}

	static void RestoreHooks()
	{
		if (!s_hooksInstalled)
			return;

		ClearAllBassAudio();
		s_initVehicleAudioHook.reset();
		s_hooksInstalled = false;
	}

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

	static void MuteNativeVehicleAudio(CVehicle* pVeh)
	{
		if (!pVeh || !IsVehiclePointerValid(pVeh))
			return;

		CAEVehicleAudioEntity& audio = pVeh->m_vehicleAudio;
		// Keep m_bSoundsStopped = false and m_fGeneralVehicleSoundVolume = 1.0f so
		// vehicle horn, sirens, and door acoustics remain fully functional.
		// Native engine audio is completely suppressed by clearing sound banks, slots, and active engine voices:
		audio.m_nEngineAccelerateSoundBankId = -1;
		audio.m_nEngineDecelerateSoundBankId = -1;
		audio.m_nEngineBankSlotId = -1;
		audio.m_settings.m_nEngineOnSoundBankId = -1;
		audio.m_settings.m_nEngineOffSoundBankId = -1;

		// Stop and discard any currently active native engine sounds created by GTA SA
		for (int i = 0; i < 12; ++i) {
			CAESound* pSound = audio.m_aEngineSounds[i].m_pSound;
			if (pSound) {
				if (pSound->m_nIsUsed && pSound->m_nPlayingState != 0) {
					pSound->StopSoundAndForget();
				}
				audio.m_aEngineSounds[i].m_pSound = nullptr;
				audio.m_aEngineSounds[i].m_nIndex = static_cast<unsigned int>(-1);
			}
		}

		if (audio.m_pReverseGearSound) {
			if (audio.m_pReverseGearSound->m_nIsUsed && audio.m_pReverseGearSound->m_nPlayingState != 0) {
				audio.m_pReverseGearSound->StopSoundAndForget();
			}
			audio.m_pReverseGearSound = nullptr;
		}
	}

	static void UnmuteNativeVehicleAudio(CVehicle* pVeh)
	{
		if (!pVeh || !IsVehiclePointerValid(pVeh))
			return;

		CAEVehicleAudioEntity& audio = pVeh->m_vehicleAudio;
		audio.m_bSoundsStopped = false;
		audio.m_fGeneralVehicleSoundVolume = 1.0f;
	}

	static std::optional<CAEVehicleAudioEntity*> ApplyCustomVehicleAudio(CVehicle& vehicle, const CustomVehicleAudioDefinition& definition)
	{
		if (!IsVehiclePointerValid(&vehicle))
			return std::nullopt;
		CAEVehicleAudioEntity& audio = vehicle.m_vehicleAudio;

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
		if (definition.hornSoundId >= 0) {
			audio.m_settings.m_bHornTon = static_cast<char>(definition.hornSoundId);
			audio.m_settings.m_fHornHigh = definition.hornPitch;
		}
		if (definition.sirenType >= 0) {
			audio.m_bModelWithSiren = (definition.sirenType > 0);
		}

		if (definition.hasCustomAudioStreams && definition.customAudio.muteNative) {
			MuteNativeVehicleAudio(&vehicle);
		}

		return std::optional<CAEVehicleAudioEntity*>(&audio);
	}

	static bool InitialiseCustomVehicleAudio(CustomVehicleAudioRuntime& state, CVehicle& vehicle, uint32_t customModelId = 0)
	{
		if (!IsVehiclePointerValid(&vehicle))
			return false;
		if (customModelId == 0) {
			auto* binding = CustomVehicleBindingManager::Instance().FindByVehicle(&vehicle);
			if (binding && binding->customModelId > 0) {
				customModelId = binding->customModelId;
			} else {
				customModelId = static_cast<std::uint32_t>(vehicle.m_nModelIndex);
			}
		}
		const auto definition = GetVehicleAudio(customModelId);
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

	static void RegisterVehicleAudio(uint32_t customModelId, uint32_t audioBaseModelId,
		int16_t engineOnSoundId, int16_t engineOffSoundId,
		int16_t accelerateSoundId, int16_t decelerateSoundId,
		int16_t hornSoundId = -1, float hornPitch = 1.0f, int8_t sirenType = -1)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		auto& def = s_customAudioMap[customModelId];
		def.audioModelId = audioBaseModelId;
		def.engineOnSoundBankId = engineOnSoundId;
		def.accelerateSoundBankId = accelerateSoundId;
		def.engineOffSoundBankId = engineOffSoundId;
		def.decelerateSoundBankId = decelerateSoundId;
		if (hornSoundId >= 0) {
			def.hornSoundId = hornSoundId;
			def.hornPitch = hornPitch;
		}
		if (sirenType >= 0) {
			def.sirenType = sirenType;
		}
	}

	static void SetModelHorn(uint32_t customModelId, int16_t hornSoundId, float hornPitch = 1.0f)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		auto& def = s_customAudioMap[customModelId];
		def.hornSoundId = hornSoundId;
		def.hornPitch = hornPitch;
	}

	static void SetModelSiren(uint32_t customModelId, bool hasSiren, int8_t sirenType = 1)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		auto& def = s_customAudioMap[customModelId];
		def.sirenType = hasSiren ? sirenType : 0;
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

	static void EnsureBassStarted()
	{
		static bool s_checked = false;
		if (!s_checked) {
			s_checked = true;
			if (!BASS_IsStarted()) {
				BASS_Init(-1, 44100, BASS_DEVICE_3D, nullptr, nullptr);
				BASS_Start();
			}
			BASS_Set3DFactors(1.0f, 1.0f, 1.0f);
		}
	}

	static void FreeVehicleStreams(VehicleBassRuntime& rt)
	{
		auto freeStream = [](HSTREAM& h) {
			if (h) {
				BASS_ChannelStop(h);
				BASS_StreamFree(h);
				h = 0;
			}
		};
		freeStream(rt.hEngine);
		freeStream(rt.hAccel);
		freeStream(rt.hDecel);
		freeStream(rt.hBrake);
		freeStream(rt.hCrash);
	}

	static void RegisterCustomAudio(uint32_t customModelId,
		const CustomVeh::Protocol::CustomAudioInstructions& instructions,
		const fs::path& enginePath,
		const fs::path& accelPath,
		const fs::path& decelPath,
		const fs::path& brakePath,
		const fs::path& crashPath)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		auto& def = s_customAudioMap[customModelId];
		def.hasCustomAudioStreams = true;
		def.customAudio = instructions;
		def.audioFiles.engineFile = enginePath.string();
		def.audioFiles.accelFile = accelPath.string();
		def.audioFiles.decelFile = decelPath.string();
		def.audioFiles.brakeFile = brakePath.string();
		def.audioFiles.crashFile = crashPath.string();

		ClientLog(LogLevel::Info, std::format("[Client] Registered custom audio for model {}: vol={:.2f}, minD={:.1f}, maxD={:.1f}, pitchM={:.2f}, accelP={:.2f}, muteNative={}",
			customModelId, instructions.volume, instructions.minDistance, instructions.maxDistance,
			instructions.pitchMultiplier, instructions.accelPitchFactor, instructions.muteNative));
	}

	static void ProcessVehicleAudio()
	{
		EnsureBassStarted();

		static uint32_t s_lastAudioTick = 0;
		uint32_t currentTick = GetTickCount();
		float dt = (s_lastAudioTick == 0) ? 0.016f : std::clamp((currentTick - s_lastAudioTick) / 1000.0f, 0.001f, 0.1f);
		s_lastAudioTick = currentTick;

		// Update 3D listener position and orientation from camera
		CVector camPos = TheCamera.GetPosition();
		CVector camFront(0.0f, 1.0f, 0.0f);
		CVector camUp(0.0f, 0.0f, 1.0f);
		if (TheCamera.m_matrix) {
			camFront = TheCamera.GetForward();
			camUp = TheCamera.GetUp();
		}
		BASS_3DVECTOR bCamPos(camPos.x, camPos.z, camPos.y);
		BASS_3DVECTOR bCamFront(camFront.x, camFront.z, camFront.y);
		BASS_3DVECTOR bCamUp(camUp.x, camUp.z, camUp.y);
		BASS_3DVECTOR bCamVel(0.0f, 0.0f, 0.0f);
		BASS_Set3DPosition(&bCamPos, &bCamVel, &bCamFront, &bCamUp);

		std::vector<uint16_t> activeRefs;

		CustomVehicleBindingManager::Instance().ForEachBinding([&](uint16_t sampVehId, const CustomVehicleBindingManager::Binding& b) {
			if (!b.modelApplied || !b.appliedGameVehicle || !IsVehiclePointerValid(b.appliedGameVehicle))
				return;

			CVehicle* pVeh = b.appliedGameVehicle;
			uint16_t vehicleRef = static_cast<uint16_t>(CPools::GetVehicleRef(pVeh));
			activeRefs.push_back(vehicleRef);

			std::lock_guard<std::mutex> lock(s_audioMutex);
			auto itDef = s_customAudioMap.find(b.customModelId);
			if (itDef == s_customAudioMap.end() || !itDef->second.hasCustomAudioStreams)
				return;

			const auto& def = itDef->second;
			const auto& inst = def.customAudio;
			const auto& files = def.audioFiles;

			auto itRt = s_vehicleBassRuntimes.find(vehicleRef);
			if (itRt == s_vehicleBassRuntimes.end()) {
				// Mute original native audiobase before triggering/creating custom audio streams
				if (inst.muteNative) {
					MuteNativeVehicleAudio(pVeh);
				}

				VehicleBassRuntime rt;
				rt.vehicleRef = vehicleRef;
				rt.customModelId = b.customModelId;
				rt.lastHealth = pVeh->m_fHealth;

				auto createStream = [](const std::string& path, bool loop, float minD, float maxD, float vol, float& outBaseFreq) -> HSTREAM {
					if (path.empty()) return 0;
					std::error_code ec;
					if (!fs::exists(path, ec)) return 0;
					DWORD flags = BASS_SAMPLE_3D | BASS_SAMPLE_MONO;
					if (loop) flags |= BASS_SAMPLE_LOOP;
					HSTREAM h = BASS_StreamCreateFile(FALSE, path.c_str(), 0, 0, flags);
					if (h) {
						BASS_ChannelSet3DAttributes(h, BASS_3DMODE_NORMAL, minD, maxD, -1, -1, -1);
						BASS_ChannelSetAttribute(h, BASS_ATTRIB_VOL, vol);
						if (!BASS_ChannelGetAttribute(h, BASS_ATTRIB_FREQ, &outBaseFreq) || outBaseFreq <= 0.0f)
							outBaseFreq = 44100.0f;
					}
					return h;
				};

				rt.hEngine = createStream(files.engineFile, true, inst.minDistance, inst.maxDistance, inst.volume, rt.baseFreqEngine);
				rt.hAccel = createStream(files.accelFile, true, inst.minDistance, inst.maxDistance, 0.0f, rt.baseFreqAccel);
				rt.hDecel = createStream(files.decelFile, true, inst.minDistance, inst.maxDistance, 0.0f, rt.baseFreqDecel);
				rt.hBrake = createStream(files.brakeFile, true, inst.minDistance, inst.maxDistance, 0.0f, rt.baseFreqBrake);
				rt.hCrash = createStream(files.crashFile, false, inst.minDistance, inst.maxDistance, inst.volume, rt.baseFreqCrash);

				auto [insertedIt, _] = s_vehicleBassRuntimes.insert_or_assign(vehicleRef, std::move(rt));
				itRt = insertedIt;
			}

			auto& rt = itRt->second;

			// Continuously ensure native audiobase remains muted while custom audio streams are active
			if (inst.muteNative) {
				MuteNativeVehicleAudio(pVeh);
			}

			// Vehicle physics state
			CVector vPos = pVeh->GetPosition();
			CVector vVel = pVeh->m_vecMoveSpeed * 50.0f;
			BASS_3DVECTOR bvPos(vPos.x, vPos.z, vPos.y);
			BASS_3DVECTOR bvVel(vVel.x, vVel.z, vVel.y);

			if (rt.hEngine) BASS_ChannelSet3DPosition(rt.hEngine, &bvPos, nullptr, &bvVel);
			if (rt.hAccel) BASS_ChannelSet3DPosition(rt.hAccel, &bvPos, nullptr, &bvVel);
			if (rt.hDecel) BASS_ChannelSet3DPosition(rt.hDecel, &bvPos, nullptr, &bvVel);
			if (rt.hBrake) BASS_ChannelSet3DPosition(rt.hBrake, &bvPos, nullptr, &bvVel);
			if (rt.hCrash) BASS_ChannelSet3DPosition(rt.hCrash, &bvPos, nullptr, &bvVel);

			float speed = pVeh->m_vecMoveSpeed.Magnitude();
			float gas = std::abs(pVeh->m_fGasPedal);
			float brake = pVeh->m_fBreakPedal;
			float health = pVeh->m_fHealth;
			bool hasDriver = (pVeh->m_pDriver != nullptr);
			bool isDestroyed = (health <= 0.0f) || pVeh->bIsDrowning;
			bool engineOn = !isDestroyed && (pVeh->bEngineOn || hasDriver || (speed > 0.03f));

			float prevSpeed = rt.lastSpeed;

			// Synchronize input detection for both local player and remote players
			bool isLocal = (hasDriver && pVeh->m_pDriver == FindPlayerPed(-1));
			if (!isLocal && hasDriver) {
				// For remote vehicles in SA-MP: m_fGasPedal and m_fBreakPedal are not updated by GTA controller.
				// Derive throttle and braking from velocity dynamics and speed trend:
				if (speed > 0.04f) {
					float speedDelta = speed - prevSpeed;
					if (speedDelta < -0.015f) {
						// Rapid deceleration indicates braking
						brake = std::clamp(-speedDelta * 25.0f, 0.3f, 1.0f);
						gas = 0.0f;
					} else if (speedDelta > -0.005f) {
						// Speed maintaining or accelerating indicates active throttle
						gas = std::clamp(speed / 0.45f, 0.4f, 1.0f);
						brake = 0.0f;
					} else {
						// Moderate coasting / lift-off deceleration
						gas = 0.0f;
						brake = 0.0f;
					}
				}
			}

			// Realistic vehicle transmission and RPM simulation
			int gear = static_cast<int>(pVeh->m_nCurrentGear);
			if (gear <= 0 || gear > 5) {
				if (speed < 0.22f) gear = 1;
				else if (speed < 0.44f) gear = 2;
				else if (speed < 0.68f) gear = 3;
				else if (speed < 0.92f) gear = 4;
				else gear = 5;
			}

			// Realistic gear speed bands
			float minSpd = 0.0f, maxSpd = 0.26f;
			switch (gear) {
			case 1: minSpd = 0.00f; maxSpd = 0.26f; break;
			case 2: minSpd = 0.18f; maxSpd = 0.48f; break;
			case 3: minSpd = 0.38f; maxSpd = 0.72f; break;
			case 4: minSpd = 0.62f; maxSpd = 0.96f; break;
			default: minSpd = 0.84f; maxSpd = 1.35f; break;
			}

			float gearProgress = (maxSpd > minSpd) ? std::clamp((speed - minSpd) / (maxSpd - minSpd), 0.0f, 1.0f) : 0.0f;

			// Target RPM based on vehicle state
			float targetRpm = 1.0f;
			if (speed < 0.04f) {
				// Standstill / neutral / free-revving
				targetRpm = 1.0f + (inst.accelPitchFactor * 2.2f) * (gas * gas);
			} else {
				// Driving through gears: RPM rises with gear progress, dropping on each upshift
				float baseGearRpm = 1.0f + 0.10f * std::clamp(gear - 1, 0, 4);
				float maxGearRpm = 1.0f + (inst.accelPitchFactor * 2.2f);
				float inGearRpm = baseGearRpm + (maxGearRpm - baseGearRpm) * std::pow(gearProgress, 0.85f);
				float loadBoost = 0.16f * gas;
				targetRpm = inGearRpm + loadBoost;
			}

			// Burnout / wheelspin check: engine screams near redline
			if (pVeh->m_fWheelSpinForAudio > 0.1f && gas > 0.15f) {
				float burnoutRpm = 1.0f + (inst.accelPitchFactor * 2.2f) * std::clamp(pVeh->m_fWheelSpinForAudio / 0.8f, 0.65f, 1.0f);
				targetRpm = std::max(targetRpm, burnoutRpm);
			}

			// Smooth flywheel rotational inertia (revs climb fast, spin down smoothly)
			float revRate = (targetRpm > rt.currentRpm) ? 14.0f : 7.0f;
			rt.currentRpm += (targetRpm - rt.currentRpm) * std::clamp(dt * revRate, 0.0f, 0.45f);

			// 1. Engine idle / running sound loop
			if (rt.hEngine) {
				if (engineOn) {
					float engPitch = inst.pitchMultiplier * (0.95f + 0.15f * (rt.currentRpm - 1.0f));
					BASS_ChannelSetAttribute(rt.hEngine, BASS_ATTRIB_FREQ, rt.baseFreqEngine * engPitch);
					BASS_ChannelSetAttribute(rt.hEngine, BASS_ATTRIB_VOL, inst.volume);
					if (BASS_ChannelIsActive(rt.hEngine) != BASS_ACTIVE_PLAYING)
						BASS_ChannelPlay(rt.hEngine, FALSE);
				} else {
					if (BASS_ChannelIsActive(rt.hEngine) == BASS_ACTIVE_PLAYING)
						BASS_ChannelPause(rt.hEngine);
				}
			}

			// 2. Acceleration "vrooom" rev sound loop
			if (rt.hAccel) {
				if (engineOn && gas > 0.05f) {
					float targetFreq = rt.baseFreqAccel * inst.pitchMultiplier * rt.currentRpm;
					float targetVol = inst.volume * std::clamp<float>(0.25f + 0.75f * gas + 0.15f * (rt.currentRpm - 1.0f), 0.0f, 1.0f);
					BASS_ChannelSetAttribute(rt.hAccel, BASS_ATTRIB_FREQ, targetFreq);
					BASS_ChannelSetAttribute(rt.hAccel, BASS_ATTRIB_VOL, targetVol);
					if (BASS_ChannelIsActive(rt.hAccel) != BASS_ACTIVE_PLAYING)
						BASS_ChannelPlay(rt.hAccel, FALSE);
				} else {
					if (BASS_ChannelIsActive(rt.hAccel) == BASS_ACTIVE_PLAYING)
						BASS_ChannelSlideAttribute(rt.hAccel, BASS_ATTRIB_VOL, 0.0f, 100);
				}
			}

			// 3. Deceleration / overrun sound loop
			if (rt.hDecel) {
				if (engineOn && gas <= 0.05f && speed > 0.07f) {
					float decelPitch = inst.pitchMultiplier * (0.88f + 0.28f * (rt.currentRpm - 1.0f));
					float decelVol = inst.volume * std::clamp(speed / 0.45f, 0.15f, 0.85f);
					BASS_ChannelSetAttribute(rt.hDecel, BASS_ATTRIB_FREQ, rt.baseFreqDecel * decelPitch);
					BASS_ChannelSetAttribute(rt.hDecel, BASS_ATTRIB_VOL, decelVol);
					if (BASS_ChannelIsActive(rt.hDecel) != BASS_ACTIVE_PLAYING)
						BASS_ChannelPlay(rt.hDecel, FALSE);
				} else {
					if (BASS_ChannelIsActive(rt.hDecel) == BASS_ACTIVE_PLAYING)
						BASS_ChannelSlideAttribute(rt.hDecel, BASS_ATTRIB_VOL, 0.0f, 80);
				}
			}

			// 4. Brake squeal sound
			if (rt.hBrake) {
				if (brake > 0.12f && speed > 0.08f && pVeh->m_fWheelSpinForAudio < 0.25f) {
					float brakeVol = inst.volume * std::clamp(brake * (speed / 0.4f), 0.25f, 1.0f);
					BASS_ChannelSetAttribute(rt.hBrake, BASS_ATTRIB_VOL, brakeVol);
					if (BASS_ChannelIsActive(rt.hBrake) != BASS_ACTIVE_PLAYING)
						BASS_ChannelPlay(rt.hBrake, FALSE);
				} else {
					if (BASS_ChannelIsActive(rt.hBrake) == BASS_ACTIVE_PLAYING)
						BASS_ChannelSlideAttribute(rt.hBrake, BASS_ATTRIB_VOL, 0.0f, 60);
				}
			}

			// 5. Crash / impact sound
			if (rt.hCrash) {
				float healthDelta = rt.lastHealth - health;
				float speedDrop = prevSpeed - speed;
				bool isCollision = (healthDelta > 6.0f) || (speedDrop > 0.18f && speed < 0.12f && prevSpeed > 0.22f);
				if (isCollision) {
					float severity = std::clamp(std::max(healthDelta / 50.0f, speedDrop / 0.35f), 0.35f, 1.0f);
					BASS_ChannelSetAttribute(rt.hCrash, BASS_ATTRIB_VOL, inst.volume * severity);
					float crashPitch = inst.pitchMultiplier * (0.95f + 0.10f * (static_cast<float>(rand() % 100) / 100.0f));
					BASS_ChannelSetAttribute(rt.hCrash, BASS_ATTRIB_FREQ, rt.baseFreqCrash * crashPitch);
					BASS_ChannelPlay(rt.hCrash, TRUE);
				}
			}

			rt.lastSpeed = speed;
			rt.lastHealth = health;
		});

		// Clean up runtimes for vehicles no longer bound or active
		{
			std::lock_guard<std::mutex> lock(s_audioMutex);
			for (auto it = s_vehicleBassRuntimes.begin(); it != s_vehicleBassRuntimes.end(); ) {
				if (std::find(activeRefs.begin(), activeRefs.end(), it->first) == activeRefs.end()) {
					FreeVehicleStreams(it->second);
					it = s_vehicleBassRuntimes.erase(it);
				} else {
					++it;
				}
			}
		}

		BASS_Apply3D();
	}

	static void RemoveVehicleBassAudio(uint16_t vehicleRef)
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		auto it = s_vehicleBassRuntimes.find(vehicleRef);
		if (it != s_vehicleBassRuntimes.end()) {
			FreeVehicleStreams(it->second);
			s_vehicleBassRuntimes.erase(it);
		}
	}

	static void ClearAllBassAudio()
	{
		std::lock_guard<std::mutex> lock(s_audioMutex);
		for (auto& [ref, rt] : s_vehicleBassRuntimes) {
			FreeVehicleStreams(rt);
		}
		s_vehicleBassRuntimes.clear();
	}
};
