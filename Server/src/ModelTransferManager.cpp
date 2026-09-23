#include "ModelTransferManager.h"
#include "extendedveh.h"
#include "utils.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <zlib.h>
#include <mutex>
#include "defs.h"

namespace fs = std::filesystem;

namespace ModelTransferMgr
{
namespace
{
	std::unordered_map<int, std::unordered_map<uint64_t, bool>> g_clientFileStatus;
	std::mutex g_clientFileStatusMutex;

	struct CachedFile
	{
		std::vector<uint8_t> compressed;
		uint32_t uncompressedSize = 0;
		std::string
			sha256Hex; // hex-encoded, matches CustomVehicleDef's ...Hash fields
		bool valid = false;
	};

	// Keyed by (modelId << 2) | kind
	std::unordered_map<uint64_t, CachedFile> g_cache;
	std::mutex g_cacheMutex;

	struct ActiveTransfer
	{
		int playerId = -1;
		uint32_t modelId = 0;
		ModelFileKind kind {};
		uint32_t nextChunkIndex = 0;
		uint32_t totalChunks = 0;
	};

	std::deque<ActiveTransfer> g_activeTransfers;
	std::mutex g_activeMutex;
	constexpr size_t kMaxActiveTransfersPerPlayer = 10;
	constexpr uint32_t kMaxModelFileSize = 128u * 1024u * 1024u;

	uint64_t CacheKey(uint32_t modelId, ModelFileKind kind)
	{
		return (static_cast<uint64_t>(modelId) << 8) | static_cast<uint64_t>(kind);
	}

	const char* FileExtensionFor(ModelFileKind kind)
	{
		switch (kind)
		{
		case ModelFileKind::Dff:
			return ".dff";
		case ModelFileKind::Txd:
			return ".txd";
		case ModelFileKind::Col:
			return ".col";
		case ModelFileKind::AudioEngine:
		case ModelFileKind::AudioAccel:
		case ModelFileKind::AudioDecel:
		case ModelFileKind::AudioBrake:
		case ModelFileKind::AudioCrash:
			return "";
		}
		return "";
	}

	const CachedFile* GetOrLoadCache(uint32_t modelId, ModelFileKind kind)
	{
		const uint64_t key = CacheKey(modelId, kind);
		{
			std::lock_guard<std::mutex> lock(g_cacheMutex);
			auto it = g_cache.find(key);
			if (it != g_cache.end() && it->second.valid)
				return &it->second;
		}

		fs::path path = GetAssetPath(modelId, static_cast<CustomVeh::Protocol::AssetType>(kind));
		if (!fs::exists(path))
		{
			const char* ext = FileExtensionFor(kind);
			if (ext && ext[0] != '\0')
			{
				fs::path fallbackPath = fs::path(g_modelsDir) / (std::to_string(modelId) + ext);
				if (fs::exists(fallbackPath))
				{
					path = fallbackPath;
				}
			}
		}
		if (!IsPathInsideBase(g_modelsDir, path))
		{
			ExtendedVehCompo* compo = ExtendedVehCompo::get();
			ICore* core = compo ? compo->getCore() : nullptr;
			if (core)
				core->logLn(LogLevel::Warning, "[ModelTransfer] Path traversal detected or path outside models dir: '%s'", path.string().c_str());
			return nullptr;
		}
		if (!fs::exists(path))
		{
			ExtendedVehCompo* compo = ExtendedVehCompo::get();
			ICore* core = compo ? compo->getCore() : nullptr;
			if (core)
				core->logLn(LogLevel::Warning, "[ModelTransfer] Asset file '%s' does not exist for model %u", path.string().c_str(), modelId);
			return nullptr;
		}
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open())
		{
			ExtendedVehCompo* compo = ExtendedVehCompo::get();
			ICore* core = compo ? compo->getCore() : nullptr;
			if (core)
				core->logLn(LogLevel::Warning, "[ModelTransfer] Failed to open asset file '%s' for model %u", path.string().c_str(), modelId);
			return nullptr;
		}

