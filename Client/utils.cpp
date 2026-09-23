#include "utils.h"
#include <plugin_sa.h>
#include "CryptoUtility.h"
#include <fstream>
#include <mutex>

static const char* LogLevelToString(LogLevel level)
{
	switch (level)
	{
	case LogLevel::Trace:
		return "TRACE";
	case LogLevel::Debug:
		return "DEBUG";
	case LogLevel::Warning:
		return "WARN";
	case LogLevel::Error:
		return "ERROR";
	case LogLevel::Info:
	case LogLevel::None:
	default:
		return "INFO";
	}
}

void ClientLog(const std::string& msg, LogLevel level)
{
	static std::mutex logMutex;
	std::lock_guard<std::mutex> lock(logMutex);

	if (level == LogLevel::None)
	{
		level = LogLevel::Info;
	}

	SYSTEMTIME st;
	GetLocalTime(&st);
	char prefix[48];
	snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u.%03u]<%s> ",
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
		LogLevelToString(level));

	std::ofstream log("brownturbo-extendedveh.log", std::ios::app);
	if (log.is_open()) {
		log << prefix << msg << "\n";
		log.flush();
	}
}

bool SendMsg(int color, const char* msg)
{
	rakhook::samp_ver version = rakhook::samp_version();
	bool _sent = false;
	switch (version) {
	case rakhook::samp_ver::v037r1: {
		SAMPAPI_EXPORT sampapi::v037r1::CChat*& p_chat = sampapi::v037r1::RefChat();
		_sent = true;
		p_chat->AddMessage(color, msg);
		break;
	}
	case rakhook::samp_ver::v037r31: {
		SAMPAPI_EXPORT sampapi::v037r3::CChat*& p_chat = sampapi::v037r3::RefChat();
		_sent = true;
		p_chat->AddMessage(color, msg);
		break;
	}
	case rakhook::samp_ver::v037r5: {
		SAMPAPI_EXPORT sampapi::v037r5::CChat*& p_chat = sampapi::v037r5::RefChat();
		_sent = true;
		p_chat->AddMessage(color, msg);
		break;
	}
	case rakhook::samp_ver::v03dlr1: {
		SAMPAPI_EXPORT sampapi::v03dl::CChat*& p_chat = sampapi::v03dl::RefChat();
		_sent = true;
		p_chat->AddMessage(color, msg);
		break;
	}
	default: {
		_sent = false;
		break;
	}
	}
	return _sent;
}

PlayerPoolVariant GetPlayerPoolPtr()
{
	rakhook::samp_ver version = rakhook::samp_version();
	switch (version) {
	case rakhook::samp_ver::v037r1: {
		auto* pNetGame = sampapi::v037r1::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pPlayer)
			return nullptr;
		return pNetGame->m_pPools->m_pPlayer;
	}
	case rakhook::samp_ver::v037r31: {
		auto* pNetGame = sampapi::v037r3::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pPlayer)
			return nullptr;
		return pNetGame->m_pPools->m_pPlayer;
	}
	case rakhook::samp_ver::v037r5: {
		auto* pNetGame = sampapi::v037r5::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pPlayer)
			return nullptr;
		return pNetGame->m_pPools->m_pPlayer;
	}
	case rakhook::samp_ver::v03dlr1: {
		auto* pNetGame = sampapi::v03dl::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pPlayer)
			return nullptr;
		return pNetGame->m_pPools->m_pPlayer;
	}
	default:
		return nullptr;
	}
}

bool MatchPlayerId(int playerId)
{
	PlayerPoolVariant poolVariant = GetPlayerPoolPtr();
	bool _matched = false;

	std::visit([playerId, &_matched](auto&& localPlayerInfo) {
		using T = std::decay_t<decltype(localPlayerInfo)>;
		if constexpr (std::is_same_v<T, std::nullptr_t>) {
			_matched = false;
		} else {
			if (localPlayerInfo) {
				if constexpr (std::is_same_v<T, sampapi::v03dl::CPlayerPool*>) {
					_matched = (localPlayerInfo->m_nLocalPlayerId == playerId);
				} else {
					_matched = (localPlayerInfo->m_localInfo.m_nId == playerId);
				}
			}
		}
	},
		poolVariant);

	return _matched;
}

