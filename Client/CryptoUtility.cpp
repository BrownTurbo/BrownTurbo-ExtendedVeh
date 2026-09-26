#include "CryptoUtility.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

std::string CryptoUtility::BytesToHex(const std::uint8_t* buffer, std::size_t length)
{
	if (length > 0 && buffer == nullptr)
		return {};

	static constexpr char HEX[] = "0123456789abcdef";
	std::string result;
	result.resize(length * 2);

	for (std::size_t i = 0; i < length; ++i) {
		result[i * 2] = HEX[(buffer[i] >> 4) & 0x0F];
		result[i * 2 + 1] = HEX[buffer[i] & 0x0F];
	}
	return result;
}

#if __has_include(<hash-library/sha256.h>)
#include <hash-library/sha256.h>
#else
#include <sha256.h>
#endif

bool CryptoUtility::ComputeSHA256(const std::uint8_t* data, std::size_t length, std::string& outHashStr)
{
	outHashStr.clear();

	if (length > 0 && data == nullptr)
		return false;

	SHA256 sha256;
	outHashStr = sha256(data, length);

	return outHashStr.size() == 64;
}

bool CryptoUtility::ComputeFileSHA256(const std::filesystem::path& filePath, std::string& outHashStr)
{
	try {
		outHashStr.clear();

		std::ifstream file(filePath.c_str(), std::ios::binary);
		if (!file.is_open())
			return false;

		SHA256 sha256;
		std::vector<char> buffer(64 * 1024);

		while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
			sha256.add(buffer.data(), static_cast<size_t>(file.gcount()));
		}

		if (file.bad())
			return false;

		outHashStr = sha256.getHash();
		return outHashStr.size() == 64;
	}
	catch (...)
	{
		return false;
	}
}

#if __has_include(<hash-library/md5.h>)
#include <hash-library/md5.h>
#else
#include <md5.h>
#endif

bool CryptoUtility::ComputeMD5(const std::uint8_t* data, std::size_t length, std::string& outHashStr)
{
	outHashStr.clear();

	if (length > 0 && data == nullptr)
		return false;

	MD5 md5;
	if (length > 0)
		md5.add(data, length);
	outHashStr = md5.getHash();
	return outHashStr.size() == 32;
}

bool CryptoUtility::ComputeMD5(const std::string& input, std::string& outHashStr)
{
	outHashStr.clear();

	MD5 md5;
	md5.add(input.data(), input.size());
	outHashStr = md5.getHash();
	return outHashStr.size() == 32;
}