		std::streamsize size = file.tellg();
		if (size <= 0)
		{
			ExtendedVehCompo* compo = ExtendedVehCompo::get();
			ICore* core = compo ? compo->getCore() : nullptr;
			if (core)
				core->logLn(LogLevel::Warning, "[ModelTransfer] Asset file '%s' is empty or invalid size (%lld)", path.string().c_str(), static_cast<long long>(size));
			return nullptr;
		}
		file.seekg(0, std::ios::beg);
		std::vector<uint8_t> raw(static_cast<size_t>(size));
		if (!file.read(reinterpret_cast<char*>(raw.data()), size))
		{
			ExtendedVehCompo* compo = ExtendedVehCompo::get();
			ICore* core = compo ? compo->getCore() : nullptr;
			if (core)
				core->logLn(LogLevel::Warning, "[ModelTransfer] Failed to read %lld bytes from '%s'", static_cast<long long>(size), path.string().c_str());
			return nullptr;
		}

		CachedFile entry;
		entry.uncompressedSize = static_cast<uint32_t>(raw.size());
		entry.sha256Hex = Sha256Hex(raw.data(), raw.size());

		uLongf compressedBound = compressBound(static_cast<uLong>(raw.size()));
		entry.compressed.resize(compressedBound);
		uLongf compressedSize = compressedBound;
		if (compress2(entry.compressed.data(), &compressedSize, raw.data(),
				static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION)
			!= Z_OK)
		{
			return nullptr;
		}
		entry.compressed.resize(compressedSize);
		entry.valid = true;

		{
			std::lock_guard<std::mutex> lock(g_cacheMutex);
			auto [insertedIt, _] = g_cache.insert_or_assign(key, std::move(entry));
			return &insertedIt->second;
		}
	}
} // namespace

void Initialize(const std::string& modelsDirectory)
{
	g_modelsDir = modelsDirectory;
	fs::create_directories(g_modelsDir);
}

void InvalidateCache(uint32_t modelId, ModelFileKind kind)
{
	std::lock_guard<std::mutex> lock(g_cacheMutex);
	g_cache.erase(CacheKey(modelId, kind));
}

