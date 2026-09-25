#pragma once
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "utils.h"

namespace fs = std::filesystem;

class TransferConfig {
public:
	long long expireTimeMs = 24LL * 60 * 60 * 1000; // 24h
	int maxCacheSizeMB = 512;
	bool cacheEnabled = true;
	int maxConcurrentTransfers = 4;
	bool showTransferWindow = false;
	int toggleKey = 0x78; // VK_F9

	int retryMaxAttempts = 5;
	int retryInitialBackoffMs = 500;
	int retryMaxBackoffMs = 60000;
	int retryResponseTimeoutMs = 8000;

	int RequestChannel = 1;
	int WorkerSleepMs = 250;

	uint32_t clientMaxUncompressedSize = 200u * 1024u * 1024u; // 200 MB
	uint32_t clientMaxCompressedSize = 160u * 1024u * 1024u;   // 160 MB

	static TransferConfig& Instance()
	{
		static TransferConfig instance;
		return instance;
	}

	void Load()
	{
		try {
			fs::path path = ResolveConfigPath();
			std::error_code ec;
			if (!fs::exists(path, ec)) {
				WriteDefaults(path);
			}

			std::ifstream file(path);
			if (!file.is_open()) {
				ClientLog(LogLevel::Warning, std::format("[TransferConfig] Unable to open '{}' for reading. Using default settings.", path.string()));
				return;
			}

			m_entries.clear();
			std::string rawLine;
			int lineNum = 0;

			while (std::getline(file, rawLine)) {
				++lineNum;
				std::string line = rawLine;

				// Strip comments
				if (auto semi = line.find(';'); semi != std::string::npos)
					line.resize(semi);
				if (auto hash = line.find('#'); hash != std::string::npos)
					line.resize(hash);
				if (auto slash = line.find("//"); slash != std::string::npos)
					line.resize(slash);

				auto eq = line.find('=');
				if (eq == std::string::npos)
					eq = line.find(':');
				if (eq == std::string::npos)
					continue;

				std::string key = Trim(line.substr(0, eq));
				std::string value = Trim(line.substr(eq + 1));
				if (!key.empty()) {
					std::string lowerKey = ToLower(key);
					m_entries[lowerKey] = { key, value, lineNum };
				}
			}

			std::string filename = path.filename().string();

			expireTimeMs = GetRangedInt64("ExpireTimeMs", expireTimeMs, 60000LL, 365LL * 24 * 60 * 60 * 1000, filename);
			maxCacheSizeMB = static_cast<int>(GetRangedInt64("MaxCacheSizeMB", maxCacheSizeMB, 16, 1048576, filename));
			cacheEnabled = GetValidatedBool("CacheEnabled", cacheEnabled, filename);
			maxConcurrentTransfers = static_cast<int>(GetRangedInt64("MaxConcurrentTransfers", maxConcurrentTransfers, 1, 32, filename));
			showTransferWindow = GetValidatedBool("ShowTransferWindowByDefault", showTransferWindow, filename);
			toggleKey = static_cast<int>(GetRangedInt64("ToggleKeyVK", toggleKey, 0x01, 0xFE, filename));
			retryMaxAttempts = static_cast<int>(GetRangedInt64("RetryMaxAttempts", retryMaxAttempts, 1, 50, filename));
			retryInitialBackoffMs = static_cast<int>(GetRangedInt64("RetryInitialBackoffMs", retryInitialBackoffMs, 50, 60000, filename));
			retryMaxBackoffMs = static_cast<int>(GetRangedInt64("RetryMaxBackoffMs", retryMaxBackoffMs, retryInitialBackoffMs, 600000, filename));
			retryResponseTimeoutMs = static_cast<int>(GetRangedInt64("RetryResponseTimeoutMs", retryResponseTimeoutMs, 500, 120000, filename));
			clientMaxUncompressedSize = static_cast<uint32_t>(GetRangedInt64("ClientMaxUncompressedSize", clientMaxUncompressedSize, 1048576LL, 1073741824LL, filename));
			clientMaxCompressedSize = static_cast<uint32_t>(GetRangedInt64("ClientMaxCompressedSize", clientMaxCompressedSize, 1048576LL, 1073741824LL, filename));
			RequestChannel = static_cast<int>(GetRangedInt64("RequestChannel", RequestChannel, 0, 31, filename));
			WorkerSleepMs = static_cast<int>(GetRangedInt64("WorkerSleepMs", WorkerSleepMs, 10, 5000, filename));

			ClientLog(LogLevel::Info, std::format("[TransferConfig] Loaded configuration from '{}' (CacheEnabled={}, MaxCacheMB={}, MaxConcurrent={}, Workers={}ms)",
				path.string(), cacheEnabled, maxCacheSizeMB, maxConcurrentTransfers, WorkerSleepMs));
		}
		catch (const std::exception& ex) {
			ClientLog(LogLevel::Error, std::format("[TransferConfig] Exception while loading configuration: {}", ex.what()));
		}
		catch (...) {
			ClientLog(LogLevel::Error, "[TransferConfig] Unknown exception while loading configuration.");
		}
	}

private:
	TransferConfig() = default;

