#pragma once
#include <sdk.hpp>
#include <RakNet/bitstream.hpp>
#include "Actions.h"
#include <cstdint>
#include <string>
#include <mutex>
#include <map>

namespace ModelTransferMgr
{
void Initialize(const std::string& modelsDirectory);

// Called from Actions::Process's ACTION_REQUEST_FILE_TRANSFER case.
void OnRequestFile(IPlayer& player, uint32_t modelId, ModelFileKind kind);

// Called from Actions::Process's ACTION_FILE_TRANSFER_CANCEL case, and from
// onPlayerDisconnect - either drops one in-flight transfer or all of a
// player's.
void CancelTransfer(IPlayer& player, uint32_t modelId, ModelFileKind kind);
void OnPlayerDisconnect(IPlayer& player);

// Called once per onTick - pumps up to kChunksPerPlayerPerTick chunks for
// every active transfer.
void ProcessTick();

// Invalidates the cached compressed bytes for one file - call this if you
// ever hot-swap a model's files on disk without restarting the server.
void InvalidateCache(uint32_t modelId, ModelFileKind kind);

// Called when a CLIENT reports that it successfully (or unsuccessfully)
void OnClientReportFileStored(IPlayer& player, uint32_t modelId, ModelFileKind kind, bool success);

// Query last-known per-player status. Returns:
// 0 = unknown/no report, 1 = success, 2 = failure.
int GetClientFileStoreStatus(int playerId, uint32_t modelId, ModelFileKind kind);
} // namespace ModelTransferMgr