void OnRequestFile(IPlayer& player, uint32_t modelId, ModelFileKind kind)
{
	if (!IsModelFileKind(kind) && !IsAudioFileKind(kind))
		return;

	const int playerId = player.getID();
	{
		std::lock_guard<std::mutex> lock(g_activeMutex);
		if (std::any_of(g_activeTransfers.begin(), g_activeTransfers.end(),
				[&](const ActiveTransfer& transfer)
				{
					return transfer.playerId == playerId && transfer.modelId == modelId && transfer.kind == kind;
				}))
			return;

		const size_t playerTransferCount = static_cast<size_t>(std::count_if(
			g_activeTransfers.begin(), g_activeTransfers.end(),
			[&](const ActiveTransfer& transfer)
			{
				return transfer.playerId == playerId;
			}));

		if (playerTransferCount >= kMaxActiveTransfersPerPlayer)
		{
			ExtendedVehCompo* compo = ExtendedVehCompo::get();
			ICore* core = compo ? compo->getCore() : nullptr;
			if (core)
				core->logLn(LogLevel::Warning, "[ModelTransfer] player %d exceeded max concurrent transfers.", player.getID());
			return;
		}
	}

	const CachedFile* cached = GetOrLoadCache(modelId, kind);
	if (!cached)
	{
		CustomVehActionPacket cancel(ACTION_ASSET_CANCEL);
		cancel.data.Write(modelId);
		cancel.data.Write(static_cast<uint8_t>(kind));
		player.sendPacket(
			Span<uint8_t>(cancel.data.GetData(), cancel.data.GetNumberOfBitsUsed()),
			kFileTransferChannel, true);
		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		if (compo)
		{
			ICore* core = compo->getCore();
			if (core)
				core->logLn(LogLevel::Warning,
					"[ModelTransfer] player %d requested modelId %u kind %u - "
					"file missing/unreadable.",
					player.getID(), modelId, static_cast<unsigned>(kind));
		}
		return;
	}

	if (cached->uncompressedSize == 0 || cached->uncompressedSize > kMaxModelFileSize || cached->compressed.empty())
	{
		CustomVehActionPacket cancel(ACTION_ASSET_CANCEL);
		cancel.data.Write(modelId);
		cancel.data.Write(static_cast<uint8_t>(kind));
		player.sendPacket(
			Span<uint8_t>(cancel.data.GetData(), cancel.data.GetNumberOfBitsUsed()),
			kFileTransferChannel, true);

		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		ICore* core = compo ? compo->getCore() : nullptr;
		if (core)
		{
			core->logLn(LogLevel::Warning,
				"[ModelTransfer] player %d requested modelId %u kind %u - invalid size: uncompressed=%u, compressed=%zu (max=%u)",
				player.getID(), modelId, static_cast<unsigned>(kind),
				cached->uncompressedSize, cached->compressed.size(), kMaxModelFileSize);
		}
		return;
	}

	const uint32_t totalChunks = (static_cast<uint32_t>(cached->compressed.size()) + kFileChunkSize - 1) / kFileChunkSize;

	CustomVehActionPacket begin(ACTION_ASSET_BEGIN);
	begin.data.Write(modelId);
	begin.data.Write(static_cast<uint8_t>(kind));
	begin.data.Write(static_cast<uint32_t>(cached->compressed.size()));
	begin.data.Write(cached->uncompressedSize);
	begin.data.Write(totalChunks);
	begin.data.Write(cached->sha256Hex.c_str(),
		static_cast<int>(cached->sha256Hex.size()) + 1); // NUL-terminated
	player.sendPacket(
		Span<uint8_t>(begin.data.GetData(), begin.data.GetNumberOfBitsUsed()),
		kFileTransferChannel, true);

	ActiveTransfer transfer;
	transfer.playerId = player.getID();
	transfer.modelId = modelId;
	transfer.kind = kind;
	transfer.totalChunks = totalChunks;
	{
		std::lock_guard<std::mutex> lock(g_activeMutex);
		g_activeTransfers.push_back(transfer);
	}
}

void CancelTransfer(IPlayer& player, uint32_t modelId, ModelFileKind kind)
{
	if (!IsModelFileKind(kind) && !IsAudioFileKind(kind))
		return;

	const int playerId = player.getID();
	std::lock_guard<std::mutex> lock(g_activeMutex);
	g_activeTransfers.erase(
		std::remove_if(g_activeTransfers.begin(), g_activeTransfers.end(),
			[&](const ActiveTransfer& t)
			{
				return t.playerId == playerId && t.modelId == modelId && t.kind == kind;
			}),
		g_activeTransfers.end());
}

void CancelTransfersForModel(uint32_t modelId)
{
	std::lock_guard<std::mutex> lock(g_activeMutex);
	g_activeTransfers.erase(
		std::remove_if(g_activeTransfers.begin(), g_activeTransfers.end(),
			[modelId](const ActiveTransfer& t)
			{
				return t.modelId == modelId;
			}),
		g_activeTransfers.end());
}

void OnPlayerDisconnect(IPlayer& player)
{
	const int playerId = player.getID();
	std::lock_guard<std::mutex> lock(g_activeMutex);
	g_activeTransfers.erase(std::remove_if(g_activeTransfers.begin(),
								g_activeTransfers.end(),
								[&](const ActiveTransfer& t)
								{
									return t.playerId == playerId;
								}),
		g_activeTransfers.end());
}

