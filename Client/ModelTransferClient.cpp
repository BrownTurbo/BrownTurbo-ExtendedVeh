
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#include "ModelTransferClient.h"
#include "MainThreadQueue.h"
#include "ModelCache.h"
#include "ImGuiOverlay.h"

#include <windows.h>
#include <RakNet/BitStream.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

#include "../Shared/CustomVehicleProtocol.hpp"
#include "defs.h"
#include "handling_manager.hpp"
#include "utils.h"

ModelTransferClient::ModelTransferClient()
{
	// nothing else here; worker started lazily during first RequestFile call.
}

ModelTransferClient::~ModelTransferClient()
{
	Shutdown();
}

void ModelTransferClient::Shutdown()
{
	if (m_workerStarted && !m_stopWorker) {
		m_stopWorker = true;
		if (m_worker.joinable())
			m_worker.join();
		m_workerStarted = false;
	}
}

void ModelTransferClient::ManualRetry(uint32_t modelId, ModelFileKind kind)
{
	ClientLog(LogLevel::Info, std::format("ModelTransferClient::ManualRetry: modelId={}, kind={}", modelId, static_cast<int>(kind)));
	const uint64_t key = Key(modelId, kind);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(key);
		if (it == m_active.end())
			return; // no in-flight transfer to retry (can't re-request without expectedSha etc.)

		InFlight& entry = it->second;
		// Reset attempt/backoff bookkeeping
		entry.attempts = 1;
		entry.progress.attempts = 1;
		entry.backoffMs = static_cast<uint32_t>(TransferConfig::Instance().retryInitialBackoffMs);
		entry.nextRetryTime = std::chrono::steady_clock::time_point::min();
		entry.progress.lastError.clear();
		entry.progress.failed = false;
		entry.progress.statusText = "manual retry";

		// reset progress bytes so UI and worker compute timeouts from now
		entry.compressedBuffer.clear();
		entry.progress.receivedBytes = 0;
		entry.progress.receivedChunks = 0;
		entry.progress.startTime = std::chrono::steady_clock::now();
	}

	// Send immediate request on the main thread to avoid any RakNet threading issues.
	MainThreadQueue::Instance().Push([modelId, kind]() {
		RakNet::BitStream bs;
		bs.Write(static_cast<uint8_t>(PKT_EXTVEH));
		bs.Write(static_cast<uint8_t>(CustomVehAction::AssetRequest)); // ACTION_REQUEST_FILE_TRANSFER
		bs.Write(modelId);
		bs.Write(static_cast<uint8_t>(kind));
		rakhook::send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, TransferConfig::Instance().RequestChannel);
	});
}

void ModelTransferClient::EnsureWorkerStarted()
{
	bool expected = false;
	if (m_workerStarted.compare_exchange_strong(expected, true)) {
		m_stopWorker = false;
		m_worker = std::thread([this] {
			WorkerMain();
		});
	}
}

