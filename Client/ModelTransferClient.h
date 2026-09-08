#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <RakHook/rakhook.hpp>
#include <RakHook/samp.hpp>
#include <RakNet/BitStream.h>
#include <RakNet/PacketEnumerations.h>
#include <RakNet/StringCompressor.h>

namespace fs = std::filesystem;

enum class ModelFileKind : uint8_t { Dff = 0,
	Txd = 1,
	Col = 2 };

struct TransferProgress {
	uint32_t modelId = 0;
	ModelFileKind kind {};
	uint32_t compressedSize = 0;
	uint32_t uncompressedSize = 0;
	uint32_t totalChunks = 0;
	uint32_t receivedChunks = 0;
	uint32_t receivedBytes = 0; // compressed bytes received so far
	std::chrono::steady_clock::time_point startTime;
	bool fromCache = false; // cache hits are shown briefly then removed
	bool failed = false;
	std::string statusText;

	// Retry & diagnostics (added for UI)
	int attempts = 0;
	std::string lastError;
};

std::string Sha256HexOfFile(const fs::path& path);
std::string Sha256HexOfBuffer(const uint8_t* data, size_t length);

class ModelTransferClient {
private:
	struct InFlight {
		TransferProgress progress;
		std::vector<uint8_t> compressedBuffer;
		std::string expectedSha256;
		std::function<void(bool, const fs::path&)> onReady;

		// Retry bookkeeping:
		int attempts = 0; // 1 == initial request just sent
		std::chrono::steady_clock::time_point nextRetryTime = std::chrono::steady_clock::time_point::min();
		uint32_t backoffMs = 0;
		// When we expect a response, we set startTime; worker checks timeouts relative to it.
	};

public:
	static ModelTransferClient& Instance()
	{
		static ModelTransferClient instance;
		return instance;
	}

	// RequestFile checks cache and either immediately calls onReady(true,path)
	// or sends ACTION_REQUEST_FILE_TRANSFER and returns.
	void RequestFile(uint32_t modelId, ModelFileKind kind, const std::string& expectedSha256Hex,
		std::function<void(bool success, const fs::path& localPath)> onReady);

	// Call these on the client's ID_CHANDLING dispatcher when corresponding actions arrive
	void OnTransferBegin(RakNet::BitStream* bs);
	void OnTransferChunk(RakNet::BitStream* bs);
	void OnTransferEnd(RakNet::BitStream* bs);
	void OnTransferCancel(RakNet::BitStream* bs);

	// Thread-safe snapshot for UI - includes attempts & lastError now
	std::vector<TransferProgress> Snapshot() const;

	void Shutdown();
	void FailImmediately(std::unordered_map<uint64_t, InFlight>::iterator it, const std::string& err);
	void ManualRetry(uint32_t modelId, ModelFileKind kind);
	void CancelAll(const std::string& reason);

	private : ModelTransferClient();
	~ModelTransferClient();

	static uint64_t Key(uint32_t modelId, ModelFileKind kind)
	{
		return (static_cast<uint64_t>(modelId) << 2) | static_cast<uint64_t>(kind);
	}

	void FinishTransfer(uint64_t key, bool success, const std::string& err = {});
	void ScheduleRetry(std::unordered_map<uint64_t, InFlight>::iterator it, const std::string& err);
	void EnsureWorkerStarted();
	void WorkerMain();

	mutable std::mutex m_mutex;
	std::unordered_map<uint64_t, InFlight> m_active;

	// Worker thread
	std::thread m_worker;
	std::atomic<bool> m_stopWorker { false };
	std::atomic<bool> m_workerStarted { false };

	// Retry/backoff policy (tweakable)
	static constexpr int kRequestChannel = 1; // must match server kFileTransferChannel
};
