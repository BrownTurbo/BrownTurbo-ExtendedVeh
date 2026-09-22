#include "ImGuiOverlay.h"

HWND hWnd;
WNDPROC oWndProc = nullptr;
void* g_targetEndScene = nullptr;
void* g_targetReset = nullptr;
_EndScene oEndScene = nullptr;
_Reset oReset = nullptr;
std::atomic<bool> g_shutdownRequested = false;
bool g_wndProcHooked = false;
std::thread g_initializationThread;
IDirect3DDevice9* g_imguiDevice = nullptr;

static inline void LogImGuiStage(const char* stage)
{
	ClientLog(std::format("[Client] ImGui Stage: {}", stage));
}

static inline const char* GetModelFileKindName(ModelFileKind kind)
{
	switch (kind) {
	case ModelFileKind::Dff:
		return "DFF";

	case ModelFileKind::Txd:
		return "TXD";

	case ModelFileKind::Col:
		return "COL";
	}

	return "UNKNOWN";
}

void RenderTransferWindow()
{
	LogImGuiStage("RenderTransferWindow begin");

	ImGui::SetNextWindowSize(ImVec2(700.0f, 280.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Model Transfers", nullptr)) {
		ImGui::End();
		return;
	}

	auto transfers = ModelTransferClient::Instance().Snapshot();

	if (ImGui::BeginTable("transfers", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 60.0f);
		ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 110.0f);
		ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 150.0f);
		ImGui::TableHeadersRow();

		for (auto& t : transfers) {
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%u", t.modelId);

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(GetModelFileKindName(t.kind));

			ImGui::TableSetColumnIndex(2);
			if (t.fromCache) {
				ImGui::TextUnformatted("cached");
			} else {
				float fraction = t.compressedSize > 0
					? static_cast<float>(t.receivedBytes) / static_cast<float>(t.compressedSize)
					: 0.0f;
				ImGui::ProgressBar(fraction, ImVec2(-1, 0));
			}

			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%.1f / %.1f KB", t.receivedBytes / 1024.0f, t.compressedSize / 1024.0f);

			ImGui::TableSetColumnIndex(4);
			// compute ETA: (compressedSize - receivedBytes) / rate
			if (t.fromCache) {
				ImGui::TextUnformatted("-");
			} else {
				double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - t.startTime).count();
				std::string etaStr = "estimating";
				if (t.receivedBytes > 0 && elapsedSec > 0.001) {
					double rate = static_cast<double>(t.receivedBytes) / elapsedSec; // bytes/sec
					const double received = static_cast<double>((std::min)(t.receivedBytes, t.compressedSize));
					const double total = static_cast<double>(t.compressedSize);
					const double remaining = (std::max)(0.0, total - received);
					if (rate > 1.0 && remaining > 0.0) {
						int eta = static_cast<int>(std::ceil(remaining / rate)); // seconds
						int m = eta / 60;
						int s = eta % 60;
						char buf[32];
						if (m > 0)
							std::snprintf(buf, sizeof(buf), "%dm%02ds", m, s);
						else
							std::snprintf(buf, sizeof(buf), "%ds", s);
						etaStr = buf;
					} else if (remaining <= 0) {
						etaStr = "done";
					}
				}
				ImGui::TextUnformatted(etaStr.c_str());
			}

			ImGui::TableSetColumnIndex(5);
			// combine status, attempts, lastError
			std::string status = t.statusText;
			if (t.attempts > 0) {
				status += " ";
				status += "(" + std::to_string(t.attempts) + ")";
			}
			if (!t.lastError.empty()) {
				status += " - ";
				status += t.lastError;
			}
			ImGui::TextUnformatted(status.c_str());

			// Show a Retry button for failed transfers
			if (t.failed) {
				ImGui::SameLine();
				char buf[64];
				std::snprintf(buf, sizeof(buf), "Retry##%u_%u", t.modelId, static_cast<unsigned>(t.kind));
				if (ImGui::Button(buf)) {
					ModelTransferClient::Instance().ManualRetry(t.modelId, t.kind);
				}
			}
		}

		ImGui::EndTable();
	}

	ImGui::End();
}

HRESULT __stdcall hkReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pp)
{
	if (!oReset)
		return D3DERR_INVALIDCALL;

	const bool wasInitialized = g_bwasInitialized;
	if (!wasInitialized || pDevice != g_imguiDevice) {
		return oReset(pDevice, pp);
	}
	LogImGuiStage("Reset begin");

	ImGui_ImplDX9_InvalidateDeviceObjects();

	HRESULT result = oReset(pDevice, pp);
	if (SUCCEEDED(result))
		ImGui_ImplDX9_CreateDeviceObjects();
	LogImGuiStage(SUCCEEDED(result) ? "Reset success" : "Reset failed");
	g_bwasInitialized = wasInitialized && SUCCEEDED(result);

	return result;
}

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT u_msg, WPARAM w_param, LPARAM l_param)
{
	if (u_msg == WM_KEYDOWN && static_cast<int>(w_param) == TransferConfig::Instance().toggleKey) {
		g_windowVisible.store(!g_windowVisible.load(std::memory_order_relaxed), std::memory_order_relaxed);
		return 0;
	}

	if (g_windowVisible && g_bwasInitialized && ImGui::GetCurrentContext() != nullptr) {
		if (ImGui_ImplWin32_WndProcHandler(hwnd, u_msg, w_param, l_param)) {
			return 0;
		}
	}

	if (oWndProc) {
		return CallWindowProcA(oWndProc, hwnd, u_msg, w_param, l_param);
	}

	return DefWindowProcA(hwnd, u_msg, w_param, l_param);
}

