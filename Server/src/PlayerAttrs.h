#pragma once

#include <unordered_map>

class PlayerAttrs
{
private:
	bool _hasExtendedVeh = false;

public:
	bool hasExtendedVeh() { return this->_hasExtendedVeh; };
	void sethasExtendedVeh() { this->_hasExtendedVeh = true; };
	void Reset();
};

extern std::unordered_map<int, PlayerAttrs> gPlayers;