void ModelTransferClient::RequestFile(uint32_t modelId, ModelFileKind kind, const std::string& expectedSha256Hex,
	std::function<void(bool, const fs::path&)> onReady)
{
	ClientLog(LogLevel::Info, std::format("ModelTransferClient::RequestFile: modelId={}, kind={}, expectedSha='{}'", modelId, static_cast<int>(kind), expectedSha256Hex));
	if (auto cached = ModelCache::Instance().TryGet(modelId, static_cast<uint8_t>(kind), expectedSha256Hex)) {
		ClientLog(LogLevel::Info, std::format("ModelTransferClient::RequestFile: cache HIT for modelId={}, kind={}, path='{}'", modelId, static_cast<int>(kind), cached->string()));
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			TransferProgress prog;
			prog.modelId = modelId;
			prog.kind = kind;
			prog.fromCache = true;
			prog.statusText = "cached";
			m_completed.push_back(std::move(prog));
		}
		onReady(true, *cached);
		return;
	}

	ClientLog(LogLevel::Debug, std::format("ModelTransferClient::RequestFile: cache MISS for modelId={}, kind={}, starting network transfer...", modelId, static_cast<int>(kind)));
	EnsureWorkerStarted();

	InFlight entry;
	entry.progress.modelId = modelId;
	entry.progress.kind = kind;
	entry.progress.startTime = std::chrono::steady_clock::now();
	entry.progress.statusText = "requesting";
	entry.progress.attempts = 1;
	entry.attempts = 1;
	entry.backoffMs = static_cast<uint32_t>(TransferConfig::Instance().retryInitialBackoffMs);
	entry.expectedSha256 = expectedSha256Hex;
	entry.onReady = std::move(onReady);

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_active[Key(modelId, kind)] = std::move(entry);
	}

	// Auto-show download window when network transfer starts
	ClientLog(LogLevel::Info, std::format("ModelTransferClient::RequestFile: auto-showing download window for modelId={}, kind={}", modelId, static_cast<int>(kind)));
	g_windowVisible.store(true, std::memory_order_release);

	// Build RakNet packet: PKT_EXTVEH (251) + ACTION_REQUEST_FILE_TRANSFER (30) + modelId + kind
	// We enqueue the send on main thread to be safe: main-thread send avoids any RakNet thread-safety issues.
	MainThreadQueue::Instance().Push([modelId, kind]() {
		RakNet::BitStream bs;
		bs.Write(static_cast<uint8_t>(PKT_EXTVEH));
		bs.Write(static_cast<uint8_t>(CustomVehAction::AssetRequest)); // ACTION_REQUEST_FILE_TRANSFER
		bs.Write(modelId);
		bs.Write(static_cast<uint8_t>(kind));
		rakhook::send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, TransferConfig::Instance().RequestChannel);
		ClientLog(LogLevel::Debug, std::format("ModelTransferClient::RequestFile: sent RakNet AssetRequest packet for modelId={}, kind={}", modelId, static_cast<int>(kind)));
	});
}

std::vector<TransferProgress> ModelTransferClient::Snapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<TransferProgress> result;
	result.reserve(m_active.size() + m_completed.size());
	for (auto& [key, entry] : m_active)
		result.push_back(entry.progress);
	for (auto& entry : m_completed)
		result.push_back(entry);
	ClientLog(LogLevel::Debug, std::format("ModelTransferClient::Snapshot: returning {} transfers ({} active, {} completed/cached)", result.size(), m_active.size(), m_completed.size()));
	return result;
}

bool ModelTransferClient::HasActiveTransfers() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return !m_active.empty();
}

bool ModelTransferClient::HasFailedTransfers() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto& [k, entry] : m_active) {
		if (entry.progress.failed)
			return true;
	}
	return false;
}

void ModelTransferClient::ClearHistory()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_completed.clear();
}

void ModelTransferClient::FailImmediately(std::unordered_map<uint64_t, InFlight>::iterator it, const std::string& err)
{
	ClientLog(LogLevel::Error, std::format("ModelTransferClient::FailImmediately: modelId={}, kind={}, err='{}'", it->second.progress.modelId, static_cast<int>(it->second.progress.kind), err));
	it->second.progress.failed = true;
	it->second.progress.lastError = err;
	it->second.progress.statusText = "failed";
	auto onReady = it->second.onReady;
	uint64_t key = it->first;

	MainThreadQueue::Instance().Push([this, key, onReady] {
		if (onReady)
			onReady(false, {});
		std::lock_guard<std::mutex> lock(m_mutex);
		m_active.erase(key);
	});
}

void ModelTransferClient::CancelAll(const std::string& reason)
{
	std::vector<std::function<void(bool, const fs::path&)>> callbacks;
	{
		std::lock_guard lock(m_mutex);
		callbacks.reserve(m_active.size());
		for (auto& [key, entry] : m_active) {
			entry.progress.failed = true;

			entry.progress.statusText = "cancelled";

			entry.progress.lastError = reason;

			if (entry.onReady) {
				callbacks.push_back(
					entry.onReady);
			}
		}
		m_active.clear();
		m_completed.clear();
	}

	MainThreadQueue::Instance().Push([callbacks = std::move(callbacks)]() {
		for (const auto& callback : callbacks) {
			callback(false, {});
		}
	});
}

