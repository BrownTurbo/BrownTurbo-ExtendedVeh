#ifndef EXTVEHSVR_H
#define EXTVEHSVR_H

#include <Impl/network_impl.hpp>
#include <Impl/pool_impl.hpp>
#include <pawn-natives/NativeFunc.hpp>
#define PAWN_NATIVES_HAS_FUNC
#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Vehicles/vehicle_components.hpp>
#include <Server/Components/Vehicles/vehicles.hpp>
#include <RakNet/bitstream.hpp>

#include <algorithm>
#include <vector>

#define EXTVEH_PHASE_DEV true
#define EXTVEH_VERSION_MAJOR 0
#define EXTVEH_VERSION_MINOR 0
#define EXTVEH_VERSION_PATCH 1

#define EXTVEH_COMPAT_VERSION 0x1001D

#define IS_VALID_PLAYERID(playerid) \
	(playerid >= 1 && playerid <= MAX_PLAYERS)

using namespace Impl;

class ExtendedVehCompo final : public IComponent,
							   public PawnEventHandler,
							   public CoreEventHandler,
							   public NetworkInEventHandler,
							   public NetworkOutEventHandler,
							   public PoolEventHandler<IVehicle>,
							   public PlayerConnectEventHandler,
							   public VehicleEventHandler,
							   public PoolIDProvider
{
public:
	PROVIDE_UID(0xFBE076EB9EA67E4C);

	StringView componentName() const override;

	SemanticVersion componentVersion() const override;

	void onLoad(ICore* c) override;

	void onInit(IComponentList* components) override;

	void onAmxLoad(IPawnScript& script) override;

	void onAmxUnload(IPawnScript& script) override;

	void onTick(Microseconds elapsed, TimePoint now) override;

	bool onReceivePacket(IPlayer& peer, int id, NetworkBitStream& bs) override;

	void onFree(IComponent* component) override;

	void reset() override;

	void free() override;

	void onIncomingConnection(IPlayer& player, StringView ipAddress, unsigned short port) override;

	void onPlayerConnect(IPlayer& player) override;

	void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) override;

	void onVehicleStreamIn(IVehicle& vehicle, IPlayer& player) override;

	void onPoolEntryDestroyed(IVehicle& vehicle) override;

	ICore*& getCore();
	static ExtendedVehCompo*& get();

	IVehicle* GetVehicleByID(int vehicleid);
	bool IsValidVehicle(int vehicleid);
	IPlayer* GetPlayerByID(int playerid);

private:
	ICore* core_ {};
	IPawnComponent* pawn_component_ {};
	IVehiclesComponent* vehicles_ = nullptr;

public:
	static std::vector<IVehicle*> VehicleStorage;
	void** AMX_EXPORTS_DTA = nullptr;
};
#endif
