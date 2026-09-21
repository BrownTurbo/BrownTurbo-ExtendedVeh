#pragma once

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <system_error>
#include <d3d9.h>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "ModelTransferClient.h"
#include "TransferConfig.h"
#include "utils.h"

#define D3D_VFUNCTIONS (119)
#define DEVICE_PTR (0xC97C28)
#define ENDSCENE_INDEX (42)
#define RESET_INDEX (16)

typedef HRESULT(__stdcall* _Reset)(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pp);
typedef HRESULT(__stdcall* _EndScene)(IDirect3DDevice9* pDevice);

class c_plugin {
public:
	c_plugin(HMODULE hmodule);
	~c_plugin();

	static void game_loop();
	static void shutdown_for_unload();

private:
	HMODULE hmodule;
};
inline bool g_bwasInitialized = false;
inline bool imGuiOn = false;
inline std::atomic<bool> g_windowVisible { false };