CVehicle* GetGameVehicleFromPool(uint16_t sampVehicleId)
{
	rakhook::samp_ver ver = rakhook::samp_version();
	switch (ver) {
	case rakhook::samp_ver::v037r1: {
		auto* pNetGame = sampapi::v037r1::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return nullptr;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		return veh ? veh->m_pGameVehicle : nullptr;
	}
	case rakhook::samp_ver::v037r31: {
		auto* pNetGame = sampapi::v037r3::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return nullptr;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		return veh ? veh->m_pGameVehicle : nullptr;
	}
	case rakhook::samp_ver::v037r5: {
		auto* pNetGame = sampapi::v037r5::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return nullptr;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		return veh ? veh->m_pGameVehicle : nullptr;
	}
	case rakhook::samp_ver::v03dlr1: {
		auto* pNetGame = sampapi::v03dl::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return nullptr;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		return veh ? veh->m_pGameVehicle : nullptr;
	}
	default:
		return nullptr;
	}
}

bool GetVehiclePlateText(uint16_t sampVehicleId, char* outText, size_t maxLen)
{
	if (!outText || maxLen == 0)
		return false;

	rakhook::samp_ver version = rakhook::samp_version();
	switch (version) {
	case rakhook::samp_ver::v037r1: {
		auto* pNetGame = sampapi::v037r1::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return false;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		if (veh && veh->m_szLicensePlateText[0] != '\0') {
			strncpy_s(outText, maxLen, veh->m_szLicensePlateText, _TRUNCATE);
			return true;
		}
		break;
	}
	case rakhook::samp_ver::v037r31: {
		auto* pNetGame = sampapi::v037r3::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return false;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		if (veh && veh->m_szLicensePlateText[0] != '\0') {
			strncpy_s(outText, maxLen, veh->m_szLicensePlateText, _TRUNCATE);
			return true;
		}
		break;
	}
	case rakhook::samp_ver::v037r5: {
		auto* pNetGame = sampapi::v037r5::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return false;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		if (veh && veh->m_szLicensePlateText[0] != '\0') {
			strncpy_s(outText, maxLen, veh->m_szLicensePlateText, _TRUNCATE);
			return true;
		}
		break;
	}
	case rakhook::samp_ver::v03dlr1: {
		auto* pNetGame = sampapi::v03dl::RefNetGame();
		if (!pNetGame || !pNetGame->m_pPools || !pNetGame->m_pPools->m_pVehicle)
			return false;
		auto* veh = pNetGame->m_pPools->m_pVehicle->Get(sampVehicleId);
		if (veh && veh->m_szLicensePlateText[0] != '\0') {
			strncpy_s(outText, maxLen, veh->m_szLicensePlateText, _TRUNCATE);
			return true;
		}
		break;
	}
	default:
		break;
	}
	return false;
}

bool IsGameInitialized()
{
	rakhook::samp_ver version = rakhook::samp_version();
	bool _initialized = false;
	switch (version) {
	case rakhook::samp_ver::v037r1: {
		_initialized = (sampapi::v037r1::RefGame() != nullptr);
		break;
	}
	case rakhook::samp_ver::v037r31: {
		_initialized = (sampapi::v037r3::RefGame() != nullptr);
		break;
	}
	case rakhook::samp_ver::v037r5: {
		_initialized = (sampapi::v037r5::RefGame() != nullptr);
		break;
	}
	case rakhook::samp_ver::v03dlr1: {
		_initialized = (sampapi::v03dl::RefGame() != nullptr);
		break;
	}
	default: {
		_initialized = false;
		break;
	}
	}
	return _initialized;
}

