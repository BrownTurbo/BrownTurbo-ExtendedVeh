#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>

namespace CVehicleMgr
{
inline constexpr uint32_t BASE_MODEL_START = 400;
inline constexpr uint32_t BASE_MODEL_END = 611;
inline constexpr uint32_t BASE_MAX_VEHICLE_MODELS = 212;
inline constexpr uint32_t CUSTOM_MODEL_START = 20000;
inline constexpr uint32_t DEFAULT_MAX_VEHICLES = 65535;
inline constexpr uint32_t MAX_NETWORK_VEHICLES = 65535;

inline constexpr bool IsBaseVehicleModel(uint32_t modelId) noexcept
{
	return modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END;
}

inline constexpr bool IsCustomVehicleModel(uint32_t modelId) noexcept
{
	return modelId >= CUSTOM_MODEL_START;
}

inline constexpr uint32_t GetBaseModelIndex(uint32_t modelId) noexcept
{
	return modelId - BASE_MODEL_START;
}

class VehicleRegistry
{
private:
	uint32_t m_maxVehicles { DEFAULT_MAX_VEHICLES };

	// Maps custom model IDs (e.g., 20000+) to a contiguous internal index
	// starting at 212
	std::unordered_map<uint32_t, uint32_t> m_customModelIndices;
	uint32_t m_nextCustomIndex { BASE_MAX_VEHICLE_MODELS };

	mutable std::shared_mutex m_mutex;

	// Strict Singleton enforcement
	VehicleRegistry() = default;

public:
	VehicleRegistry(const VehicleRegistry&) = delete;
	VehicleRegistry& operator=(const VehicleRegistry&) = delete;

	static VehicleRegistry& Get() noexcept
	{
		static VehicleRegistry instance;
		return instance;
	}

	void SetMaxVehiclesLimit(uint32_t limit) noexcept
	{
		std::unique_lock lock(m_mutex); // C++17 CTAD
		m_maxVehicles = limit;
	}

	uint32_t GetMaxVehiclesLimit() const noexcept
	{
		std::shared_lock lock(m_mutex);
		return m_maxVehicles;
	}

	bool IsValidVehicleID(uint32_t vehicleId) const noexcept
	{
		return (vehicleId >= 1 && vehicleId <= GetMaxVehiclesLimit());
	}

	bool IsValidVehicleModel(uint32_t modelId) const noexcept
	{
		if (modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END)
		{
			return true;
		}

		std::shared_lock lock(m_mutex);
		return m_customModelIndices.find(modelId) != m_customModelIndices.end();
	}

	uint32_t RegisterCustomModel(uint32_t modelId)
	{
		if (modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END)
		{
			return modelId - BASE_MODEL_START;
		}

		std::unique_lock lock(m_mutex);

		auto it = m_customModelIndices.find(modelId);
		if (it != m_customModelIndices.end())
		{
			return it->second;
		}

		uint32_t allocatedIndex = m_nextCustomIndex++;
		m_customModelIndices.emplace(modelId, allocatedIndex);
		return allocatedIndex;
	}

	void UnregisterCustomModel(uint32_t modelId)
	{
		std::unique_lock lock(m_mutex);
		m_customModelIndices.erase(modelId);
	}

