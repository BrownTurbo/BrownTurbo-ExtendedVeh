#include "extendedveh.h"

#include "Actions.h"
#include "PlayerAttrs.h"
#include "CustomVehicleBindingRegistry.h"
#include "CustomVehicleTransport.h"
#include "HandlingDefault.h"
#include "HandlingManager.h"
#include "Hooks.hpp"
#include "ModelTransferManager.h"
#include "Natives.h"
#include "PacketEnum.h"
#include "defs.h"

#include <pawn-natives/NativesMain.hpp>

ICore* core_ {};
IPawnComponent* pawn_component_ {};
IVehiclesComponent* vehicles_ = nullptr;

StringView ExtendedVehCompo::componentName() const { return "ExtendedVeh"; }

SemanticVersion ExtendedVehCompo::componentVersion() const
{
	return SemanticVersion(EXTVEH_VERSION_MAJOR, EXTVEH_VERSION_MINOR, EXTVEH_VERSION_PATCH, 0);
}

void ExtendedVehCompo::onLoad(ICore* c)
{
	core_ = c;

	getCore() = c;
	get() = this;

	core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
	core_->getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);

	HandlingDefault::Initialize();
	HandlingMgr::InitializeModelHandlings();

	core_->logLn(LogLevel::Message, "");
	core_->logLn(LogLevel::Message, " =======================================================================");
	core_->logLn(LogLevel::Message,
		"  ExtendedVehicles %d.%d.%d%s by Zorono loaded!",
		EXTVEH_VERSION_MAJOR, EXTVEH_VERSION_MINOR, EXTVEH_VERSION_PATCH,
		(EXTVEH_PHASE_DEV ? "-dev" : ""));
	core_->logLn(LogLevel::Message, " =======================================================================");
	core_->logLn(LogLevel::Message, "");
}

IVehicle* ExtendedVehCompo::GetVehicleByID(int vehicleid)
{
	if (!CVehicleMgr::IS_VALID_VEHICLEID(vehicleid))
		return nullptr;

	ExtendedVehCompo*& compo = get();
	if (!compo)
		return nullptr;
	IVehiclesComponent* vehicles = compo->vehicles_;
	if (!vehicles)
		return nullptr;
	return vehicles->get(vehicleid);
}

bool ExtendedVehCompo::IsValidVehicle(int vehicleid)
{
	return GetVehicleByID(vehicleid) != nullptr;
}

IPlayer* ExtendedVehCompo::GetPlayerByID(int playerid)
{
	if (!IS_VALID_PLAYERID(playerid))
		return nullptr;

	ICore* core = getCore();
	if (!core)
		return nullptr;

	const auto& players_ = core->getPlayers().players();
	for (auto it = players_.begin(); it != players_.end(); ++it)
	{
		IPlayer* player = *it;
		if (player && player->getID() == playerid)
		{
			return player;
		}
	}
	return nullptr;
}

namespace
{
static amx_GetAddr_t g_origAmxGetAddr = nullptr;

static int AMXAPI ExtendedVeh_AmxGetAddr(AMX* amx, cell amx_addr, cell** phys_addr)
{
	if (!amx || !phys_addr)
		return AMX_ERR_PARAMS;
	AMX_HEADER* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
	if (hdr && hdr->magic == 0xf1e0)
	{
		unsigned char* data = (amx->data != nullptr) ? amx->data : (reinterpret_cast<unsigned char*>(amx->base) + hdr->dat);
		*phys_addr = reinterpret_cast<cell*>(data + static_cast<uint32_t>(amx_addr));
		return AMX_ERR_NONE;
	}
	if (g_origAmxGetAddr)
	{
		return g_origAmxGetAddr(amx, amx_addr, phys_addr);
	}
	return AMX_ERR_PARAMS;
}
}

