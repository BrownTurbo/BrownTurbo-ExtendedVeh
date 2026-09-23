#include "ModelCache.h"
#include "CryptoUtility.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <unordered_map>

struct AssetRecord {
	std::string sha256;
	std::string relPath;
	uintmax_t size = 0;
	int64_t timestamp = 0;
};

struct ModelCache::Impl {
	std::mutex mutex;
	std::string currentServerHash;
	std::string currentServerAddress;
	bool manifestLoaded = false;
	// (modelId << 8) | fileKind -> AssetRecord
	std::unordered_map<uint64_t, AssetRecord> assets;
	nlohmann::json manifestJson;
};

ModelCache::ModelCache()
	: m_impl(std::make_unique<Impl>())
{
}

ModelCache::~ModelCache()
{
	SaveManifest();
}

std::string ModelCache::GetKindKey(uint8_t fileKind)
{
	switch (fileKind) {
	case 0: return "dff";
	case 1: return "txd";
	case 2: return "col";
	case 3: return "engine";
	case 4: return "acceleration";
	case 5: return "deacceleration";
	case 6: return "brake";
	case 7: return "crash";
	default: return "kind_" + std::to_string(fileKind);
	}
}

std::string ModelCache::GetAssetFileName(uint8_t fileKind, bool isWav)
{
	switch (fileKind) {
	case 0: return "model.dff";
	case 1: return "model.txd";
	case 2: return "model.col";
	case 3: return isWav ? "engine.wav" : "engine.ogg";
	case 4: return isWav ? "acceleration.wav" : "acceleration.ogg";
	case 5: return isWav ? "deacceleration.wav" : "deacceleration.ogg";
	case 6: return isWav ? "brake.wav" : "brake.ogg";
	case 7: return isWav ? "crash.wav" : "crash.ogg";
	default: return "asset_" + std::to_string(fileKind) + ".bin";
	}
}

fs::path ModelCache::GetServerCacheRoot() const
{
	std::string hash = GetCurrentServerHash();
	fs::path root = GetSampCacheRoot() / hash;
	std::error_code ec;
	fs::create_directories(root, ec);
	return root;
}

void ModelCache::UpdateServersMasterIndex(const std::string& serverAddr, const std::string& serverHash)
{
	fs::path masterFile = GetSampCacheRoot() / "servers-cache.json";
	nlohmann::json root;
	std::error_code ec;

	if (fs::exists(masterFile, ec)) {
		std::ifstream in(masterFile);
		if (in.is_open()) {
			try {
				in >> root;
			} catch (...) {
				root = nlohmann::json::object();
			}
		}
	}

	if (!root.contains("servers") || !root["servers"].is_object()) {
		root["servers"] = nlohmann::json::object();
	}

	root["servers"][serverAddr] = {
		{ "hash", serverHash },
		{ "lastConnected", std::time(nullptr) }
	};

	fs::path tempFile = masterFile.string() + ".tmp";
	std::ofstream out(tempFile, std::ios::trunc);
	if (out.is_open()) {
		out << root.dump(2);
		out.close();
		fs::remove(masterFile, ec);
		fs::rename(tempFile, masterFile, ec);
	}
}