uint16_t GetLocalPlayerId()
{
	rakhook::samp_ver version = rakhook::samp_version();
	uint16_t localId = 0xFFFF;
	switch (version) {
	case rakhook::samp_ver::v037r1: {
		SAMPAPI_EXPORT sampapi::v037r1::CNetGame* pNetGame = sampapi::v037r1::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pPlayer) {
			localId = pNetGame->m_pPools->m_pPlayer->m_localInfo.m_nId;
		}
		break;
	}
	case rakhook::samp_ver::v037r31: {
		SAMPAPI_EXPORT sampapi::v037r3::CNetGame* pNetGame = sampapi::v037r3::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pPlayer) {
			localId = pNetGame->m_pPools->m_pPlayer->m_localInfo.m_nId;
		}
		break;
	}
	case rakhook::samp_ver::v037r5: {
		SAMPAPI_EXPORT sampapi::v037r5::CNetGame* pNetGame = sampapi::v037r5::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pPlayer) {
			localId = pNetGame->m_pPools->m_pPlayer->m_localInfo.m_nId;
		}
		break;
	}
	case rakhook::samp_ver::v03dlr1: {
		SAMPAPI_EXPORT sampapi::v03dl::CNetGame* pNetGame = sampapi::v03dl::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pPlayer) {
			localId = pNetGame->m_pPools->m_pPlayer->m_nLocalPlayerId;
		}
		break;
	}
	default: {
		localId = 0xFFFF;
		break;
	}
	}
	return localId;
}

VehiclePoolVariant GetVehiclesPool()
{
	VehiclePoolVariant vehPool = nullptr;
	if (GetModuleHandleA("samp.dll") == nullptr)
		return vehPool;
	rakhook::samp_ver version = rakhook::samp_version();
	if (version == rakhook::samp_ver::unknown)
		return vehPool;
	switch (version) {
	case rakhook::samp_ver::v037r1: {
		SAMPAPI_EXPORT sampapi::v037r1::CNetGame* pNetGame = sampapi::v037r1::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pVehicle) {
			vehPool = pNetGame->m_pPools->m_pVehicle;
		}
		break;
	}
	case rakhook::samp_ver::v037r31: {
		SAMPAPI_EXPORT sampapi::v037r3::CNetGame* pNetGame = sampapi::v037r3::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pVehicle) {
			vehPool = pNetGame->m_pPools->m_pVehicle;
		}
		break;
	}
	case rakhook::samp_ver::v037r5: {
		SAMPAPI_EXPORT sampapi::v037r5::CNetGame* pNetGame = sampapi::v037r5::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pVehicle) {
			vehPool = pNetGame->m_pPools->m_pVehicle;
		}
		break;
	}
	case rakhook::samp_ver::v03dlr1: {
		SAMPAPI_EXPORT sampapi::v03dl::CNetGame* pNetGame = sampapi::v03dl::RefNetGame();
		if (pNetGame && pNetGame->m_pPools && pNetGame->m_pPools->m_pVehicle) {
			vehPool = pNetGame->m_pPools->m_pVehicle;
		}
		break;
	}
	default: {
		vehPool = nullptr;
		break;
	}
	}
	return vehPool;
}

bool IsVehicleStreamedForLocalPlayer(CVehicle* gtaVeh)
{
	if (!IsVehiclePointerValid(gtaVeh))
		return false;

	auto* localPed = FindPlayerPed();
	if (!localPed)
		return false;

	if (gtaVeh->m_nAreaCode != localPed->m_nAreaCode && gtaVeh->m_nAreaCode != 0 && localPed->m_nAreaCode != 0) {
		return false;
	}

	CVector pPos = localPed->GetPosition();
	CVector vPos = gtaVeh->GetPosition();
	float dx = vPos.x - pPos.x;
	float dy = vPos.y - pPos.y;
	float dz = vPos.z - pPos.z;
	float distSq = dx * dx + dy * dy + dz * dz;

	constexpr float kMaxStreamRadiusSq = 300.0f * 300.0f; // 300 meters
	if (distSq > kMaxStreamRadiusSq) {
		return false;
	}

	return true;
}

