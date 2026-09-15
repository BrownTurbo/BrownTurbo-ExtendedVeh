#pragma once

#include <cstdint>
#include <cstring>

namespace CustomVeh::Protocol {

	constexpr uint16_t PROTOCOL_VERSION = 2;
	constexpr std::size_t SHA256_HEX_LENGTH = 64;
	constexpr std::size_t SHA256_BUFFER_SIZE = SHA256_HEX_LENGTH + 1;
	constexpr std::size_t FILENAME_SIZE = 128;

	enum class Action : uint8_t
	{
		Init = 10,
		InitResponse = 11,
		ResetModel = 15,
		ResetVehicle = 16,
		SetVehicleHandling= 17,
		SetModelHandling = 18,
		SetPlayerHandling = 19,
		ResetPlayerHandling = 20,
		GetVehicleHandling = 21,
		GetModelHandling = 22,
		GetPlayerHandling = 23,
		ResetAll = 24,
		CustomVehicleDefine = 40,
		CustomVehicleBind = 41,
		CustomVehicleUnbind = 42,
		CustomVehicleDestroy = 43,
		AssetManifest = 50,
		AssetRequest = 51,
		AssetResume = 52,
		AssetBegin = 53,
		AssetChunk = 54,
		AssetEnd = 55,
		AssetVerified = 56,
		AssetCancel = 57,
		AssetRejected = 58,
		AssetReady = 59
	};

	enum class AssetType : uint8_t {
		Dff = 0,
		Txd = 1,
		Col = 2
	};

	enum AssetFlags : uint32_t {
		None = 0,
		HasDff = 1u << 0,
		HasTxd = 1u << 1,
		HasCol = 1u << 2
	};

	enum class RejectReason : uint8_t {
		Unknown = 0,
		InvalidProtocol = 1,
		InvalidModel = 2,
		InvalidAsset = 3,
		AssetNotFound = 4,
		HashMismatch = 5,
		SizeExceeded = 6,
		RateLimited = 7,
		TooManyTransfers= 8,
		InvalidResume = 9,
		InvalidTransfer = 10,
		ServerError = 11
	};

#pragma pack(push, 1)

	struct AssetDescriptor {
		AssetType type = AssetType::Dff;
		uint64_t size = 0;
		uint32_t compressedSize = 0;
		uint32_t chunkSize = 0;
		uint32_t chunkCount = 0;
		char sha256[SHA256_BUFFER_SIZE] = {};
		char filename[FILENAME_SIZE] = {};
	};

	struct EngineSound {
		int16_t OnSound = -1;
		int16_t OffSound = -1;
	};

	struct CelerateSound {
		int16_t accelerateSound = -1;
		int16_t decelerateSound = -1;
	};

	// Full custom vehicle definition sent server->client.
	struct VehicleDefinition {
		uint32_t customModelId = 0;
		uint16_t visualBaseModel = 0;
		uint16_t handlingBaseModel = 0;
		uint16_t audioBaseModel = 0;
		EngineSound engineSoundId = {};
		CelerateSound celerateSoundId = {};
		uint32_t flags = 0;
		AssetDescriptor dff = {};
		AssetDescriptor txd = {};
		AssetDescriptor col = {};
	};

	struct VehicleBinding {
		uint16_t sampVehicleId = 0;
		uint32_t customModelId = 0;
	};

	struct VehicleUnbinding {
		uint16_t sampVehicleId = 0;
	};

	struct AssetRequest {
		uint32_t transferId = 0;
		uint32_t customModelId = 0;
		AssetType type = AssetType::Dff;
		char sha256[SHA256_BUFFER_SIZE] = {};
	};

	struct AssetResume {
		uint32_t transferId = 0;
		uint32_t customModelId = 0;
		AssetType type = AssetType::Dff;
		char sha256[SHA256_BUFFER_SIZE] = {};
		uint32_t nextChunk = 0;
	};

	struct AssetBegin {
		uint32_t transferId = 0;
		uint32_t customModelId = 0;
		AssetType type = AssetType::Dff;
		uint64_t uncompressedSize = 0;
		uint64_t compressedSize = 0;
		uint32_t chunkSize = 0;
		uint32_t chunkCount = 0;
		char sha256[SHA256_BUFFER_SIZE] = {};
	};

	struct AssetChunkHeader {
		uint32_t transferId = 0;
		uint32_t chunkIndex = 0;
		uint16_t payloadSize = 0;
	};

	struct AssetEnd {
		uint32_t transferId = 0;
		uint32_t chunkCount = 0;
		char sha256[SHA256_BUFFER_SIZE] = {};
	};

	struct AssetVerified {
		uint32_t transferId = 0;
		uint32_t customModelId = 0;
		AssetType type = AssetType::Dff;
		char sha256[SHA256_BUFFER_SIZE] = {};
	};

	struct AssetCancel {
		uint32_t transferId = 0;
		uint32_t customModelId = 0;
		AssetType type = AssetType::Dff;
		RejectReason reason = RejectReason::Unknown;
	};

	struct AssetRejected {
		uint32_t transferId = 0;
		uint32_t customModelId = 0;
		AssetType type = AssetType::Dff;
		RejectReason reason = RejectReason::Unknown;
		char message[128] = {};
	};

	struct AssetReady {
		uint32_t transferId = 0;
		uint32_t customModelId = 0;
		AssetType type = AssetType::Dff;
	};

#pragma pack(pop)
}
