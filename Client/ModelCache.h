#pragma once
#include "TransferConfig.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

	void Sweep()
	{
		auto& cfg = TransferConfig::Instance();
		if (!cfg.cacheEnabled)
			return;

		fs::path dir = GetSampCacheRoot();
		std::error_code ec;
		if (!fs::exists(dir, ec))
			return;

		struct Entry {
			fs::path path;
			fs::file_time_type writeTime;
			uintmax_t size;
		};
		std::vector<Entry> entries;

		const auto now = std::chrono::file_clock::now();
		for (auto& de : fs::directory_iterator(dir, ec)) {
			if (!de.is_regular_file(ec))
				continue;
			auto writeTime = de.last_write_time(ec);
			auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - writeTime)
							 .count();
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
	}

	fs::path PathFor(uint32_t modelId, uint8_t fileKind) const
	{
		return GetSampCacheRoot() / (std::to_string(modelId) + "_" + std::to_string(fileKind) + ".bin");
	}

	std::optional<fs::path> TryGet(uint32_t modelId, uint8_t fileKind,
		const std::string& expectedSha256Hex) const;

	bool Store(uint32_t modelId, uint8_t fileKind,
		const std::vector<uint8_t>& decompressedBytes) const
	{
		if (!TransferConfig::Instance().cacheEnabled)
			return false;
		const fs::path finalPath = PathFor(modelId, fileKind);
		const fs::path temporaryPath = finalPath.string() + ".tmp";
		std::ofstream file(temporaryPath,
			std::ios::binary | std::ios::trunc);
		if (!file.is_open())
			return false;
		file.write(reinterpret_cast<const char*>(decompressedBytes.data()),
			decompressedBytes.size());
		file.close();
		if (!file)
			return false;

		std::error_code ec;
		fs::remove(finalPath, ec);
		fs::rename(temporaryPath, finalPath, ec);
		if (ec) {
			fs::remove(temporaryPath, ec);
			return false;
		}
		return true;
	}

private:
	ModelCache() = default;
};