void ModelTransferClient::CancelModelTransfers(uint32_t customModelId)
{
	std::vector<std::function<void(bool, const fs::path&)>> callbacks;
	{
		std::lock_guard lock(m_mutex);
		for (auto it = m_active.begin(); it != m_active.end();) {
			if (it->second.progress.modelId == customModelId) {
				it->second.progress.failed = true;
				it->second.progress.statusText = "cancelled";
				it->second.progress.lastError = "model destroyed";
				if (it->second.onReady) {
					callbacks.push_back(it->second.onReady);
				}
				it = m_active.erase(it);
			} else {
				++it;
			}
		}
	}

	if (!callbacks.empty()) {
		MainThreadQueue::Instance().Push([callbacks = std::move(callbacks)]() {
			for (const auto& callback : callbacks) {
				callback(false, {});
			}
		});
	}
}

void ModelTransferClient::OnTransferBegin(RakNet::BitStream* bs)
{
	uint32_t modelId;
	uint8_t kindByte;
	uint32_t compressedSize, uncompressedSize, totalChunks;
	char shaBuf[65] = {};

	if (!bs->Read(modelId))
		return;
	if (!bs->Read(kindByte))
		return;
	if (!bs->Read(compressedSize))
		return;
	if (!bs->Read(uncompressedSize))
		return;
	if (!bs->Read(totalChunks))
		return;
	if (!bs->Read(shaBuf, CustomVeh::Protocol::SHA256_BUFFER_SIZE))
		return;

	const auto& cfg = TransferConfig::Instance();
	if (compressedSize == 0 || compressedSize > cfg.clientMaxCompressedSize || uncompressedSize == 0 || uncompressedSize > cfg.clientMaxUncompressedSize || totalChunks == 0) {
		ClientLog(LogLevel::Error, std::format("ModelTransferClient::OnTransferBegin: REJECTED invalid size modelId={}, kind={}, compSize={}, uncompSize={}, totalChunks={}", modelId, static_cast<int>(kindByte), compressedSize, uncompressedSize, totalChunks));
		// Reject: unexpected size (too large or zero)
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(Key(modelId, static_cast<ModelFileKind>(kindByte)));
		if (it != m_active.end()) {
			it->second.progress.statusText = "rejected: size";
			it->second.progress.lastError = "transfer size/chunk count out of bounds";
			// schedule an immediate retry with backoff via existing scheduling logic:
			it->second.nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(it->second.backoffMs ? it->second.backoffMs : 500);
			it->second.attempts++;
			it->second.progress.attempts = it->second.attempts;
		}
		return;
	}

	ClientLog(LogLevel::Info, std::format("ModelTransferClient::OnTransferBegin: ACCEPTED modelId={}, kind={}, compSize={}, uncompSize={}, totalChunks={}, sha='{}'", modelId, static_cast<int>(kindByte), compressedSize, uncompressedSize, totalChunks, shaBuf));

	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_active.find(Key(modelId, static_cast<ModelFileKind>(kindByte)));
	if (it == m_active.end()) {
		ClientLog(LogLevel::Warning, std::format("ModelTransferClient::OnTransferBegin: WARNING: modelId={}, kind={} not found in m_active!", modelId, static_cast<int>(kindByte)));
		return;
	}

	it->second.progress.compressedSize = compressedSize;
	it->second.progress.uncompressedSize = uncompressedSize;
	it->second.progress.totalChunks = totalChunks;
	it->second.progress.statusText = "downloading";
	it->second.compressedBuffer.resize(compressedSize);
	it->second.receivedChunkBitmap.assign(totalChunks, 0);
	it->second.expectedSha256 = std::string(shaBuf);

	// refresh start time so timeout counts from begin arrival
	it->second.progress.startTime = std::chrono::steady_clock::now();
	it->second.nextRetryTime = std::chrono::steady_clock::time_point::min();
	it->second.backoffMs = static_cast<uint32_t>(TransferConfig::Instance().retryInitialBackoffMs);
}