HRESULT __stdcall hkEndScene(IDirect3DDevice9* pDevice)
{
	if (!g_bwasInitialized) {
		if (!pDevice)
			return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;

		LogImGuiStage("initialization begin");

		D3DDEVICE_CREATION_PARAMETERS d3dcp {};
		if (FAILED(pDevice->GetCreationParameters(&d3dcp)) || !IsWindow(d3dcp.hFocusWindow)) {
			LogImGuiStage("GetCreationParameters failed.");
			return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;
		}

		hWnd = d3dcp.hFocusWindow;
		g_imguiDevice = pDevice;

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();

		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		LogImGuiStage("Win32 backend init");

		if (!ImGui_ImplWin32_Init(hWnd)) {
			LogImGuiStage("ImGui_ImplWin32_Init failed.");

			ImGui::DestroyContext();

			g_imguiDevice = nullptr;
			g_bwasInitialized = false;
			return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;
		}

		LogImGuiStage("DX9 backend init");
		if (!ImGui_ImplDX9_Init(pDevice)) {
			LogImGuiStage("ImGui_ImplDX9_Init failed.");

			ImGui_ImplWin32_Shutdown();
			if (ImGui::GetCurrentContext() != nullptr)
				ImGui::DestroyContext();

			g_imguiDevice = nullptr;
			g_bwasInitialized = false;

			LogImGuiStage("initialization failed; all ImGui state rolled back.");
			return false;
		}

		LogImGuiStage("adding tahoma font.");
		ImGui::StyleColorsDark();

		ImVector<ImWchar> ranges;
		ImFontGlyphRangesBuilder builder;
		builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
		builder.BuildRanges(&ranges);

		ImFontConfig cfg {};
		cfg.OversampleH = 2;
		cfg.OversampleV = 2;

		ImFont* imFnt;
		imFnt = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 16.0f, &cfg, ranges.Data);
		io.Fonts->Build();
		io.FontDefault = imFnt;

		LogImGuiStage("tahoma font ready");

		if (!g_wndProcHooked) {
			SetLastError(ERROR_SUCCESS);

			const LONG_PTR previousWndProc = GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
			DWORD getError = GetLastError();

			if (previousWndProc == 0 && getError != ERROR_SUCCESS) {
				LogImGuiStage(std::format("GetWindowLongPtrW failed. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getError, std::system_category().message(static_cast<int>(getError))).c_str());
				ImGui_ImplDX9_Shutdown();
				ImGui_ImplWin32_Shutdown();
				if (ImGui::GetCurrentContext() != nullptr)
					ImGui::DestroyContext();

				g_imguiDevice = nullptr;
				g_bwasInitialized = false;

				LogImGuiStage("initialization failed; all ImGui state rolled back.");
				return false;
			}

			// ...
			oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
			const DWORD getWndProcError = GetLastError();
			g_wndProcHooked = (oWndProc != nullptr && getWndProcError == ERROR_SUCCESS);
			if (!g_wndProcHooked) {
				LogImGuiStage(std::format("GetWindowLongPtrW(GWLP_WNDPROC) failed. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getWndProcError, std::system_category().message(static_cast<int>(getWndProcError))).c_str());

				ImGui_ImplDX9_Shutdown();
				ImGui_ImplWin32_Shutdown();
				if (ImGui::GetCurrentContext() != nullptr)
					ImGui::DestroyContext();

				g_imguiDevice = nullptr;
				g_bwasInitialized = false;

				LogImGuiStage("initialization failed; all ImGui state rolled back.");

				const LONG_PTR restoreResult = SetWindowLongPtrW(hWnd, GWLP_WNDPROC, previousWndProc);
				getError = GetLastError();
				if (restoreResult == 0 && getError != ERROR_SUCCESS) {
					ClientLog(std::format("Failed to restore WndProc. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getError, std::system_category().message(static_cast<int>(getError))));
				} else {
					LogImGuiStage("Original WndProc restored.");
				}
				return false;
			}
			else
			{
				const LONG_PTR installedWndProc = GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
				LogImGuiStage(std::format("WndProc hooked successfully. HWND=0x{:X}, oldWndProc=0x{:X}, newWndProc=0x{:X}", reinterpret_cast<std::uintptr_t>(hWnd), static_cast<std::uintptr_t>(reinterpret_cast<uintptr_t>(oWndProc)), static_cast<uintptr_t>(installedWndProc)).c_str());
			}
		}

		LogImGuiStage("initialization complete");

		g_windowVisible.store(false, std::memory_order_relaxed);
		g_bwasInitialized = true;
	}

	if (ImGui::GetCurrentContext() != nullptr && g_windowVisible.load(std::memory_order_acquire)) {
		IDirect3DStateBlock9* stateBlock = nullptr;
		if (pDevice->CreateStateBlock(D3DSBT_ALL, &stateBlock) == D3D_OK && stateBlock) {
			stateBlock->Capture();
		}

		LogImGuiStage("frame begin");

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		RenderTransferWindow();

		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

		LogImGuiStage("frame end");

		if (stateBlock) {
			LogImGuiStage("stateBlock apply");
			stateBlock->Apply();
			LogImGuiStage("stateBlock release");
			stateBlock->Release();
		}
	}
	return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;
}

void BackgroundInitializationWorker()
{
	DWORD deviceAddr = 0;
	int attempts = 0;
	while (!g_shutdownRequested && (deviceAddr = *(DWORD*)DEVICE_PTR) == 0 && attempts < 300) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		++attempts;
	}

	if (deviceAddr != 0) {
		void** vTableDevice = *(void***)(deviceAddr);
		g_targetEndScene = vTableDevice[ENDSCENE_INDEX];
		g_targetReset = vTableDevice[RESET_INDEX];

		MH_STATUS statusES = MH_CreateHook(g_targetEndScene, reinterpret_cast<void*>(&hkEndScene), reinterpret_cast<void**>(&oEndScene));
		if (statusES == MH_OK) {
			MH_STATUS enableStatus =MH_EnableHook(g_targetEndScene);
			if (enableStatus == MH_OK) {
				ClientLog("[Client] Direct3D 9 hkEndScene hooked successfully via MinHook");
			} else {
				ClientLog(std::format("[Client] Failed to enable Direct3D 9 hkEndScene hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(std::format("[Client] Failed to hook Direct3D 9 hkEndScene via MinHook: {}", MH_StatusToString(statusES)));
		}

		MH_STATUS statusReset = MH_CreateHook(g_targetReset, reinterpret_cast<void*>(&hkReset), reinterpret_cast<void**>(&oReset));
		if (statusReset == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(g_targetReset);
			if (enableStatus == MH_OK) {
				ClientLog("[Client] Direct3D 9 hkReset hooked successfully via MinHook");
			} else {
				ClientLog(std::format("[Client] Failed to enable Direct3D 9 hkReset hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(std::format("[Client] Failed to hook Direct3D 9 hkReset via MinHook: {}", MH_StatusToString(statusReset)));
		}
	} else {
		ClientLog("[Client] Failed to hook Direct3D 9: DEVICE_PTR was 0 after timeout");
	}
}

void c_plugin::game_loop()
{
	static bool threadSpawned = false;
	if (!threadSpawned) {
		threadSpawned = true;
		g_initializationThread = std::thread(BackgroundInitializationWorker);
	}
}

void c_plugin::shutdown_for_unload()
{
	g_shutdownRequested = true;
	if (g_initializationThread.joinable())
		g_initializationThread.join();

	if (g_targetEndScene) {
		MH_DisableHook(g_targetEndScene);
		MH_RemoveHook(g_targetEndScene);
		g_targetEndScene = nullptr;
		oEndScene = nullptr;
	}

	if (g_targetReset) {
		MH_DisableHook(g_targetReset);
		MH_RemoveHook(g_targetReset);
		g_targetReset = nullptr;
		oReset = nullptr;
	}

	if (hWnd && g_wndProcHooked && oWndProc) {
		const WNDPROC result = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc)));
		const DWORD getWndProcError = GetLastError();
		g_wndProcHooked = (result != nullptr && getWndProcError == ERROR_SUCCESS);
		if (!g_wndProcHooked) {
			LogImGuiStage(std::format("GetWindowLongPtrW(GWLP_WNDPROC) failed. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getWndProcError, std::system_category().message(static_cast<int>(getWndProcError))).c_str());

			ImGui_ImplDX9_Shutdown();
			ImGui_ImplWin32_Shutdown();
			if (ImGui::GetCurrentContext() != nullptr)
				ImGui::DestroyContext();

			g_imguiDevice = nullptr;
			g_bwasInitialized = false;

			LogImGuiStage("initialization failed; all ImGui state rolled back.");
			return;
		}
		oWndProc = nullptr;
		g_wndProcHooked = false;
	}

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	if (ImGui::GetCurrentContext() != nullptr)
		ImGui::DestroyContext();
	g_imguiDevice = nullptr;
	g_bwasInitialized = false;
}

c_plugin::c_plugin(HMODULE hmodule)
	: hmodule(hmodule)
{
	c_plugin::game_loop();
}

c_plugin::~c_plugin()
{
	c_plugin::shutdown_for_unload();
}
