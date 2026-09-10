#include "CPlayer.h"

std::unordered_map<int, CPlayer> gPlayers;

void CPlayer::Reset()
{
	this->_hasExtendedVeh = false;
}