void ExtendedVehCompo::onInit(IComponentList* components)
{
	StringView name = componentName();
	pawn_component_ = components->queryComponent<IPawnComponent>();
	if (!pawn_component_)
	{
		core_->logLn(LogLevel::Error, "Error loading component %s: Pawn component not loaded", name.data());
		return;
	}

	core_->getEventDispatcher().addEventHandler(this);

	auto amxFuncs = pawn_component_->getAmxFunctions();
	g_origAmxGetAddr = reinterpret_cast<amx_GetAddr_t>(amxFuncs[AMX_FUNC_GetAddr]);
	amxFuncs[AMX_FUNC_GetAddr] = reinterpret_cast<void*>(&ExtendedVeh_AmxGetAddr);
	setAmxFunctions(amxFuncs);
	setAmxLookups(core_);
	setAmxLookups(components);

	if (pawn_component_)
	{
		pawn_component_->getEventDispatcher().addEventHandler(this);
		AMX_EXPORTS_DTA = const_cast<void**>(pawn_component_->getAmxFunctions().data());
	}

	vehicles_ = components->queryComponent<IVehiclesComponent>();
	if (!vehicles_)
	{
		core_->logLn(LogLevel::Error, "Error loading component %s: Vehicles component not loaded", name.data());
		return;
	}
	if (vehicles_)
	{
		vehicles_->getPoolEventDispatcher().addEventHandler(this);
		vehicles_->getEventDispatcher().addEventHandler(this);
	}

	ModelTransferMgr::Initialize("models");
	RegisterNativeHooks();
}

void ExtendedVehCompo::onReady()
{
	size_t netCount = 0;
	for (auto network : core_->getNetworks())
	{
		network->getInEventDispatcher().addEventHandler(this, EventPriority_FairlyHigh);
		network->getPerPacketInEventDispatcher().addEventHandler(this, (uint8_t)ExtendedVehPacketID::PKT_EXTVEH, EventPriority_FairlyHigh);
		netCount++;
	}
	core_->logLn(LogLevel::Message, "[ExtendedVeh] onReady: Registered inEvent handler on %zu network(s)", netCount);
}

void ExtendedVehCompo::onAmxLoad(IPawnScript& script)
{
	NativeHookManager::Instance().LoadAMX(script.GetAMX());

	AMX* amx = static_cast<AMX*>(script.GetAMX());
	if (amx != nullptr)
	{
		pawn_natives::AmxLoad(amx);
	}
};

void ExtendedVehCompo::onAmxUnload(IPawnScript& script)
{
	NativeHookManager::Instance().UnloadAMX(script.GetAMX());
};

void ExtendedVehCompo::onTick(Microseconds elapsed, TimePoint now)
{
	HandlingMgr::ProcessTick();
	ModelTransferMgr::ProcessTick();
}

bool ExtendedVehCompo::onReceive(IPlayer& peer, NetworkBitStream& bs)
{
	return onReceivePacket(peer, (uint8_t)ExtendedVehPacketID::PKT_EXTVEH, bs);
}

bool ExtendedVehCompo::onReceivePacket(IPlayer& peer, int id,
	NetworkBitStream& bs)
{
	if (id == (uint8_t)ExtendedVehPacketID::PKT_EXTVEH)
	{
		core_->logLn(LogLevel::Message,
			"[ExtendedVeh] Received custom packet ID %d from player %d (size=%d, unreadBits=%d)",
			id, peer.getID(), bs.GetNumberOfBytesUsed(), bs.GetNumberOfUnreadBits());

		if (bs.GetNumberOfUnreadBits() >= 8)
		{
			uint8_t action;
			if (!bs.Read(action))
			{
				core_->logLn(LogLevel::Warning,
					"[ExtendedVeh] Failed to read action byte from player %d packet",
					peer.getID());
				return false;
			}

			core_->logLn(LogLevel::Message,
				"[ExtendedVeh] Processing action %d from player %d",
				action, peer.getID());

			Actions::Process((CustomVehAction)action, bs, peer);
		}
		else
		{
			core_->logLn(LogLevel::Warning,
				"[ExtendedVeh] Custom packet from player %d has insufficient bits (%d)",
				peer.getID(), bs.GetNumberOfUnreadBits());
		}
		return false;
	}
	return true;
}

void ExtendedVehCompo::onFree(IComponent* component)
{
	if (component == pawn_component_)
		pawn_component_ = nullptr;
	else if (component == vehicles_)
	{
		if (vehicles_)
		{
			vehicles_->getPoolEventDispatcher().removeEventHandler(this);
			vehicles_->getEventDispatcher().removeEventHandler(this);
		}
		vehicles_ = nullptr;
	}
	else if (component == this)
	{
		core_->getEventDispatcher().removeEventHandler(this);
		core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
		core_->getPlayers().getPlayerSpawnDispatcher().removeEventHandler(this);
		for (auto network : core_->getNetworks())
		{
			network->getInEventDispatcher().removeEventHandler(this);
			network->getPerPacketInEventDispatcher().removeEventHandler(this, (uint8_t)ExtendedVehPacketID::PKT_EXTVEH);
		}
	}
}

