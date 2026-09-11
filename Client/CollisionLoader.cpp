#include "CollisionLoader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>

#include "game_sa/CBaseModelInfo.h"
#include "game_sa/CColModel.h"
#include "game_sa/CFileLoader.h"
#include "game_sa/CModelInfo.h"
#include "streamingextender.hpp"

namespace {
inline CColModel* GetCollisionModel(CBaseModelInfo* modelInfo)
{
	return modelInfo
		? modelInfo->m_pColModel
		: nullptr;
}

inline bool OwnsCollisionModel(CBaseModelInfo* modelInfo)
{
	return modelInfo && modelInfo->bDoWeOwnTheColModel;
}

inline void SetCollisionOwnership(CBaseModelInfo* modelInfo, bool owns)
{
	if (!modelInfo)
		return;

	modelInfo->SetIsLod(int(owns));
}
}

namespace ExtendedVeh::Collision {
namespace {
	constexpr std::size_t FOURCC_OFFSET = 0;
	constexpr std::size_t SIZE_OFFSET = 4;
	constexpr std::size_t NAME_OFFSET = 8;
	constexpr std::size_t NAME_LENGTH = 22;
	constexpr std::size_t MODEL_ID_OFFSET = 30;

	constexpr int GTA_MODEL_INFO_COUNT = 20000;

