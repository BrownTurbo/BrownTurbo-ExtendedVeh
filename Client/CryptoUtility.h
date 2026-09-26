#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

class CryptoUtility {
public:
	static bool ComputeSHA256(const std::uint8_t* data, std::size_t length, std::string& outHashStr);
	static bool ComputeFileSHA256(const std::filesystem::path& filePath, std::string& outHashStr);
	static bool ComputeMD5(const std::uint8_t* data, std::size_t length, std::string& outHashStr);
	static bool ComputeMD5(const std::string& input, std::string& outHashStr);
};