bool IsExecutableAddress(uintptr_t address)
{
	MEMORY_BASIC_INFORMATION mbi {};

	if (!VirtualQuery(
			reinterpret_cast<void*>(
				address),
			&mbi,
			sizeof(mbi))) {
		return false;
	}

	const auto protect = mbi.Protect;

	return mbi.State == MEM_COMMIT && !(protect & PAGE_NOACCESS) && !(protect & PAGE_GUARD) && (protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY);
}

bool IsInsideMainModule(uintptr_t address)
{
	HMODULE module = GetModuleHandleW(nullptr);

	if (!module)
		return false;

	MODULEINFO info {};

	if (!GetModuleInformation(
			GetCurrentProcess(),
			module,
			&info,
			sizeof(info))) {
		return false;
	}

	const auto begin = reinterpret_cast<uintptr_t>(
		info.lpBaseOfDll);

	const auto end = begin + info.SizeOfImage;

	return address >= begin && address < end;
}

void* GtaAddress(uintptr_t gtaAddress)
{
	const auto module = reinterpret_cast<
		uintptr_t>(
		GetModuleHandleW(nullptr));

	/*
	 * Plugin-SDK addresses are based on
	 * GTA SA US 1.0 image base 0x00400000.
	 */
	constexpr uintptr_t GTA_IMAGE_BASE = 0x00400000;

	return reinterpret_cast<void*>(module + (gtaAddress - GTA_IMAGE_BASE));
}

bool LooksLikeFunctionEntry(uintptr_t address)
{
	if (!address)
		return false;
	const uint8_t* code = reinterpret_cast<uint8_t*>(address);
	/*
	 * Obvious near jump.
	 */
	if (code[0] == 0xE9)
		return false;
	/*
	 * Short jump.
	 */
	if (code[0] == 0xEB)
		return false;
	return true;
}

namespace fs = std::filesystem;

fs::path GetDocumentsDirectory()
{
	PWSTR raw = nullptr;

	const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw);
	if (FAILED(hr) || !raw)
		return {};

	fs::path result(raw);
	CoTaskMemFree(raw);

	return result;
}

fs::path GetSampCacheRoot()
{
	fs::path documents = GetDocumentsDirectory();

	if (documents.empty())
		return {};

	fs::path cachePath = documents / "GTA San Andreas User Files" / "SAMP" / "cache";
	std::error_code ec;
	fs::create_directories(cachePath, ec);
	return cachePath;
}

std::string Sha256HexOfBuffer(const unsigned char* data, unsigned int size)
{
	std::string hash;
	if (!CryptoUtility::ComputeSHA256(data, static_cast<std::size_t>(size), hash)) {
		return {};
	}
	return hash;
}

bool IsRcVehicleModel(int modelIndex)
{
	switch (modelIndex) {
	case 441: // RC Bandit
	case 464: // RC Baron
	case 465: // RC Raider
	case 501: // RC Goblin
	case 564: // RC Tiger
	case 594: // RC Cam
		return true;
	default:
		return false;
	}
}

bool IsHeavyVehicleModel(int modelIndex)
{
	switch (modelIndex) {
	case 403: // Linerunner
	case 406: // Dumper
	case 407: // Firetruck
	case 408: // Trashmaster
	case 414: // Mule
	case 431: // Bus
	case 433: // Barracks
	case 437: // Coach
	case 443: // Packer
	case 444: // Monster
	case 455: // Flatbed
	case 456: // Yankee
	case 486: // Dozer
	case 498: // Boxville
	case 499: // Benson
	case 514: // Roadtrain
	case 515: // Tanker
	case 524: // Cement Truck
	case 532: // Combine Harvester
	case 544: // Firetruck Ladd
	case 556: // Monster A
	case 557: // Monster B
	case 573: // Dune
	case 578: // DFT-30
		return true;
	default:
		return false;
	}
}
