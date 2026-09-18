#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

class CVehicle;

class CustomVehicleBindingManager {
public:
	struct Binding {
		uint16_t sampVehicleId {};
		uint32_t customModelId {};

		uint32_t gtaModelId {};
		int originalModelId { -1 };

		CVehicle* appliedGameVehicle { nullptr };
		bool modelApplied {};
	};

	static CustomVehicleBindingManager& Instance()
	{
		static CustomVehicleBindingManager instance;
		return instance;
	}

	void Bind(uint16_t vehicleId, uint32_t customModelId);

	void Unbind(uint16_t vehicleId);

	void Process();

	Binding* Find(uint16_t vehicleId);

	bool IsModelInUse(uint32_t customModelId);

private:
	std::mutex m_mutex;

	std::unordered_map<
		uint16_t,
		Binding>
		m_bindings;
};
