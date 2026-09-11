#include "CollisionAssetRepository.h"

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#include <windows.h>
#include <bcrypt.h>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace CustomVeh::collision
{
namespace
{
	constexpr std::size_t
		FOURCC_OFFSET
		= 0;

	constexpr std::size_t
		SIZE_OFFSET
		= 4;

	constexpr std::size_t
		NAME_OFFSET
		= 8;

	constexpr std::size_t
		NAME_LENGTH
		= 22;

	constexpr std::size_t
		MODEL_ID_OFFSET
		= 30;
}

CollisionAssetRepository&
CollisionAssetRepository::Instance()
{
	static CollisionAssetRepository
		instance;

	return instance;
}

bool CollisionAssetRepository::Initialize(
	const std::filesystem::path&
		modelsRoot)
{
	std::unique_lock lock(mutex_);

	root_ = modelsRoot;

	std::error_code ec;

	std::filesystem::create_directories(
		root_,
		ec);

	return !ec;
}

std::optional<Version>
CollisionAssetRepository::DecodeVersion(
	const std::uint8_t* fourcc)
{
	if (!fourcc)
		return std::nullopt;

	if (std::memcmp(
			fourcc,
			"COLL",
			4)
		== 0)
	{
		return Version::COL1;
	}

	if (std::memcmp(
			fourcc,
			"COL2",
			4)
		== 0)
	{
		return Version::COL2;
	}

	if (std::memcmp(
			fourcc,
			"COL3",
			4)
		== 0)
	{
		return Version::COL3;
	}

	if (std::memcmp(
			fourcc,
			"COL4",
			4)
		== 0)
	{
		return Version::COL4;
	}

	return std::nullopt;
}

std::uint32_t
CollisionAssetRepository::ReadU32LE(
	const std::uint8_t* p)
{
	return static_cast<std::uint32_t>(
			   p[0])
		|

		(static_cast<std::uint32_t>(
			 p[1])
			<< 8)
		|

		(static_cast<std::uint32_t>(
			 p[2])
			<< 16)
		|

		(static_cast<std::uint32_t>(
			 p[3])
			<< 24);
}

std::int16_t
CollisionAssetRepository::ReadI16LE(
	const std::uint8_t* p)
{
	const auto value = static_cast<std::uint16_t>(
						   p[0])
		|

		(static_cast<std::uint16_t>(
			 p[1])
			<< 8);

	return static_cast<std::int16_t>(
		value);
}

bool CollisionAssetRepository::IsValidNameByte(
	std::uint8_t c)
{
	return c == 0 || (c >= 32 && c <= 126);
}

bool CollisionAssetRepository::ReadName(
	const std::uint8_t* data,
	std::string& outName)
{
	if (!data)
		return false;

	outName.clear();
	outName.reserve(NAME_LENGTH);

	for (std::size_t i = 0;
		 i < NAME_LENGTH;
		 ++i)
	{
		const auto c = data[i];

		if (!IsValidNameByte(c))
			return false;

		if (c == 0)
			break;

		outName.push_back(
			static_cast<char>(c));
	}

	/*
	 * Empty names can occur in poorly authored/generated files.
	 * We don't reject them here because the sourceModelId may still be
	 * usable for single-record packages.
	 */
	return true;
}

bool CollisionAssetRepository::IsZeroTail(
	const std::uint8_t* data,
	std::size_t size)
{
	if (!data)
		return true;

	for (std::size_t i = 0;
		 i < size;
		 ++i)
	{
		if (data[i] != 0)
			return false;
	}

	return true;
}

bool CollisionAssetRepository::RangeWithin(
	std::size_t offset,
	std::size_t size,
	std::size_t total)
{
	if (offset > total)
		return false;

	return size <= (total - offset);
}

bool CollisionAssetRepository::LoadWholeFile(
	const std::filesystem::path& path,
	std::vector<std::uint8_t>&
		outBytes,
	std::string& outError) const
{
	std::error_code ec;

	if (!std::filesystem::is_regular_file(
			path,
			ec))
	{
		outError = "COL is not a regular file.";

		return false;
	}

	const auto size = std::filesystem::file_size(
		path,
		ec);

	if (ec)
	{
		outError = "file_size failed: " + ec.message();

		return false;
	}

	if (size == 0)
	{
		outError = "COL file is empty.";

		return false;
	}

	if (size > kMaxFileSize)
	{
		outError = "COL exceeds maximum file size.";

		return false;
	}

	std::ifstream file(
		path,
		std::ios::binary);

	if (!file)
	{
		outError = "Unable to open COL file.";

		return false;
	}

	outBytes.resize(
		static_cast<std::size_t>(
			size));

	file.read(
		reinterpret_cast<char*>(
			outBytes.data()),
		static_cast<std::streamsize>(
			outBytes.size()));

	if (file.gcount() != static_cast<std::streamsize>(outBytes.size()))
	{
		outBytes.clear();

		outError = "COL file read was incomplete.";

		return false;
	}

	return true;
}

CustomVeh::collision::Result CollisionAssetRepository::ParseRecords(
	const std::vector<std::uint8_t>&
		bytes,
	std::vector<RecordInfo>&
		outRecords,
	std::string& outError) const
{
	outRecords.clear();

	if (bytes.empty())
	{
		outError = "COL buffer is empty.";

		return Result::EmptyFile;
	}

	std::size_t offset = 0;

	while (offset < bytes.size())
	{
		const auto remaining = bytes.size() - offset;

		if (remaining < kRecordHeaderSize)
		{
			if (IsZeroTail(
					bytes.data() + offset,
					remaining))
			{
				break;
			}

			outError = "Truncated COL header.";

			return Result::TruncatedHeader;
		}

		const auto* header = bytes.data() + offset;

		if (IsZeroTail(
				header,
				kRecordHeaderSize))
		{
			break;
		}

		const auto version = DecodeVersion(
			header + FOURCC_OFFSET);

		if (!version)
		{
			outError = "Invalid COL FourCC.";

			return Result::BadMagic;
		}

		const auto sizeAfterFourCC = ReadU32LE(
			header + SIZE_OFFSET);

		if (sizeAfterFourCC < 24)
		{
			outError = "Invalid COL record size.";

			return Result::InvalidRecordSize;
		}

		const std::uint64_t
			totalRecord
			= static_cast<
				  std::uint64_t>(
				  sizeAfterFourCC)
			+ 8ull;

		if (totalRecord < kRecordHeaderSize || totalRecord > remaining)
		{
			outError = "COL record exceeds "
					   "file bounds.";

			return Result::RecordOutOfBounds;
		}

		RecordInfo record {};

		record.version = *version;

		record.offset = offset;

		record.totalSize = totalRecord;

		record.bodyOffset = offset + kRecordHeaderSize;

		record.bodySize = totalRecord - kRecordHeaderSize;

		record.sourceModelId = ReadI16LE(
			header + MODEL_ID_OFFSET);

		if (!ReadName(
				header + NAME_OFFSET,
				record.modelName))
		{
			outError = "COL model name is invalid.";

			return Result::InvalidModelName;
		}

		outRecords.push_back(
			std::move(record));

		if (outRecords.size() > kMaxRecords)
		{
			outError = "Too many COL records.";

			return Result::InvalidRecordSize;
		}

		offset += static_cast<std::size_t>(
			totalRecord);
	}

	if (outRecords.empty())
	{
		outError = "No collision records found.";

		return Result::NoRecords;
	}

	return Result::Success;
}

bool CollisionAssetRepository::ComputeSha256(
	const std::filesystem::path& path,
	Hash256& outHash,
	std::string& outError) const
{
	std::ifstream file(
		path,
		std::ios::binary);

	if (!file)
	{
		outError = "Unable to open COL for SHA-256.";

		return false;
	}

	BCRYPT_ALG_HANDLE
	algorithm = nullptr;

	BCRYPT_HASH_HANDLE
	hash = nullptr;

	unsigned long objectLength = 0;
	unsigned long digestLength = 0;
	unsigned long returned = 0;

	if (BCryptOpenAlgorithmProvider(
			&algorithm,
			BCRYPT_SHA256_ALGORITHM,
			nullptr,
			0)
		< 0)
	{
		outError = "BCrypt SHA-256 provider failed.";

		return false;
	}

	if (BCryptGetProperty(
			algorithm,
			BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<unsigned char*>(
				&objectLength),
			sizeof(objectLength),
			&returned,
			0)
		< 0)
	{
		BCryptCloseAlgorithmProvider(
			algorithm,
			0);

		outError = "BCrypt object-size query failed.";

		return false;
	}

	if (BCryptGetProperty(
			algorithm,
			BCRYPT_HASH_LENGTH,
			reinterpret_cast<unsigned char*>(
				&digestLength),
			sizeof(digestLength),
			&returned,
			0)
		< 0)
	{
		BCryptCloseAlgorithmProvider(
			algorithm,
			0);

		outError = "BCrypt digest-size query failed.";

		return false;
	}

	if (digestLength != outHash.bytes.size())
	{
		BCryptCloseAlgorithmProvider(
			algorithm,
			0);

		outError = "Unexpected SHA-256 size.";

		return false;
	}

	std::vector<std::uint8_t>
		object(objectLength);

	if (BCryptCreateHash(
			algorithm,
			&hash,
			object.data(),
			objectLength,
			nullptr,
			0,
			0)
		< 0)
	{
		BCryptCloseAlgorithmProvider(
			algorithm,
			0);

		outError = "BCryptCreateHash failed.";

		return false;
	}

	std::array<
		std::uint8_t,
		64 * 1024>
		buffer {};

	while (file)
	{
		file.read(
			reinterpret_cast<char*>(
				buffer.data()),
			static_cast<
				std::streamsize>(
				buffer.size()));

		const auto count = file.gcount();

		if (count <= 0)
			break;

		if (BCryptHashData(
				hash,
				buffer.data(),
				static_cast<unsigned long>(
					count),
				0)
			< 0)
		{
			BCryptDestroyHash(
				hash);

			BCryptCloseAlgorithmProvider(
				algorithm,
				0);

			outError = "BCryptHashData failed.";

			return false;
		}
	}

	if (BCryptFinishHash(
			hash,
			outHash.bytes.data(),
			static_cast<unsigned long>(
				outHash.bytes.size()),
			0)
		< 0)
	{
		BCryptDestroyHash(
			hash);

		BCryptCloseAlgorithmProvider(
			algorithm,
			0);

		outError = "BCryptFinishHash failed.";

		return false;
	}

	BCryptDestroyHash(hash);

	BCryptCloseAlgorithmProvider(
		algorithm,
		0);

	return true;
}

std::string
CollisionAssetRepository::HashToHex(
	const Hash256& hash)
{
	std::ostringstream stream;

	for (const auto byte :
		hash.bytes)
	{
		stream
			<< std::hex
			<< std::setw(2)
			<< std::setfill('0')
			<< static_cast<unsigned>(
				   byte);
	}

	return stream.str();
}

CustomVeh::collision::Result CollisionAssetRepository::ValidateFile(
	const std::filesystem::path& path,
	Metadata& outMetadata,
	std::vector<RecordInfo>&
		outRecords,
	std::string& outError) const
{
	outMetadata = {};

	std::vector<
		std::uint8_t>
		bytes;

	if (!LoadWholeFile(
			path,
			bytes,
			outError))
	{
		if (!std::filesystem::exists(path))
			return Result::FileNotFound;

		if (outError.find("maximum") != std::string::npos)
		{
			return Result::FileTooLarge;
		}

		return Result::OpenFailed;
	}

	auto result = ParseRecords(
		bytes,
		outRecords,
		outError);

	if (result != Result::Success)
	{
		return result;
	}

	if (!ComputeSha256(
			path,
			outMetadata.sha256,
			outError))
	{
		return Result::HashFailed;
	}

	outMetadata.fileSize = bytes.size();

	outMetadata.recordCount = static_cast<std::uint32_t>(
		outRecords.size());

	outMetadata.firstVersion = outRecords.front().version;

	outMetadata.multipleRecords = outRecords.size() > 1;

	outMetadata.mixedVersions = std::any_of(
		outRecords.begin() + 1,
		outRecords.end(),
		[&](const RecordInfo& record)
		{
			return record.version != outMetadata.firstVersion;
		});

	return Result::Success;
}

bool CollisionAssetRepository::DiscoverModelId(
	const std::filesystem::path&
		directory,
	CustomModelId& outId) const
{
	const auto name = directory.filename().string();

	if (name.empty())
		return false;

	try
	{
		std::size_t consumed = 0;

		const auto value = std::stoull(
			name,
			&consumed,
			10);

		if (consumed != name.size())
			return false;

		if (value > std::numeric_limits<CustomModelId>::max())
		{
			return false;
		}

		outId = static_cast<
			CustomModelId>(
			value);

		return true;
	}
	catch (...)
	{
		return false;
	}
}

std::filesystem::path CollisionAssetRepository::GetCollisionPath(CustomModelId customModelId) const
{
	return root_ / std::to_string(customModelId) / "model.col";
}

bool CollisionAssetRepository::Scan()
{
	std::vector<Asset>
		discovered;

	std::error_code ec;

	if (!std::filesystem::is_directory(
			root_,
			ec))
	{
		return false;
	}

	for (const auto&
			 directory :
		std::filesystem::
			directory_iterator(
				root_,
				ec))
	{
		if (ec)
			break;

		if (!directory.is_directory(
				ec))
			continue;

		CustomModelId customId {};

		if (!DiscoverModelId(
				directory.path(),
				customId))
			continue;

		const auto colPath = directory.path() / "model.col";

		if (!std::filesystem::
				is_regular_file(
					colPath,
					ec))
		{
			continue;
		}

		Metadata metadata {};
		std::vector<RecordInfo>
			records;

		std::string error;

		const auto result = ValidateFile(
			colPath,
			metadata,
			records,
			error);

		if (result != Result::Success)
		{
			continue;
		}

		Asset asset {};

		asset.customModelId = customId;

		asset.path = colPath;

		asset.metadata = metadata;

		asset.records = std::move(records);

		asset.available = true;

		discovered.push_back(
			std::move(asset));
	}

	{
		std::unique_lock lock(
			mutex_);

		assets_.clear();

		for (auto& asset :
			discovered)
		{
			assets_.emplace(
				asset.customModelId,
				std::move(asset));
		}
	}

	return true;
}

bool CollisionAssetRepository::Reload(
	CustomModelId customModelId,
	std::string& outError)
{
	const auto path = GetCollisionPath(
		customModelId);

	Metadata metadata {};
	std::vector<RecordInfo>
		records;

	const auto result = ValidateFile(
		path,
		metadata,
		records,
		outError);

	if (result != Result::Success)
	{
		return false;
	}

	Asset asset {};

	asset.customModelId = customModelId;

	asset.path = path;

	asset.metadata = metadata;

	asset.records = std::move(records);

	asset.available = true;

	{
		std::unique_lock lock(
			mutex_);

		assets_[customModelId] = std::move(asset);
	}

	return true;
}

std::optional<Asset>
CollisionAssetRepository::Get(
	CustomModelId customModelId)
	const
{
	std::shared_lock lock(
		mutex_);

	const auto it = assets_.find(
		customModelId);

	if (it == assets_.end())
		return std::nullopt;

	return it->second;
}

bool CollisionAssetRepository::Has(
	CustomModelId customModelId)
	const
{
	std::shared_lock lock(
		mutex_);

	return assets_.contains(
		customModelId);
}
}
