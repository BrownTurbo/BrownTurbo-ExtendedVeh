#include "utils.h"
#include "defs.h"

std::string Sha256Hex(const uint8_t* data, size_t length)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD cbHash = 0, cbData = 0;
    std::string result;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return result;
    if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&cbHash), sizeof(DWORD),
            &cbData, 0)
        < 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }
    std::vector<uint8_t> hash(cbHash);
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0)
    {
        BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(length),
            0);
        if (BCryptFinishHash(hHash, hash.data(), cbHash, 0) >= 0)
        {
            char hex[2 * 32 + 1] = {};
            for (size_t i = 0; i < hash.size(); ++i)
                std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
            result.assign(hex);
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
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
		fs::path candidate = fs::path(g_modelsDir) / fs::path(relativePath);
		if (!IsPathInsideBase(g_modelsDir, candidate))
			return false;
		std::ifstream file(candidate, std::ios::binary);
		if (!file.is_open())
			return false;
		std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		outHex = Sha256Hex(data.data(), data.size());
		return !outHex.empty();
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
    }

    return {};
}
