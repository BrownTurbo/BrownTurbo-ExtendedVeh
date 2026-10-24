#ifndef EXTVEHSVR_H
#define EXTVEHSVR_H

#include <Impl/network_impl.hpp>
#include <Impl/pool_impl.hpp>
#include <amx/amx.h>
#include <bit>
#include <cstring>
#include <cstdint>

inline float amx_ctof_impl(cell c) noexcept
{
	static_assert(sizeof(float) == sizeof(uint32_t), "Size of float and uint32_t must match");
	const uint32_t u = static_cast<uint32_t>(static_cast<ucell>(c));
#if defined(__cpp_lib_bit_cast)
	return std::bit_cast<float>(u);
#else
	float f;
	std::memcpy(&f, &u, sizeof(f));
	return f;
#endif
}

inline cell amx_ftoc_impl(float f) noexcept
{
	static_assert(sizeof(float) == sizeof(uint32_t), "Size of float and uint32_t must match");
#if defined(__cpp_lib_bit_cast)
	const uint32_t u = std::bit_cast<uint32_t>(f);
#else
	uint32_t u;
	std::memcpy(&u, &f, sizeof(u));
#endif
	return static_cast<cell>(u);
}

#undef amx_ctof
#define amx_ctof(c) (amx_ctof_impl(static_cast<cell>(c)))
#undef amx_ftoc
#define amx_ftoc(f) (amx_ftoc_impl(static_cast<float>(f)))
#include <pawn-natives/NativeFunc.hpp>
#include <limits>
#undef PAWN_NATIVE__DEFAULT_RETURN_cell
#define PAWN_NATIVE__DEFAULT_RETURN_cell(...) return (std::numeric_limits<cell>::min)()
#undef PAWN_NATIVE__DEFAULT_RETURN_float
#define PAWN_NATIVE__DEFAULT_RETURN_float(...) return (std::numeric_limits<float>::quiet_NaN)()
#define PAWN_NATIVES_HAS_FUNC
#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Vehicles/vehicle_components.hpp>
#include <Server/Components/Vehicles/vehicles.hpp>
#include <RakNet/bitstream.hpp>

#include <algorithm>
#include <vector>

#define EXTVEH_COMPAT_VERSION 0xA839F

#define IS_VALID_PLAYERID(playerid) \
	((playerid) >= 0 && (playerid) < MAX_PLAYERS)

using namespace Impl;

class ExtendedVehCompo final : public IComponent,
							   public PawnEventHandler,
							   public CoreEventHandler,
							   public NetworkInEventHandler,
							   public SingleNetworkInEventHandler,
							   public PoolEventHandler<IVehicle>,
							   public PlayerConnectEventHandler,
							   public PlayerSpawnEventHandler,
							   public VehicleEventHandler,
							   public PoolIDProvider
{
public:
	PROVIDE_UID(0xFBE076EB9EA67E4C);

	StringView componentName() const override;

	SemanticVersion componentVersion() const override;

	void onLoad(ICore* c) override;

	void onInit(IComponentList* components) override;

	void onReady() override;

	void onAmxLoad(IPawnScript& script) override;

	void onAmxUnload(IPawnScript& script) override;

	void onTick(Microseconds elapsed, TimePoint now) override;

	bool onReceivePacket(IPlayer& peer, int id, NetworkBitStream& bs) override;

	bool onReceive(IPlayer& peer, NetworkBitStream& bs) override;

	void onFree(IComponent* component) override;

	void reset() override;

	void free() override;

	void onIncomingConnection(IPlayer& player, StringView ipAddress, unsigned short port) override;

	void onPlayerConnect(IPlayer& player) override;

	void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) override;

	bool onPlayerRequestSpawn(IPlayer& player) override { return true; }
	void onPlayerSpawn(IPlayer& player) override;

	void onVehicleStreamIn(IVehicle& vehicle, IPlayer& player) override;
	void onPlayerEnterVehicle(IPlayer& player, IVehicle& vehicle, bool passenger) override;
	bool onVehiclePaintJob(IPlayer& player, IVehicle& vehicle, int paintJob) override;
	bool onVehicleMod(IPlayer& player, IVehicle& vehicle, int component) override;

	void SyncCustomVehicleToPlayer(IVehicle& vehicle, IPlayer& player);
	void SyncCustomVehiclesToPlayer(IPlayer& player);

	void onPoolEntryCreated(IVehicle& vehicle) override;

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
