#include "ServerConfig.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

ServerConfig& ServerConfig::Instance()
{
	static ServerConfig config;
	return config;
}

static nlohmann::json ConfigToJson(const ServerConfig& cfg)
{
	return {
		{"FileTransferChannel", cfg.fileTransferChannel},
		{"ChunksPerPlayerPerTick", cfg.chunksPerPlayerPerTick},
		{"MaxActiveTransfersPerPlayer", cfg.maxActiveTransfersPerPlayer},
		{"MaxModelFileSizeBytes", cfg.maxModelFileSizeBytes},
		{"ModelsDirectory", cfg.modelsDirectory}
	};
}

bool ServerConfig::LoadOrCreate(const std::filesystem::path& path, std::string& error)
{
	*this = ServerConfig {};
	error.clear();

	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
	{
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), ec);

		std::ofstream output(path, std::ios::binary);
		if (!output)
		{
			error = "could not create config file";
			return false;
		}
		output << ConfigToJson(*this).dump(4) << '\n';
		if (!output)
		{
			error = "could not write default config";
			return false;
		}
		return true;
	}

	try
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			error = "could not open config file";
			return false;
		}

		const nlohmann::json json = nlohmann::json::parse(input);
		if (!json.is_object())
		{
			error = "root must be a JSON object";
			return false;
		}

		auto readUnsigned = [&](const char* key, uint32_t minValue, uint32_t maxValue, uint32_t defaultValue)
		{
			if (!json.contains(key))
				return defaultValue;

			const auto& value = json.at(key);
			if (!value.is_number_integer())
				return defaultValue;

			try
			{
				if (value.get<int64_t>() < 0)
					return defaultValue;

				const uint64_t parsed = value.get<uint64_t>();
				return std::clamp(static_cast<uint32_t>(parsed), minValue, maxValue);
			}
			catch (const std::exception&)
			{
				return defaultValue;
			}
		};

		fileTransferChannel = static_cast<uint8_t>(readUnsigned("FileTransferChannel", 0, 31, 0));
		chunksPerPlayerPerTick = readUnsigned("ChunksPerPlayerPerTick", 1, 64, 4);
		maxActiveTransfersPerPlayer = readUnsigned("MaxActiveTransfersPerPlayer", 1, 1000, 10);
		maxModelFileSizeBytes = readUnsigned("MaxModelFileSizeBytes", 1, 512u * 1024u * 1024u, 128u * 1024u * 1024u);

		if (json.contains("ModelsDirectory") && json.at("ModelsDirectory").is_string())
		{
			const std::string configuredDirectory = json.at("ModelsDirectory").get<std::string>();
			if (!configuredDirectory.empty())
				modelsDirectory = configuredDirectory;
		}
	}
	catch (const nlohmann::json::exception& exception)
	{
		error = exception.what();
		*this = ServerConfig {};
		return false;
	}
	catch (const std::exception& exception)
	{
		error = exception.what();
		*this = ServerConfig {};
		return false;
	}
	catch (...)
	{
		error = "unknown error";
		*this = ServerConfig {};
		return false;
	}

	return true;
}
