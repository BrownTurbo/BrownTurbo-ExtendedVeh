#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

class CryptoUtility
{
public:
    static std::string BytesToHex(const std::uint8_t* buffer,std::size_t length);
    static bool ComputeSHA256(const std::uint8_t* data, std::size_t length, std::string& outHashStr);
    static bool ComputeFileSHA256(const std::filesystem::path& filePath, std::string& outHashStr);
};
