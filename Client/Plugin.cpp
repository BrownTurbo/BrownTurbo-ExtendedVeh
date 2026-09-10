// SDK
#include <plugin_sa.h>

#include <game_sa/CHandlingDataMgr.h>
#include <game_sa/CModelInfo.h>
#include <game_sa/CTxdStore.h>
#include <game_sa/CVisibilityPlugins.h>
#include <game_sa/rw/rpworld.h>
#include <shared/game/CVector.h>

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

#include "CollisionLoader.h"
#include "ImGuiOverlay.h"
#include "../Shared/CustomVehicleProtocol.hpp"
#include "CryptoUtility.h"
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

		char sha256[CustomVeh::Protocol::SHA256_BUFFER_SIZE];
		if (!bs.Read(sha256, CustomVeh::Protocol::SHA256_BUFFER_SIZE))
			return false;
		asset.sha256 = std::string(sha256);

		char filename[CustomVeh::Protocol::FILENAME_SIZE];
		if (!bs.Read(filename, CustomVeh::Protocol::FILENAME_SIZE))
			return false;
		asset.filename = filename;

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
				std::lock_guard<std::mutex> lock(pending->assetMutex);
				pending->colState = AssetState::Failed;
				return;
			}
			ModelTransferClient::Instance().RequestFile(
				pending->def.customModelId, ModelFileKind::Col, pending->def.col.sha256,
				[pending, pushToQueue](bool ok, const fs::path& path) {
					if (ok) {
						std::lock_guard<std::mutex> lock(pending->assetMutex);
						pending->colPath = path;
						pending->colState = AssetState::Ready;
					}
					pushToQueue();
				});
		};

		auto beginDff = [this, pending, beginCol](bool ok, const fs::path& path) {
			if (!ok) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
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
				return;
			}
			const auto size = file.tellg();
			if (size <= 0) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
				return;
			}

			file.seekg(0, std::ios::beg);

			std::vector<std::uint8_t>raw(static_cast<std::size_t>(size));

			if (!file.read(reinterpret_cast<char*>(raw.data()), size)) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
				return;
			}

			pending->dff = BinaryRwParser::ExtractClump(raw);
			if (pending->dff.empty()) {
				{
					std::lock_guard<std::mutex> lock(pending->assetMutex);
					pending->dffState = AssetState::Failed;
				}
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
			[this, pending, beginDff](bool ok, const fs::path& path) {
				if (!ok)
					return;

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
		CVehicleModelInfo* newModel = pending->modelInfo;

		int txdSlot = CTxdStore::AddTxdSlot(std::format("custom_veh_{}", pending->def.customModelId).c_str());

		RwMemory txdMem { pending->txd.data(), static_cast<RwUInt32>(pending->txd.size()) };
		RwStream* txdStream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &txdMem);

		if (!txdStream) {
			CTxdStore::RemoveTxdSlot(txdSlot);
			SendMsg(0xFF0000, std::format("[CustomVeh] Failed to open TXD stream for model {}", pending->def.customModelId).c_str());
			return;
		}

		if (!CTxdStore::LoadTxd(txdSlot, txdStream)) {
			RwStreamClose(txdStream, nullptr);
			CTxdStore::RemoveTxdSlot(txdSlot);
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
			if (RwStreamFindChunk(dffStream, rwID_CLUMP, nullptr, nullptr)) {
				RpClump* pClump = RpClumpStreamRead(dffStream);
				if (pClump) {
					if (!StreamingExtender::FinalizeClump(newModel, pClump)) {
						CTxdStore::RemoveTxdSlot(txdSlot);
						SendMsg(0xFF0000, std::format("[CustomVeh] Failed to finalize clump for model {}", pending->def.customModelId).c_str());
					}
					if (pending->colState == AssetState::Ready && (pending->def.flags & CustomVeh::Protocol::HasCol) != 0) {
						ExtendedVeh::Collision::CollisionLoader* colLoader = &ExtendedVeh::Collision::CollisionLoader::Instance();
						if (!colLoader->LoadCollisionFromMemory(pending->col.data(), pending->col.size(), newModel)) {
							SendMsg(0xFF8800, std::format("[CustomVeh] Warning: Failed to parse COL for model {}", pending->def.customModelId).c_str());
						}
					}
				} else {
					CTxdStore::RemoveTxdSlot(txdSlot);
					SendMsg(0xFF0000, std::format("[CustomVeh] Failed to parse DFF for model {}", pending->def.customModelId).c_str());
				}
			} else {
				SendMsg(0xFF0000, std::format("[CustomVeh] No CLUMP chunk in DFF for model {}", pending->def.customModelId).c_str());
			}
			RwStreamClose(dffStream, nullptr);
		} else {
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
				// AudioExtender::InstallHooks();
				TransferConfig::Instance().Load();
				ModelCache::Instance().Sweep();
				m_runtimeInitialized = true;
			}
		});

		Events::shutdownRwEvent.Add([this]() {
			ModelTransferClient::Instance().CancelAll("client shutdown");
			ModelTransferClient::Instance().Shutdown();
			// AudioExtender::RestoreHooks();
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
			_customVehInstance.RequestClearAllCustomModels();
			HandlingManager::ProcessAction(ACTION_RESET_ALL, nullptr);
			ModelTransferClient::Instance().CancelAll("server connection lost");
		});
	}

	~CustomVehiclesASI() = default;
} _customVehInstance;

