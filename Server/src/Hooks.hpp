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
	cell (*origFn)(AMX*, cell*);
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

static std::unordered_map<amx_native_fn_t, std::function<cell(AMX*, cell*, amx_native_fn_t)>> g_hookMap;

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
		// Wrap the lambda into a std::function
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
			cell (*orig)(AMX*, cell*) = nullptr;
			amx_GetNative(amx, index, reinterpret_cast<char*>(&orig));

			// Store the lambda in the global map
			g_hookMap[orig] = hookData.handler;

			subhook_t hook = subhook_new(reinterpret_cast<void*>(orig), reinterpret_cast<void*>(HookTrampoline), {});
			subhook_install(hook);

			if (!subhook_is_installed(hook))
				core_->logLn(LogLevel::Error, "[ExtendedVeh] Failed to install Hook: %s at index %d", hookData.nativeName.c_str(), index);
			else
			{
				activeHooks_[key] = { index, amx, hook, orig };
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
		PendingHook(const std::string& n, std::function<cell(AMX*, cell*, amx_native_fn_t)> h)
			: nativeName(n)
			, handler(h)
		{
		}
	};

	std::vector<PendingHook> pendingHooks_;
	std::unordered_map<Key, HookEntry, KeyHash> activeHooks_;
	std::mutex mutex_;
	std::unordered_set<AMX*> hookedScripts_;

	int findNativeIndex(AMX* amx, const std::string& name)
	{
		int index = 0;
		AMX_NATIVE_INFO* nativeInfo = nullptr;
		while (amx_GetNative(amx, index, reinterpret_cast<char*>(&nativeInfo)) == AMX_ERR_NONE && nativeInfo)
		{
			if (strcmp(nativeInfo->name, name.c_str()) == 0)
				return index;
			index++;
		}
		return -1;
	}

	static cell HookTrampoline(AMX* amx, cell* params)
	{
		// The original function is stored as a hidden first param (or use another way to identify)
		auto orig = reinterpret_cast<amx_native_fn_t>(params[-1]);
		auto& func = g_hookMap[orig];
		return func(amx, params, orig);
	}
};
}
