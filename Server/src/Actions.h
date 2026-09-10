#pragma once

#include <sdk.hpp>
#include <RakNet/bitstream.hpp>
#include "PacketEnum.h"
#include <Server/Components/Pawn/pawn.hpp>
#include <cstdint>

#include "../../Shared/CustomVehicleProtocol.hpp"

// action identifier is sent as single byte
using CustomVehAction = CustomVeh::Protocol::Action;

inline constexpr CustomVehAction ACTION_INIT = CustomVehAction::Init;
inline constexpr CustomVehAction ACTION_INIT_RESPONSE = CustomVehAction::InitResponse;
inline constexpr CustomVehAction ACTION_RESET_MODEL = CustomVehAction::ResetModel;
inline constexpr CustomVehAction ACTION_RESET_VEHICLE = CustomVehAction::ResetVehicle;
inline constexpr CustomVehAction ACTION_SET_VEHICLE_HANDLING = CustomVehAction::SetVehicleHandling;
inline constexpr CustomVehAction ACTION_SET_MODEL_HANDLING = CustomVehAction::SetModelHandling;
inline constexpr CustomVehAction ACTION_SET_PLAYER_HANDLING = CustomVehAction::SetPlayerHandling;
inline constexpr CustomVehAction ACTION_RESET_PLAYER_HANDLING = CustomVehAction::ResetPlayerHandling;
inline constexpr CustomVehAction ACTION_GET_VEHICLE_HANDLING = CustomVehAction::GetVehicleHandling;
inline constexpr CustomVehAction ACTION_GET_MODEL_HANDLING = CustomVehAction::GetModelHandling;
inline constexpr CustomVehAction ACTION_GET_PLAYER_HANDLING = CustomVehAction::GetPlayerHandling;
inline constexpr CustomVehAction ACTION_RESET_ALL = CustomVehAction::ResetAll;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_DEFINE = CustomVehAction::CustomVehicleDefine;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_BIND = CustomVehAction::CustomVehicleBind;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_UNBIND = CustomVehAction::CustomVehicleUnbind;
inline constexpr CustomVehAction ACTION_CUSTOM_VEHICLE_DESTROY = CustomVehAction::CustomVehicleDestroy;
inline constexpr CustomVehAction ACTION_ASSET_MANIFEST = CustomVehAction::AssetManifest;
inline constexpr CustomVehAction ACTION_ASSET_REQUEST = CustomVehAction::AssetRequest;
inline constexpr CustomVehAction ACTION_ASSET_RESUME = CustomVehAction::AssetResume;
inline constexpr CustomVehAction ACTION_ASSET_BEGIN = CustomVehAction::AssetBegin;
inline constexpr CustomVehAction ACTION_ASSET_CHUNK = CustomVehAction::AssetChunk;
inline constexpr CustomVehAction ACTION_ASSET_END = CustomVehAction::AssetEnd;
inline constexpr CustomVehAction ACTION_ASSET_VERIFIED = CustomVehAction::AssetVerified;
inline constexpr CustomVehAction ACTION_ASSET_CANCEL = CustomVehAction::AssetCancel;
inline constexpr CustomVehAction ACTION_ASSET_REJECTED = CustomVehAction::AssetRejected;
inline constexpr CustomVehAction ACTION_ASSET_READY = CustomVehAction::AssetReady;

struct CustomVehActionPacket
{
	NetworkBitStream data;

	CustomVehActionPacket(CustomVehAction actionID)
	{
		data.Write((uint8_t)ExtendedVehPacketID::PKT_EXTVEH);
		data.Write((uint8_t)actionID);
	}
};

namespace Actions
{
bool Process(CustomVehAction id, NetworkBitStream& bs, IPlayer& player);
}

enum class ModelFileKind : uint8_t
{
	Dff = 0,
	Txd = 1,
	Col = 2
};

inline constexpr int kFileTransferChannel = 1;
inline constexpr uint32_t kFileChunkSize = 4096;
inline constexpr uint32_t kChunksPerPlayerPerTick = 4;