bool ASIinitialized = false;
void InitializeHooks()
{
	while (GetModuleHandleA("samp.dll") == nullptr) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	while (!ASIinitialized) {
		if (rakhook::samp_addr() && rakhook::samp_version() != rakhook::samp_ver::unknown) {
			if (IsGameInitialized()) {
				if (rakhook::initialize()) {
					ASIinitialized = true;
					break;
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	rakhook::on_receive_rpc += [](unsigned char& id, RakNet::BitStream* bs) -> bool {
		if (id == RPC_WorldPlayerAdd) {
			size_t originalOffset = bs->GetReadOffset();
            uint16_t playerId = 0;
            bs->Read(playerId);
            bs->SetReadOffset(originalOffset);
            _customVehInstance.onPlayerStreamIn(playerId);
        }
        else if (id == RPC_WorldPlayerRemove) {
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
				_customVehInstance.PushDestructionCommand(customModelId);
				return false;
			}
			return HandlingManager::ProcessAction(actionID, &bs);
		} else if (packetId == ID_DISCONNECTION_NOTIFICATION || packetId == ID_CONNECTION_LOST || packetId == ID_CONNECTION_BANNED) {
			_customVehInstance.RequestClearAllCustomModels();
			HandlingManager::ProcessAction(ACTION_RESET_ALL, nullptr);
		}
		return true;
	};
}

std::unique_ptr<c_plugin> Plugn;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason) {
	case DLL_PROCESS_ATTACH: {
		DisableThreadLibraryCalls(hModule);

		Plugn = std::make_unique<c_plugin>(hModule);
		Events::initGameEvent += []() {
			static bool threadSpawned = false;
			if (!threadSpawned) {
				threadSpawned = true;
				std::thread(InitializeHooks).detach();
			}
			colLoader->Initialize();
		};
		static CVehicle* s_prevLocalVehicle = nullptr;
		Events::gameProcessEvent += []() {
			MainThreadQueue::Instance().DrainOnMainThread();
			Plugn->game_loop();
			_customVehInstance.ProcessPendingDefinitions();
			_customVehInstance.ProcessCompletedDownloads();
			_customVehInstance.ProcessPendingDestructions();
			_customVehInstance.ProcessPendingClearAll();
			HandlingManager::ProcessPendingCommands();

			// Update vehicle cache and model use counts
			struct VehicleEntry {
				CVehicle* gameVeh;
				uint16_t sampId;
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
							for (uint16_t id = 0; id < (std::min)(static_cast<uint16_t>(p->m_nCount), static_cast<uint16_t>(MAX_SAMP_VEHICLES)); ++id) {
								auto* sampVeh = p->Get(id);
								if (sampVeh && sampVeh->m_pGameVehicle && IsVehicleStreamedForLocalPlayer(sampVeh->m_pGameVehicle)) {
									currentVehicles.push_back({ sampVeh->m_pGameVehicle, id });
								}
							}
						}
					}
				},
					pool);
			}

			// Detect new vehicles
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
				}
			}

			// Update previousVehicles for next frame
			previousVehicles = std::move(currentVehicles);

			// Vehicle destructor cleanup
			HandlingManager::OnVehicleDestructor(nullptr);

			// Vehicle enter / exit for local player
			auto* localPed = FindPlayerPed();
			if (!localPed) {
				s_prevLocalVehicle = nullptr;
				return;
			}
			CVehicle* curVehicle = localPed->m_pVehicle;

			if (curVehicle != s_prevLocalVehicle) {
				uint16_t localId = GetLocalPlayerId();
				if (s_prevLocalVehicle)
					HandlingManager::RemovePlayerHandling(localId, s_prevLocalVehicle);
				if (curVehicle)
					HandlingManager::ApplyPlayerHandling(localId, curVehicle);
				s_prevLocalVehicle = curVehicle;
			}
		};
		break;
	}
	case DLL_PROCESS_DETACH: {
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
