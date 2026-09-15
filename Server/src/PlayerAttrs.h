#pragma once

#include <unordered_map>
#include <mutex>

class PlayerAttrs
{
private:
	bool _hasExtendedVeh = false;

public:
	bool hasExtendedVeh() const { return this->_hasExtendedVeh; }
	void sethasExtendedVeh() { this->_hasExtendedVeh = true; }
	void Reset() { this->_hasExtendedVeh = false; }
};

class PlayerAttrsMap
{
private:
	mutable std::mutex m_mutex;
	std::unordered_map<int, PlayerAttrs> m_map;

public:
	PlayerAttrs& operator[](int playerid)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_map[playerid];
	}

	bool HasExtendedVeh(int playerid) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_map.find(playerid);
		return (it != m_map.end()) && it->second.hasExtendedVeh();
	}

	void SetExtendedVeh(int playerid, bool value = true)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (value)
			m_map[playerid].sethasExtendedVeh();
		else
			m_map[playerid].Reset();
	}

	void Reset(int playerid)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_map.erase(playerid);
	}
};

extern PlayerAttrsMap gPlayers;
