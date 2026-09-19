#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <unordered_map>

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
	uint32_t clientMaxCompressedSize = 160u * 1024u * 1024u; // 160MB

	static TransferConfig& Instance()
	{
		static TransferConfig instance;
		return instance;
	}

	void Load()
	{
		fs::path path = ConfigPath();
		if (!fs::exists(path)) {
			WriteDefaults(path);
		}

		std::ifstream file(path);
		if (!file.is_open())
			return;

		std::string line;
		while (std::getline(file, line)) {
			if (auto semi = line.find(';'); semi != std::string::npos)
				line.resize(semi);
			if (auto hash = line.find('#'); hash != std::string::npos)
				line.resize(hash);

			auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;

			std::string key = Trim(line.substr(0, eq));
			std::string value = Trim(line.substr(eq + 1));
			if (!key.empty())
				m_values[key] = value;
		}

		expireTimeMs = GetInt("ExpireTimeMs", expireTimeMs);
		maxCacheSizeMB = GetInt("MaxCacheSizeMB", maxCacheSizeMB);
		cacheEnabled = GetBool("CacheEnabled", cacheEnabled);
		maxConcurrentTransfers = static_cast<int>(GetInt("MaxConcurrentTransfers", maxConcurrentTransfers));
		showTransferWindow = GetBool("ShowTransferWindowByDefault", showTransferWindow);
		toggleKey = static_cast<int>(GetInt("ToggleKeyVK", toggleKey));
		retryMaxAttempts = static_cast<int>(GetInt("RetryMaxAttempts", retryMaxAttempts));
		retryInitialBackoffMs = static_cast<int>(GetInt("RetryInitialBackoffMs", retryInitialBackoffMs));
		retryMaxBackoffMs = static_cast<int>(GetInt("RetryMaxBackoffMs", retryMaxBackoffMs));
		retryResponseTimeoutMs = static_cast<int>(GetInt("RetryResponseTimeoutMs", retryResponseTimeoutMs));
		clientMaxUncompressedSize = static_cast<uint32_t>(GetInt("ClientMaxUncompressedSize", clientMaxUncompressedSize));
		clientMaxCompressedSize = static_cast<uint32_t>(GetInt("ClientMaxCompressedSize", clientMaxCompressedSize));
		RequestChannel = static_cast<uint32_t>(GetInt("RequestChannel", RequestChannel));
		WorkerSleepMs = static_cast<uint32_t>(GetInt("WorkerSleepMs", WorkerSleepMs));
	}

private:
	TransferConfig() = default;

	static fs::path ConfigPath()
	{
		return GetSampCacheRoot() / "transfer_config.ini";
	}

	void WriteDefaults(const fs::path& path)
	{
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
			 << "ClientMaxCompressedSize=167772160\n" // 160 * 1024 * 1024
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

	long long GetInt(const std::string& key, long long fallback) const
	{
		auto it = m_values.find(key);
		if (it == m_values.end())
			return fallback;
		try {
			return std::stoll(it->second);
		} catch (...) {
			return fallback;
		}
	}

	bool GetBool(const std::string& key, bool fallback) const
	{
		auto it = m_values.find(key);
		if (it == m_values.end())
			return fallback;
		return it->second == "1" || it->second == "true" || it->second == "True";
	}

	std::unordered_map<std::string, std::string> m_values;
};
