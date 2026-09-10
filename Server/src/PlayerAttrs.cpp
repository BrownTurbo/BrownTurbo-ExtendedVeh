#include "PlayerAttrs.h"

std::unordered_map<int, PlayerAttrs> gPlayers;

void PlayerAttrs::Reset()
{
	this->_hasExtendedVeh = false;
}
