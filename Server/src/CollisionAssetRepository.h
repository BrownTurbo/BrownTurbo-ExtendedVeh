#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace CustomVeh::collision
{
using CustomModelId = std::uint32_t;

struct Hash256
{
	std::array<
		std::uint8_t,
		32>
		bytes {};

	bool operator==(
		const Hash256&) const
		= default;
};

enum class Version : std::uint8_t
{
	Unknown = 0,
	COL1 = 1,
	COL2 = 2,
	COL3 = 3,
	COL4 = 4
};

enum class Result : std::uint8_t
{
	Success = 0,

	NotInitialized,

	FileNotFound,
	OpenFailed,
	EmptyFile,
	FileTooLarge,

	TruncatedHeader,
	BadMagic,
	UnsupportedVersion,
	InvalidRecordSize,
	RecordOutOfBounds,
	InvalidModelName,
	NoRecords,

	HashFailed,
	RepositoryError
};

struct RecordInfo
{
	Version version {
		Version::Unknown
	};

	std::uint64_t offset {};
	std::uint64_t totalSize {};

	std::uint64_t bodyOffset {};
	std::uint64_t bodySize {};

	std::int16_t sourceModelId {
		-1
	};

	std::string modelName;
};

struct Metadata
{
	std::uint64_t fileSize {};

	Hash256 sha256 {};

	std::uint32_t recordCount {};

	Version firstVersion {
		Version::Unknown
	};

	bool multipleRecords {};
	bool mixedVersions {};
};

struct Asset
{
	CustomModelId customModelId {};

	std::filesystem::path path;

	Metadata metadata;

	std::vector<RecordInfo>
		records;

	bool available {};
};

class CollisionAssetRepository final
{
public:
	static CollisionAssetRepository&
	Instance();

	bool Initialize(
		const std::filesystem::path&
			modelsRoot);

	bool Scan();

	bool Reload(
		CustomModelId customModelId,
		std::string& outError);

	std::optional<Asset>
	Get(
		CustomModelId customModelId) const;

	bool Has(
		CustomModelId customModelId) const;

	Result ValidateFile(
		const std::filesystem::path& path,
		Metadata& outMetadata,
		std::vector<RecordInfo>&
			outRecords,
		std::string& outError) const;

	bool ComputeSha256(
		const std::filesystem::path& path,
		Hash256& outHash,
		std::string& outError) const;

	static std::string
	HashToHex(
		const Hash256& hash);

	const std::filesystem::path&
	Root() const noexcept
	{
		return root_;
	}

private:
	CollisionAssetRepository() = default;

	static constexpr std::uint64_t kMaxFileSize = 32ull * 1024ull * 1024ull;

	static constexpr std::size_t kRecordHeaderSize = 32;

	static constexpr std::uint32_t kMaxRecords = 256;

	std::filesystem::path
		root_;

	mutable std::shared_mutex
		mutex_;

	std::unordered_map<
		CustomModelId,
		Asset>
		assets_;

	static std::optional<Version>
	DecodeVersion(
		const std::uint8_t* fourcc);

	static std::uint32_t
	ReadU32LE(
		const std::uint8_t* p);

	static std::int16_t
	ReadI16LE(
		const std::uint8_t* p);

	static bool
	IsValidNameByte(
		std::uint8_t c);

	static bool
	ReadName(
		const std::uint8_t* data,
		std::string& outName);

	static bool
	IsZeroTail(
		const std::uint8_t* data,
		std::size_t size);

	static bool
	RangeWithin(
		std::size_t offset,
		std::size_t size,
		std::size_t total);

	bool LoadWholeFile(
		const std::filesystem::path& path,
		std::vector<std::uint8_t>&
			outBytes,
		std::string& outError) const;

	Result ParseRecords(
		const std::vector<std::uint8_t>&
			bytes,
		std::vector<RecordInfo>&
			outRecords,
		std::string& outError) const;

	bool DiscoverModelId(
		const std::filesystem::path&
			directory,
		CustomModelId& outId) const;

	std::filesystem::path
	GetCollisionPath(
		CustomModelId customModelId)
		const;
};
}