	/*
	 * This is only for sanity validation.
	 *
	 * It is intentionally conservative. GTA's own collision loader still
	 * performs the real format-specific parsing.
	 */
	constexpr std::uint32_t MAX_RECORDS = 256;
}

CollisionLoader& CollisionLoader::Instance()
{
	static CollisionLoader instance;
	return instance;
}

bool CollisionLoader::Initialize()
{
	std::lock_guard lock(mutex_);

	if (initialized_)
		return true;

	initialized_ = true;
	return true;
}

void CollisionLoader::Shutdown()
{
	std::lock_guard lock(mutex_);

	for (auto& [customId, entry] : loaded_) {
		auto& handle = entry.handle;

		if (handle.installed && handle.gtaModelId >= 0) {
			auto* modelInfo = GetEngineModelInfo(
				handle.gtaModelId);

			if (modelInfo && modelInfo->m_pColModel == handle.collision) {
				/*
				 * Restore the previous collision pointer.
				 */
				modelInfo->SetColModel(
					handle.previousCollision,
					false);

				modelInfo->SetIsLod(int(handle.previousOwned));
			}
		}

		/*
		 * At this point the model-info no longer references our collision.
		 *
		 * CColModel has a GTA-specific operator delete which returns the
		 * object to GTA's collision model pool.
		 */
		if (handle.collision) {
			delete handle.collision;

			handle.collision = nullptr;
		}

		handle.installed = false;
	}

	loaded_.clear();
	initialized_ = false;
}

bool CollisionLoader::ReadFile(
	const std::filesystem::path& path,
	FileBuffer& outFile,
	std::string& outError) const
{
	std::error_code ec;

	if (!std::filesystem::is_regular_file(path, ec)) {
		outError = "Collision file is not a regular file.";

		return false;
	}

	const auto size = std::filesystem::file_size(
		path,
		ec);

	if (ec) {
		outError = "Unable to obtain COL file size: " + ec.message();

		return false;
	}

	if (size == 0) {
		outError = "Collision file is empty.";

		return false;
	}

	if (size > kMaxFileSize) {
		outError = "Collision file exceeds configured maximum.";

		return false;
	}

	std::ifstream input(
		path,
		std::ios::binary);

	if (!input) {
		outError = "Unable to open collision file: " + path.string();

		return false;
	}

	outFile.bytes.resize(
		static_cast<std::size_t>(size));

	input.read(
		reinterpret_cast<char*>(
			outFile.bytes.data()),
		static_cast<std::streamsize>(
			outFile.bytes.size()));

	if (input.gcount() != static_cast<std::streamsize>(outFile.bytes.size())) {
		outFile.bytes.clear();

		outError = "COL file read was incomplete.";

		return false;
	}

	return true;
}

std::optional<Version>
CollisionLoader::DecodeVersion(
	const std::uint8_t* fourcc)
{
	if (!fourcc)
		return std::nullopt;

	if (std::memcmp(
			fourcc,
			"COLL",
			4)
		== 0) {
		return Version::COL1;
	}

	if (std::memcmp(
			fourcc,
			"COL2",
			4)
		== 0) {
		return Version::COL2;
	}

	if (std::memcmp(
			fourcc,
			"COL3",
			4)
		== 0) {
		return Version::COL3;
	}

	if (std::memcmp(
			fourcc,
			"COL4",
			4)
		== 0) {
		return Version::COL4;
	}

	return std::nullopt;
}

std::uint32_t CollisionLoader::ReadU32LE(
	const std::uint8_t* p)
{
	return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int16_t CollisionLoader::ReadI16LE(
	const std::uint8_t* p)
{
	const auto value = static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);

	return static_cast<std::int16_t>(
		value);
}

bool CollisionLoader::IsValidModelNameByte(
	std::uint8_t value)
{
	return value == 0 || (value >= 32 && value <= 126);
}

bool CollisionLoader::ReadModelName(
	const std::uint8_t* p,
	char outName[23])
{
	if (!p || !outName)
		return false;

	std::memset(
		outName,
		0,
		23);

	for (std::size_t i = 0;
		 i < NAME_LENGTH;
		 ++i) {
		const auto c = p[i];

		if (!IsValidModelNameByte(c))
			return false;

		if (c == 0)
			break;

		outName[i] = static_cast<char>(c);
	}

	return true;
}

bool CollisionLoader::ModelNameEquals(
	const char* lhs,
	const char* rhs)
{
	if (!lhs || !rhs)
		return false;

	while (*lhs || *rhs) {
		const auto a = static_cast<unsigned char>(*lhs);

		const auto b = static_cast<unsigned char>(*rhs);

		if (std::tolower(a) != std::tolower(b)) {
			return false;
		}

		if (*lhs)
			++lhs;

		if (*rhs)
			++rhs;
	}

	return true;
}

bool CollisionLoader::IsZeroTail(
	const std::uint8_t* data,
	std::size_t size)
{
	if (!data)
		return true;

	for (std::size_t i = 0;
		 i < size;
		 ++i) {
		if (data[i] != 0)
			return false;
	}

	return true;
}

bool CollisionLoader::RangeWithin(
	std::size_t offset,
	std::size_t size,
	std::size_t total)
{
	if (offset > total)
		return false;

	return size <= (total - offset);
}

Result
CollisionLoader::ParseFile(
	const FileBuffer& file,
	Statistics& outStatistics,
	std::vector<RecordInfo>& outRecords,
	std::string& outError) const
{
	outStatistics = {};
	outRecords.clear();

	outStatistics.fileSize = file.bytes.size();

	if (file.bytes.empty()) {
		outError = "COL buffer is empty.";

		return Result::EmptyFile;
	}

	std::size_t offset = 0;

	while (offset < file.bytes.size()) {
		const auto remaining = file.bytes.size() - offset;

		/*
		 * A small zero-filled tail is harmless.
		 */
		if (remaining < kRecordHeaderSize) {
			if (IsZeroTail(
					file.bytes.data() + offset,
					remaining)) {
				break;
			}

			outError = "Truncated COL header at EOF.";

			return Result::TruncatedHeader;
		}

		const auto* header = file.bytes.data() + offset;

		if (IsZeroTail(
				header,
				kRecordHeaderSize)) {
			break;
		}

		const auto version = DecodeVersion(
			header + FOURCC_OFFSET);

		if (!version) {
			outError = "Unknown COL FourCC at offset " + std::to_string(offset);

			return Result::BadMagic;
		}

		const auto sizeAfterFourCC = ReadU32LE(
			header + SIZE_OFFSET);

		/*
		 * Fixed file header:
		 *
		 * FOURCC 4
		 * SIZE   4
		 * NAME  22
		 * ID     2
		 *
		 * Total = 32.
		 *
		 * The size field is the payload/header-body size excluding the
		 * first 8 bytes, so the complete record is size + 8.
		 */
		if (sizeAfterFourCC < 24) {
			outError = "Invalid COL record size.";

			return Result::InvalidRecordSize;
		}

		const std::uint64_t totalRecord = static_cast<std::uint64_t>(
											  sizeAfterFourCC)
			+ 8ull;

		if (totalRecord < kRecordHeaderSize || totalRecord > remaining) {
			outError = "COL record exceeds file bounds.";

			return Result::RecordOutOfBounds;
		}

		RecordInfo record {};

		record.version = *version;

		record.fileOffset = offset;

		record.totalSize = totalRecord;

		record.bodyOffset = offset + kRecordHeaderSize;

		record.bodySize = totalRecord - kRecordHeaderSize;

		record.sourceModelId = ReadI16LE(
			header + MODEL_ID_OFFSET);

		if (!ReadModelName(
				header + NAME_OFFSET,
				record.modelName)) {
			outError = "COL model name contains invalid bytes.";

			return Result::InvalidModelName;
		}

		outRecords.push_back(
			record);

		if (outRecords.size() > MAX_RECORDS) {
			outError = "COL contains too many records.";

			return Result::InvalidRecordSize;
		}

		offset += static_cast<std::size_t>(
			totalRecord);
	}

	if (outRecords.empty()) {
		outError = "No collision records were found.";

		return Result::NoRecords;
	}

	outStatistics.recordCount = static_cast<std::uint32_t>(
		outRecords.size());

	outStatistics.multipleRecords = outRecords.size() > 1;

	const auto firstVersion = outRecords.front().version;

	outStatistics.mixedVersions = std::any_of(
		outRecords.begin() + 1,
		outRecords.end(),
		[&](const RecordInfo& record) {
			return record.version != firstVersion;
		});

	return Result::Success;
}

Result
CollisionLoader::InspectFile(
	const std::filesystem::path& path,
	Statistics& outStatistics,
	std::vector<RecordInfo>& outRecords,
	std::string& outError) const
{
	FileBuffer file;

	if (!ReadFile(
			path,
			file,
			outError)) {
		if (!std::filesystem::exists(path))
			return Result::FileNotFound;

		if (outError.find("maximum") != std::string::npos) {
			return Result::FileTooLarge;
		}

		return Result::OpenFailed;
	}

	return ParseFile(
		file,
		outStatistics,
		outRecords,
		outError);
}

bool CollisionLoader::LoadCollisionFromMemory(uint8_t* data, size_t size, CVehicleModelInfo* modelInfo)
{
	if (!data || size < 8)
		return false;

	uint8_t* colData = nullptr;

	// A standard .col archive starts with "COLL" and a 4-byte total size.
	// We must skip these 8 bytes to reach the actual COL2/COL3 model chunk.
	if (memcmp(data, "COLL", 4) == 0) {
		colData = data + 8;
	}
	// If the file is already a raw exported chunk without the archive wrapper
	else if (memcmp(data, "COL2", 4) == 0 || memcmp(data, "COL3", 4) == 0) {
		colData = data;
	} else {
		return false; // Unknown/Invalid magic header
	}

	// Allocate a new collision model instance if the vehicle doesn't have one
	if (!modelInfo->m_pColModel) {
		modelInfo->m_pColModel = new CColModel();
	}

	// Directly parse the chunk into our specific model, ignoring the embedded ID
	CFileLoader::LoadCollisionModel(colData, *modelInfo->m_pColModel);

	return true;
}

const RecordInfo*
CollisionLoader::SelectRecord(
	const std::vector<RecordInfo>& records,
	GtaModelId targetModelId,
	const char* expectedModelName) const
{
	/*
	 * First choice:
	 * explicit model name.
	 */
	if (expectedModelName && *expectedModelName) {
		for (const auto& record : records) {
			if (ModelNameEquals(
					record.modelName,
					expectedModelName)) {
				return &record;
			}
		}
	}

	/*
	 * Second choice:
	 * embedded COL model ID.
	 */
	if (targetModelId >= 0 && targetModelId <= std::numeric_limits<std::int16_t>::max()) {
		for (const auto& record : records) {
			if (record.sourceModelId == targetModelId) {
				return &record;
			}
		}
	}

	/*
	 * A single-record custom file can safely be accepted even when the
	 * exporter did not preserve a useful GTA source model ID.
	 *
	 * The caller has already identified this file as belonging to this
	 * custom package.
	 */
	if (records.size() == 1)
		return &records.front();

	return nullptr;
}

Result CollisionLoader::LoadRecordIntoGta(
	const FileBuffer& file,
	const RecordInfo& record,
	CColModel& outCollision,
	Statistics& statistics,
	std::string& outError) const
{
	if (!RangeWithin(
			static_cast<std::size_t>(
				record.bodyOffset),
			static_cast<std::size_t>(
				record.bodySize),
			file.bytes.size())) {
		outError = "COL record payload is outside file.";

		return Result::RecordOutOfBounds;
	}

	unsigned char* body = (unsigned char*)(file.bytes.data() + record.bodyOffset);

	/*
	 * These are actual Plugin-SDK GTA SA APIs.
	 *
	 * GTA's reversed source confirms that the pointer is expected to point
	 * at the version-specific data after the 32-byte file header, and
	 * selects the appropriate COL1/COL2/COL3/COL4 decoder.
	 */
	__try {
		switch (record.version) {
		case Version::COL1: {
			CFileLoader::
				LoadCollisionModel(
					body,
					outCollision);

			break;
		}

		case Version::COL2: {
			CFileLoader::
				LoadCollisionModelVer2(
					body,
					static_cast<
						unsigned int>(
						record.bodySize),
					outCollision,
					record.modelName);

			break;
		}

		case Version::COL3: {
			CFileLoader::
				LoadCollisionModelVer3(
					body,
					static_cast<
						unsigned int>(
						record.bodySize),
					outCollision,
					record.modelName);

			break;
		}

		case Version::COL4: {
			CFileLoader::
				LoadCollisionModelVer4(
					body,
					static_cast<
						unsigned int>(
						record.bodySize),
					outCollision,
					record.modelName);

			break;
		}

		default:
			outError = "Unsupported COL version.";

			return Result::UnsupportedVersion;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		outError = "GTA collision parser raised a structured exception.";

		return Result::GtaLoaderFailed;
	}

	statistics.selectedVersion = record.version;

	statistics.hasCollisionData = outCollision.m_bHasCollisionVolumes;

	if (!outCollision.m_pColData) {
		statistics.sphereCount = 0;
		statistics.boxCount = 0;
		statistics.lineCount = 0;
		statistics.triangleCount = 0;

		return Result::Success;
	}

	statistics.sphereCount = outCollision.m_pColData->m_nNumSpheres;

	statistics.boxCount = outCollision.m_pColData->m_nNumBoxes;

	statistics.lineCount = outCollision.m_pColData->m_nNumLines;

	statistics.triangleCount = outCollision.m_pColData->m_nNumTriangles;

	statistics.shadowVertexCount = outCollision.m_pColData->m_nNumShadowVertices;

	statistics.shadowTriangleCount = outCollision.m_pColData->m_nNumShadowTriangles;

	statistics.hasShadowData = outCollision.m_pColData->m_pShadowVertices != nullptr && outCollision.m_pColData->m_pShadowTriangles != nullptr && outCollision.m_pColData->m_nNumShadowTriangles > 0;

	return Result::Success;
}

Result
CollisionLoader::Load(
	CustomModelId customModelId,
	GtaModelId targetModelId,
	const std::filesystem::path& path,
	const char* expectedModelName,
	Handle& outHandle,
	std::string& outError)
{
	std::lock_guard lock(mutex_);

	if (!initialized_) {
		outError = "CollisionLoader is not initialized.";

		return Result::NotInitialized;
	}

	if (loaded_.contains(
			customModelId)) {
		outError = "Custom collision is already loaded.";

		return Result::AlreadyLoaded;
	}

	if (targetModelId < 0 || targetModelId >= GTA_MODEL_INFO_COUNT) {
		outError = "Invalid GTA model ID.";

		return Result::TargetModelNotFound;
	}

	auto* target = GetEngineModelInfo(
		targetModelId);

	if (!target) {
		outError = "GTA model-info does not exist.";

		return Result::TargetModelNotFound;
	}

	/*
	 * Valid GTA model types for vehicle-oriented collision.
	 */
	const auto type = target->GetModelType();

	if (type != MODEL_INFO_VEHICLE && type != MODEL_INFO_CLUMP && type != MODEL_INFO_ATOMIC) {
		outError = "Target GTA model-info is not an appropriate "
				   "collision model target.";

		return Result::TargetWrongType;
	}

	/*
	 * Extremely important:
	 *
	 * Existing vanilla model collision is global for that model-info.
	 *
	 * We therefore reject replacement when the target already owns its
	 * collision, instead of silently destroying somebody else's model.
	 */
	if (target->m_pColModel && target->bDoWeOwnTheColModel) {
		outError = "Target already owns a collision model. "
				   "A shared/vanilla model must not be overwritten.";

		return Result::TargetAlreadyOwned;
	}

	FileBuffer file;

	if (!ReadFile(
			path,
			file,
			outError)) {
		if (!std::filesystem::exists(path))
			return Result::FileNotFound;

		if (outError.find("maximum") != std::string::npos) {
			return Result::FileTooLarge;
		}

		return Result::OpenFailed;
	}

	Statistics statistics {};
	std::vector<RecordInfo> records;

	auto parseResult = ParseFile(
		file,
		statistics,
		records,
		outError);

	if (parseResult != Result::Success) {
		return parseResult;
	}

	const auto* selected = SelectRecord(
		records,
		targetModelId,
		expectedModelName);

	if (!selected) {
		outError = "No COL record matches target model/name.";

		return Result::ModelNotFoundInFile;
	}

	statistics.selectedRecordIndex = static_cast<std::uint32_t>(
		selected - records.data());

	CColModel* collision = new CColModel();

	if (!collision) {
		outError = "GTA CColModel allocation failed.";

		return Result::AllocationFailed;
	}

	auto loadResult = LoadRecordIntoGta(
		file,
		*selected,
		*collision,
		statistics,
		outError);

	if (loadResult != Result::Success) {
		delete collision;
		return loadResult;
	}

	/*
	 * This collision is NOT registered with CColStore because this loader
	 * is intentionally outside GTA's ordinary global COL streaming system.
	 *
	 * Never feed this marker to CColStore::RemoveCol().
	 */
	collision->m_nColSlot = 0xFF;

	Handle handle {};

	handle.customModelId = customModelId;

	handle.gtaModelId = targetModelId;

	handle.collision = collision;

	handle.previousCollision = target->m_pColModel;

	handle.previousOwned = target->bDoWeOwnTheColModel != 0;

	handle.statistics = statistics;

	/*
	 * Attach only to the exact target supplied by the caller.
	 *
	 * For your production architecture this should be a dedicated
	 * custom-model representation, not a shared vanilla model.
	 */
	target->SetColModel(
		collision,
		false);

	target->SetIsLod(1);

	if (target->m_pColModel != collision) {
		/*
		 * Since ownership was just requested, explicitly restore the
		 * previous state before freeing our collision.
		 */
		target->SetColModel(
			handle.previousCollision,
			false);

		target->SetIsLod(int(handle.previousOwned));

		delete collision;

		outError = "GTA failed to install the CColModel.";

		return Result::AttachFailed;
	}

	handle.installed = true;
	handle.references = 1;

	RuntimeEntry entry {};

	entry.handle = handle;

	entry.sourcePath = path;

	if (expectedModelName)
		entry.expectedModelName = expectedModelName;

	loaded_.emplace(
		customModelId,
		std::move(entry));

	outHandle = handle;

	return Result::Success;
}

Result CollisionLoader::Attach(
	Handle& handle,
	std::string& outError)
{
	std::lock_guard lock(mutex_);

	if (!handle.collision) {
		outError = "Collision handle has no CColModel.";

		return Result::NotLoaded;
	}

	if (handle.installed)
		return Result::AlreadyLoaded;

	if (handle.gtaModelId < 0 || handle.gtaModelId >= GTA_MODEL_INFO_COUNT) {
		outError = "Invalid GTA target model ID.";

		return Result::TargetModelNotFound;
	}

	auto* modelInfo = GetEngineModelInfo(
		handle.gtaModelId);

	if (!modelInfo) {
		outError = "Target GTA model-info does not exist.";

		return Result::TargetModelNotFound;
	}

	/*
	 * Refuse to steal an owned collision.
	 */
	if (modelInfo->m_pColModel && modelInfo->bDoWeOwnTheColModel) {
		outError = "Target model already owns collision.";

		return Result::TargetAlreadyOwned;
	}

	handle.previousCollision = modelInfo->m_pColModel;

	handle.previousOwned = modelInfo->bDoWeOwnTheColModel != 0;

	modelInfo->SetColModel(
		handle.collision,
		false);

	modelInfo->SetIsLod(1);

	if (modelInfo->m_pColModel != handle.collision) {
		modelInfo->SetColModel(
			handle.previousCollision,
			false);

		modelInfo->SetIsLod(int(handle.previousOwned));

		outError = "GTA rejected collision installation.";

		return Result::AttachFailed;
	}

	handle.installed = true;

	return Result::Success;
}

Result
CollisionLoader::Detach(
	Handle& handle,
	std::string& outError)
{
	std::lock_guard lock(mutex_);

	if (!handle.installed)
		return Result::NotLoaded;

	auto* modelInfo = GetEngineModelInfo(
		handle.gtaModelId);

	if (!modelInfo) {
		outError = "Target model-info no longer exists.";

		return Result::TargetModelNotFound;
	}

	if (modelInfo->m_pColModel != handle.collision) {
		/*
		 * Somebody else replaced it.
		 *
		 * Do not blindly restore our old pointer over somebody else's
		 * newer collision.
		 */
		outError = "Collision was replaced by another owner.";

		return Result::DetachFailed;
	}

	/*
	 * Detach the custom collision pointer first.
	 */
	modelInfo->SetColModel(
		handle.previousCollision,
		false);

	modelInfo->SetIsLod(int(handle.previousOwned));

	/*
	 * IMPORTANT:
	 *
	 * SetColModel may update ownership state. We intentionally do NOT
	 * call DeleteCollisionModel() here because ownership may have just
	 * been restored to another pointer.
	 */
	handle.installed = false;

	return Result::Success;
}

bool CollisionLoader::AddReference(
	CustomModelId customModelId)
{
	std::lock_guard lock(mutex_);

	auto it = loaded_.find(
		customModelId);

	if (it == loaded_.end())
		return false;

	++it->second.handle.references;
	return true;
}

bool CollisionLoader::RemoveReference(
	CustomModelId customModelId)
{
	std::lock_guard lock(mutex_);

	auto it = loaded_.find(
		customModelId);

	if (it == loaded_.end())
		return false;

	if (it->second.handle.references == 0)
		return false;

	--it->second.handle.references;

	return true;
}

Result
CollisionLoader::Release(
	CustomModelId customModelId,
	std::string& outError)
{
	std::lock_guard lock(mutex_);

	auto it = loaded_.find(
		customModelId);

	if (it == loaded_.end())
		return Result::NotLoaded;

	auto& handle = it->second.handle;

	if (handle.references > 0)
		--handle.references;

	if (handle.references != 0)
		return Result::Success;

	if (handle.installed) {
		/*
		 * We cannot safely call the public Detach() here because the
		 * mutex is already held.
		 */
		auto* modelInfo = GetEngineModelInfo(
			handle.gtaModelId);

		if (!modelInfo) {
			outError = "Target model-info disappeared.";

			return Result::TargetModelNotFound;
		}

		if (modelInfo->m_pColModel == handle.collision) {
			modelInfo->SetColModel(handle.previousCollision, false);
			modelInfo->SetIsLod(int(handle.previousOwned));
		} else {
			outError = "Collision ownership was lost.";

			return Result::DetachFailed;
		}

		handle.installed = false;
	}

	if (handle.collision) {
		delete handle.collision;
		handle.collision = nullptr;
	}

	loaded_.erase(it);

	return Result::Success;
}

Result
CollisionLoader::Unload(
	CustomModelId customModelId,
	std::string& outError)
{
	std::lock_guard lock(mutex_);

	auto it = loaded_.find(
		customModelId);

	if (it == loaded_.end())
		return Result::NotLoaded;

	auto& handle = it->second.handle;

	if (handle.references > 0) {
		outError = "Collision still has active references.";

		return Result::ReferenceStillActive;
	}

	if (handle.installed) {
		auto* modelInfo = GetEngineModelInfo(
			handle.gtaModelId);

		if (!modelInfo) {
			outError = "Target model-info disappeared.";

			return Result::TargetModelNotFound;
		}

		if (modelInfo->m_pColModel != handle.collision) {
			outError = "Collision no longer belongs to this loader.";

			return Result::DetachFailed;
		}

		modelInfo->SetColModel(
			handle.previousCollision,
			false);

		modelInfo->SetIsLod(int(handle.previousOwned));

		handle.installed = false;
	}

	delete handle.collision;
	handle.collision = nullptr;

	loaded_.erase(it);

	return Result::Success;
}

bool CollisionLoader::IsLoaded(
	CustomModelId customModelId) const
{
	std::lock_guard lock(mutex_);

	return loaded_.contains(
		customModelId);
}

std::uint32_t
CollisionLoader::GetReferenceCount(
	CustomModelId customModelId) const
{
	std::lock_guard lock(mutex_);

	auto it = loaded_.find(
		customModelId);

	if (it == loaded_.end())
		return 0;

	return it->second.handle.references;
}

std::optional<Statistics>
CollisionLoader::GetStatistics(
	CustomModelId customModelId) const
{
	std::lock_guard lock(mutex_);

	auto it = loaded_.find(
		customModelId);

	if (it == loaded_.end())
		return std::nullopt;

	return it->second.handle.statistics;
}

const char*
CollisionLoader::ResultToString(
	Result result) const
{
	switch (result) {
	case Result::Success:
		return "Success";

	case Result::AlreadyLoaded:
		return "AlreadyLoaded";

	case Result::NotLoaded:
		return "NotLoaded";

	case Result::NotInitialized:
		return "NotInitialized";

	case Result::FileNotFound:
		return "FileNotFound";

	case Result::OpenFailed:
		return "OpenFailed";

	case Result::EmptyFile:
		return "EmptyFile";

	case Result::FileTooLarge:
		return "FileTooLarge";

	case Result::TruncatedHeader:
		return "TruncatedHeader";

	case Result::BadMagic:
		return "BadMagic";

	case Result::UnsupportedVersion:
		return "UnsupportedVersion";

	case Result::InvalidRecordSize:
		return "InvalidRecordSize";

	case Result::RecordOutOfBounds:
		return "RecordOutOfBounds";

	case Result::InvalidModelName:
		return "InvalidModelName";

	case Result::NoRecords:
		return "NoRecords";

	case Result::ModelNotFoundInFile:
		return "ModelNotFoundInFile";

	case Result::TargetModelNotFound:
		return "TargetModelNotFound";

	case Result::TargetWrongType:
		return "TargetWrongType";

	case Result::TargetSharedModel:
		return "TargetSharedModel";

	case Result::TargetAlreadyOwned:
		return "TargetAlreadyOwned";

	case Result::AllocationFailed:
		return "AllocationFailed";

	case Result::GtaLoaderFailed:
		return "GtaLoaderFailed";

	case Result::AttachFailed:
		return "AttachFailed";

	case Result::DetachFailed:
		return "DetachFailed";

	case Result::ReferenceStillActive:
		return "ReferenceStillActive";

	case Result::CollisionEmpty:
		return "CollisionEmpty";

	case Result::InternalError:
		return "InternalError";
	}

	return "Unknown";
}
}
