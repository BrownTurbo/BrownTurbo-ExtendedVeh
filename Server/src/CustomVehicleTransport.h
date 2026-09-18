#pragma once
#include <cstdint>
#include <span>
#include <sdk.hpp>
#include "../../Shared/CustomVehicleProtocol.hpp"

namespace CustomVehicleTransport
{
void SendVehicleBind(IPlayer& player, uint16_t sampVehicleId, uint32_t customModelId);
void SendVehicleUnbind(IPlayer& player, uint16_t sampVehicleId);
void SendVehicleStance(IPlayer& player, const CustomVeh::Protocol::VehicleStancePacket& stance);
void SendVehicleExtras(IPlayer& player, const CustomVeh::Protocol::VehicleExtrasPacket& extras);
void SendVehiclePaintjob(IPlayer& player, const CustomVeh::Protocol::VehiclePaintjobPacket& pj);
void SendVehicleNeon(IPlayer& player, const CustomVeh::Protocol::VehicleNeonPacket& neon);
void SendVehicleWindowTint(IPlayer& player, const CustomVeh::Protocol::VehicleWindowTintPacket& tint);
void SendVehicleWheelColor(IPlayer& player, const CustomVeh::Protocol::VehicleWheelColorPacket& wc);
void SendVehicleBackfire(IPlayer& player, const CustomVeh::Protocol::VehicleBackfirePacket& bf);
void SendVehicleHorn(IPlayer& player, const CustomVeh::Protocol::VehicleHornPacket& horn);
void SendVehicleSiren(IPlayer& player, const CustomVeh::Protocol::VehicleSirenPacket& siren);
}
