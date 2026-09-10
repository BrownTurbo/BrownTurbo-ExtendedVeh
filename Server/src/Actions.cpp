#include "Actions.h"
#include "CHandlingStore.hpp"
#include "CPlayer.h"
#include "CVehicleManager.hpp"
#include "HandlingManager.h"
#include "ModelTransferManager.h"
#include "extendedveh.h"

namespace HandlingMgr
{
void __WriteHandlingEntryToBitStream(NetworkBitStream* bs, const struct stHandlingEntry entry);
}

bool Actions::Process(CustomVehAction id, NetworkBitStream& bs, IPlayer& player)
{
	switch (id)
	{
	case ACTION_INIT:
	{
		uint32_t compat_ver;
		if (!bs.Read(compat_ver))
			return false;

		CustomVehActionPacket pkt(ACTION_INIT_RESPONSE);
		pkt.data.Write((uint32_t)EXTVEH_COMPAT_VERSION);

		int playerid = player.getID();
		if (compat_ver >= EXTVEH_COMPAT_VERSION)
		{
			pkt.data.Write(true);
			gPlayers[playerid].sethasExtendedVeh();
			ExtendedVehCompo* compo = ExtendedVehCompo::get();
			ICore* core_ = compo->getCore();
			if (core_)
			{
				core_->logLn(LogLevel::Message, "[ExtendedVeh] Player %d reports having chandling plugin", playerid);
			}
			player.sendPacket(Span<uint8_t>(pkt.data.GetData(), pkt.data.GetNumberOfBytesUsed()), 0, false);
			HandlingMgr::OnPlayerConnect(player);
			return true;
		}
		else
		{
			pkt.data.Write(false);
			player.sendPacket(Span<uint8_t>(pkt.data.GetData(), pkt.data.GetNumberOfBytesUsed()), 0, false);
			return true;
		}
		return true;
	}
	case ACTION_SET_PLAYER_HANDLING:
	{
		uint16_t playerId;
		uint8_t count;
		if (!bs.Read(playerId) || !bs.Read(count))
			return false;
		for (int i = 0; i < count; ++i)
		{
			CHandlingAttrib attrib;
			CHandlingAttribType type;
			if (!bs.Read(attrib) || !bs.Read(type))
				return false;

			if (type == TYPE_FLOAT)
			{
				float v;
				if (bs.Read(v))
					HandlingMgr::SetPlayerHandling(playerId, attrib, v);
			}
			else if (type == TYPE_UINT)
			{
				unsigned int v;
				if (bs.Read(v))
					HandlingMgr::SetPlayerHandling(playerId, attrib, v);
			}
			else if (type == TYPE_BYTE)
			{
				uint8_t v;
				if (bs.Read(v))
					HandlingMgr::SetPlayerHandling(playerId, attrib, v);
			}
			else
			{
				return false;
			}
		}
		return true;
	}
	case ACTION_RESET_PLAYER_HANDLING:
	{
		uint16_t playerId;
		if (!bs.Read(playerId))
			return false;
		HandlingMgr::ResetPlayerHandling(playerId);
		return false;
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
		player.sendPacket(Span<uint8_t>(response.data.GetData(), response.data.GetNumberOfBytesUsed()), 0, true);
		return false;
	}
	case ACTION_GET_MODEL_HANDLING:
	{
		uint16_t modelId;
		if (!bs.Read(modelId))
			return false;

		if (!CVehicleMgr::IS_VALID_VEHICLE_MODEL(modelId))
		{
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
		player.sendPacket(Span<uint8_t>(response.data.GetData(), response.data.GetNumberOfBytesUsed()), 0, true);
		return false;
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
		player.sendPacket(Span<uint8_t>(response.data.GetData(), response.data.GetNumberOfBytesUsed()), 0, true);
		return false;
	}
	case ACTION_RESET_ALL:
	{
		uint16_t playerId;
		if (!bs.Read(playerId))
			return false;
		HandlingMgr::ResetAll(playerId);
		return false;
	}
	case ACTION_ASSET_REQUEST:
	{
		uint32_t modelId;
		uint8_t kindByte;
		if (!bs.Read(modelId) || !bs.Read(kindByte) || kindByte > static_cast<uint8_t>(ModelFileKind::Col))
			return false;
		ModelTransferMgr::OnRequestFile(player, modelId, static_cast<ModelFileKind>(kindByte));
		return true;
	}
	case ACTION_ASSET_CANCEL:
	{
		uint32_t modelId;
		uint8_t kindByte;
		if (!bs.Read(modelId) || !bs.Read(kindByte) || kindByte > static_cast<uint8_t>(ModelFileKind::Col))
			return false;
		ModelTransferMgr::CancelTransfer(player, modelId, static_cast<ModelFileKind>(kindByte));
		return true;
	}
	case ACTION_ASSET_READY:
	{
		uint32_t modelId;
		uint8_t kindByte;
		uint8_t successByte = 0;
		bs.Read(modelId);
		bs.Read(kindByte);
		bs.Read(successByte);
		ModelTransferMgr::OnClientReportFileStored(player, modelId, static_cast<ModelFileKind>(kindByte), successByte != 0);
		return true;
	}
	default:
		break;
	}
	return false;
}
