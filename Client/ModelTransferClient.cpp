
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

#include <windows.h>
#include <RakNet/BitStream.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

#include "defs.h"
#include "utils.h"
#include "handling_manager.hpp"
#include "../Shared/CustomVehicleProtocol.hpp"

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
	if (auto cached = ModelCache::Instance().TryGet(modelId, static_cast<uint8_t>(kind), expectedSha256Hex)) {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			TransferProgress p;
			p.modelId = modelId;
			p.kind = kind;
			p.fromCache = true;
			p.statusText = "cached";
			p.attempts = 0;
			p.lastError.clear();
			p.startTime = std::chrono::steady_clock::now();
			m_active[Key(modelId, kind)].progress = p;
		}
		onReady(true, *cached);
		return;
	}

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

	// Build RakNet packet: PKT_EXTVEH (251) + ACTION_REQUEST_FILE_TRANSFER (30) + modelId + kind
	// We enqueue the send on main thread to be safe: main-thread send avoids any RakNet thread-safety issues.
	MainThreadQueue::Instance().Push([modelId, kind]() {
		RakNet::BitStream bs;
		bs.Write(static_cast<uint8_t>(PKT_EXTVEH));
		bs.Write(static_cast<uint8_t>(CustomVehAction::AssetRequest)); // ACTION_REQUEST_FILE_TRANSFER
		bs.Write(modelId);
		bs.Write(static_cast<uint8_t>(kind));
		rakhook::send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, TransferConfig::Instance().RequestChannel);
	});
}

std::vector<TransferProgress> ModelTransferClient::Snapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<TransferProgress> result;
	result.reserve(m_active.size());
	for (auto& [key, entry] : m_active)
		result.push_back(entry.progress);
	return result;
}

void ModelTransferClient::FailImmediately(std::unordered_map<uint64_t, InFlight>::iterator it, const std::string& err)
{
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
	std::vector<std::function<void(bool, const fs::path&)>>callbacks;
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
	}

	MainThreadQueue::Instance().Push([callbacks = std::move(callbacks)]() {
		for (const auto& callback : callbacks) {
			callback(false, {});
		}
	});
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

	if (uncompressedSize == 0 || uncompressedSize > TransferConfig::Instance().clientMaxUncompressedSize) {
		// Reject: unexpected uncompressed size (too large or zero)
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(Key(modelId, static_cast<ModelFileKind>(kindByte)));
		if (it != m_active.end()) {
			it->second.progress.statusText = "rejected: size";
			it->second.progress.lastError = "uncompressed size out of bounds";
			// schedule an immediate retry with backoff via existing scheduling logic:
			it->second.nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(it->second.backoffMs ? it->second.backoffMs : 500);
			it->second.attempts++;
			it->second.progress.attempts = it->second.attempts;
		}
		return;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_active.find(Key(modelId, static_cast<ModelFileKind>(kindByte)));
	if (it == m_active.end())
		return;

	it->second.progress.compressedSize = compressedSize;
	it->second.progress.uncompressedSize = uncompressedSize;
	it->second.progress.totalChunks = totalChunks;
	it->second.progress.statusText = "downloading";
	it->second.compressedBuffer.resize(compressedSize);
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
		bs->IgnoreBits(8);
		return;
	}

	auto& entry = it->second;
    if (chunkIndex >= entry.progress.totalChunks)
    {
        ScheduleRetry(it, "chunk index out of bounds");
        return;
    }
	const uint32_t offset = static_cast<std::uint64_t>(chunkIndex * 4096u);
	if (offset >= entry.compressedBuffer.size()) {
		ScheduleRetry(it, "chunk offset out of bounds");
		return;
	}
	const uint32_t remaining = static_cast<uint32_t>(entry.compressedBuffer.size() - offset);
	if (chunkLen > remaining) {
		ScheduleRetry(it, "chunk exceeds compressed buffer");
		return;
	}

	if (!bs->Read(reinterpret_cast<char*>(entry.compressedBuffer.data() + offset), chunkLen)) {
		ScheduleRetry(it, "failed to read chunk payload");
		return;
	}

	entry.progress.receivedChunks++;
	entry.progress.receivedBytes += chunkLen;

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

		if (entry.progress.receivedBytes != entry.progress.compressedSize) {
			ScheduleRetry(it, "received byte count mismatch");
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

	if (ok)
		ok = (Sha256HexOfBuffer(decompressed.data(), decompressed.size()) == expectedSha);

	if (ok) {
		// store and finish
		bool stored = ModelCache::Instance().Store(modelId, static_cast<uint8_t>(kindByte), decompressed);
		fs::path finalPath;
		if (stored) {
			finalPath = ModelCache::Instance().PathFor(modelId, static_cast<uint8_t>(kindByte));
		}
		else {
			std::lock_guard lock(m_mutex);
			auto it = m_active.find(key);
			if (it != m_active.end()) {
				ScheduleRetry(it, "failed to store cache file");
			}
			return;
		}
		MainThreadQueue::Instance().Push([this, key, ok, finalPath, onReady, modelId = static_cast<uint32_t>(key >> 2), kind = static_cast<ModelFileKind>(key & 0x3)] {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_active.erase(key);
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

	const uint64_t key = Key(modelId, static_cast<ModelFileKind>(kindByte));
	std::function<void(bool, const fs::path&)> onReady;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_active.find(key);
		if (it == m_active.end())
			return;

		onReady = it->second.onReady;
		// server explicitly canceled - schedule retry if any attempts left
		// ScheduleRetry(it, "server canceled / file missing");
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