void ExtendedVehCompo::reset() { }

void ExtendedVehCompo::free() { delete this; }

ICore*& ExtendedVehCompo::getCore()
{
	static ICore* core {};

	return core;
}

ExtendedVehCompo*& ExtendedVehCompo::get()
{
	static ExtendedVehCompo* component {};

	return component;
}

COMPONENT_ENTRY_POINT()
{
	return new ExtendedVehCompo();
}

void ExtendedVehCompo::onIncomingConnection(IPlayer& player,
	StringView ipAddress,
	unsigned short port)
{
	int playerid = player.getID();
	if (core_)
	{
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] onIncomingConnection: playerid=%d", playerid);
	}
	gPlayers.Reset(playerid);
}

void ExtendedVehCompo::onPlayerConnect(IPlayer& player)
{
	int playerid = player.getID();
	if (core_)
	{
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnPlayerConnect: playerid=%d", playerid);
	}
	gPlayers.Reset(playerid);
	HandlingMgr::ResetPlayerHandling(playerid);
}

void ExtendedVehCompo::onPlayerDisconnect(IPlayer& player,
	PeerDisconnectReason reason)
{
	int playerid = player.getID();
	if (core_)
	{
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] onPlayerDisconnect: playerid=%d, reason=%d", playerid, static_cast<int>(reason));
	}
	gPlayers.Reset(playerid);
	HandlingMgr::OnPlayerDisconnect(player, reason);
	ModelTransferMgr::OnPlayerDisconnect(player);
}

void ExtendedVehCompo::onPlayerSpawn(IPlayer& player)
{
	int playerid = player.getID();
	if (core_)
	{
		core_->logLn(LogLevel::Debug, "[ExtendedVeh] onPlayerSpawn: playerid=%d", playerid);
	}

	if (!gPlayers.HasExtendedVeh(playerid))
		return;

	SyncCustomVehiclesToPlayer(player);
}

void ExtendedVehCompo::SyncCustomVehicleToPlayer(IVehicle& vehicle, IPlayer& player)
{
	if (!gPlayers.HasExtendedVeh(player.getID()))
		return;

	const uint16_t vId = static_cast<uint16_t>(vehicle.getID());
	const auto customModel = CustomVehicleBindingRegistry::Instance().Get(vId);
	if (!customModel)
		return;

	CustomVehicleTransport::SendVehicleBind(player, vId, *customModel);

	auto stanceOpt = CustomVehicleBindingRegistry::Instance().GetStance(vId);
	if (stanceOpt)
	{
		CustomVehicleTransport::SendVehicleStance(player, *stanceOpt);
	}

	auto extrasOpt = CustomVehicleBindingRegistry::Instance().GetExtras(vId);
	if (extrasOpt)
	{
		CustomVeh::Protocol::VehicleExtrasPacket pkt {};
		pkt.sampVehicleId = vId;
		pkt.extrasMask = *extrasOpt;
		CustomVehicleTransport::SendVehicleExtras(player, pkt);
	}

	int pj = -1;
	auto pjOpt = CustomVehicleBindingRegistry::Instance().GetPaintjob(vId);
	if (pjOpt)
		pj = *pjOpt;
	else
		pj = vehicle.getPaintJob();

	if (pj >= 0)
	{
		CustomVeh::Protocol::VehiclePaintjobPacket pkt {};
		pkt.sampVehicleId = vId;
		pkt.paintjobIndex = static_cast<int8_t>(pj);
		CustomVehicleTransport::SendVehiclePaintjob(player, pkt);
	}

	auto neonOpt = CustomVehicleBindingRegistry::Instance().GetNeon(vId);
	if (neonOpt)
	{
		CustomVehicleTransport::SendVehicleNeon(player, *neonOpt);
	}

	auto tintOpt = CustomVehicleBindingRegistry::Instance().GetWindowTint(vId);
	if (tintOpt)
	{
		CustomVehicleTransport::SendVehicleWindowTint(player, *tintOpt);
	}

	auto wcOpt = CustomVehicleBindingRegistry::Instance().GetWheelColor(vId);
	if (wcOpt)
	{
		CustomVehicleTransport::SendVehicleWheelColor(player, *wcOpt);
	}

	auto bfOpt = CustomVehicleBindingRegistry::Instance().GetBackfire(vId);
	if (bfOpt)
	{
		CustomVeh::Protocol::VehicleBackfirePacket pkt {};
		pkt.sampVehicleId = vId;
		pkt.enabled = *bfOpt ? 1 : 0;
		CustomVehicleTransport::SendVehicleBackfire(player, pkt);
	}

	auto hornOpt = CustomVehicleBindingRegistry::Instance().GetHorn(vId);
	if (hornOpt)
	{
		CustomVehicleTransport::SendVehicleHorn(player, *hornOpt);
	}

	auto sirenOpt = CustomVehicleBindingRegistry::Instance().GetSiren(vId);
	if (sirenOpt)
	{
		CustomVehicleTransport::SendVehicleSiren(player, *sirenOpt);
	}

	auto lightsOpt = CustomVehicleBindingRegistry::Instance().GetLights(vId);
	if (lightsOpt)
	{
		CustomVehicleTransport::SendVehicleLights(player, *lightsOpt);
	}

	auto wheelOpt = CustomVehicleBindingRegistry::Instance().GetWheel(vId);
	if (wheelOpt)
	{
		CustomVehicleTransport::SendVehicleWheel(player, *wheelOpt);
	}
}