	std::optional<uint32_t> GetModelIndex(uint32_t modelId) const noexcept
	{
		if (modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END)
		{
			return static_cast<uint32_t>(modelId - BASE_MODEL_START);
		}

		std::shared_lock lock(m_mutex);
		auto it = m_customModelIndices.find(modelId);
		if (it != m_customModelIndices.end())
		{
			return it->second;
		}

		return std::nullopt;
	}
};

inline bool IS_VALID_VEHICLEID(uint32_t id) noexcept
{
	return VehicleRegistry::Get().IsValidVehicleID(id);
}

inline bool IS_VALID_VEHICLE_MODEL(uint32_t modelid) noexcept
{
	return VehicleRegistry::Get().IsValidVehicleModel(modelid);
}

inline uint32_t VEHICLE_MODEL_INDEX(uint32_t modelid)
{
	auto index = VehicleRegistry::Get().GetModelIndex(modelid);
	if (!index.has_value())
	{
		throw std::out_of_range("Attempted to resolve handling index for an "
								"unregistered custom model.");
	}
	return index.value();
}

enum class VehicleCategory
{
	Automobile,
	Bike,
	Bmx,
	Quad,
	MonsterTruck,
	Boat,
	Plane,
	Helicopter,
	Train,
	Trailer,
	Unknown
};

inline const char* GetVehicleCategoryName(VehicleCategory cat) noexcept
{
	switch (cat)
	{
	case VehicleCategory::Automobile:
		return "Automobile";
	case VehicleCategory::Bike:
		return "Motorcycle";
	case VehicleCategory::Bmx:
		return "Bicycle";
	case VehicleCategory::Quad:
		return "Quad";
	case VehicleCategory::MonsterTruck:
		return "Monster Truck";
	case VehicleCategory::Boat:
		return "Boat";
	case VehicleCategory::Plane:
		return "Plane";
	case VehicleCategory::Helicopter:
		return "Helicopter";
	case VehicleCategory::Train:
		return "Train";
	case VehicleCategory::Trailer:
		return "Trailer";
	case VehicleCategory::Unknown:
	default:
		return "Unknown";
	}
}

inline VehicleCategory GetVehicleModelCategory(uint32_t modelId) noexcept
{
	switch (modelId)
	{
	// Planes (including RC Baron 464, Skimmer 460)
	case 460:
	case 464:
	case 476:
	case 511:
	case 512:
	case 513:
	case 519:
	case 520:
	case 553:
	case 577:
	case 592:
	case 593:
		return VehicleCategory::Plane;

	// Helicopters (including RC Raider 465, RC Goblin 501, Leviathan 417, Sea Sparrow 447)
	case 417:
	case 425:
	case 447:
	case 465:
	case 469:
	case 487:
	case 488:
	case 497:
	case 501:
	case 548:
	case 563:
		return VehicleCategory::Helicopter;

	// Boats
	case 430:
	case 446:
	case 452:
	case 453:
	case 454:
	case 472:
	case 473:
	case 484:
	case 493:
	case 595:
		return VehicleCategory::Boat;

	// Trains & Trams
	case 449:
	case 537:
	case 538:
	case 569:
	case 570:
	case 590:
		return VehicleCategory::Train;

	// Trailers
	case 435:
	case 450:
	case 584:
	case 591:
	case 606:
	case 607:
	case 608:
	case 610:
	case 611:
		return VehicleCategory::Trailer;

	// Bicycles
	case 481:
	case 509:
	case 510:
		return VehicleCategory::Bmx;

	// Bikes / Motorcycles
	case 448:
	case 461:
	case 462:
	case 463:
	case 468:
	case 521:
	case 522:
	case 523:
	case 581:
	case 586:
		return VehicleCategory::Bike;

	// Quads
	case 471:
		return VehicleCategory::Quad;

	// Monster Trucks
	case 444:
	case 556:
	case 557:
		return VehicleCategory::MonsterTruck;

	default:
		if (modelId >= BASE_MODEL_START && modelId <= BASE_MODEL_END)
			return VehicleCategory::Automobile;
		return VehicleCategory::Unknown;
	}
}

inline bool IsVehicleModelFlightCapable(uint32_t modelId) noexcept
{
	VehicleCategory cat = GetVehicleModelCategory(modelId);
	// Prohibited on: Planes, Helicopters, Trains, Trailers
	// Allowed on: Automobiles, Bikes, Quads, Monster Trucks, Boats
	return (cat != VehicleCategory::Plane && cat != VehicleCategory::Helicopter && cat != VehicleCategory::Train && cat != VehicleCategory::Trailer && cat != VehicleCategory::Unknown);
}

inline bool IsVehicleModelWaterDriveCapable(uint32_t modelId) noexcept
{
	VehicleCategory cat = GetVehicleModelCategory(modelId);
	// Prohibited on: Planes (including Skimmer), Helicopters (including Leviathan/SeaSparrow),
	// Boats (already boats!), Trains, Trailers, Bikes, Motorcycles (BMX included).
	// Allowed on: Automobiles, Quads, Monster Trucks.
	return (cat != VehicleCategory::Plane && cat != VehicleCategory::Helicopter && cat != VehicleCategory::Boat && cat != VehicleCategory::Train && cat != VehicleCategory::Trailer && cat != VehicleCategory::Bike && cat != VehicleCategory::Bmx && cat != VehicleCategory::Unknown);
}
}
