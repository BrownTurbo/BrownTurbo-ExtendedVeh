#pragma once
#include "TransferConfig.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "utils.h"

namespace fs = std::filesystem;

class ModelCache {
public:
	static ModelCache& Instance()
	{
		static ModelCache instance;
		return instance;
	}

	// Evicts expired and oversized cached assets for the current server
	void Sweep();

	// Server cache directory: SAMP/cache/<server_md5>/
	fs::path GetServerCacheRoot() const;

	// Path for a specific model asset inside the server cache directory: SAMP/cache/<server_md5>/<modelId>/<name>.<ext>
	fs::path PathFor(uint32_t modelId, uint8_t fileKind) const;

	// Instantaneous cache lookup via cache.json
	std::optional<fs::path> TryGet(uint32_t modelId, uint8_t fileKind,
		const std::string& expectedSha256Hex);

	// Stores decompressed asset into SAMP/cache/<server_md5>/<modelId>/<name>.<ext> and updates cache.json
	bool Store(uint32_t modelId, uint8_t fileKind,
		const std::vector<uint8_t>& decompressedBytes,
		const std::string& sha256Hex = "");

	// Flush cache manifest to disk
	void SaveManifest();

	// Force reload manifest for current server
	void ReloadManifest();

	// Explicitly invalidate and delete a corrupted/failed cached asset
	void Invalidate(uint32_t modelId, uint8_t fileKind);

private:
	ModelCache();
	~ModelCache();

	ModelCache(const ModelCache&) = delete;
	ModelCache& operator=(const ModelCache&) = delete;

	void EnsureServerInitialized();
	void UpdateServersMasterIndex(const std::string& serverAddr, const std::string& serverHash);
	static std::string GetKindKey(uint8_t fileKind);
	static std::string GetAssetFileName(uint8_t fileKind, bool isWav = false);

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