void ModelTransferClient::OnTransferChunk(RakNet::BitStream* bs)
{
	uint32_t modelId;
	uint8_t kindByte;
	uint32_t chunkIndex;
	uint16_t chunkLen;

	if (!bs->Read(modelId))
		return;
	if (!bs->Read(kindByte))
		return;
	if (!bs->Read(chunkIndex))
		return;
	if (!bs->Read(chunkLen))
		return;

	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_active.find(Key(modelId, static_cast<ModelFileKind>(kindByte)));
	if (it == m_active.end()) {
		bs->IgnoreBits(chunkLen * 8);
		return;
	}

	auto& entry = it->second;
	if (chunkIndex >= entry.progress.totalChunks) {
		ClientLog(LogLevel::Error, std::format("ModelTransferClient::OnTransferChunk: ERROR chunkIndex {} >= totalChunks {}", chunkIndex, entry.progress.totalChunks));
		ScheduleRetry(it, "chunk index out of bounds");
		return;
	}

	if (chunkIndex < entry.receivedChunkBitmap.size() && entry.receivedChunkBitmap[chunkIndex]) {
		bs->IgnoreBits(chunkLen * 8);
		return;
	}

	const uint32_t offset = static_cast<std::uint64_t>(chunkIndex * 4096u);
	if (offset >= entry.compressedBuffer.size()) {
		ClientLog(LogLevel::Error, std::format("ModelTransferClient::OnTransferChunk: ERROR offset {} >= buffer {}", offset, entry.compressedBuffer.size()));
		ScheduleRetry(it, "chunk offset out of bounds");
		return;
	}
	const uint32_t remaining = static_cast<uint32_t>(entry.compressedBuffer.size() - offset);
	if (chunkLen > remaining) {
		ClientLog(LogLevel::Error, std::format("ModelTransferClient::OnTransferChunk: ERROR chunkLen {} > remaining {}", chunkLen, remaining));
		ScheduleRetry(it, "chunk exceeds compressed buffer");
		return;
	}

	if (!bs->Read(reinterpret_cast<char*>(entry.compressedBuffer.data() + offset), chunkLen)) {
		ClientLog(LogLevel::Error, std::format("ModelTransferClient::OnTransferChunk: ERROR failed to read payload"));
		ScheduleRetry(it, "failed to read chunk payload");
		return;
	}

	if (chunkIndex < entry.receivedChunkBitmap.size()) {
		entry.receivedChunkBitmap[chunkIndex] = 1;
	}
	entry.progress.receivedChunks++;
	entry.progress.receivedBytes += chunkLen;

	ClientLog(LogLevel::Debug, std::format("ModelTransferClient::OnTransferChunk: modelId={}, kind={}, chunk={}/{}, len={}, bytes={}/{}", modelId, static_cast<int>(kindByte), chunkIndex + 1, entry.progress.totalChunks, chunkLen, entry.progress.receivedBytes, entry.progress.compressedSize));

	// refresh startTime so the worker's timeout is relative to last activity
	entry.progress.startTime = std::chrono::steady_clock::now();
	entry.nextRetryTime = std::chrono::steady_clock::time_point::min();
}

