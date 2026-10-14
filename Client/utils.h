#pragma once

#include <sampapi/CChat.h>
#include <sampapi/CGame.h>
#include <sampapi/CLocalPlayer.h>
#include <sampapi/CNetGame.h>
#include <sampapi/CPlayerPool.h>
#include <sampapi/CVehicle.h>
#include <sampapi/CVehiclePool.h>
#include <sampapi/sampapi.h>

#include <RakHook/rakhook.hpp>
#include <RakHook/samp.hpp>

#include <windows.h>
#include <Psapi.h>
#include <cstdint>
#include <filesystem>
#include <shlobj.h>
#include <variant>

namespace fs = std::filesystem;

enum class LogLevel : uint8_t {
	None = 0,
	Trace,
	Debug,
	Info,
	Warning,
	Error
};
using ClientLogLevel = LogLevel;

bool SendMsg(int color, const char* msg);
void ClientLog(const std::string& msg, LogLevel level = LogLevel::None);
inline void ClientLog(LogLevel level, const std::string& msg) { ClientLog(msg, level); }
void SetClientDebugMode(bool enabled);
bool IsClientDebugMode();
void ToggleSampCursor(bool enable);

using PlayerPoolVariant = std::variant<
	std::nullptr_t,
	sampapi::v037r1::CPlayerPool*,
	sampapi::v037r3::CPlayerPool*,
	sampapi::v037r5::CPlayerPool*,
	sampapi::v03dl::CPlayerPool*>;

using VehiclePoolVariant = std::variant<
	std::nullptr_t,
	sampapi::v037r1::CVehiclePool*,
	sampapi::v037r3::CVehiclePool*,
	sampapi::v037r5::CVehiclePool*,
	sampapi::v03dl::CVehiclePool*>;

PlayerPoolVariant GetPlayerPoolPtr();
bool MatchPlayerId(int playerId);
CVehicle* GetGameVehicleFromPool(uint16_t sampVehicleId);
bool GetVehiclePlateText(uint16_t sampVehicleId, char* outText, size_t maxLen);
bool UpdateSampVehiclePlateText(uint16_t sampVehicleId, const char* plateText);
bool IsGameInitialized();
uint16_t GetLocalPlayerId();
VehiclePoolVariant GetVehiclesPool();
bool IsVehicleStreamedForLocalPlayer(CVehicle* gtaVeh);
bool IsExecutableAddress(uintptr_t address);
bool IsInsideMainModule(uintptr_t address);
void* GtaAddress(uintptr_t gtaAddress);
bool LooksLikeFunctionEntry(uintptr_t address);
fs::path GetDocumentsDirectory();
fs::path GetSampCacheRoot();
std::string GetCurrentServerAddress();
std::string GetCurrentServerHash();
std::string Sha256HexOfBuffer(const unsigned char* data, unsigned int size);
bool IsRcVehicleModel(int modelIndex);
bool IsHeavyVehicleModel(int modelIndex);

inline void UseCommonVehicleTexDictionary()
{
	// GTA SA 0x004C75A0 saves previous texture find callback into [0x00B4E6A0]
	// and sets CVehicleModelInfo::FindTextureCB (0x004C7510).
	// Prevent clobbering if already active.
	if (*reinterpret_cast<void**>(0x00B4E6A0) == nullptr) {
		reinterpret_cast<void(__cdecl*)()>(0x004C75A0)();
	}
}

inline void StopUsingCommonVehicleTexDictionary()
{
	// GTA SA 0x004C75C0 restores texture find callback from [0x00B4E6A0] and clears it to 0.
	// Only call if [0x00B4E6A0] != nullptr to avoid setting the callback to nullptr (which crashes on texture read).
	if (*reinterpret_cast<void**>(0x00B4E6A0) != nullptr) {
		reinterpret_cast<void(__cdecl*)()>(0x004C75C0)();
	}
}

struct CommonVehicleTexDictionaryGuard {
	bool m_active { false };

	CommonVehicleTexDictionaryGuard() {
		if (*reinterpret_cast<void**>(0x00B4E6A0) == nullptr) {
			UseCommonVehicleTexDictionary();
			m_active = true;
		}
	}

	~CommonVehicleTexDictionaryGuard() {
		if (m_active) {
			StopUsingCommonVehicleTexDictionary();
			m_active = false;
		}
	}

	CommonVehicleTexDictionaryGuard(const CommonVehicleTexDictionaryGuard&) = delete;
	CommonVehicleTexDictionaryGuard& operator=(const CommonVehicleTexDictionaryGuard&) = delete;
};