void ProcessTick()
{
	std::deque<ActiveTransfer> currentBatch;
	{
		std::lock_guard<std::mutex> lock(g_activeMutex);
		if (g_activeTransfers.empty())
			return;
		currentBatch.swap(g_activeTransfers);
	}

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	std::deque<ActiveTransfer> remainingTransfers;

	while (!currentBatch.empty())
	{
		ActiveTransfer transfer = currentBatch.front();
		currentBatch.pop_front();

		IPlayer* player = compo->GetPlayerByID(transfer.playerId);
		if (!player)
			continue;

		const CachedFile* cached = GetOrLoadCache(transfer.modelId, transfer.kind);
		if (!cached)
			continue;

		uint32_t sentThisTick = 0;
		while (sentThisTick < kChunksPerPlayerPerTick && transfer.nextChunkIndex < transfer.totalChunks)
		{
			const uint32_t offset = transfer.nextChunkIndex * kFileChunkSize;
			const uint32_t remaining = static_cast<uint32_t>(cached->compressed.size()) - offset;
			const uint16_t chunkLen = static_cast<uint16_t>(std::min<uint32_t>(remaining, kFileChunkSize));

			CustomVehActionPacket chunkPkt(ACTION_ASSET_CHUNK);
			chunkPkt.data.Write(transfer.modelId);
			chunkPkt.data.Write(static_cast<uint8_t>(transfer.kind));
			chunkPkt.data.Write(transfer.nextChunkIndex);
			chunkPkt.data.Write(chunkLen);
			chunkPkt.data.Write(reinterpret_cast<const char*>(cached->compressed.data() + offset), chunkLen);
			player->sendPacket(Span<uint8_t>(chunkPkt.data.GetData(), chunkPkt.data.GetNumberOfBitsUsed()), kFileTransferChannel, true);

			++transfer.nextChunkIndex;
			++sentThisTick;
		}

		if (transfer.nextChunkIndex >= transfer.totalChunks)
		{
			CustomVehActionPacket end(ACTION_ASSET_END);
			end.data.Write(transfer.modelId);
			end.data.Write(static_cast<uint8_t>(transfer.kind));
			player->sendPacket(
				Span<uint8_t>(end.data.GetData(), end.data.GetNumberOfBitsUsed()),
				kFileTransferChannel, true);
			// done
		}
		else
		{
			remainingTransfers.push_back(transfer);
		}
	}

	if (!remainingTransfers.empty())
	{
		std::lock_guard<std::mutex> lock(g_activeMutex);
		g_activeTransfers.insert(g_activeTransfers.end(), remainingTransfers.begin(), remainingTransfers.end());
	}
}

// Called from Actions::Process when client reports it stored a file.
// Stores per-player result for later Pawn/native query.
void OnClientReportFileStored(IPlayer& player, uint32_t modelId, ModelFileKind kind, bool success)
{
	if (!IsModelFileKind(kind) && !IsAudioFileKind(kind))
		return;

	const int pid = player.getID();
	const uint64_t key = CacheKey(modelId, kind);

	std::lock_guard<std::mutex> lock(g_clientFileStatusMutex);
	g_clientFileStatus[pid][key] = success;

	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core = compo->getCore();
	if (core)
	{
		core->logLn(LogLevel::Debug, "[ModelTransfer] player %d reported file store: model=%u kind=%u success=%d",
			pid, modelId, static_cast<unsigned>(kind), success ? 1 : 0);
	}
}

int GetClientFileStoreStatus(int playerId, uint32_t modelId, ModelFileKind kind)
{
	const uint64_t key = CacheKey(modelId, kind);
	std::lock_guard<std::mutex> lock(g_clientFileStatusMutex);
	auto pit = g_clientFileStatus.find(playerId);
	if (pit == g_clientFileStatus.end())
		return 0;
	auto fit = pit->second.find(key);
	if (fit == pit->second.end())
		return 0;
	return fit->second ? 1 : 2;
}
}
