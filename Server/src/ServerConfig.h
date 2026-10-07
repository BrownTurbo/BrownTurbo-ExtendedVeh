#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ServerConfig
{
	uint8_t fileTransferChannel = 0;
	uint32_t chunksPerPlayerPerTick = 4;
	uint32_t maxActiveTransfersPerPlayer = 10;
	uint32_t maxModelFileSizeBytes = 128u * 1024u * 1024u;
	std::string modelsDirectory = "models";

	static ServerConfig& Instance();
	bool LoadOrCreate(const std::filesystem::path& path, std::string& error);
};
