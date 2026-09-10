#pragma once

#include <unordered_map>

#ifndef MAX_PLAYERS
#define MAX_PLAYERS 1000
#endif

class CPlayer
{
private:
	bool _hasExtendedVeh = false;

public:
	bool hasExtendedVeh() { return this->_hasExtendedVeh; };

	void sethasExtendedVeh() { this->_hasExtendedVeh = true; };

	void Reset();
};

extern std::unordered_map<int, CPlayer> gPlayers;