void ModelTransferClient::OnTransferEnd(RakNet::BitStream* bs)
{
	uint32_t modelId;
	uint8_t kindByte;
	if (!bs->Read(modelId))
		return;
	if (!bs->Read(kindByte))
		return;

	const uint64_t key = Key(modelId, static_cast<ModelFileKind>(kindByte));

	std::vector<uint8_t> compressed;
	uint32_t uncompressedSize = 0;
	std::string expectedSha;
	std::function<void(bool, const fs::path&)> onReady;

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(key);
		if (it == m_active.end())
			return;
		auto& entry = it->second;

		bool allChunksReceived = (entry.receivedChunkBitmap.size() == entry.progress.totalChunks);
		for (uint8_t bit : entry.receivedChunkBitmap) {
			if (!bit) {
				allChunksReceived = false;
				break;
			}
		}

		if (!allChunksReceived || entry.progress.receivedBytes != entry.progress.compressedSize) {
			ScheduleRetry(it, "missing chunks or byte count mismatch");
			return;
		}

		compressed = std::move(it->second.compressedBuffer);
		uncompressedSize = it->second.progress.uncompressedSize;
		expectedSha = it->second.expectedSha256;
		onReady = it->second.onReady;
		// keep the entry in map - we'll either finish it or schedule retry
	}

	// Sanity checks before decompressing
	if (uncompressedSize == 0 || uncompressedSize > TransferConfig::Instance().clientMaxUncompressedSize || compressed.empty()) {
		// schedule retry: either oversized or malformed
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(key);
		if (it != m_active.end())
			ScheduleRetry(it, "invalid size or empty compressed data");
		return;
	}

	std::vector<uint8_t> decompressed(uncompressedSize);
	uLongf destLen = decompressed.size();
	bool ok = true;
	if ((uncompressedSize == 0 && compressed.empty()) || (uncompressedSize > 0 && compressed.empty()))
		ok = false; // defensive
	else
		ok = (uncompress(decompressed.data(), &destLen, compressed.data(),
				  static_cast<uLong>(compressed.size()))
				== Z_OK
			&& destLen == uncompressedSize);

	if (ok) {
		std::string computedSha = Sha256HexOfBuffer(decompressed.data(), decompressed.size());
		ok = (_stricmp(computedSha.c_str(), expectedSha.c_str()) == 0);
	}

	if (ok) {
		// store and finish
		bool stored = ModelCache::Instance().Store(modelId, static_cast<uint8_t>(kindByte), decompressed, expectedSha);
		fs::path finalPath;
		if (stored) {
			finalPath = ModelCache::Instance().PathFor(modelId, static_cast<uint8_t>(kindByte));
			ClientLog(LogLevel::Info, std::format("ModelTransferClient::OnTransferEnd: SUCCESS modelId={}, kind={}, decompressBytes={}, stored='{}'", modelId, static_cast<int>(kindByte), decompressed.size(), finalPath.string()));
		} else {
			ClientLog(LogLevel::Error, std::format("ModelTransferClient::OnTransferEnd: ERROR failed to store cache for modelId={}, kind={}", modelId, static_cast<int>(kindByte)));
			std::lock_guard lock(m_mutex);
			auto it = m_active.find(key);
			if (it != m_active.end()) {
				ScheduleRetry(it, "failed to store cache file");
			}
			return;
		}
		MainThreadQueue::Instance().Push([this, key, ok, finalPath, onReady, modelId = static_cast<uint32_t>(key >> 8), kind = static_cast<ModelFileKind>(key & 0xFF)] {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				auto it = m_active.find(key);
				if (it != m_active.end()) {
					TransferProgress done = it->second.progress;
					done.receivedBytes = done.compressedSize;
					done.statusText = "done";
					m_completed.push_back(std::move(done));
					m_active.erase(it);
				}
			}

			RakNet::BitStream bs;
			bs.Write(static_cast<uint8_t>(PKT_EXTVEH));
			bs.Write(static_cast<uint8_t>(CustomVehAction::AssetReady));
			bs.Write(modelId);
			bs.Write(static_cast<uint8_t>(kind));
			bs.Write(static_cast<uint8_t>(ok ? 1 : 0));
			rakhook::send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0);

			if (onReady)
				onReady(ok, finalPath);
		});
		return;
	}

	// Not OK -> schedule retry if allowed
	{
		ClientLog(LogLevel::Error, std::format("ModelTransferClient::OnTransferEnd: FAILED validation (inflate or hash mismatch) modelId={}, kind={}, expectedSha='{}'", modelId, static_cast<int>(kindByte), expectedSha));
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(key);
		if (it == m_active.end())
			return;
		ScheduleRetry(it, "hash/inflate mismatch");
	}
}