void ModelCache::EnsureServerInitialized()
{
	std::string currentAddr = GetCurrentServerAddress();
	std::string currentHash = GetCurrentServerHash();

	if (m_impl->currentServerHash != currentHash || !m_impl->manifestLoaded) {
		m_impl->currentServerAddress = currentAddr;
		m_impl->currentServerHash = currentHash;
		m_impl->assets.clear();

		fs::path serverDir = GetServerCacheRoot();
		std::error_code ec;
		fs::create_directories(serverDir, ec);

		UpdateServersMasterIndex(currentAddr, currentHash);

		// Load server-specific cache.json
		fs::path manifestFile = serverDir / "cache.json";
		if (fs::exists(manifestFile, ec)) {
			std::ifstream in(manifestFile);
			if (in.is_open()) {
				try {
					nlohmann::json j;
					in >> j;
					m_impl->manifestJson = j;
					if (j.contains("models") && j["models"].is_object()) {
						for (auto& [modelStr, kindsObj] : j["models"].items()) {
							if (!kindsObj.is_object()) continue;
							uint32_t modelId = 0;
							try {
								modelId = static_cast<uint32_t>(std::stoul(modelStr));
							} catch (...) {
								continue;
							}
							for (auto& [kindStr, infoObj] : kindsObj.items()) {
								if (!infoObj.is_object()) continue;
								uint8_t kind = 0xFF;
								if (kindStr == "dff") kind = 0;
								else if (kindStr == "txd") kind = 1;
								else if (kindStr == "col") kind = 2;
								else if (kindStr == "engine") kind = 3;
								else if (kindStr == "acceleration") kind = 4;
								else if (kindStr == "deacceleration") kind = 5;
								else if (kindStr == "brake") kind = 6;
								else if (kindStr == "crash") kind = 7;
								else continue;

								AssetRecord rec;
								if (infoObj.contains("sha256") && infoObj["sha256"].is_string())
									rec.sha256 = infoObj["sha256"].get<std::string>();
								if (infoObj.contains("file") && infoObj["file"].is_string())
									rec.relPath = infoObj["file"].get<std::string>();
								if (infoObj.contains("size") && infoObj["size"].is_number())
									rec.size = infoObj["size"].get<uintmax_t>();
								if (infoObj.contains("timestamp") && infoObj["timestamp"].is_number())
									rec.timestamp = infoObj["timestamp"].get<int64_t>();

								uint64_t key = (static_cast<uint64_t>(modelId) << 8) | static_cast<uint64_t>(kind);
								m_impl->assets[key] = std::move(rec);
							}
						}
					}
					ClientLog(LogLevel::Info, std::format("ModelCache: Loaded manifest for server {} ({}) with {} cached assets",
						currentAddr, currentHash, m_impl->assets.size()));
				} catch (...) {
					m_impl->manifestJson = nlohmann::json::object();
				}
			}
		}
		m_impl->manifestLoaded = true;
	}
}

void ModelCache::ReloadManifest()
{
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	m_impl->manifestLoaded = false;
	EnsureServerInitialized();
}

void ModelCache::SaveManifest()
{
	if (m_impl->currentServerHash.empty())
		return;

	fs::path serverDir = GetServerCacheRoot();
	std::error_code ec;
	fs::create_directories(serverDir, ec);

	nlohmann::json root;
	root["server"] = m_impl->currentServerAddress;
	root["serverHash"] = m_impl->currentServerHash;
	root["lastUpdated"] = std::time(nullptr);
	root["models"] = nlohmann::json::object();

	for (const auto& [key, rec] : m_impl->assets) {
		uint32_t modelId = static_cast<uint32_t>(key >> 8);
		uint8_t kind = static_cast<uint8_t>(key & 0xFF);
		std::string modelStr = std::to_string(modelId);
		std::string kindStr = GetKindKey(kind);

		if (!root["models"].contains(modelStr)) {
			root["models"][modelStr] = nlohmann::json::object();
		}

		root["models"][modelStr][kindStr] = {
			{ "sha256", rec.sha256 },
			{ "file", rec.relPath },
			{ "size", rec.size },
			{ "timestamp", rec.timestamp }
		};
	}

	fs::path manifestFile = serverDir / "cache.json";
	fs::path tempFile = manifestFile.string() + ".tmp";
	std::ofstream out(tempFile, std::ios::trunc);
	if (out.is_open()) {
		out << root.dump(2);
		out.close();
		fs::remove(manifestFile, ec);
		fs::rename(tempFile, manifestFile, ec);
	}
}

