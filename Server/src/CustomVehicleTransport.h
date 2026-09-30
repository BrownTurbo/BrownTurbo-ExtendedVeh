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
void SendVehicleLights(IPlayer& player, const CustomVeh::Protocol::VehicleLightsPacket& lights);
void SendVehicleWheel(IPlayer& player, const CustomVeh::Protocol::VehicleWheelPacket& wheel);
void SendVehicleOffsets(IPlayer& player, const CustomVeh::Protocol::VehicleOffsetsPacket& offsets);
void SendVehiclePlate(IPlayer& player, const CustomVeh::Protocol::VehiclePlatePacket& plate);
void SendVehiclePlateMesh(IPlayer& player, const CustomVeh::Protocol::VehiclePlateMeshPacket& pkt);
void SendModelPlateConfig(IPlayer& player, const CustomVeh::Protocol::ModelPlateConfigPacket& pkt);
void SendVehiclePlateTexture(IPlayer& player, const CustomVeh::Protocol::VehiclePlateTexturePacket& pkt);
void SendDebugMode(IPlayer& player, bool enabled);
}
