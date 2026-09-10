#include "CustomVehicleTransport.h"
#include "../../Shared/CustomVehicleProtocol.hpp"
#include <RakNet/bitstream.hpp>
#include "PacketEnum.h"

namespace CustomVehicleTransport
{
namespace
{
	// Mirrors CHandlingActionPacket's shape (packet id + action byte header)
	// but for CustomVeh::Protocol::Action instead of the v1 CHandlingAction enum -
	// every future v2 send (AssetBegin/Chunk/etc.) can reuse this.
	struct ScvActionPacket
	{
		NetworkBitStream data;

		explicit ScvActionPacket(CustomVeh::Protocol::Action action)
		{
			data.Write(static_cast<uint8_t>(ExtendedVehPacketID::PKT_EXTVEH));
			data.Write(static_cast<uint8_t>(action));
		}
	};
}

void SendVehicleBind(IPlayer& player, uint16_t sampVehicleId, uint32_t customModelId)
{
	CustomVeh::Protocol::VehicleBinding binding {};
	binding.protocol = CustomVeh::Protocol::PROTOCOL_VERSION;
	binding.sampVehicleId = sampVehicleId;
	binding.customModelId = customModelId;

	ScvActionPacket pkt(CustomVeh::Protocol::Action::CustomVehicleBind);
	pkt.data.Write(reinterpret_cast<const char*>(&binding), sizeof(binding));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBytesUsed()), 0, true);
}

void SendVehicleUnbind(IPlayer& player, uint16_t sampVehicleId)
{
	CustomVeh::Protocol::VehicleUnbinding unbinding {};
	unbinding.protocol = CustomVeh::Protocol::PROTOCOL_VERSION;
	unbinding.sampVehicleId = sampVehicleId;

	ScvActionPacket pkt(CustomVeh::Protocol::Action::CustomVehicleUnbind);
	pkt.data.Write(reinterpret_cast<const char*>(&unbinding), sizeof(unbinding));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBytesUsed()), 0, true);
}
};