fs::path ModelCache::PathFor(uint32_t modelId, uint8_t fileKind) const
{
	std::lock_guard<std::mutex> lock(const_cast<ModelCache*>(this)->m_impl->mutex);
	const_cast<ModelCache*>(this)->EnsureServerInitialized();

	uint64_t key = (static_cast<uint64_t>(modelId) << 8) | static_cast<uint64_t>(fileKind);
	auto it = m_impl->assets.find(key);
	if (it != m_impl->assets.end()) {
		return GetServerCacheRoot() / it->second.relPath;
	}

	// Check if a .wav file already exists on disk
	fs::path wavPath = GetServerCacheRoot() / std::to_string(modelId) / GetAssetFileName(fileKind, true);
	std::error_code ec;
	if (fs::exists(wavPath, ec)) {
		return wavPath;
	}

	return GetServerCacheRoot() / std::to_string(modelId) / GetAssetFileName(fileKind, false);
}

std::optional<fs::path> ModelCache::TryGet(uint32_t modelId, uint8_t fileKind,
	const std::string& expectedSha256Hex)
{
	if (!TransferConfig::Instance().cacheEnabled)
		return std::nullopt;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	EnsureServerInitialized();

	uint64_t key = (static_cast<uint64_t>(modelId) << 8) | static_cast<uint64_t>(fileKind);
	auto it = m_impl->assets.find(key);
	if (it != m_impl->assets.end()) {
		if (!expectedSha256Hex.empty() && _stricmp(it->second.sha256.c_str(), expectedSha256Hex.c_str()) != 0) {
			// Hash mismatch: server asset updated or hash changed! Delete stale asset and manifest entry
			fs::path stale = GetServerCacheRoot() / it->second.relPath;
			std::error_code ec;
			fs::remove(stale, ec);
			m_impl->assets.erase(it);
			SaveManifest();
			return std::nullopt;
		}

		fs::path fullPath = GetServerCacheRoot() / it->second.relPath;
		std::error_code ec;
		if (fs::exists(fullPath, ec) && fs::file_size(fullPath, ec) > 0) {
			return fullPath;
		} else {
			// Asset is missing or 0 bytes: purge stale entry to force re-transfer
			if (fs::exists(fullPath, ec)) {
				fs::remove(fullPath, ec);
			}
			m_impl->assets.erase(it);
			SaveManifest();
			return std::nullopt;
		}
	}

	// Fallback check on disk (in case file exists on disk e.g. from manual installation or previous session)
	fs::path checkPath = GetServerCacheRoot() / std::to_string(modelId) / GetAssetFileName(fileKind, false);
	std::error_code ec;
	if (!fs::exists(checkPath, ec) && (fileKind >= 3 && fileKind <= 7)) {
		// check wav
		fs::path wavPath = GetServerCacheRoot() / std::to_string(modelId) / GetAssetFileName(fileKind, true);
		if (fs::exists(wavPath, ec)) {
			checkPath = wavPath;
		}
	}

	if (fs::exists(checkPath, ec)) {
		if (fs::file_size(checkPath, ec) > 0 && !expectedSha256Hex.empty()) {
			std::string calculatedSha;
			if (CryptoUtility::ComputeFileSHA256(checkPath, calculatedSha)) {
				if (_stricmp(calculatedSha.c_str(), expectedSha256Hex.c_str()) == 0) {
					fs::path rel = fs::relative(checkPath, GetServerCacheRoot(), ec);
					AssetRecord rec;
					rec.sha256 = calculatedSha;
					rec.relPath = rel.string();
					rec.size = fs::file_size(checkPath, ec);
					rec.timestamp = std::time(nullptr);
					m_impl->assets[key] = rec;
					SaveManifest();
					return checkPath;
				}
			}
		}
		// File on disk is corrupted, 0 bytes, or hash doesn't match: delete it to force re-transfer
		fs::remove(checkPath, ec);
	}

	return std::nullopt;
}

void ModelCache::Invalidate(uint32_t modelId, uint8_t fileKind)
{
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	EnsureServerInitialized();

	uint64_t key = (static_cast<uint64_t>(modelId) << 8) | static_cast<uint64_t>(fileKind);
	auto it = m_impl->assets.find(key);
	std::error_code ec;
	if (it != m_impl->assets.end()) {
		fs::path p = GetServerCacheRoot() / it->second.relPath;
		fs::remove(p, ec);
		m_impl->assets.erase(it);
		SaveManifest();
	}

	fs::path standard = GetServerCacheRoot() / std::to_string(modelId) / GetAssetFileName(fileKind, false);
	fs::remove(standard, ec);
	if (fileKind >= 3 && fileKind <= 7) {
		fs::path wav = GetServerCacheRoot() / std::to_string(modelId) / GetAssetFileName(fileKind, true);
		fs::remove(wav, ec);
	}
}