void ModelTransferClient::OnTransferCancel(RakNet::BitStream* bs)
{
	uint32_t modelId;
	uint8_t kindByte;
	if (!bs->Read(modelId))
		return;
	if (!bs->Read(kindByte))
		return;

	ClientLog(LogLevel::Warning, std::format("ModelTransferClient::OnTransferCancel: server canceled transfer for modelId={}, kind={}", modelId, static_cast<int>(kindByte)));
	const uint64_t key = Key(modelId, static_cast<ModelFileKind>(kindByte));
	std::function<void(bool, const fs::path&)> onReady;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(key);
		if (it == m_active.end())
			return;

		onReady = it->second.onReady;
		FailImmediately(it, "server is failing to find file");
	}

	MainThreadQueue::Instance().Push([onReady]() {
		if (onReady)
			onReady(false, {});
	});
}

void ModelTransferClient::ScheduleRetry(std::unordered_map<uint64_t, InFlight>::iterator it, const std::string& err)
{
	const int cfgMaxAttempts = TransferConfig::Instance().retryMaxAttempts;
	// called under lock
	InFlight& entry = it->second;
	entry.attempts++;
	entry.progress.attempts = entry.attempts;
	entry.progress.lastError = err;
	entry.progress.statusText = (entry.attempts <= cfgMaxAttempts)
		? ("retrying (" + std::to_string(entry.attempts) + "/" + std::to_string(cfgMaxAttempts) + ")")
		: "failed";

	ClientLog(LogLevel::Warning, std::format("ModelTransferClient::ScheduleRetry: modelId={}, kind={}, attempt={}/{}, err='{}'", entry.progress.modelId, static_cast<int>(entry.progress.kind), entry.attempts, cfgMaxAttempts, err));

	if (entry.attempts > cfgMaxAttempts) {
		// permanent failure - call onReady(false) on main thread and erase entry
		auto onReady = entry.onReady;
		uint64_t key = it->first;
		MainThreadQueue::Instance().Push([this, key, onReady] {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				auto it2 = m_active.find(key);
				if (it2 != m_active.end()) {
					it2->second.progress.failed = true;
					// keep diagnostic text in progress for snapshot / UI
				}
			}
			if (onReady)
				onReady(false, {});
			std::lock_guard<std::mutex> lock(m_mutex);
			m_active.erase(key);
		});
		return;
	}

	uint32_t cfgInitialBackoff = static_cast<uint32_t>(TransferConfig::Instance().retryInitialBackoffMs);
	uint32_t cfgMaxBackoff = static_cast<uint32_t>(TransferConfig::Instance().retryMaxBackoffMs);
	uint32_t useBackoff = entry.backoffMs > 0 ? entry.backoffMs : cfgInitialBackoff;
	entry.nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(useBackoff);
	entry.backoffMs = std::min(static_cast<uint32_t>(entry.backoffMs ? entry.backoffMs * 2 : cfgInitialBackoff * 2), cfgMaxBackoff);

	// reset receive buffer/positions for next attempt
	entry.compressedBuffer.clear();
	entry.progress.receivedBytes = 0;
	entry.progress.receivedChunks = 0;
	entry.progress.startTime = std::chrono::steady_clock::now();
}