void ExtendedVehCompo::SyncCustomVehiclesToPlayer(IPlayer& player)
{
	if (!vehicles_)
		return;

	if (!gPlayers.HasExtendedVeh(player.getID()))
		return;

	for (IVehicle* vehicle : *vehicles_)
	{
		if (!vehicle)
			continue;

		if (!vehicle->isStreamedInForPlayer(player))
			continue;

		HandlingMgr::OnVehicleStreamIn(*vehicle, player);
		SyncCustomVehicleToPlayer(*vehicle, player);
	}
}

void ExtendedVehCompo::onVehicleStreamIn(IVehicle& vehicle, IPlayer& player)
{
	core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnVehicleStreamIn(%d,%d)",
		vehicle.getID(), player.getID());

	// Send handling modifications for this vehicle
	HandlingMgr::OnVehicleStreamIn(vehicle, player);

	SyncCustomVehicleToPlayer(vehicle, player);
}

bool ExtendedVehCompo::onVehiclePaintJob(IPlayer& player, IVehicle& vehicle, int paintJob)
{
	uint16_t vId = static_cast<uint16_t>(vehicle.getID());
	if (CustomVehicleBindingRegistry::Instance().Get(vId))
	{
		CustomVehicleBindingRegistry::Instance().SetPaintjob(vId, static_cast<int8_t>(paintJob));
		CustomVeh::Protocol::VehiclePaintjobPacket pkt {};
		pkt.sampVehicleId = vId;
		pkt.paintjobIndex = static_cast<int8_t>(paintJob);
		if (core_)
		{
			for (IPlayer* p : core_->getPlayers().players())
			{
				if (p && gPlayers.HasExtendedVeh(p->getID()))
				{
					CustomVehicleTransport::SendVehiclePaintjob(*p, pkt);
				}
			}
		}
	}
	return true;
}

void ExtendedVehCompo::onPlayerEnterVehicle(IPlayer& player, IVehicle& vehicle, bool passenger)
{
	core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnPlayerEnterVehicle(veh=%d, player=%d, passenger=%d)",
		vehicle.getID(), player.getID(), passenger);

	// Ensure entering players (especially passengers) receive the vehicle's latest handling
	HandlingMgr::OnVehicleStreamIn(vehicle, player);

	if (gPlayers.HasExtendedVeh(player.getID()))
	{
		SyncCustomVehicleToPlayer(vehicle, player);
	}
}

void ExtendedVehCompo::onPoolEntryCreated(IVehicle& vehicle)
{
	HandlingMgr::OnCreateVehicle(vehicle.getID());
}

void ExtendedVehCompo::onPoolEntryDestroyed(IVehicle& vehicle)
{
	HandlingMgr::OnDestroyVehicle(vehicle.getID());
	uint16_t vId = static_cast<uint16_t>(vehicle.getID());
	if (CustomVehicleBindingRegistry::Instance().Get(vId).has_value())
	{
		CustomVehicleBindingRegistry::Instance().Unbind(vId);
		if (core_)
		{
			for (IPlayer* player : core_->getPlayers().players())
			{
				if (player && gPlayers.HasExtendedVeh(player->getID()))
				{
					CustomVehicleTransport::SendVehicleUnbind(*player, vId);
				}
			}
		}
	}
}