bool ModelCache::Store(uint32_t modelId, uint8_t fileKind,
	const std::vector<uint8_t>& decompressedBytes,
	const std::string& sha256Hex)
{
	if (!TransferConfig::Instance().cacheEnabled || decompressedBytes.empty())
		return false;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	EnsureServerInitialized();

	bool isWav = false;
	if (fileKind >= 3 && fileKind <= 7 && decompressedBytes.size() >= 4) {
		if (decompressedBytes[0] == 'R' && decompressedBytes[1] == 'I' &&
			decompressedBytes[2] == 'F' && decompressedBytes[3] == 'F') {
			isWav = true;
		}
	}

	std::string fileName = GetAssetFileName(fileKind, isWav);
	fs::path relPath = fs::path(std::to_string(modelId)) / fileName;
	fs::path finalPath = GetServerCacheRoot() / relPath;

	std::error_code ec;
	fs::create_directories(finalPath.parent_path(), ec);

	fs::path tempPath = finalPath.string() + ".tmp";
	{
		std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
			return false;
		file.write(reinterpret_cast<const char*>(decompressedBytes.data()), decompressedBytes.size());
		file.close();
		if (!file) {
			fs::remove(tempPath, ec);
			return false;
		}
	}

	fs::remove(finalPath, ec);
	fs::rename(tempPath, finalPath, ec);
	if (ec) {
		fs::remove(tempPath, ec);
		return false;
	}

	std::string finalSha = sha256Hex;
	if (finalSha.empty()) {
		finalSha = Sha256HexOfBuffer(decompressedBytes.data(), static_cast<unsigned int>(decompressedBytes.size()));
	}

	uint64_t key = (static_cast<uint64_t>(modelId) << 8) | static_cast<uint64_t>(fileKind);
	AssetRecord rec;
	rec.sha256 = finalSha;
	rec.relPath = relPath.string();
	rec.size = decompressedBytes.size();
	rec.timestamp = std::time(nullptr);
	m_impl->assets[key] = rec;

	SaveManifest();
	return true;
}

void ModelCache::Sweep()
{
	auto& cfg = TransferConfig::Instance();
	if (!cfg.cacheEnabled)
		return;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	EnsureServerInitialized();

	fs::path dir = GetServerCacheRoot();
	std::error_code ec;
	if (!fs::exists(dir, ec))
		return;

	struct Entry {
		fs::path path;
		fs::file_time_type writeTime;
		uintmax_t size = 0;
	};
	std::vector<Entry> entries;

	const auto now = std::chrono::file_clock::now();
	for (auto& de : fs::recursive_directory_iterator(dir, ec)) {
		if (!de.is_regular_file(ec))
			continue;
		if (de.path().filename() == "cache.json")
			continue;

		auto writeTime = de.last_write_time(ec);
		auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - writeTime).count();
		if (ageMs > cfg.expireTimeMs) {
			fs::remove(de.path(), ec);
			continue;
		}
		entries.push_back({ de.path(), writeTime, de.file_size(ec) });
	}

	uintmax_t totalBytes = 0;
	for (auto& e : entries)
		totalBytes += e.size;

	const uintmax_t maxBytes = static_cast<uintmax_t>(cfg.maxCacheSizeMB) * 1024ull * 1024ull;
	if (totalBytes <= maxBytes)
		return;

	std::sort(entries.begin(), entries.end(),
		[](const Entry& a, const Entry& b) {
			return a.writeTime < b.writeTime;
		});

	for (auto& e : entries) {
		if (totalBytes <= maxBytes)
			break;
		fs::remove(e.path, ec);
		totalBytes -= e.size;
	}

	SaveManifest();
}
