#include "utils.h"
#include "defs.h"
#include "HandlingManager.h"

#if __has_include(<hash-library/sha256.h>)
#include <hash-library/sha256.h>
#else
#include <sha256.h>
#endif

std::string Sha256Hex(const uint8_t* data, size_t length)
{
	if (!data && length > 0)
		return {};

	SHA256 sha256;
	return sha256(data, length);
}

bool IsPathInsideBase(const std::filesystem::path& baseDir, const std::filesystem::path& candidate)
{
	std::error_code ec;
	auto baseCan = std::filesystem::weakly_canonical(baseDir, ec);
	if (ec)
		return false;
	auto candCan = std::filesystem::weakly_canonical(candidate, ec);
	if (ec)
		return false;

	// Make both paths absolute and compare prefix
	auto baseStr = baseCan.native();
	auto candStr = candCan.native();
#ifdef _WIN32
	// Case-insensitive on Windows
	std::transform(baseStr.begin(), baseStr.end(), baseStr.begin(), ::tolower);
	std::transform(candStr.begin(), candStr.end(), candStr.begin(), ::tolower);
#endif
	if (candStr.size() < baseStr.size())
		return false;
	// require baseStr to be a prefix and either equal or followed by path separator
	if (candStr.compare(0, baseStr.size(), baseStr) != 0)
		return false;
	if (candStr.size() == baseStr.size())
		return true;
	char sep = std::filesystem::path::preferred_separator;
	return candStr[baseStr.size()] == sep;
}

bool ComputeFileSha256(const std::string& relativePath, std::string& outHex)
{
	try
	{
		outHex.clear();

		fs::path candidate = fs::path(relativePath);
		if (!candidate.is_absolute() && !IsPathInsideBase(g_modelsDir, candidate))
		{
			candidate = fs::path(g_modelsDir) / candidate;
		}
		if (!IsPathInsideBase(g_modelsDir, candidate))
			return false;
		std::ifstream file(candidate, std::ios::binary);
		if (!file.is_open())
			return false;

		SHA256 sha256;
		std::vector<char> buffer(64 * 1024);

		while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0)
		{
			sha256.add(buffer.data(), static_cast<size_t>(file.gcount()));
		}

		if (file.bad())
			return false;

		outHex = sha256.getHash();
		return outHex.size() == 64;
	}
	catch (...)
	{
		return false;
	}
}

fs::path GetAssetPath(std::uint32_t customModelId, CustomVeh::Protocol::AssetType type)
{
	const fs::path modelDirectory = fs::path(g_modelsDir) / std::to_string(customModelId);

	switch (type)
	{
	case CustomVeh::Protocol::AssetType::Dff:
		return modelDirectory / "model.dff";
	case CustomVeh::Protocol::AssetType::Txd:
		return modelDirectory / "model.txd";
	case CustomVeh::Protocol::AssetType::Col:
		return modelDirectory / "model.col";
	case CustomVeh::Protocol::AssetType::AudioEngine:
	case CustomVeh::Protocol::AssetType::AudioAccel:
	case CustomVeh::Protocol::AssetType::AudioDecel:
	case CustomVeh::Protocol::AssetType::AudioBrake:
	case CustomVeh::Protocol::AssetType::AudioCrash:
	{
		auto it = HandlingMgr::customVehicleConfigs.find(customModelId);
		if (it != HandlingMgr::customVehicleConfigs.end())
		{
			const auto& cfg = it->second;
			if (type == CustomVeh::Protocol::AssetType::AudioEngine && !cfg.engineFile.empty())
				return modelDirectory / cfg.engineFile;
			if (type == CustomVeh::Protocol::AssetType::AudioAccel && !cfg.accelerationFile.empty())
				return modelDirectory / cfg.accelerationFile;
			if (type == CustomVeh::Protocol::AssetType::AudioDecel && !cfg.deaccelerationFile.empty())
				return modelDirectory / cfg.deaccelerationFile;
			if (type == CustomVeh::Protocol::AssetType::AudioBrake && !cfg.brakeFile.empty())
				return modelDirectory / cfg.brakeFile;
			if (type == CustomVeh::Protocol::AssetType::AudioCrash && !cfg.crashFile.empty())
				return modelDirectory / cfg.crashFile;
		}
		break;
	}
	}

	return {};
}
