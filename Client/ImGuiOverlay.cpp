#include "ImGuiOverlay.h"

HWND hWnd;
WNDPROC oWndProc = nullptr;
VTableHookManager* g_vmtHooks = nullptr;
_Present oPresent = nullptr;
_EndScene oEndScene = nullptr;
_Reset oReset = nullptr;
std::atomic<bool> g_shutdownRequested = false;
bool g_wndProcHooked = false;
std::thread g_initializationThread;

void RenderTransferWindow()
{
	ImGui::SetNextWindowSize(ImVec2(700, 280), ImGuiCond_FirstUseEver);
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

		static const char* kKindNames[] = { "DFF", "TXD", "COL" };

		for (auto& t : transfers) {
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%u", t.modelId);

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(kKindNames[static_cast<int>(t.kind)]);

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
					double remaining = static_cast<double>(t.compressedSize - t.receivedBytes);
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
	if (wasInitialized) {
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}
	HRESULT result = oReset(pDevice, pp);
	if (SUCCEEDED(result) && wasInitialized)
		ImGui_ImplDX9_CreateDeviceObjects();
	g_bwasInitialized = wasInitialized && SUCCEEDED(result);

	return result;
}

HRESULT __stdcall hkPresent(IDirect3DDevice9* pDevice, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
{
	return oPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT u_msg, WPARAM w_param, LPARAM l_param)
{
	if (imGuiOn && ImGui_ImplWin32_WndProcHandler(hwnd, u_msg, w_param, l_param)) {
		return true;
	}

	if (u_msg == WM_KEYDOWN && static_cast<int>(w_param) == TransferConfig::Instance().toggleKey)
		g_windowVisible = !g_windowVisible;

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

		D3DDEVICE_CREATION_PARAMETERS d3dcp {};
		if (FAILED(pDevice->GetCreationParameters(&d3dcp)) || !IsWindow(d3dcp.hFocusWindow))
			return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;

		hWnd = d3dcp.hFocusWindow;

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();

		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

		if (!ImGui_ImplWin32_Init(hWnd) || !ImGui_ImplDX9_Init(pDevice)) {
			ImGui::DestroyContext();
			return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;
		}

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

		if (!g_wndProcHooked) {
			oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
				hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
			g_wndProcHooked = oWndProc != nullptr;
		}

		g_windowVisible = TransferConfig::Instance().showTransferWindow;
		g_bwasInitialized = true;
		imGuiOn = true;
	}

	if (imGuiOn) {
		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		if (g_windowVisible)
			RenderTransferWindow();

		ImGui::EndFrame();
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
	}
	return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;
}

void BackgroundInitializationWorker()
{
	DWORD deviceAddr = 0;
	int attempts = 0;
	while (!g_shutdownRequested && (deviceAddr = *(DWORD*)DEVICE_PTR) == 0 && attempts < 50) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		++attempts;
	}

	if (deviceAddr != 0) {
		void** vTableDevice = *(void***)(deviceAddr);
		g_vmtHooks = new VTableHookManager(vTableDevice, D3D_VFUNCTIONS);
		oPresent = (_Present)g_vmtHooks->Hook(PRESENT_INDEX, (void*)hkPresent);
		oEndScene = (_EndScene)g_vmtHooks->Hook(ENDSCENE_INDEX, (void*)hkEndScene);
		oReset = (_Reset)g_vmtHooks->Hook(RESET_INDEX, (void*)hkReset);
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

	if (g_vmtHooks) {
		g_vmtHooks->Unhook(PRESENT_INDEX);
		g_vmtHooks->Unhook(RESET_INDEX);
		g_vmtHooks->Unhook(ENDSCENE_INDEX);
		delete g_vmtHooks;
		g_vmtHooks = nullptr;
	}

	if (hWnd && g_wndProcHooked && oWndProc) {
		SetWindowLongPtrA(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
		oWndProc = nullptr;
		g_wndProcHooked = false;
	}

	if (imGuiOn) {
		ImGui_ImplDX9_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		imGuiOn = false;
		g_bwasInitialized = false;
	}
}

c_plugin::c_plugin(HMODULE hmodule)
	: hmodule(hmodule)
{
	// Constructor does nothing.
}

c_plugin::~c_plugin()
{
	c_plugin::shutdown_for_unload();
}
