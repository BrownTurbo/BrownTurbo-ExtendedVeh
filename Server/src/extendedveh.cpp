#include "extendedveh.h"

#include "Actions.h"
#include "CPlayer.h"
#include "CustomVehicleBindingRegistry.h"
#include "CustomVehicleTransport.h"
#include "HandlingDefault.h"
#include "HandlingManager.h"
#include "Hooks.hpp"
#include "ModelTransferManager.h"
#include "Natives.h"
#include "PacketEnum.h"

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

	if (get() && get()->vehicles_)
	{
		return get()->vehicles_->get(vehicleid);
	}
	return nullptr;
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

	setAmxFunctions(pawn_component_->getAmxFunctions());
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
	}

	for (auto network : core_->getNetworks())
	{
		network->getInEventDispatcher().addEventHandler(this);
		network->getOutEventDispatcher().addEventHandler(this);
	}

	ModelTransferMgr::Initialize("models");
}

void ExtendedVehCompo::onAmxLoad(IPawnScript& script)
{
	NativeHookManager::Instance().LoadAMX(script.GetAMX());
	RegisterNativeHooks();

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

bool ExtendedVehCompo::onReceivePacket(IPlayer& peer, int id,
	NetworkBitStream& bs)
{
	if (id == (uint8_t)CHandlingPacketID::PKT_CHANDLING)
	{
		core_->logLn(LogLevel::Debug,
			"[ExtendedVeh] Received custom packet ID %d from player "
			"%d (size=%d)\n",
			id, peer.getID(), bs.GetNumberOfBytesUsed());
		if (bs.GetNumberOfUnreadBits() >= 8)
		{
			uint8_t action;
			if (!bs.Read(action))
				return false;

			Actions::Process((CustomVehAction)action, bs, peer);
		}
	}
	return true;
}

void ExtendedVehCompo::onFree(IComponent* component)
{
	if (component == pawn_component_)
		pawn_component_ = nullptr;
	else if (component == vehicles_)
		vehicles_ = nullptr;
	else if (component == this)
		core_->getEventDispatcher().removeEventHandler(this);

	core_->logLn(LogLevel::Message, "");
	core_->logLn(LogLevel::Message, " =======================================================================");
	core_->logLn(LogLevel::Message,
		"  ExtendedVehicles %d.%d.%d%s by Zorono) unloaded!",
		EXTVEH_VERSION_MAJOR, EXTVEH_VERSION_MINOR, EXTVEH_VERSION_PATCH,
		(EXTVEH_PHASE_DEV ? "-dev" : ""));
	core_->logLn(LogLevel::Message, " =======================================================================");
	core_->logLn(LogLevel::Message, "");
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
	gPlayers[playerid].Reset();
}

void ExtendedVehCompo::onPlayerConnect(IPlayer& player)
{
	core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnPlayerConnect");
	HandlingMgr::OnPlayerConnect(player);
}

void ExtendedVehCompo::onPlayerDisconnect(IPlayer& player,
	PeerDisconnectReason reason)
{
	int playerid = player.getID();
	gPlayers[playerid].Reset();
	HandlingMgr::OnPlayerDisconnect(player, reason);
	ModelTransferMgr::OnPlayerDisconnect(player);
}

void ExtendedVehCompo::onVehicleStreamIn(IVehicle& vehicle, IPlayer& player)
{
	core_->logLn(LogLevel::Debug, "[ExtendedVeh] OnVehicleStreamIn(%d,%d)",
		vehicle.getID(), player.getID());

	// Send handling modifications for this vehicle
	HandlingMgr::OnVehicleStreamIn(vehicle, player);

	const auto customModel = CustomVehicleBindingRegistry::Instance().Get(static_cast<uint16_t>(vehicle.getID()));
	if (!customModel)
		return;

	CustomVehicleTransport::SendVehicleBind(player, static_cast<uint16_t>(vehicle.getID()), *customModel);
}

void ExtendedVehCompo::onPoolEntryDestroyed(IVehicle& vehicle)
{
	CustomVehicleBindingRegistry::Instance().Unbind(static_cast<uint16_t>(vehicle.getID()));
}
