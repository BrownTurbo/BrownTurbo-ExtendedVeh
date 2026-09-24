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

static inline void LogImGuiStage(const char* stage, LogLevel level = LogLevel::Debug)
{
	ClientLog(level, std::format("ImGui Stage: {}", stage));
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

	case ModelFileKind::AudioEngine:
		return "AUDIO_ENGINE";

	case ModelFileKind::AudioAccel:
		return "AUDIO_ACCEL";

	case ModelFileKind::AudioDecel:
		return "AUDIO_DECEL";

	case ModelFileKind::AudioBrake:
		return "AUDIO_BRAKE";

	case ModelFileKind::AudioCrash:
		return "AUDIO_CRASH";
	}

	return "UNKNOWN";
}

void RenderTransferWindow()
{
	LogImGuiStage("RenderTransferWindow begin");

	ImGuiIO& io = ImGui::GetIO();

	ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f);
	ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(740.0f, 320.0f), ImGuiCond_FirstUseEver);

	bool open = g_windowVisible.load(std::memory_order_relaxed);
	if (!ImGui::Begin("Asset Downloader", &open, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		if (!open) {
			ClientLog(LogLevel::Info, "RenderTransferWindow: window closed by user");
			g_windowVisible.store(false, std::memory_order_relaxed);
			g_manuallyToggled.store(true, std::memory_order_relaxed);
			io.MouseDrawCursor = false;
		}
		LogImGuiStage("RenderTransferWindow end (collapsed/hidden)");
		return;
	}

	if (!open) {
		ClientLog(LogLevel::Debug, "RenderTransferWindow: open == false");
		g_windowVisible.store(false, std::memory_order_relaxed);
		g_manuallyToggled.store(true, std::memory_order_relaxed);
		io.MouseDrawCursor = false;
		ImGui::End();
		LogImGuiStage("RenderTransferWindow end (closed)");
		return;
	}

	auto transfers = ModelTransferClient::Instance().Snapshot();
	ClientLog(LogLevel::Debug, std::format("RenderTransferWindow: rendering table with {} snapshot transfers", transfers.size()));

	// Summary stats
	size_t activeCount = 0;
	size_t cachedCount = 0;
	size_t failedCount = 0;
	size_t completedCount = 0;
	uint64_t totalBytesReceived = 0;
	uint64_t totalBytesExpected = 0;

	for (const auto& t : transfers) {
		if (t.fromCache) {
			cachedCount++;
		} else if (t.failed) {
			failedCount++;
		} else if (t.statusText == "done" || (t.compressedSize > 0 && t.receivedBytes >= t.compressedSize)) {
			completedCount++;
			totalBytesReceived += t.receivedBytes;
			totalBytesExpected += t.compressedSize;
		} else {
			activeCount++;
			totalBytesReceived += t.receivedBytes;
			totalBytesExpected += t.compressedSize;
		}
	}

	bool allDone = (activeCount == 0 && (completedCount > 0 || cachedCount > 0) && failedCount == 0);

	ImGui::Text("Active: %zu | Completed: %zu | Cached: %zu | Failed: %zu", activeCount, completedCount, cachedCount, failedCount);

	if (totalBytesExpected > 0) {
		float totalFraction = static_cast<float>(totalBytesReceived) / static_cast<float>(totalBytesExpected);
		if (totalFraction > 1.0f)
			totalFraction = 1.0f;
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.1f / %.1f KB (%.0f%%)", totalBytesReceived / 1024.0f, totalBytesExpected / 1024.0f, totalFraction * 100.0f);
		ImGui::ProgressBar(totalFraction, ImVec2(-1, 0), buf);
	}

	static auto s_allDoneTime = (std::chrono::steady_clock::time_point::min)();
	if (allDone) {
		if (s_allDoneTime == (std::chrono::steady_clock::time_point::min)()) {
			s_allDoneTime = std::chrono::steady_clock::now();
		}
		double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - s_allDoneTime).count();
		double remain = 4.0 - elapsedSec;
		if (remain > 0.0) {
			ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "All vehicle assets downloaded successfully! Closing in %.0f s (or press F9)...", std::ceil(remain));
		} else {
			if (!g_manuallyToggled.load(std::memory_order_relaxed)) {
				g_windowVisible.store(false, std::memory_order_relaxed);
				io.MouseDrawCursor = false;
				s_allDoneTime = (std::chrono::steady_clock::time_point::min)();
				ImGui::End();
				return;
			}
		}
	} else {
		s_allDoneTime = (std::chrono::steady_clock::time_point::min)();
		if (failedCount > 0) {
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Some transfers failed. Check network or click Retry below.");
		}
	}

	ImGui::Separator();

	if (transfers.empty()) {
		ImGui::TextDisabled("No active or recent vehicle transfers. Press F9 to hide.");
	} else {
		if (ImGui::BeginTable("transfers", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 180.0f))) {
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
					ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "cached");
				} else if (t.statusText == "done" || (t.compressedSize > 0 && t.receivedBytes >= t.compressedSize)) {
					ImGui::ProgressBar(1.0f, ImVec2(-1, 0), "100%");
				} else {
					float fraction = t.compressedSize > 0
						? static_cast<float>(t.receivedBytes) / static_cast<float>(t.compressedSize)
						: 0.0f;
					ImGui::ProgressBar(fraction, ImVec2(-1, 0));
				}

				ImGui::TableSetColumnIndex(3);
				if (t.fromCache) {
					ImGui::TextUnformatted("-");
				} else {
					ImGui::Text("%.1f / %.1f KB", t.receivedBytes / 1024.0f, t.compressedSize / 1024.0f);
				}

				ImGui::TableSetColumnIndex(4);
				if (t.fromCache) {
					ImGui::TextUnformatted("-");
				} else if (t.statusText == "done" || (t.compressedSize > 0 && t.receivedBytes >= t.compressedSize)) {
					ImGui::TextUnformatted("done");
				} else {
					double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - t.startTime).count();
					std::string etaStr = "estimating";
					if (t.receivedBytes > 0 && elapsedSec > 0.001) {
						double rate = static_cast<double>(t.receivedBytes) / elapsedSec;
						const double received = static_cast<double>((std::min)(t.receivedBytes, t.compressedSize));
						const double total = static_cast<double>(t.compressedSize);
						const double remaining = (std::max)(0.0, total - received);
						if (rate > 1.0 && remaining > 0.0) {
							int eta = static_cast<int>(std::ceil(remaining / rate));
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
				std::string status = t.statusText;
				if (t.attempts > 0 && !t.fromCache && t.statusText != "done") {
					status += " (" + std::to_string(t.attempts) + ")";
				}
				if (!t.lastError.empty()) {
					status += " - " + t.lastError;
				}
				if (t.failed) {
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", status.c_str());
					ImGui::SameLine();
					char buf[64];
					std::snprintf(buf, sizeof(buf), "Retry##%u_%u", t.modelId, static_cast<unsigned>(t.kind));
					if (ImGui::Button(buf)) {
						ModelTransferClient::Instance().ManualRetry(t.modelId, t.kind);
					}
				} else if (t.fromCache) {
					ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", status.c_str());
				} else if (status == "done") {
					ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", status.c_str());
				} else {
					ImGui::TextUnformatted(status.c_str());
				}
			}

			ImGui::EndTable();
		}
	}

	ImGui::End();
	LogImGuiStage("RenderTransferWindow end");
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
	LogImGuiStage(SUCCEEDED(result) ? "Reset success" : "Reset failed", SUCCEEDED(result) ? LogLevel::Debug : LogLevel::Error);
	g_bwasInitialized = wasInitialized && SUCCEEDED(result);

	return result;
}

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT u_msg, WPARAM w_param, LPARAM l_param)
{
	if (u_msg == WM_KEYDOWN && static_cast<int>(w_param) == TransferConfig::Instance().toggleKey) {
		bool newVis = !g_windowVisible.load(std::memory_order_relaxed);
		g_windowVisible.store(newVis, std::memory_order_relaxed);
		g_manuallyToggled.store(true, std::memory_order_relaxed);
		ClientLog(LogLevel::Info, std::format("hkWndProc: toggleKey pressed (key=0x{:X})! new g_windowVisible={}", static_cast<int>(w_param), newVis));
		if (!newVis && g_bwasInitialized && ImGui::GetCurrentContext() != nullptr) {
			ImGui::GetIO().MouseDrawCursor = false;
		}
		return 0;
	}

	if (g_windowVisible.load(std::memory_order_relaxed) && g_bwasInitialized && ImGui::GetCurrentContext() != nullptr) {
		if (ImGui_ImplWin32_WndProcHandler(hwnd, u_msg, w_param, l_param)) {
			return 0;
		}
		ImGuiIO& io = ImGui::GetIO();
		if (io.WantCaptureMouse && (u_msg >= WM_MOUSEFIRST && u_msg <= WM_MOUSELAST)) {
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
			LogImGuiStage("GetCreationParameters failed.", LogLevel::Error);
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
			LogImGuiStage("ImGui_ImplWin32_Init failed.", LogLevel::Error);

			ImGui::DestroyContext();

			g_imguiDevice = nullptr;
			g_bwasInitialized = false;
			return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;
		}

		LogImGuiStage("DX9 backend init");
		if (!ImGui_ImplDX9_Init(pDevice)) {
			LogImGuiStage("ImGui_ImplDX9_Init failed.", LogLevel::Error);

			ImGui_ImplWin32_Shutdown();
			if (ImGui::GetCurrentContext() != nullptr)
				ImGui::DestroyContext();

			g_imguiDevice = nullptr;
			g_bwasInitialized = false;

			LogImGuiStage("initialization failed; all ImGui state rolled back.", LogLevel::Error);
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

		ImFont* imFnt = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 16.0f, &cfg, ranges.Data);
		if (!imFnt) {
			imFnt = io.Fonts->AddFontDefault();
		}
		io.Fonts->Build();
		io.FontDefault = imFnt;

		LogImGuiStage("tahoma font ready");

		if (!g_wndProcHooked) {
			SetLastError(ERROR_SUCCESS);

			const LONG_PTR previousWndProc = GetWindowLongPtrA(hWnd, GWLP_WNDPROC);
			DWORD getError = GetLastError();

			if (previousWndProc == 0 && getError != ERROR_SUCCESS) {
				LogImGuiStage(std::format("GetWindowLongPtrA failed. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getError, std::system_category().message(static_cast<int>(getError))).c_str(), LogLevel::Error);
				ImGui_ImplDX9_Shutdown();
				ImGui_ImplWin32_Shutdown();
				if (ImGui::GetCurrentContext() != nullptr)
					ImGui::DestroyContext();

				g_imguiDevice = nullptr;
				g_bwasInitialized = false;

				LogImGuiStage("initialization failed; all ImGui state rolled back.", LogLevel::Error);
				return false;
			}

			SetLastError(ERROR_SUCCESS);
			oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
			const DWORD getWndProcError = GetLastError();
			g_wndProcHooked = (oWndProc != nullptr || getWndProcError == ERROR_SUCCESS);
			if (!g_wndProcHooked) {
				LogImGuiStage(std::format("SetWindowLongPtrA(GWLP_WNDPROC) failed. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getWndProcError, std::system_category().message(static_cast<int>(getWndProcError))).c_str(), LogLevel::Error);

				ImGui_ImplDX9_Shutdown();
				ImGui_ImplWin32_Shutdown();
				if (ImGui::GetCurrentContext() != nullptr)
					ImGui::DestroyContext();

				g_imguiDevice = nullptr;
				g_bwasInitialized = false;

				LogImGuiStage("initialization failed; all ImGui state rolled back.", LogLevel::Error);

				const LONG_PTR restoreResult = SetWindowLongPtrA(hWnd, GWLP_WNDPROC, previousWndProc);
				getError = GetLastError();
				if (restoreResult == 0 && getError != ERROR_SUCCESS) {
					ClientLog(LogLevel::Error, std::format("Failed to restore WndProc. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getError, std::system_category().message(static_cast<int>(getError))));
				} else {
					LogImGuiStage("Original WndProc restored.", LogLevel::Info);
				}
				return false;
			} else {
				const LONG_PTR installedWndProc = GetWindowLongPtrA(hWnd, GWLP_WNDPROC);
				LogImGuiStage(std::format("WndProc hooked successfully. HWND=0x{:X}, oldWndProc=0x{:X}, newWndProc=0x{:X}", reinterpret_cast<std::uintptr_t>(hWnd), static_cast<std::uintptr_t>(reinterpret_cast<uintptr_t>(oWndProc)), static_cast<uintptr_t>(installedWndProc)).c_str(), LogLevel::Info);
			}
		}

		LogImGuiStage("initialization complete", LogLevel::Info);

		g_windowVisible.store(false, std::memory_order_relaxed);
		g_bwasInitialized = true;
	}

	if (ImGui::GetCurrentContext() != nullptr && g_windowVisible.load(std::memory_order_acquire)) {
		ImGuiIO& io = ImGui::GetIO();
		io.MouseDrawCursor = true;

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
			MH_STATUS enableStatus = MH_EnableHook(g_targetEndScene);
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "Direct3D 9 hkEndScene hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable Direct3D 9 hkEndScene hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook Direct3D 9 hkEndScene via MinHook: {}", MH_StatusToString(statusES)));
		}

		MH_STATUS statusReset = MH_CreateHook(g_targetReset, reinterpret_cast<void*>(&hkReset), reinterpret_cast<void**>(&oReset));
		if (statusReset == MH_OK) {
			MH_STATUS enableStatus = MH_EnableHook(g_targetReset);
			if (enableStatus == MH_OK) {
				ClientLog(LogLevel::Info, "Direct3D 9 hkReset hooked successfully via MinHook");
			} else {
				ClientLog(LogLevel::Error, std::format("Failed to enable Direct3D 9 hkReset hook: {}", MH_StatusToString(enableStatus)));
			}
		} else {
			ClientLog(LogLevel::Error, std::format("Failed to hook Direct3D 9 hkReset via MinHook: {}", MH_StatusToString(statusReset)));
		}
	} else {
		ClientLog(LogLevel::Error, "Failed to hook Direct3D 9: DEVICE_PTR was 0 after timeout");
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
			LogImGuiStage(std::format("GetWindowLongPtrW(GWLP_WNDPROC) failed. HWND=0x{:X}, error={} ({})", reinterpret_cast<std::uintptr_t>(hWnd), getWndProcError, std::system_category().message(static_cast<int>(getWndProcError))).c_str(), LogLevel::Error);

			ImGui_ImplDX9_Shutdown();
			ImGui_ImplWin32_Shutdown();
			if (ImGui::GetCurrentContext() != nullptr)
				ImGui::DestroyContext();

			g_imguiDevice = nullptr;
			g_bwasInitialized = false;

			LogImGuiStage("initialization failed; all ImGui state rolled back.", LogLevel::Error);
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
