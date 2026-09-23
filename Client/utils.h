#pragma once

#include <sampapi/CChat.h>
#include <sampapi/CGame.h>
#include <sampapi/CLocalPlayer.h>
#include <sampapi/CNetGame.h>
#include <sampapi/CPlayerPool.h>
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

enum class LogLevel : uint8_t
{
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
std::string Sha256HexOfBuffer(const unsigned char* data, unsigned int size);
bool IsRcVehicleModel(int modelIndex);
bool IsHeavyVehicleModel(int modelIndex);