	struct ConfigEntry {
		std::string origKey;
		std::string value;
		int lineNum = 0;
	};

	static fs::path ResolveConfigPath()
	{
		fs::path cacheRoot = GetSampCacheRoot();
		std::vector<fs::path> candidates = {
			cacheRoot / "transferconfig.ini",
			cacheRoot / "transfer_config.ini",
			fs::current_path() / "transferconfig.ini",
			fs::current_path() / "transfer_config.ini"
		};
		for (const auto& p : candidates) {
			std::error_code ec;
			if (fs::exists(p, ec) && fs::is_regular_file(p, ec))
				return p;
		}
		return cacheRoot / "transferconfig.ini";
	}

	void WriteDefaults(const fs::path& path)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		std::ofstream file(path);
		if (!file.is_open())
			return;
		file << "; model transfer cache settings\n"
			 << "ExpireTimeMs=86400000\n"
			 << "MaxCacheSizeMB=512\n"
			 << "CacheEnabled=1\n"
			 << "MaxConcurrentTransfers=4\n"
			 << "ShowTransferWindowByDefault=0\n"
			 << "ToggleKeyVK=120\n"
			 << "RetryMaxAttempts=5\n"
			 << "RetryInitialBackoffMs=500\n"
			 << "RetryMaxBackoffMs=60000\n"
			 << "RetryResponseTimeoutMs=8000\n"
			 << "ClientMaxUncompressedSize=209715200\n" // 200 * 1024 * 1024
			 << "ClientMaxCompressedSize=167772160\n"   // 160 * 1024 * 1024
			 << "WorkerSleepMs=250\n"
			 << "RequestChannel=1\n";
	}

	static std::string Trim(std::string s)
	{
		size_t start = s.find_first_not_of(" \t\r\n");
		size_t end = s.find_last_not_of(" \t\r\n");
		if (start == std::string::npos)
			return "";
		return s.substr(start, end - start + 1);
	}

	static std::string ToLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return s;
	}

	long long GetRangedInt64(const std::string& key, long long fallback, long long minVal, long long maxVal, const std::string& filename) const
	{
		auto it = m_entries.find(ToLower(key));
		if (it == m_entries.end())
			return fallback;

		const auto& entry = it->second;
		try {
			size_t processed = 0;
			long long val = std::stoll(entry.value, &processed, 0);
			if (processed != entry.value.size() || val < minVal || val > maxVal) {
				ClientLog(LogLevel::Warning, std::format("[TransferConfig] {}:{} Invalid value '{}' for '{}' (valid range {}..{}). Using default {}.",
					filename, entry.lineNum, entry.value, key, minVal, maxVal, fallback));
				return fallback;
			}
			return val;
		} catch (...) {
			ClientLog(LogLevel::Warning, std::format("[TransferConfig] {}:{} Non-numeric value '{}' for '{}'. Using default {}.",
				filename, entry.lineNum, entry.value, key, fallback));
			return fallback;
		}
	}

	bool GetValidatedBool(const std::string& key, bool fallback, const std::string& filename) const
	{
		auto it = m_entries.find(ToLower(key));
		if (it == m_entries.end())
			return fallback;

		const auto& entry = it->second;
		std::string valLower = ToLower(entry.value);
		if (valLower == "1" || valLower == "true" || valLower == "yes" || valLower == "on")
			return true;
		if (valLower == "0" || valLower == "false" || valLower == "no" || valLower == "off")
			return false;

		ClientLog(LogLevel::Warning, std::format("[TransferConfig] {}:{} Invalid boolean value '{}' for '{}' (expected 1/0 or true/false). Using default {}.",
			filename, entry.lineNum, entry.value, key, fallback ? "true" : "false"));
		return fallback;
	}

	std::unordered_map<std::string, ConfigEntry> m_entries;
};
