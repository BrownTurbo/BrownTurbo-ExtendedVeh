#pragma once

#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../Shared/CustomVehicleProtocol.hpp"

namespace fs = std::filesystem;

std::string Sha256Hex(const uint8_t* data, size_t length);
bool IsPathInsideBase(const std::filesystem::path& baseDir, const std::filesystem::path& candidate);
bool ComputeFileSha256(const std::string& relativePath, std::string& outHex);
fs::path GetAssetPath(std::uint32_t customModelId, CustomVeh::Protocol::AssetType type);
