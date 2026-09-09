#pragma once

#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <subhook/subhook.h>
#include <subhook/subhook_private.h>

#include <sdk.hpp>
#include <Server\Components\Pawn\Impl\pawn_impl.hpp>

#include "extendedveh.h"

namespace NativeHook
{
using amx_native_fn_t = cell (*)(AMX*, cell*);

struct HookEntry
{
	int index;
	AMX* amx;
	subhook_t hook;
	amx_native_fn_t origFn;
	amx_native_fn_t trampoline;
};

struct Key
{
	AMX* amx;
	int index;

	bool operator==(const Key& other) const { return amx == other.amx && index == other.index; }
};

struct KeyHash
{
	size_t operator()(const Key& k) const
	{
		return std::hash<AMX*>()(k.amx) ^ (std::hash<int>()(k.index) << 1);
	}
};

class NativeHookManager
{
public:
	static NativeHookManager& Instance()
	{
		static NativeHookManager instance;
		return instance;
	}

	template <typename Lambda>
	void RegisterHookByName(const std::string& nativeName, Lambda handler)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pendingHooks_.emplace_back(nativeName, std::function<cell(AMX*, cell*, amx_native_fn_t)>(handler));
	}

	void LoadAMX(AMX* amx)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (hookedScripts_.contains(amx))
			return;
		hookedScripts_.insert(amx);
		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		if (!compo)
			return;
		ICore* core_ = compo->getCore();
		if (!core_)
			return;

		for (auto& hookData : pendingHooks_)
		{
			int index = findNativeIndex(amx, hookData.nativeName);
			if (index == -1)
			{
				core_->logLn(LogLevel::Error, "[ExtendedVeh] native %s not found in AMX.", hookData.nativeName.c_str());
				continue;
			}

			Key key { amx, index };
			if (activeHooks_.count(key))
				continue;

			// Get original native
			amx_native_fn_t orig = nullptr;
			if (amx_GetNative(amx, index, reinterpret_cast<char*>(&orig)) != AMX_ERR_NONE)
				continue;
			if (!orig)
				continue;

			subhook_t hook = subhook_new(reinterpret_cast<void*>(orig), reinterpret_cast<void*>(HookTrampoline), {});
			if (!hook)
			{
				core_->logLn(LogLevel::Error, "[ExtendedVeh] Failed to create hook for '%s'.", hookData.nativeName.c_str());
				continue;
			}
			subhook_install(hook);

			if (!subhook_is_installed(hook))
			{
				core_->logLn(LogLevel::Error, "[ExtendedVeh] Failed to install Hook: %s at index %d", hookData.nativeName.c_str(), index);
				subhook_free(hook);
				continue;
			}
			else
			{
				amx_native_fn_t tramp = reinterpret_cast<amx_native_fn_t>(subhook_get_trampoline(hook));
				activeHooks_[key] = { index, amx, hook, orig, tramp };
				g_hookMap[orig] = { hookData.handler, tramp };
				core_->logLn(LogLevel::Debug, "[ExtendedVeh] Hook installed: %s at index %d", hookData.nativeName.c_str(), index);
			}
		}
	}

	void UnloadAMX(AMX* amx)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		hookedScripts_.erase(amx);
		ExtendedVehCompo* compo = ExtendedVehCompo::get();
		if (!compo)
			return;
		ICore* core_ = compo->getCore();
		if (!core_)
			return;

		for (auto it = activeHooks_.begin(); it != activeHooks_.end();)
		{
			if (it->first.amx == amx)
			{
				subhook_remove(it->second.hook);
				subhook_free(it->second.hook);
				core_->logLn(LogLevel::Debug, "[ExtendedVeh] Hook removed: index %d", it->second.index);
				it = activeHooks_.erase(it);
			}
			else
				++it;
		}
	}

	amx_native_fn_t GetOrig(AMX* amx, int index)
	{
		if (!amx)
			return nullptr;
		std::lock_guard<std::mutex> lock(mutex_);
		const Key key { amx, index };
		const auto it = activeHooks_.find(key);
		if (it == activeHooks_.end())
			return nullptr;
		return it->second.origFn;
	}

private:
	NativeHookManager() = default;
	~NativeHookManager()
	{
		for (auto& kv : activeHooks_)
		{
			subhook_remove(kv.second.hook);
			subhook_free(kv.second.hook);
		}
	}

	NativeHookManager(const NativeHookManager&) = delete;
	NativeHookManager& operator=(const NativeHookManager&) = delete;

	struct PendingHook
	{
		std::string nativeName;
		std::function<cell(AMX*, cell*, amx_native_fn_t)> handler;
	};

	struct HookHandlerInfo
	{
		std::function<cell(AMX*, cell*, amx_native_fn_t)> handler;
		amx_native_fn_t trampoline;
	};

	std::vector<PendingHook> pendingHooks_;
	std::unordered_map<Key, HookEntry, KeyHash> activeHooks_;
	std::unordered_map<amx_native_fn_t, HookHandlerInfo> g_hookMap;
	std::mutex mutex_;
	std::unordered_set<AMX*> hookedScripts_;

	int findNativeIndex(AMX* amx, const std::string& name)
	{
		if (!amx)
			return -1;

		int num_natives = 0;
		if (amx_NumNatives(amx, &num_natives) != AMX_ERR_NONE)
			return -1;
		char native_name[64];
		for (int idx = 0; idx < num_natives; idx++)
		{
			native_name[0] = '\0';
			if (amx_GetNative(amx, idx, native_name) == AMX_ERR_NONE) {
				if(strcmp(native_name, name.c_str()) == 0) {
					return idx;
				}
			}
		}
		return -1;
	}

	static cell HookTrampoline(AMX* amx, cell* params)
	{
		auto& manager = Instance();
		std::lock_guard<std::mutex> lock(manager.mutex_);

		for (const auto& [orig, info] : manager.g_hookMap)
		{
			if (info.handler)
				return info.handler(amx, params, info.trampoline);
		}
		return 0;
	}
};
}
