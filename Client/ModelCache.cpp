#include "ModelCache.h"
#include "CryptoUtility.h"

std::optional<std::filesystem::path>
ModelCache::TryGet(uint32_t modelId, uint8_t fileKind,
	const std::string& expectedSha256Hex) const
{
	if (!TransferConfig::Instance().cacheEnabled)
		return std::nullopt;

	fs::path path = PathFor(modelId, fileKind);
	std::error_code ec;
	if (!fs::exists(path, ec))
		return std::nullopt;

	auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::file_clock::now() - fs::last_write_time(path, ec))
					 .count();
	if (ageMs > TransferConfig::Instance().expireTimeMs) {
		fs::remove(path, ec);
		return std::nullopt;
	}

	std::string calculatedsha256;
	CryptoUtility::ComputeFileSHA256(path, calculatedsha256);
	if (calculatedsha256 != expectedSha256Hex) {
		fs::remove(path, ec); // stale/corrupt
		return std::nullopt;
	}

	return path;
}