void ModelTransferClient::WorkerMain()
{
	while (!m_stopWorker) {
		auto now = std::chrono::steady_clock::now();
		const int cfgMaxAttempts = TransferConfig::Instance().retryMaxAttempts;
		const uint32_t cfgResponseTimeout = static_cast<uint32_t>(TransferConfig::Instance().retryResponseTimeoutMs);
		const uint32_t cfgInitialBackoff = static_cast<uint32_t>(TransferConfig::Instance().retryInitialBackoffMs);
		const uint32_t cfgMaxBackoff = static_cast<uint32_t>(TransferConfig::Instance().retryMaxBackoffMs);
		std::vector<std::pair<uint64_t, std::function<void()>>> sendTasks; // key + send lambda
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto it = m_active.begin(); it != m_active.end(); ++it) {
				InFlight& entry = it->second;
				if (entry.progress.failed)
					continue;
				if (entry.attempts > cfgMaxAttempts) {
					entry.progress.failed = true;
					entry.progress.statusText = "failed (max attempts exceeded)";
					continue;
				}
				// 1) scheduled retry time hit?
				if (entry.nextRetryTime != std::chrono::steady_clock::time_point::min()
					&& entry.nextRetryTime <= now) {
					// prepare a send task (run on main thread)
					uint32_t modelId = entry.progress.modelId;
					ModelFileKind kind = entry.progress.kind;
					sendTasks.emplace_back(it->first, [modelId, kind]() {
						RakNet::BitStream bs;
						bs.Write(static_cast<uint8_t>(PKT_EXTVEH)); // PKT_EXTVEH
						bs.Write(static_cast<uint8_t>(CustomVehAction::AssetRequest)); // ACTION_REQUEST_FILE_TRANSFER
						bs.Write(modelId);
						bs.Write(static_cast<uint8_t>(kind));
						rakhook::send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, TransferConfig::Instance().RequestChannel);
					});

					// update state
					entry.progress.statusText = "requesting";
					entry.progress.startTime = std::chrono::steady_clock::now();
					entry.nextRetryTime = std::chrono::steady_clock::time_point::min();
					continue;
				}

				// 2) no activity timeout -> trigger a retry attempt proactively
				if (!entry.progress.fromCache && (entry.progress.statusText == "requesting" || entry.progress.statusText == "downloading")) {
					auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.progress.startTime).count();
					if (static_cast<uint32_t>(elapsed) > cfgResponseTimeout) {
						// schedule retry now (no error string available because we timed out)
						sendTasks.emplace_back(it->first, [modelId = entry.progress.modelId, kind = entry.progress.kind]() {
							RakNet::BitStream bs;
							bs.Write(static_cast<uint8_t>(PKT_EXTVEH)); // PKT_EXTVEH
							bs.Write(static_cast<uint8_t>(CustomVehAction::AssetRequest)); // ACTION_REQUEST_FILE_TRANSFER
							bs.Write(modelId);
							bs.Write(static_cast<uint8_t>(kind));
							rakhook::send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, TransferConfig::Instance().RequestChannel);
						});

						// update internally to show retry scheduled
						entry.attempts++;
						entry.progress.attempts = entry.attempts;
						entry.progress.lastError = "timeout";
						entry.progress.statusText = "retrying (timeout)";
						entry.backoffMs = std::min(entry.backoffMs ? entry.backoffMs * 2 : cfgInitialBackoff, cfgMaxBackoff);
						entry.nextRetryTime = now + std::chrono::milliseconds(entry.backoffMs);
						entry.compressedBuffer.clear();
						entry.progress.receivedBytes = 0;
						entry.progress.receivedChunks = 0;
						entry.progress.startTime = std::chrono::steady_clock::now();
					}
				}
			}
		}

		// dispatch sends on main thread outside of lock
		for (auto& p : sendTasks) {
			MainThreadQueue::Instance().Push(p.second);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<uint32_t>(TransferConfig::Instance().WorkerSleepMs)));
	}
}

void ModelTransferClient::FinishTransfer(uint64_t key, bool success, const std::string& err)
{
	// not used directly in these changes but kept for API parity - finishing is done inline in handlers
	if (!success) {
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(key);
		if (it != m_active.end()) {
			ScheduleRetry(it, err);
		}
	}
}
