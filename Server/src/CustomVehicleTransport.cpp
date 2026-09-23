#include "CustomVehicleTransport.h"
#include "../../Shared/CustomVehicleProtocol.hpp"
#include <RakNet/bitstream.hpp>
#include "PacketEnum.h"
#include "Actions.h"

namespace CustomVehicleTransport
{
void SendVehicleBind(IPlayer& player, uint16_t sampVehicleId, uint32_t customModelId)
{
	CustomVeh::Protocol::VehicleBinding binding {};
	binding.sampVehicleId = sampVehicleId;
	binding.customModelId = customModelId;

	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::CustomVehicleBind);
	pkt.data.Write(reinterpret_cast<const char*>(&binding), sizeof(binding));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleUnbind(IPlayer& player, uint16_t sampVehicleId)
{
	CustomVeh::Protocol::VehicleUnbinding unbinding {};
	unbinding.sampVehicleId = sampVehicleId;

	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::CustomVehicleUnbind);
	pkt.data.Write(reinterpret_cast<const char*>(&unbinding), sizeof(unbinding));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleStance(IPlayer& player, const CustomVeh::Protocol::VehicleStancePacket& stance)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleStance);
	pkt.data.Write(reinterpret_cast<const char*>(&stance), sizeof(stance));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleExtras(IPlayer& player, const CustomVeh::Protocol::VehicleExtrasPacket& extras)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleExtras);
	pkt.data.Write(reinterpret_cast<const char*>(&extras), sizeof(extras));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehiclePaintjob(IPlayer& player, const CustomVeh::Protocol::VehiclePaintjobPacket& pj)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehiclePaintjob);
	pkt.data.Write(reinterpret_cast<const char*>(&pj), sizeof(pj));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleNeon(IPlayer& player, const CustomVeh::Protocol::VehicleNeonPacket& neon)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleNeon);
	pkt.data.Write(reinterpret_cast<const char*>(&neon), sizeof(neon));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleWindowTint(IPlayer& player, const CustomVeh::Protocol::VehicleWindowTintPacket& tint)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleWindowTint);
	pkt.data.Write(reinterpret_cast<const char*>(&tint), sizeof(tint));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleWheelColor(IPlayer& player, const CustomVeh::Protocol::VehicleWheelColorPacket& wc)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleWheelColor);
	pkt.data.Write(reinterpret_cast<const char*>(&wc), sizeof(wc));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleBackfire(IPlayer& player, const CustomVeh::Protocol::VehicleBackfirePacket& bf)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleBackfire);
	pkt.data.Write(reinterpret_cast<const char*>(&bf), sizeof(bf));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleHorn(IPlayer& player, const CustomVeh::Protocol::VehicleHornPacket& horn)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleHorn);
	pkt.data.Write(reinterpret_cast<const char*>(&horn), sizeof(horn));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleSiren(IPlayer& player, const CustomVeh::Protocol::VehicleSirenPacket& siren)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleSiren);
	pkt.data.Write(reinterpret_cast<const char*>(&siren), sizeof(siren));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleLights(IPlayer& player, const CustomVeh::Protocol::VehicleLightsPacket& lights)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleLights);
	pkt.data.Write(reinterpret_cast<const char*>(&lights), sizeof(lights));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}

void SendVehicleWheel(IPlayer& player, const CustomVeh::Protocol::VehicleWheelPacket& wheel)
{
	CustomVehActionPacket pkt(CustomVeh::Protocol::Action::SetVehicleWheel);
	pkt.data.Write(reinterpret_cast<const char*>(&wheel), sizeof(wheel));

	player.sendPacket(Span<uint8_t>(reinterpret_cast<uint8_t*>(pkt.data.GetData()), pkt.data.GetNumberOfBitsUsed()), 0, true);
}
};
