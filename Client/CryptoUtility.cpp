#include "CryptoUtility.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

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

bool CryptoUtility::ComputeSHA256(const std::uint8_t* data, std::size_t length, std::string& outHashStr)
{
	outHashStr.clear();

	if (length > 0 && data == nullptr)
		return false;

	BCRYPT_ALG_HANDLE hAlg = nullptr;
	BCRYPT_HASH_HANDLE hHash = nullptr;

	std::vector<std::uint8_t> hashObject;
	std::vector<std::uint8_t> hashBuffer;

	DWORD objectSize = 0;
	DWORD hashLength = 0;
	DWORD resultSize = 0;

	NTSTATUS status = BCryptOpenAlgorithmProvider(
		&hAlg,
		BCRYPT_SHA256_ALGORITHM,
		nullptr,
		0);

	if (!BCRYPT_SUCCESS(status))
		return false;

	status = BCryptGetProperty(
		hAlg,
		BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&objectSize),
		sizeof(objectSize),
		&resultSize,
		0);

	if (!BCRYPT_SUCCESS(status)) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return false;
	}

	status = BCryptGetProperty(
		hAlg,
		BCRYPT_HASH_LENGTH,
		reinterpret_cast<PUCHAR>(&hashLength),
		sizeof(hashLength),
		&resultSize,
		0);

	if (!BCRYPT_SUCCESS(status)) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return false;
	}

	if (hashLength != 32) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return false;
	}

	hashObject.resize(objectSize);
	hashBuffer.resize(hashLength);

	status = BCryptCreateHash(
		hAlg,
		&hHash,
		hashObject.data(),
		static_cast<ULONG>(
			hashObject.size()),
		nullptr,
		0,
		0);

	if (!BCRYPT_SUCCESS(status)) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return false;
	}

	constexpr std::size_t MAX_HASH_CHUNK = 64 * 1024;
	std::size_t offset = 0;

	while (offset < length) {
		const std::size_t remaining = length - offset;
		const std::size_t chunkSize = remaining > MAX_HASH_CHUNK ? MAX_HASH_CHUNK : remaining;

		status = BCryptHashData(
			hHash,
			const_cast<PUCHAR>(
				data + offset),
			static_cast<ULONG>(
				chunkSize),
			0);

		if (!BCRYPT_SUCCESS(status)) {
			BCryptDestroyHash(hHash);
			BCryptCloseAlgorithmProvider(
				hAlg,
				0);

			return false;
		}

		offset += chunkSize;
	}

	status = BCryptFinishHash(
		hHash,
		hashBuffer.data(),
		static_cast<ULONG>(
			hashBuffer.size()),
		0);

	if (!BCRYPT_SUCCESS(status)) {
		BCryptDestroyHash(hHash);
		BCryptCloseAlgorithmProvider(
			hAlg,
			0);

		return false;
	}

	BCryptDestroyHash(hHash);
	BCryptCloseAlgorithmProvider(hAlg, 0);

	outHashStr = BytesToHex(hashBuffer.data(), hashBuffer.size());
	return outHashStr.size() == 64;
}

bool CryptoUtility::ComputeFileSHA256(const std::filesystem::path& filePath, std::string& outHashStr)
{
	outHashStr.clear();

	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open())
		return false;

	BCRYPT_ALG_HANDLE hAlg = nullptr;
	BCRYPT_HASH_HANDLE hHash = nullptr;

	DWORD objectSize = 0;
	DWORD hashLength = 0;
	DWORD resultSize = 0;

	NTSTATUS status = BCryptOpenAlgorithmProvider(
		&hAlg,
		BCRYPT_SHA256_ALGORITHM,
		nullptr,
		0);

	if (!BCRYPT_SUCCESS(status))
		return false;

	status = BCryptGetProperty(
		hAlg,
		BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(
			&objectSize),
		sizeof(objectSize),
		&resultSize,
		0);

	if (!BCRYPT_SUCCESS(status)) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return false;
	}

	status = BCryptGetProperty(
		hAlg,
		BCRYPT_HASH_LENGTH,
		reinterpret_cast<PUCHAR>(
			&hashLength),
		sizeof(hashLength),
		&resultSize,
		0);

	if (!BCRYPT_SUCCESS(status)) {
		BCryptCloseAlgorithmProvider(
			hAlg,
			0);

		return false;
	}

	if (hashLength != 32) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return false;
	}

	std::vector<std::uint8_t> hashObject(objectSize);
	std::vector<std::uint8_t> hashBuffer(hashLength);
	status = BCryptCreateHash(
		hAlg,
		&hHash,
		hashObject.data(),
		static_cast<ULONG>(
			hashObject.size()),
		nullptr,
		0,
		0);

	if (!BCRYPT_SUCCESS(status)) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return false;
	}

	constexpr std::size_t FILE_CHUNK_SIZE = 64 * 1024;
	std::array<std::uint8_t, FILE_CHUNK_SIZE> buffer {};

	while (true) {
		file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

		const std::streamsize bytesRead = file.gcount();
		if (bytesRead > 0) {
			status = BCryptHashData(
				hHash,
				buffer.data(),
				static_cast<ULONG>(
					bytesRead),
				0);

			if (!BCRYPT_SUCCESS(status)) {
				BCryptDestroyHash(hHash);
				BCryptCloseAlgorithmProvider(hAlg, 0);
				return false;
			}
		}

		if (file.eof())
			break;

		if (file.fail()) {
			BCryptDestroyHash(hHash);
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}
	}

	status = BCryptFinishHash(
		hHash,
		hashBuffer.data(),
		static_cast<ULONG>(
			hashBuffer.size()),
		0);

	BCryptDestroyHash(hHash);
	BCryptCloseAlgorithmProvider(hAlg, 0);

	if (!BCRYPT_SUCCESS(status))
		return false;

	outHashStr = BytesToHex(hashBuffer.data(), hashBuffer.size());
	return outHashStr.size() == 64;
}
