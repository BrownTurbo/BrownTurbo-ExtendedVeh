#include "Actions.h"
#include "CHandlingStore.hpp"
#include "PlayerAttrs.h"
#include "CVehicleManager.hpp"
#include "HandlingManager.h"
#include "ModelTransferManager.h"
#include "extendedveh.h"

bool Actions::Process(CustomVehAction id, NetworkBitStream& bs, IPlayer& player)
{
	ExtendedVehCompo* compo = ExtendedVehCompo::get();
	ICore* core_ = compo ? compo->getCore() : nullptr;
	int playerid = player.getID();

	switch (id)
	{
	case ACTION_INIT:
	{
		uint32_t compat_ver;
		if (!bs.Read(compat_ver))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_INIT: Failed to read compat_ver from player %d", playerid);
			return false;
		}

		CustomVehActionPacket pkt(ACTION_INIT_RESPONSE);
		pkt.data.Write((uint32_t)EXTVEH_COMPAT_VERSION);

		if (compat_ver >= EXTVEH_COMPAT_VERSION)
		{
			pkt.data.Write(true);
			const bool alreadyAuthorized = gPlayers.HasExtendedVeh(playerid);
			gPlayers.SetExtendedVeh(playerid, true);
			if (core_)
			{
				core_->logLn(LogLevel::Message, "[ExtendedVeh] Player %d reports having chandling plugin (compat_ver=0x%X). Sent INIT_RESPONSE(allowed=true)", playerid, compat_ver);
			}
			player.sendPacket(Span<uint8_t>(pkt.data.GetData(), pkt.data.GetNumberOfBitsUsed()), 0, false);
			if (!alreadyAuthorized)
			{
				HandlingMgr::OnPlayerAuthorized(player);
			}
			return true;
		}
		else
		{
			pkt.data.Write(false);
			if (core_)
			{
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] Player %d rejected: version mismatch (client=0x%X, server=0x%X)", playerid, compat_ver, EXTVEH_COMPAT_VERSION);
			}
			player.sendPacket(Span<uint8_t>(pkt.data.GetData(), pkt.data.GetNumberOfBitsUsed()), 0, false);
			return true;
		}
		return true;
	}
	case ACTION_SET_PLAYER_HANDLING:
	{
		uint16_t playerId;
		uint8_t count;
		if (!bs.Read(playerId) || !bs.Read(count))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_SET_PLAYER_HANDLING: Truncated header from player %d", playerid);
			return false;
		}
		if (playerId != playerid)
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_SET_PLAYER_HANDLING: Player %d attempted to modify handling for player %d (rejected)", playerid, playerId);
			return false;
		}
		if (count > 64)
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_SET_PLAYER_HANDLING: Player %d sent excessive count %d (rejected)", playerid, count);
			return false;
		}

		for (int i = 0; i < count; ++i)
		{
			CHandlingAttrib attrib;
			CHandlingAttribType type;
			if (!bs.Read(attrib) || !bs.Read(type))
			{
				if (core_)
					core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_SET_PLAYER_HANDLING: Truncated entry %d from player %d", i, playerid);
				return false;
			}

			if (type == TYPE_FLOAT)
			{
				float v;
				if (bs.Read(v))
					HandlingMgr::SetPlayerHandling(playerId, attrib, v);
				else
					return false;
			}
			else if (type == TYPE_UINT)
			{
				unsigned int v;
				if (bs.Read(v))
					HandlingMgr::SetPlayerHandling(playerId, attrib, v);
				else
					return false;
			}
			else if (type == TYPE_BYTE)
			{
				uint8_t v;
				if (bs.Read(v))
					HandlingMgr::SetPlayerHandling(playerId, attrib, v);
				else
					return false;
			}
			else
			{
				if (core_)
					core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_SET_PLAYER_HANDLING: Unknown type %d for attrib %d from player %d", static_cast<int>(type), static_cast<int>(attrib), playerid);
				return false;
			}
		}
		return true;
	}
	case ACTION_RESET_PLAYER_HANDLING:
	{
		uint16_t playerId;
		if (!bs.Read(playerId))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_RESET_PLAYER_HANDLING: Truncated packet from player %d", playerid);
			return false;
		}
		if (playerId != playerid)
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_RESET_PLAYER_HANDLING: Player %d attempted to reset player %d (rejected)", playerid, playerId);
			return false;
		}
		HandlingMgr::ResetPlayerHandling(playerId);
		return true;
	}
	case ACTION_GET_VEHICLE_HANDLING:
	{
		uint16_t vehicleId;
		if (!bs.Read(vehicleId))
			return false;
		auto it = HandlingMgr::vehicleHandlings.find(vehicleId);
		if (it == HandlingMgr::vehicleHandlings.end() || it->second.handlingModMap.empty())
		{
			return false;
		}
		struct CustomVehActionPacket response(ACTION_SET_VEHICLE_HANDLING);
		response.data.Write(vehicleId);
		HandlingMgr::__WriteHandlingEntryToBitStream(&response.data, it->second);
		player.sendPacket(Span<uint8_t>(response.data.GetData(), response.data.GetNumberOfBitsUsed()), 0, true);
		return true;
	}
	case ACTION_GET_MODEL_HANDLING:
	{
		uint16_t modelId;
		if (!bs.Read(modelId))
			return false;

		if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelId))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_GET_MODEL_HANDLING: Invalid model ID %d from player %d", modelId, playerid);
			return false;
		}

		HandlingMgr::stHandlingEntry* entry = HandlingMgr::GetModelHandlingEntry(modelId);
		if (!entry || entry->handlingModMap.empty())
		{
			return false;
		}

		struct CustomVehActionPacket response(ACTION_SET_MODEL_HANDLING);
		response.data.Write(modelId);
		HandlingMgr::__WriteHandlingEntryToBitStream(&response.data, *entry);
		player.sendPacket(Span<uint8_t>(response.data.GetData(), response.data.GetNumberOfBitsUsed()), 0, true);
		return true;
	}
	case ACTION_GET_PLAYER_HANDLING:
	{
		uint16_t playerId;
		if (!bs.Read(playerId))
			return false;
		auto it = HandlingMgr::playerHandlings.find(playerId);
		if (it == HandlingMgr::playerHandlings.end() || it->second.handlingModMap.empty())
		{
			return false;
		}
		struct CustomVehActionPacket response(ACTION_SET_PLAYER_HANDLING);
		response.data.Write(playerId);
		HandlingMgr::__WriteHandlingEntryToBitStream(&response.data, it->second);
		player.sendPacket(Span<uint8_t>(response.data.GetData(), response.data.GetNumberOfBitsUsed()), 0, true);
		return true;
	}
	case ACTION_RESET_ALL:
	{
		uint16_t playerId;
		if (!bs.Read(playerId))
			return false;
		if (playerId != playerid)
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_RESET_ALL: Player %d attempted to reset all for player %d (rejected)", playerid, playerId);
			return false;
		}
		HandlingMgr::ResetAll(playerId);
		return true;
	}
	case ACTION_ASSET_REQUEST:
	{
		uint32_t modelId;
		uint8_t kindByte;
		if (!bs.Read(modelId) || !bs.Read(kindByte))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_ASSET_REQUEST: Malformed request from player %d", playerid);
			return false;
		}
		const auto kind = static_cast<ModelFileKind>(kindByte);
		if (!IsModelFileKind(kind) && !IsAudioFileKind(kind))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_ASSET_REQUEST: Invalid file kind %u from player %d", kindByte, playerid);
			return false;
		}
		if (!gPlayers.HasExtendedVeh(playerid))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_ASSET_REQUEST: Unauthorized player %d", playerid);
			return false;
		}
		if (core_)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] ACTION_ASSET_REQUEST: Player %d requested model %u %s %u",
				playerid, modelId, IsAudioFileKind(kind) ? "audio kind" : "model kind", kindByte);
		ModelTransferMgr::OnRequestFile(player, modelId, kind);
		return true;
	}
	case ACTION_ASSET_CANCEL:
	{
		uint32_t modelId;
		uint8_t kindByte;
		if (!bs.Read(modelId) || !bs.Read(kindByte))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_ASSET_CANCEL: Malformed cancel from player %d", playerid);
			return false;
		}
		const auto kind = static_cast<ModelFileKind>(kindByte);
		if (!IsModelFileKind(kind) && !IsAudioFileKind(kind))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_ASSET_CANCEL: Invalid file kind %u from player %d", kindByte, playerid);
			return false;
		}
		if (core_)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] ACTION_ASSET_CANCEL: Player %d canceled transfer for model %u %s %u",
				playerid, modelId, IsAudioFileKind(kind) ? "audio kind" : "model kind", kindByte);
		ModelTransferMgr::CancelTransfer(player, modelId, kind);
		return true;
	}
	case ACTION_ASSET_READY:
	{
		uint32_t modelId;
		uint8_t kindByte;
		uint8_t successByte = 0;
		if (!bs.Read(modelId) || !bs.Read(kindByte) || !bs.Read(successByte))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_ASSET_READY: Malformed ready report from player %d", playerid);
			return false;
		}
		const auto kind = static_cast<ModelFileKind>(kindByte);
		if (!IsModelFileKind(kind) && !IsAudioFileKind(kind))
		{
			if (core_)
				core_->logLn(LogLevel::Warning, "[ExtendedVeh] ACTION_ASSET_READY: Invalid file kind %u from player %d", kindByte, playerid);
			return false;
		}
		if (core_)
			core_->logLn(LogLevel::Debug, "[ExtendedVeh] ACTION_ASSET_READY: Player %d reported file stored for model %u %s %u (success=%d)",
				playerid, modelId, IsAudioFileKind(kind) ? "audio kind" : "model kind", kindByte, successByte);
		ModelTransferMgr::OnClientReportFileStored(player, modelId, kind, successByte != 0);
		return true;
	}
	default:
	{
		if (core_)
			core_->logLn(LogLevel::Warning, "[ExtendedVeh] Actions::Process: Unknown or unhandled action %d from player %d (packet bytes=%d)", static_cast<int>(id), playerid, bs.GetNumberOfBytesUsed());
		break;
	}
	}
	return false;
}
