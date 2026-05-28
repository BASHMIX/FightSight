// FightSight - FGC Telemetry & Automation Engine
// Phase 4: + CV worker thread, websocketpp client, template capture, triggers.

#include <windows.h>
#include <d3d11.h>
#include <tchar.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include "App/SpoutReceiverDX.h"
#include "App/ConfigManager.h"
#include "App/FrameProcessor.h"
#include "App/RoiManager.h"
#include "App/RoiEditor.h"
#include "App/WsClient.h"
#include "App/CvWorker.h"
#include "App/ProfileManager.h"
#include "App/GhostTextureCache.h"
#include "App/MatchManager.h"
#include "App/Util.h"

#include <shellapi.h>   // ShellExecute for "Open Profiles Folder"
#include <commdlg.h>    // GetOpenFileNameW for View > Load Reference Image

// ---------- DX11 globals ----------
static ID3D11Device*           g_pd3dDevice            = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext     = nullptr;
static IDXGISwapChain*         g_pSwapChain            = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView  = nullptr;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------- Helpers ----------
namespace {

// Snapshot the current Spout frame and write the ROI's crop to
// /templates/<profile>/<sanitized_name>_State_<int>.png. Saved as BGRA so
// it round-trips in any viewer; alpha is 255 on capture - the user
// post-processes transparency in their image editor and the alpha channel
// becomes matchTemplate's per-pixel mask.
bool CaptureRoiTemplate(fightsight::FrameProcessor& fp,
                        const fightsight::Roi& roi,
                        const std::string& templatesDir,
                        int stateValue,
                        std::string& outPathOrError) {
    cv::Mat snap;
    if (!fp.SnapshotProcessedFrame(snap)) {
        outPathOrError = "no frame yet";
        return false;
    }
    const cv::Rect safe =
        roi.rect & cv::Rect(0, 0, snap.cols, snap.rows);
    if (safe.width < 4 || safe.height < 4) {
        outPathOrError = "ROI does not fit current frame";
        return false;
    }

    cv::Mat crop = snap(safe).clone();
    if (fp.IsSenderRGBA()) {
        cv::cvtColor(crop, crop, cv::COLOR_RGBA2BGRA);
    }

    try { std::filesystem::create_directories(templatesDir); }
    catch (...) { /* best effort */ }

    const std::string path =
        templatesDir + "/" + fightsight::SanitizeName(roi.name)
        + "_State_" + std::to_string(stateValue) + ".png";
    if (!cv::imwrite(path, crop)) {
        outPathOrError = "cv::imwrite failed (" + path + ")";
        return false;
    }
    outPathOrError = path;
    return true;
}

// Phase 12b: capture the FULL processed Spout frame and write it to
// the active profile's templates/<Profile>/screen_shots/ folder. Name
// uses local-time timestamp with millisecond precision so rapid-fire
// captures don't collide.
bool CaptureFullScreenshot(fightsight::FrameProcessor& fp,
                           const std::string& templatesDir,
                           std::string& outPathOrError) {
    cv::Mat snap;
    if (!fp.SnapshotProcessedFrame(snap)) {
        outPathOrError = "no frame yet";
        return false;
    }
    if (fp.IsSenderRGBA()) {
        cv::cvtColor(snap, snap, cv::COLOR_RGBA2BGRA);
    }
    const std::string dir = templatesDir + "/screen_shots";
    try { std::filesystem::create_directories(dir); }
    catch (...) { /* best effort */ }

    using clk = std::chrono::system_clock;
    const auto now = clk::now();
    const std::time_t t = clk::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char fname[80];
    std::snprintf(fname, sizeof(fname),
        "screenshot_%04d%02d%02d_%02d%02d%02d_%03lld.png",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(ms.count()));

    const std::string path = dir + "/" + fname;
    if (!cv::imwrite(path, snap)) {
        outPathOrError = "cv::imwrite failed (" + path + ")";
        return false;
    }
    outPathOrError = path;
    return true;
}

// Enumerate "<sanitized>_State_<int>.png" filenames in `dir` and return
// the integer states found. Render-thread only; called from inside the
// Cfg popup so the per-frame syscall cost is moot.
std::vector<int> EnumerateCapturedStates(const std::string& templatesDir,
                                         const std::string& roiName) {
    std::vector<int> states;
    const std::string prefix =
        fightsight::SanitizeName(roiName) + "_State_";
    const std::string ext = ".png";
    try {
        if (!std::filesystem::exists(templatesDir)) return states;
        for (const auto& e :
             std::filesystem::directory_iterator(templatesDir)) {
            if (!e.is_regular_file()) continue;
            const std::string fname = e.path().filename().string();
            if (fname.size() < prefix.size() + ext.size()) continue;
            if (fname.compare(0, prefix.size(), prefix) != 0) continue;
            if (fname.compare(fname.size() - ext.size(), ext.size(), ext)
                != 0) continue;
            const std::string mid =
                fname.substr(prefix.size(),
                             fname.size() - prefix.size() - ext.size());
            try { states.push_back(std::stoi(mid)); }
            catch (...) {}
        }
    } catch (...) {}
    std::sort(states.begin(), states.end());
    return states;
}

std::string BuildWsUrl(const nlohmann::json& wsCfg) {
    const std::string host     = wsCfg.value("host",     std::string{"127.0.0.1"});
    const int         port     = wsCfg.value("port",     8080);
    const std::string endpoint = wsCfg.value("endpoint", std::string{"/"});
    return "ws://" + host + ":" + std::to_string(port) + endpoint;
}

const char* CvStatusLabel(fightsight::CvResult::Status s) {
    using S = fightsight::CvResult::Status;
    switch (s) {
        case S::Empty:       return "----";
        case S::NoTemplate:  return "no tpl";
        case S::OutOfBounds: return "oob";
        case S::Ok:          return "ok";
        case S::Error:       return "err";
    }
    return "?";
}

// ---- HSV <-> RGB conversion via OpenCV (single-pixel Mat) --------------
// Used by the designer Gradient popup so the user can pick a target colour
// in RGB-space via ImGui::ColorEdit3 while we store it as HSV internally.

void HsvIntsToRgbFloats(int h, int s, int v, float out[3]) {
    cv::Mat3b hsv(1, 1, cv::Vec3b(
        static_cast<uchar>(std::clamp(h, 0, 179)),
        static_cast<uchar>(std::clamp(s, 0, 255)),
        static_cast<uchar>(std::clamp(v, 0, 255))));
    cv::Mat3b bgr;
    cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);
    const cv::Vec3b p = bgr(0, 0);
    out[0] = p[2] / 255.0f; // R
    out[1] = p[1] / 255.0f; // G
    out[2] = p[0] / 255.0f; // B
}

void RgbFloatsToHsvInts(const float in[3], int out[3]) {
    const int r = std::clamp(int(in[0] * 255.0f), 0, 255);
    const int g = std::clamp(int(in[1] * 255.0f), 0, 255);
    const int b = std::clamp(int(in[2] * 255.0f), 0, 255);
    cv::Mat3b bgr(1, 1, cv::Vec3b(
        static_cast<uchar>(b),
        static_cast<uchar>(g),
        static_cast<uchar>(r)));
    cv::Mat3b hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    const cv::Vec3b p = hsv(0, 0);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

ImVec4 HsvIntsToImVec4(int h, int s, int v) {
    float rgb[3];
    HsvIntsToRgbFloats(h, s, v, rgb);
    return ImVec4(rgb[0], rgb[1], rgb[2], 1.0f);
}

// ---- Phase 12: Reference image overlay --------------------------------
// Small holder for a user-loaded reference image that the Spout viewer
// overlays at 50% alpha so the user can trace ROIs precisely against a
// known-good frame. Owns its own DX11 texture + SRV pair so we can free
// them deterministically (the GhostTextureCache is path-keyed and tied
// to template files; this is a one-off the user toggles on/off).
struct ReferenceImage {
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D*          tex = nullptr;
    int                       width  = 0;
    int                       height = 0;
    std::string               path;

    bool Loaded() const { return srv != nullptr; }

    void Release() {
        if (srv) { srv->Release(); srv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
        width = 0;
        height = 0;
        path.clear();
    }
};

// Build a DX11 SRV from an on-disk image. Reads via OpenCV so we get
// the same format coverage as the rest of the app (PNG/JPG/BMP/TGA).
bool LoadReferenceImage(const std::string& path,
                        ID3D11Device* device,
                        ReferenceImage& out) {
    out.Release();
    if (!device) return false;

    cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
    if (img.empty()) return false;

    cv::Mat bgra;
    cv::cvtColor(img, bgra, cv::COLOR_BGR2BGRA);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = bgra.cols;
    desc.Height           = bgra.rows;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem     = bgra.data;
    srd.SysMemPitch = static_cast<UINT>(bgra.step);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &srd, &tex)) || !tex) {
        return false;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device->CreateShaderResourceView(tex, nullptr, &srv)) || !srv) {
        tex->Release();
        return false;
    }
    out.tex    = tex;
    out.srv    = srv;
    out.width  = bgra.cols;
    out.height = bgra.rows;
    out.path   = path;
    return true;
}

// Win32 file dialog wrapper. Modal, blocks the calling thread; we fire
// it from the deferred "requestLoadReferenceImage" path in the main
// loop so the ImGui frame containing the menu click has already
// finished rendering. Returns empty string when the user cancels.
std::string OpenImageFileDialog(HWND owner) {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0"
                        L"*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
                        L"All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrTitle   = L"Load Reference Image";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
                     | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&ofn)) return {};

    const int sz = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                         nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return {};
    std::string utf8(static_cast<size_t>(sz - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), sz,
                          nullptr, nullptr);
    return utf8;
}

// Case-insensitive substring search for the searchable action combo.
bool ContainsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        hay.begin(), hay.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower((unsigned char)a)
                == std::tolower((unsigned char)b);
        });
    return it != hay.end();
}

} // namespace

// ---------- Entry ----------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // --- Window ---
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"FightSightWnd";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"FightSight - FGC Telemetry Engine",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1700, 950,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // --- ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // --- Config + profile manager ---
    fightsight::ConfigManager config;
    config.Load("config/config.json");

    fightsight::ProfileManager profileMgr;
    profileMgr.Init("profiles", "templates");

    // --- Spout receiver ---
    fightsight::SpoutReceiverDX spout;
    if (!spout.Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        ::MessageBoxW(hwnd, L"Spout2 DX11 init failed.", L"FightSight", MB_ICONERROR);
    }
    spout.SetReceiverName(config.SpoutReceiverName());

    // --- Frame processor ---
    fightsight::FrameProcessor processor;
    if (!processor.Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        ::MessageBoxW(hwnd, L"FrameProcessor init failed.", L"FightSight", MB_ICONERROR);
    }

    // GPU texture cache for Text-ROI ghost overlays. Lifetime spans the
    // whole app; entries get invalidated on capture and cleared on
    // profile switch.
    fightsight::GhostTextureCache ghostCache;
    ghostCache.Init(g_pd3dDevice);

    char receiverNameBuf[256]{};
    {
        const auto n = config.SpoutReceiverName();
        std::snprintf(receiverNameBuf, sizeof(receiverNameBuf), "%s", n.c_str());
    }

    // Viewport prefs
    bool force1080p = false;
    if (config.Json().contains("viewport") &&
        config.Json()["viewport"].contains("force_1080p")) {
        force1080p = config.Json()["viewport"]["force_1080p"].get<bool>();
    }

    // --- ROIs + per-game profile load (with legacy migration) ---
    fightsight::RoiManager roiManager;
    fightsight::RoiEditor  roiEditor(roiManager);
    {
        auto profileList = profileMgr.ListProfiles();
        std::string activeProfileName =
            config.Json().value("active_profile", std::string{});

        // Migration path: very-first-launch w/ an existing legacy config.
        if (profileList.empty()) {
            const bool legacyRois =
                config.Json().contains("rois")
                && config.Json()["rois"].is_array()
                && !config.Json()["rois"].empty();
            if (legacyRois) {
                profileMgr.CreateProfile("Default");
                roiManager.LoadFromJson(config.Json()["rois"]);
                profileMgr.SetActiveProfile("Default");
                profileMgr.SaveProfile("Default", roiManager);
                config.Json().erase("rois");
            } else {
                profileMgr.CreateProfile("Default");
            }
            activeProfileName = "Default";
            profileList = profileMgr.ListProfiles();
        } else if (activeProfileName.empty()
                   || !profileMgr.HasProfile(activeProfileName)) {
            activeProfileName = profileList.front();
        }

        profileMgr.LoadProfile(activeProfileName, roiManager);
        config.Json()["active_profile"] = activeProfileName;
    }

    // --- WebSocket client (Asio thread) ---
    if (!config.Json().contains("websocket")) {
        config.Json()["websocket"] = {
            {"host","127.0.0.1"}, {"port",8080}, {"endpoint","/"}
        };
    }
    char wsHostBuf[128]{};
    int  wsPort = config.Json()["websocket"].value("port", 8080);
    char wsEndpointBuf[64]{};
    std::snprintf(wsHostBuf, sizeof(wsHostBuf), "%s",
        config.Json()["websocket"].value("host", std::string{"127.0.0.1"}).c_str());
    std::snprintf(wsEndpointBuf, sizeof(wsEndpointBuf), "%s",
        config.Json()["websocket"].value("endpoint", std::string{"/"}).c_str());

    fightsight::WsClient wsClient;
    wsClient.Start(BuildWsUrl(config.Json()["websocket"]));

    // --- Match Manager (Phase 12) ---
    // Holds the global match-state machine + per-state allowed-group
    // map. Loaded from config so the operator's mapping survives
    // restarts; current_state itself always boots in WAITING.
    fightsight::MatchManager matchMgr;
    if (config.Json().contains("match_manager")) {
        matchMgr.FromJson(config.Json()["match_manager"]);
    }

    // --- CV worker thread ---
    fightsight::CvWorker cvWorker(processor, roiManager, wsClient, matchMgr);
    cvWorker.SetTemplatesDir(profileMgr.ActiveTemplatesDir());
    int cvTargetHz = 30;
    if (config.Json().contains("cv") &&
        config.Json()["cv"].contains("target_hz")) {
        cvTargetHz = config.Json()["cv"].value("target_hz", 30);
    }
    cvWorker.SetTargetHz(cvTargetHz);
    cvWorker.Start();

    // Captured "last action" feedback for UI (transient toast in ROIs panel)
    std::string lastCaptureMsg;
    auto lastCaptureAt = std::chrono::steady_clock::now()
                       - std::chrono::seconds(10);

    // Spout Viewer zoom + pan (Phase 8). Persisted-only-in-memory state;
    // zoom resets to 1.0 on relaunch which is fine.
    float  spoutZoom = 1.0f;
    ImVec2 spoutPanUV(0.0f, 0.0f);

    // Phase 9: window-visibility toggles (headless performance mode) +
    // floating ROI Inspector selection. selected_roi_id mirrors the
    // RoiEditor's selection so the Inspector window can be driven from
    // either the table or the viewer canvas (-1 == nothing selected).
    bool show_viewer_window        = true;
    bool show_rois_window          = true;
    bool show_inspector_window     = true;
    bool show_match_manager_window = true;
    int  selected_roi_id           = -1;

    // Phase 12: reference image overlay state. Toggled per-frame from
    // the Spout viewer's "Show Ref" checkbox; loaded via View > Load
    // Reference Image... in the main menu bar.
    ReferenceImage refImage;
    bool           show_reference_image = true;

    // --- Main loop (vsync = 60 FPS when monitor is 60Hz) ---
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Pull a Spout frame; round-trip through CPU/OpenCV pipeline.
        spout.ReceiveFrame();
        if (spout.IsConnected()) {
            if (ID3D11Texture2D* srcTex = spout.GetSenderTexture()) {
                processor.Process(srcTex, force1080p);
            }
        }

        // Snapshot CV results for this frame's UI (cheap; CV thread publishes).
        const auto cvResults = cvWorker.SnapshotResults();
        // Snapshot Streamer.bot's action catalogue once per frame so we
        // don't re-lock per-row inside the table loop.
        const auto availableActions = wsClient.GetAvailableActions();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ----- Main menu bar (top) ----------------------------------------
        // Deferred profile actions: avoid mutating roiManager while we're
        // still iterating it later in the frame. Applied at end of frame.
        std::string requestSwitchProfile;
        bool requestCreateProfileDialog = false;
        bool requestSaveCurrentProfile  = false;
        bool requestOpenProfilesFolder  = false;
        bool requestLoadReferenceImage  = false;
        bool requestClearReferenceImage = false;
        bool requestCaptureScreenshot   = false;

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::BeginMenu("Profiles")) {
                    ImGui::Text("Active: %s",
                        profileMgr.ActiveProfile().c_str());
                    ImGui::Separator();

                    const auto profilesList = profileMgr.ListProfiles();
                    if (profilesList.empty()) {
                        ImGui::TextDisabled("(no profiles)");
                    } else {
                        for (const auto& p : profilesList) {
                            const bool isActive =
                                (p == profileMgr.ActiveProfile());
                            // MenuItem second arg = shortcut text;
                            // third arg = checked state (shows tick mark).
                            if (ImGui::MenuItem(p.c_str(), nullptr, isActive)) {
                                if (!isActive) requestSwitchProfile = p;
                            }
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("New Profile...")) {
                        requestCreateProfileDialog = true;
                    }
                    if (ImGui::MenuItem("Save Current Profile")) {
                        requestSaveCurrentProfile = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Open Profiles Folder")) {
                        requestOpenProfilesFolder = true;
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    done = true;
                }
                ImGui::EndMenu();
            }
            // Phase 9: window toggles. Hiding the Spout Viewer is the
            // headless-mode switch - the GPU upload + ImDrawList work in
            // the viewer block below is gated on show_viewer_window, but
            // the CvWorker thread keeps running stateless in the bg.
            if (ImGui::BeginMenu("Window")) {
                ImGui::MenuItem("Spout Viewer",  nullptr, &show_viewer_window);
                ImGui::MenuItem("ROIs Table",    nullptr, &show_rois_window);
                ImGui::MenuItem("ROI Inspector", nullptr, &show_inspector_window);
                ImGui::MenuItem("Match Manager", nullptr, &show_match_manager_window);
                ImGui::EndMenu();
            }
            // Phase 12: View menu - reference image for precise ROI
            // tracing. The dialog itself is fired AFTER the frame ends
            // so the menu's own ImGui state finishes cleanly first.
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Load Reference Image...")) {
                    requestLoadReferenceImage = true;
                }
                ImGui::BeginDisabled(!refImage.Loaded());
                if (ImGui::MenuItem("Clear Reference Image")) {
                    requestClearReferenceImage = true;
                }
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::MenuItem("Show Reference Image",
                                nullptr, &show_reference_image);
                if (refImage.Loaded()) {
                    ImGui::TextDisabled("  Loaded: %s",
                        refImage.path.c_str());
                    ImGui::TextDisabled("  %d x %d",
                        refImage.width, refImage.height);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                ImGui::TextDisabled("FightSight - FGC Telemetry Engine");
                ImGui::TextDisabled("Spout2 -> DX11 -> OpenCV -> Streamer.bot");
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Keep selected_roi_id in sync with the canvas-side editor so an
        // ROI clicked in the Spout Viewer also drives the Inspector. The
        // table path writes both directly when its Selectable fires.
        if (roiEditor.SelectedId() != 0
            && roiEditor.SelectedId() != selected_roi_id) {
            selected_roi_id = roiEditor.SelectedId();
        }

        // ----- Create-Profile modal ---------------------------------------
        if (requestCreateProfileDialog) {
            ImGui::OpenPopup("Create New Profile");
        }
        if (ImGui::BeginPopupModal("Create New Profile", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            static char newName[64] = "";
            ImGui::SetNextItemWidth(280);
            const bool enter = ImGui::InputText(
                "Profile name", newName, sizeof(newName),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled(
                "(letters / digits / underscore - e.g. SF6, Tekken8, MK1)");

            const std::string sanitized =
                fightsight::ProfileManager::SanitizeProfileName(newName);
            const bool isUnnamed = sanitized.empty() || sanitized == "Unnamed";
            const bool exists    = profileMgr.HasProfile(sanitized);
            const bool valid     = !isUnnamed && !exists;

            if (!isUnnamed && exists) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                    "Already exists.");
            } else if (!isUnnamed && sanitized != newName) {
                ImGui::TextDisabled("Will be saved as: %s", sanitized.c_str());
            }

            ImGui::Separator();
            ImGui::BeginDisabled(!valid);
            const bool doCreate = ImGui::Button("Create") || (enter && valid);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                newName[0] = 0;
                ImGui::CloseCurrentPopup();
            }

            if (doCreate && valid) {
                // Save the active profile first so we don't lose pending
                // edits, then create the new one and queue a switch.
                {
                    std::lock_guard<std::mutex> lk(roiManager.Mutex());
                    profileMgr.SaveProfile(
                        profileMgr.ActiveProfile(), roiManager);
                }
                profileMgr.CreateProfile(sanitized);
                requestSwitchProfile = sanitized;
                newName[0] = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // ----- Spout Viewer + ROI editor overlay -----
        // HEADLESS PERFORMANCE GATE: when the user hides this window we
        // skip the entire ImGui::Begin block. That means no texture
        // sampling (ImGui::Image), no ghost-overlay AddImage calls, and
        // no RoiEditor input handling - saving the per-frame GPU upload
        // + ImDrawList work. CvWorker keeps running on its own thread so
        // triggers still fire while running "in the background".
        if (show_viewer_window) {
        if (ImGui::Begin("Spout Viewer", &show_viewer_window)) {
            ID3D11ShaderResourceView* viewSRV = processor.GetDisplaySRV();
            // Phase 12b: the canvas now renders whenever EITHER a
            // Spout feed is connected OR a reference image is loaded
            // and toggled on. Without a feed, the ref image becomes
            // the canvas (full alpha) and ROIs can be drawn against
            // it for trace-from-screenshot workflows.
            const bool haveFeed = spout.IsConnected() && viewSRV;
            const bool haveRef  = refImage.Loaded();
            const bool refAsBase = !haveFeed && haveRef && show_reference_image;
            const bool canRender = haveFeed || refAsBase;

            // Status line at the top adapts to the current source.
            if (haveFeed) {
                ImGui::Text("Sender: %s   src %ux%u   display %dx%d%s",
                    spout.GetSenderName().c_str(),
                    spout.GetWidth(), spout.GetHeight(),
                    processor.GetDisplayWidth(), processor.GetDisplayHeight(),
                    force1080p ? "  [NORMALIZED]" : "");
            } else if (refAsBase) {
                ImGui::TextDisabled(
                    "No Spout feed - tracing on reference image (%dx%d)",
                    refImage.width, refImage.height);
            } else if (haveRef && !show_reference_image) {
                ImGui::TextDisabled(
                    "Waiting for Spout sender...  (tick Show Ref to use the "
                    "loaded reference as canvas)");
            } else {
                ImGui::TextDisabled("Waiting for Spout sender...");
            }

            if (canRender) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                // Reserve a strip at the bottom for the controls row
                // (zoom slider + checkboxes + capture button) so they
                // don't get pushed out of view.
                avail.y -= ImGui::GetFrameHeightWithSpacing() * 2.5f;
                if (avail.y < 50.0f) avail.y = 50.0f;

                // Canon dims drive both the aspect ratio and the
                // RoiEditor coordinate space. When the ref image is
                // the canvas we adopt its dims; ROIs created in that
                // mode line up correctly when the same resolution
                // later arrives via Spout.
                const float srcW = haveFeed
                    ? static_cast<float>(processor.GetDisplayWidth())
                    : static_cast<float>(refImage.width);
                const float srcH = haveFeed
                    ? static_cast<float>(processor.GetDisplayHeight())
                    : static_cast<float>(refImage.height);
                if (srcW > 0.0f && srcH > 0.0f && avail.x > 0.0f && avail.y > 0.0f) {
                    const float aspect = srcW / srcH;
                    float w = avail.x;
                    float h = w / aspect;
                    if (h > avail.y) { h = avail.y; w = h * aspect; }

                    // Zoom/pan via UV clipping. uv0 = top-left of the
                    // visible texture region in normalized coords; uv1 the
                    // opposite corner. Span shrinks as zoom grows.
                    const float zoomNow = (spoutZoom > 0.0f) ? spoutZoom : 1.0f;
                    const float spanU   = 1.0f / zoomNow;
                    const ImVec2 uv0(spoutPanUV.x, spoutPanUV.y);
                    const ImVec2 uv1(spoutPanUV.x + spanU,
                                     spoutPanUV.y + spanU);
                    // Base layer: live feed when present, otherwise
                    // the reference image at full alpha.
                    ID3D11ShaderResourceView* baseSRV =
                        haveFeed ? viewSRV : refImage.srv;
                    ImGui::Image(
                        (ImTextureID)(intptr_t)baseSRV,
                        ImVec2(w, h), uv0, uv1);
                    const ImVec2 imgMin = ImGui::GetItemRectMin();
                    const ImVec2 imgMax = ImGui::GetItemRectMax();

                    // Middle-mouse-button panning. LMB still belongs to
                    // the RoiEditor; MMB is reserved for navigation.
                    const bool overImage =
                        ImGui::IsMouseHoveringRect(imgMin, imgMax);
                    if (overImage
                        && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                        const ImVec2 d = ImGui::GetIO().MouseDelta;
                        if (w > 0.0f && h > 0.0f) {
                            spoutPanUV.x -= d.x / (w * zoomNow);
                            spoutPanUV.y -= d.y / (h * zoomNow);
                        }
                    }
                    const float maxPan = std::max(0.0f, 1.0f - spanU);
                    spoutPanUV.x = std::clamp(spoutPanUV.x, 0.0f, maxPan);
                    spoutPanUV.y = std::clamp(spoutPanUV.y, 0.0f, maxPan);

                    // ---- Reference image overlay (Phase 12) ----
                    // Only drawn as an overlay when BOTH a feed and
                    // the ref are present (and ref-display is on).
                    // When refAsBase is true the ref was already drawn
                    // at full alpha as the base layer above, so this
                    // skip avoids double-blending.
                    if (haveFeed && haveRef && show_reference_image) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImU32 refTint = IM_COL32(255, 255, 255, 128); // 50% alpha
                        dl->AddImage(
                            (ImTextureID)(intptr_t)refImage.srv,
                            imgMin, imgMax,
                            uv0, uv1,
                            refTint);
                    }

                    // ---- Ghost overlays (Phase 7) ----
                    // Drawn AFTER the feed, BEFORE the ROI editor's outlines
                    // so handles remain visible on top. We snapshot the
                    // (rect, name) pairs out of the locked manager so the
                    // ImDrawList calls below don't fight RoiEditor for the
                    // mutex (DrawAndHandleInput takes it next).
                    struct GhostInfo {
                        cv::Rect rect;
                        std::string name;
                    };
                    std::vector<GhostInfo> ghosts;
                    {
                        std::lock_guard<std::mutex> lk(roiManager.Mutex());
                        for (const auto& roi : roiManager.All()) {
                            if (roi.type == fightsight::RoiType::Text
                                && roi.ghost_overlay) {
                                ghosts.push_back({roi.rect, roi.name});
                            }
                        }
                    }
                    // Canon -> screen transform that honors zoom + pan.
                    // Mirrors the math inside RoiEditor so ghost overlays
                    // and the editor's handles stay perfectly aligned.
                    // Canon dims source matches the canvas: feed when
                    // present, otherwise the reference image (Phase 12b).
                    const float canonW = haveFeed
                        ? static_cast<float>(processor.GetDisplayWidth())
                        : static_cast<float>(refImage.width);
                    const float canonH = haveFeed
                        ? static_cast<float>(processor.GetDisplayHeight())
                        : static_cast<float>(refImage.height);
                    const float dispW2 = imgMax.x - imgMin.x;
                    const float dispH2 = imgMax.y - imgMin.y;
                    const float visW   = canonW / zoomNow;
                    const float visH   = canonH / zoomNow;
                    auto canonToScreen = [&](float cx, float cy) -> ImVec2 {
                        if (visW <= 0.0f || visH <= 0.0f) return imgMin;
                        const float fx = (cx - spoutPanUV.x * canonW) / visW;
                        const float fy = (cy - spoutPanUV.y * canonH) / visH;
                        return ImVec2(imgMin.x + fx * dispW2,
                                      imgMin.y + fy * dispH2);
                    };

                    if (!ghosts.empty()) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const std::string activeTpls =
                            profileMgr.ActiveTemplatesDir();
                        const ImU32 tint = IM_COL32(255, 255, 255, 128);

                        for (const auto& g : ghosts) {
                            const std::string path = activeTpls + "/"
                                + fightsight::SanitizeName(g.name)
                                + "_State_0.png";
                            ID3D11ShaderResourceView* gsrv =
                                ghostCache.GetSRV(path);
                            if (!gsrv) continue;
                            const ImVec2 a = canonToScreen(
                                static_cast<float>(g.rect.x),
                                static_cast<float>(g.rect.y));
                            const ImVec2 b = canonToScreen(
                                static_cast<float>(g.rect.x + g.rect.width),
                                static_cast<float>(g.rect.y + g.rect.height));
                            dl->AddImage(
                                (ImTextureID)(intptr_t)gsrv,
                                a, b,
                                ImVec2(0, 0), ImVec2(1, 1),
                                tint);
                        }
                    }

                    roiEditor.DrawAndHandleInput(
                        imgMin, imgMax,
                        static_cast<int>(canonW),
                        static_cast<int>(canonH),
                        zoomNow, spoutPanUV);
                }
            }  // if (canRender)

            ImGui::TextDisabled(
                "LMB: draw/move/resize ROIs  |  MMB-drag: pan");
                ImGui::SetNextItemWidth(220);
                ImGui::SliderFloat("Zoom", &spoutZoom, 1.0f, 4.0f, "%.2fx");
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset View")) {
                    spoutZoom = 1.0f;
                    spoutPanUV = ImVec2(0.0f, 0.0f);
                }
            // Phase 12: visibility tick for the reference image
            // overlay. Disabled (and visually flagged) when no
            // reference is loaded, so the affordance is clear.
            ImGui::SameLine();
            ImGui::BeginDisabled(!refImage.Loaded());
            ImGui::Checkbox("Show Ref", &show_reference_image);
            ImGui::EndDisabled();
            // Phase 12b: Capture writes the current Spout frame into
            // templates/<profile>/screen_shots/. Disabled when no feed
            // - there's nothing to capture from the reference image.
            ImGui::SameLine();
            ImGui::BeginDisabled(!haveFeed);
            if (ImGui::SmallButton("Capture")) {
                requestCaptureScreenshot = true;
            }
            ImGui::EndDisabled();
            if (!refImage.Loaded()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(View > Load Reference Image)");
            }
        }
        ImGui::End();    // pair the Begin() above (also runs when collapsed)
        }
        // (No matching ImGui::Begin/End at all when show_viewer_window
        //  is false - this is the headless-mode fast-path.)

        // ----- Connection / viewport / CV pipeline timings panel -----
        if (ImGui::Begin("Connection")) {
            ImGui::Text("Receiver name (empty = first available):");
            if (ImGui::InputText("##rxname", receiverNameBuf, sizeof(receiverNameBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                spout.SetReceiverName(receiverNameBuf);
                config.Json()["spout"]["receiver_name"] = std::string(receiverNameBuf);
            }
            if (ImGui::Button("Apply")) {
                spout.SetReceiverName(receiverNameBuf);
                config.Json()["spout"]["receiver_name"] = std::string(receiverNameBuf);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Config")) {
                config.Save("config/config.json");
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Viewport");
            if (ImGui::Checkbox("Force Internal 1080p", &force1080p)) {
                config.Json()["viewport"]["force_1080p"] = force1080p;
            }

            ImGui::Separator();
            ImGui::Text("Spout: %s", spout.IsConnected() ? "CONNECTED" : "waiting");
            ImGui::Text("Frame total:  %.1f FPS  (%.2f ms)",
                        io.Framerate, 1000.0f / io.Framerate);

            ImGui::Separator();
            ImGui::TextUnformatted("CV pipeline (last frame)");
            ImGui::Text("  Readback: %6.3f ms  Resize: %6.3f ms  Upload: %6.3f ms",
                        processor.GetLastReadbackMs(),
                        processor.GetLastResizeMs(),
                        processor.GetLastUploadMs());
            ImGui::Text("  Round-trip total: %6.3f ms",
                        processor.GetLastReadbackMs()
                        + processor.GetLastResizeMs()
                        + processor.GetLastUploadMs());
        }
        ImGui::End();

        // ----- CV Engine panel (worker rate + stats) -----
        if (ImGui::Begin("CV Engine")) {
            int hz = cvWorker.GetTargetHz();
            ImGui::SetNextItemWidth(180);
            if (ImGui::SliderInt("Target Hz", &hz, 1, 120)) {
                cvWorker.SetTargetHz(hz);
                config.Json()["cv"]["target_hz"] = hz;
            }
            ImGui::Text("Actual:    %5.1f Hz", cvWorker.GetActualHz());
            ImGui::Text("Last cycle: %5.2f ms", cvWorker.GetLastCycleMs());
            ImGui::Separator();
            ImGui::Text("Templates dir: %s", cvWorker.GetTemplatesDir().c_str());
            ImGui::TextDisabled(
                "Worker runs off the render thread.\n"
                "Stale frames are dropped (always-latest pattern).");
        }
        ImGui::End();

        // ----- WebSocket panel (Streamer.bot) -----
        if (ImGui::Begin("WebSocket (Streamer.bot)")) {
            ImGui::Text("URL: %s", wsClient.Url().c_str());
            const bool wsOk = wsClient.IsConnected();
            ImGui::TextColored(
                wsOk ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                     : ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                "%s", wsOk ? "CONNECTED" : "DISCONNECTED");

            ImGui::Separator();
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("Host", wsHostBuf, sizeof(wsHostBuf));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::InputInt("Port", &wsPort, 0, 0);
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("Path", wsEndpointBuf, sizeof(wsEndpointBuf));

            if (ImGui::Button("Apply + Reconnect")) {
                config.Json()["websocket"]["host"]     = std::string(wsHostBuf);
                config.Json()["websocket"]["port"]     = wsPort;
                config.Json()["websocket"]["endpoint"] = std::string(wsEndpointBuf);
                // Hot-swap URL + reconnect WITHOUT tearing down Asio.
                // Calling Stop()+Start() here would try to init_asio() on
                // an already-initialized endpoint -> invalid_state.
                wsClient.Reconnect(BuildWsUrl(config.Json()["websocket"]));
            }

            ImGui::Separator();
            ImGui::Text("Sent:    %llu", (unsigned long long)wsClient.TotalSent());
            ImGui::Text("Dropped: %llu", (unsigned long long)wsClient.TotalDropped());
            ImGui::Text("Queue:   %zu",  wsClient.QueueSize());

            ImGui::Separator();
            ImGui::Text("Streamer.bot actions: %zu",
                        wsClient.AvailableActionsCount());
            ImGui::SameLine();
            ImGui::BeginDisabled(!wsClient.IsConnected());
            if (ImGui::SmallButton("Refresh")) {
                wsClient.RefreshActions();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled(
                "  Fetched via GetActions on connect; pick per ROI in the table.");

            const auto err = wsClient.LastError();
            if (!err.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                                   "Last error: %s", err.c_str());
            }
        }
        ImGui::End();

        // Phase 9: deferred-mutation requests shared between the ROIs
        // table and the floating Inspector. Both panels write to these;
        // they're applied in one pass after both windows have closed so
        // we never mutate the list while it's being iterated and never
        // call CaptureRoiTemplate / disk I/O under the ROI mutex.
        using clk = std::chrono::steady_clock;
        int  captureId             = 0;
        int  captureState          = 0;
        int  deleteId              = 0;
        int  copyId                = 0;
        bool pasteNewRequested     = false;
        bool pasteMirroredRequested= false;
        int  resetWatermarkId      = 0;
        int  deleteStateRoi        = 0;
        int  deleteStateValue      = 0;

        // ----- ROIs panel (per-row capture + threshold + live readout) -----
        if (show_rois_window) {
        if (ImGui::Begin("ROIs & Triggers", &show_rois_window)) {
            // Order matches RoiType enum values 0..3 - DO NOT reorder
            // without renumbering the enum.
            const char* typeNames[] = { "Text", "Gradient", "Pixel", "OCR Zone" };

            int defType = static_cast<int>(roiEditor.DefaultType());
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo("Default type for new ROIs", &defType,
                             typeNames, IM_ARRAYSIZE(typeNames))) {
                roiEditor.SetDefaultType(static_cast<fightsight::RoiType>(defType));
            }

            if (ImGui::Button("Save Profile")) {
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                profileMgr.SaveProfile(
                    profileMgr.ActiveProfile(), roiManager);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(roiEditor.SelectedId() == 0);
            if (ImGui::Button("Delete Selected")) {
                {
                    std::lock_guard<std::mutex> lk(roiManager.Mutex());
                    roiManager.Remove(roiEditor.SelectedId());
                }
                roiEditor.ClearSelection();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                {
                    std::lock_guard<std::mutex> lk(roiManager.Mutex());
                    roiManager.All().clear();
                }
                roiEditor.ClearSelection();
            }
            // Clipboard actions are accessed via right-click on any row
            // (see RoiContextMenu below). No toolbar buttons by design.

            ImGui::Separator();
            {
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                ImGui::Text("Count: %zu   Selected: %s",
                            roiManager.All().size(),
                            roiEditor.SelectedId()
                                ? std::to_string(roiEditor.SelectedId()).c_str()
                                : "(none)");
            }

            if (!force1080p) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                    "Note: Force Internal 1080p is OFF - ROIs are in "
                    "current source resolution, not canonical 1920x1080.");
            }

            // Transient capture feedback (3-second toast)
            const auto sinceCapture = clk::now() - lastCaptureAt;
            if (sinceCapture < std::chrono::seconds(3) && !lastCaptureMsg.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f),
                                   "Capture: %s", lastCaptureMsg.c_str());
            }
            ImGui::Separator();

            // Deferred-mutation vars are declared above the panel so the
            // floating Inspector can also write into them. Anything set
            // here is processed in the single block after the Inspector
            // closes.
            {
            std::lock_guard<std::mutex> lk(roiManager.Mutex());

            if (ImGui::BeginTable("rois_tbl", 11,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                ImGuiTableFlags_SizingFixedFit |
                ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_ScrollX,
                ImVec2(0, 0))) {

                ImGui::TableSetupColumn("ID",            ImGuiTableColumnFlags_WidthFixed, 36.0f);
                ImGui::TableSetupColumn("Name",          ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("Linked Action", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Type",          ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Rect",          ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Thresh",        ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Fire",          ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Value",         ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("State",         ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Cooldown",      ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Action",        ImGuiTableColumnFlags_WidthFixed, 220.0f);
                ImGui::TableHeadersRow();

                // ----- Group ROIs by group_name (Phase 8) -----
                // std::map keeps groups sorted by name; each group's rows
                // can be collapsed via a header row that spans all columns.
                std::map<std::string, std::vector<fightsight::Roi*>> grouped;
                for (auto& roi : roiManager.All()) {
                    const std::string g =
                        roi.group_name.empty() ? "Default" : roi.group_name;
                    grouped[g].push_back(&roi);
                }
                // Open/closed state persists across frames (static map).
                // New groups default to open.
                static std::unordered_map<std::string, bool> sGroupOpen;

                for (auto& kv : grouped) {
                    const std::string& groupName = kv.first;
                    auto& rs = kv.second;

                    auto itOpen = sGroupOpen.find(groupName);
                    if (itOpen == sGroupOpen.end()) {
                        sGroupOpen[groupName] = true;
                        itOpen = sGroupOpen.find(groupName);
                    }
                    bool& groupOpen = itOpen->second;

                    // Render group header row (full-width selectable).
                    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                    ImGui::TableSetColumnIndex(0);
                    char hdrLabel[128];
                    std::snprintf(hdrLabel, sizeof(hdrLabel),
                        "%s  %s  (%zu)##grp_%s",
                        groupOpen ? "[-]" : "[+]",
                        groupName.c_str(), rs.size(),
                        groupName.c_str());
                    if (ImGui::Selectable(
                            hdrLabel, false,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        groupOpen = !groupOpen;
                    }

                    if (!groupOpen) continue;

                    for (auto* roiPtr : rs) {
                        auto& roi = *roiPtr;
                    ImGui::PushID(roi.id);
                    ImGui::TableNextRow();
                    // Phase 9: paint the ROW BACKGROUND with the user's
                    // colour (alpha included). Text stays default white
                    // so glyphs remain readable against any tint. We
                    // colour RowBg0 (the base row layer) and let the
                    // selection layer paint on top when active.
                    ImGui::TableSetBgColor(
                        ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(ImVec4(
                            roi.row_color[0], roi.row_color[1],
                            roi.row_color[2], roi.row_color[3])));

                    // Phase 11b: visual cooldown feedback. While the
                    // CV worker has this ROI gated (cooldown_remaining
                    // > 0) we dim every text widget in the row by
                    // pushing TextDisabled over ImGuiCol_Text. The
                    // matching PopStyleColor lives just above PopID
                    // below, so a row that lights back up on the very
                    // next CV cycle gets its bright text back without
                    // bleeding the disabled style into other rows.
                    //
                    // Lookup happens once at the top of the row so we
                    // can reuse `it` in the Value / Fired / Cooldown
                    // columns further down without re-finding.
                    auto it = cvResults.find(roi.id);
                    const bool rowInCooldown =
                        (it != cvResults.end()
                      && it->second.cooldown_remaining_ms > 0);
                    if (rowInCooldown) {
                        ImGui::PushStyleColor(
                            ImGuiCol_Text,
                            ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    }

                    // ID + row selection. SpanAllColumns gives the
                    // whole-row highlight; AllowOverlap tells the
                    // selectable not to claim hover/clicks for the cells
                    // its rect crosses, so the InputText / DragFloat /
                    // Combos in subsequent columns remain editable.
                    ImGui::TableNextColumn();
                    char idLabel[16];
                    std::snprintf(idLabel, sizeof(idLabel), "%d", roi.id);
                    const bool isSel = (roi.id == roiEditor.SelectedId());
                    if (ImGui::Selectable(idLabel, isSel,
                                          ImGuiSelectableFlags_SpanAllColumns
                                        | ImGuiSelectableFlags_AllowOverlap)) {
                        roiEditor.SetSelected(roi.id);
                        // Phase 9: drive the floating Inspector.
                        selected_roi_id = roi.id;
                    }

                    // ----- Right-click context menu (clipboard ops) -----
                    // Attached to the row Selectable above. Right-click any
                    // row cell that isn't covered by an interactive widget
                    // (ID column, empty space) to bring it up. The popup ID
                    // is scoped by the row's PushID(roi.id).
                    if (ImGui::BeginPopupContextItem("RoiContextMenu")) {
                        if (ImGui::MenuItem("Copy ROI")) {
                            copyId = roi.id;
                        }
                        ImGui::Separator();
                        const bool clip = roiManager.HasClipboard();
                        if (ImGui::MenuItem("Paste New",
                                            nullptr, false, clip)) {
                            pasteNewRequested = true;
                        }
                        if (ImGui::MenuItem("Paste Mirrored (1080p)",
                                            nullptr, false, clip)) {
                            pasteMirroredRequested = true;
                        }
                        ImGui::EndPopup();
                    }

                    // Name = free-form label (also drives the template
                    // PNG filename). Decoupled from the Streamer.bot
                    // action binding - that lives in its own column.
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    {
                        char nameBuf[128];
                        std::snprintf(nameBuf, sizeof(nameBuf), "%s",
                                      roi.name.c_str());
                        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
                            roi.name = nameBuf;
                        }
                    }

                    // Linked Action - Streamer.bot action this ROI fires.
                    // Empty = fallback to "<sanitized name>_Triggered" /
                    // "..._Cleared" (Phase 4 behavior). Otherwise sends
                    // the picked action verbatim on both edges with
                    // args.state distinguishing them.
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    {
                        const char* preview = roi.linked_action.empty()
                            ? "(auto)"
                            : roi.linked_action.c_str();
                        if (availableActions.empty()) {
                            // WS offline - still allow manual typing
                            // so configs can be edited without Streamer.bot
                            char linkBuf[128];
                            std::snprintf(linkBuf, sizeof(linkBuf), "%s",
                                          roi.linked_action.c_str());
                            if (ImGui::InputTextWithHint(
                                    "##link", "(auto - WS offline)",
                                    linkBuf, sizeof(linkBuf))) {
                                roi.linked_action = linkBuf;
                            }
                        } else if (ImGui::BeginCombo("##link", preview)) {
                            static char filterBuf[64] = "";
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            ImGui::InputTextWithHint(
                                "##linkfilter", "filter...",
                                filterBuf, sizeof(filterBuf));
                            const std::string filt(filterBuf);

                            if (ImGui::Selectable("(auto)",
                                roi.linked_action.empty())) {
                                roi.linked_action.clear();
                            }
                            int shown = 0;
                            for (const auto& action : availableActions) {
                                if (!ContainsCI(action, filt)) continue;
                                const bool sel = (action == roi.linked_action);
                                if (ImGui::Selectable(action.c_str(), sel)) {
                                    roi.linked_action = action;
                                }
                                ++shown;
                                if (shown >= 200) {
                                    ImGui::TextDisabled("...truncated");
                                    break;
                                }
                            }
                            if (shown == 0) ImGui::TextDisabled("no match");
                            ImGui::EndCombo();
                        }
                    }

                    // Type. Changing type resets threshold to the new
                    // type's default - Text is [0,1] confidence while
                    // Gradient/Pixel are [0,100] percentages, so the unit
                    // change would silently mis-calibrate the trigger.
                    ImGui::TableNextColumn();
                    int t = static_cast<int>(roi.type);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::Combo("##type", &t, typeNames, IM_ARRAYSIZE(typeNames))) {
                        const auto newType = static_cast<fightsight::RoiType>(t);
                        if (newType != roi.type) {
                            roi.threshold = fightsight::DefaultThresholdFor(newType);
                        }
                        roi.type = newType;
                    }

                    // Rect - clickable button opens a precise-tuning popup.
                    // Critical for tiny ROIs (e.g. 9px tall health bars)
                    // that are impossible to drag-resize in the canvas.
                    ImGui::TableNextColumn();
                    {
                        char rectLabel[80];
                        std::snprintf(rectLabel, sizeof(rectLabel),
                            "%d,%d %dx%d",
                            roi.rect.x, roi.rect.y,
                            roi.rect.width, roi.rect.height);
                        if (ImGui::SmallButton(rectLabel)) {
                            ImGui::OpenPopup("RectTunePopup");
                        }
                        if (ImGui::BeginPopup("RectTunePopup")) {
                            const int canonW = processor.GetDisplayWidth();
                            const int canonH = processor.GetDisplayHeight();
                            const int maxW = std::max(1, canonW);
                            const int maxH = std::max(1, canonH);

                            int x = roi.rect.x;
                            int y = roi.rect.y;
                            int w = roi.rect.width;
                            int h = roi.rect.height;

                            ImGui::Text("Canon: %dx%d", maxW, maxH);
                            ImGui::Separator();

                            const float field = 110.0f;
                            ImGui::SetNextItemWidth(field);
                            ImGui::DragInt("X", &x, 1.0f, 0, maxW);
                            ImGui::SetNextItemWidth(field);
                            ImGui::DragInt("Y", &y, 1.0f, 0, maxH);
                            ImGui::SetNextItemWidth(field);
                            ImGui::DragInt("W", &w, 1.0f, 1, maxW);
                            ImGui::SetNextItemWidth(field);
                            ImGui::DragInt("H", &h, 1.0f, 1, maxH);

                            // Clamp post-edit so cross-axis dependencies
                            // (e.g. X moved right -> W must shrink) settle
                            // in one shot. cv::Rect can't store negatives.
                            x = std::clamp(x, 0, maxW);
                            y = std::clamp(y, 0, maxH);
                            w = std::clamp(w, 1, maxW - x);
                            h = std::clamp(h, 1, maxH - y);

                            roi.rect = cv::Rect(x, y, w, h);

                            ImGui::Separator();
                            if (ImGui::Button("Close")) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                    }

                    // Threshold - unified [0,100] scale across all types
                    // (Phase 9). Text confidence is multiplied by 100 in
                    // CvWorker so this single editor works everywhere.
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::DragFloat("##thr", &roi.threshold,
                                     0.5f, 0.0f, 100.0f, "%.1f");

                    // Fire direction
                    ImGui::TableNextColumn();
                    int dir = roi.fire_when_above ? 0 : 1;
                    const char* dirs[] = { "above", "below" };
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::Combo("##dir", &dir, dirs, IM_ARRAYSIZE(dirs))) {
                        roi.fire_when_above = (dir == 0);
                    }

                    // Live value + status (0-100 for every ROI type now).
                    // `it` was looked up at the top of the row.
                    ImGui::TableNextColumn();
                    if (it != cvResults.end() &&
                        it->second.status == fightsight::CvResult::Status::Ok) {
                        ImGui::Text("%.2f", it->second.value);
                    } else {
                        ImGui::TextDisabled("%s",
                            it != cvResults.end()
                                ? CvStatusLabel(it->second.status)
                                : "----");
                    }

                    // Fired?
                    ImGui::TableNextColumn();
                    const bool fired =
                        it != cvResults.end() && it->second.isFired;
                    if (fired) {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                           "FIRED");
                    } else {
                        ImGui::TextDisabled("idle");
                    }

                    // Cooldown duration (seconds). Phase 9: lifted out of
                    // the Cfg popup into an inline editor next to Thresh.
                    // Only affects Drop-Only Gradient/Pixel ROIs (the CV
                    // worker reads roi.cooldown_duration). The live
                    // remaining-ms readout is shown as a tooltip when the
                    // grace period is currently counting down.
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::DragFloat("##cd", &roi.cooldown_duration,
                                     0.05f, 0.0f, 10.0f, "%.2fs");
                    if (it != cvResults.end()
                        && it->second.cooldown_remaining_ms > 0
                        && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Grace remaining: %.2fs",
                            it->second.cooldown_remaining_ms / 1000.0f);
                    }

                    // Actions: Inspect (selects + ensures the floating
                    // Inspector is visible), Capture (Text only), Del.
                    // The legacy Cfg popup is gone in Phase 9 - all
                    // tuning happens in the persistent Inspector window.
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Inspect")) {
                        roiEditor.SetSelected(roi.id);
                        selected_roi_id       = roi.id;
                        show_inspector_window = true;
                    }
                    ImGui::SameLine();
                    if (roi.type == fightsight::RoiType::Text) {
                        if (ImGui::SmallButton("Capture")) {
                            captureId    = roi.id;
                            captureState = 0;
                        }
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("Del")) {
                        deleteId = roi.id;
                    }

                    // Phase 9: the legacy "Cfg" popup is gone. All
                    // per-ROI tuning lives in the floating Inspector
                    // (rendered after this panel closes).
                    // Phase 11b: pop the cooldown-dim style if we
                    // pushed it at the top of the row. Pairing is one
                    // push -> one pop per row so a fresh-firing row
                    // never inherits a dimmed style from the previous
                    // iteration.
                    if (rowInCooldown) {
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                    } // for roiPtr in group
                } // for group in grouped
                ImGui::EndTable();
            } // BeginTable
            } // lock_guard scope (table iteration)
        } // if Begin "ROIs & Triggers"
        ImGui::End();
        } // if (show_rois_window)

        // ----- Floating ROI Inspector (Phase 9 - replaces Cfg popups) -----
        // Persistent, dockable window. Reads selected_roi_id (mirrored
        // from RoiEditor) and renders all configuration properties for
        // the chosen ROI. Closing it via the X just hides it - the
        // selection itself lives in selected_roi_id / RoiEditor.
        if (show_inspector_window) {
        if (ImGui::Begin("ROI Inspector", &show_inspector_window)) {
            if (selected_roi_id < 0) {
                ImGui::TextDisabled(
                    "No ROI selected. Click an ROI in the table or the "
                    "Spout Viewer to populate this panel.");
            } else {
                fightsight::Roi* roi = nullptr;
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                roi = roiManager.Find(selected_roi_id);
                if (!roi) {
                    ImGui::TextDisabled(
                        "Selection #%d no longer exists.", selected_roi_id);
                } else {
                    ImGui::Text("#%d  %s  (%s)",
                                roi->id, roi->name.c_str(),
                                fightsight::RoiTypeName(roi->type));
                    ImGui::Separator();

                    // ---- Group + Row colour (all ROI types) ----
                    {
                        char groupBuf[64];
                        std::snprintf(groupBuf, sizeof(groupBuf),
                                      "%s", roi->group_name.c_str());
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::InputText("Group", groupBuf,
                                             sizeof(groupBuf))) {
                            roi->group_name = groupBuf;
                            if (roi->group_name.empty())
                                roi->group_name = "Default";
                        }
                        // RGBA picker - alpha lets the user tune how
                        // intense the row-background tint should be.
                        ImGui::ColorEdit4("Row Colour", roi->row_color,
                            ImGuiColorEditFlags_AlphaBar
                          | ImGuiColorEditFlags_AlphaPreview);
                    }

                    // ---- System role (all ROI types) ----
                    {
                        const char* roleNames[] = {
                            "None", "MatchStart", "RoundTransition"
                        };
                        int roleIdx = static_cast<int>(roi->system_role);
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::Combo("System Role", &roleIdx,
                            roleNames, IM_ARRAYSIZE(roleNames))) {
                            roi->system_role =
                                static_cast<fightsight::SystemRole>(roleIdx);
                        }
                    }

                    // ---- Phase 12: state-transition trigger ----
                    // Maps this ROI's successful fire to a global
                    // MatchState switch. Index 0 == "(none)", the rest
                    // line up with the kAllMatchStates enum values
                    // (Waiting / InMatch / Result).
                    {
                        const char* transNames[] = {
                            "(none)", "WAITING", "IN_MATCH", "RESULT"
                        };
                        int transIdx = roi->triggers_state_transition.has_value()
                            ? static_cast<int>(*roi->triggers_state_transition) + 1
                            : 0;
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::Combo("Triggers State Change On Fire:",
                                         &transIdx, transNames,
                                         IM_ARRAYSIZE(transNames))) {
                            if (transIdx == 0) {
                                roi->triggers_state_transition.reset();
                            } else {
                                roi->triggers_state_transition =
                                    static_cast<fightsight::MatchState>(
                                        transIdx - 1);
                            }
                        }
                    }
                    ImGui::Separator();

                    auto liveIt = cvResults.find(roi->id);
                    const bool haveLive =
                        (liveIt != cvResults.end()
                      && liveIt->second.status
                         == fightsight::CvResult::Status::Ok);

                    switch (roi->type) {
                    // -----------------------------------------------
                    case fightsight::RoiType::OcrZone: {
                        // Passive container - no tuning surface beyond
                        // the rect (which is edited in the table) and
                        // group / row colour (above).
                        ImGui::TextWrapped(
                            "OCR Zone - this is a bounding-box "
                            "container used by Text ROIs that link to "
                            "it via \"Read Text From\". No CV evaluation "
                            "runs on this ROI; it just defines the area "
                            "Tesseract will read when its linked Text "
                            "trigger fires.");
                        break;
                    }
                    // -----------------------------------------------
                    case fightsight::RoiType::Text: {
                        // ---- Phase 11: OCR linkage -----------------
                        // Pick an OcrZone ROI whose rect Tesseract will
                        // read when this Text ROI fires (rising edge,
                        // past cooldown). Empty list -> user hasn't
                        // created any OcrZone ROIs in this profile yet.
                        {
                            // We're already holding roiManager.Mutex()
                            // (the inspector locks at entry), so this
                            // direct iteration over All() is safe.
                            struct OcrOpt {
                                int         id;
                                std::string label;
                            };
                            std::vector<OcrOpt> opts;
                            opts.push_back({-1, "(none)"});
                            for (const auto& other : roiManager.All()) {
                                if (other.type
                                    == fightsight::RoiType::OcrZone) {
                                    opts.push_back({
                                        other.id,
                                        "#" + std::to_string(other.id)
                                            + "  " + other.name
                                    });
                                }
                            }

                            int curIdx = 0;
                            for (size_t i = 0; i < opts.size(); ++i) {
                                if (opts[i].id == roi->linked_ocr_roi_id) {
                                    curIdx = static_cast<int>(i);
                                    break;
                                }
                            }

                            ImGui::SetNextItemWidth(240);
                            if (ImGui::BeginCombo("Read Text From:",
                                opts[curIdx].label.c_str())) {
                                for (size_t i = 0; i < opts.size(); ++i) {
                                    const bool sel =
                                        (static_cast<int>(i) == curIdx);
                                    if (ImGui::Selectable(
                                            opts[i].label.c_str(), sel)) {
                                        roi->linked_ocr_roi_id = opts[i].id;
                                    }
                                    if (sel) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            if (roi->linked_ocr_roi_id < 0) {
                                ImGui::TextDisabled(
                                    "(no OCR - normal WS payload only)");
                            } else {
                                ImGui::TextDisabled(
                                    "Tesseract runs in a detached "
                                    "thread on each rising-edge fire.");
                            }
                            ImGui::Separator();
                        }

                        const std::string dir =
                            profileMgr.ActiveTemplatesDir();
                        const std::string sanitized =
                            fightsight::SanitizeName(roi->name);

                        ImGui::Text("Templates dir: %s", dir.c_str());
                        ImGui::Text("Filename pattern: %s_State_<N>.png",
                                    sanitized.c_str());

                        if (haveLive) {
                            // Phase 9: Text confidence is 0-100, same
                            // scale as Gradient/Pixel.
                            ImGui::Text(
                                "Live confidence: %.2f / 100   winning state: %d",
                                liveIt->second.value,
                                liveIt->second.winningState);
                        } else {
                            ImGui::TextDisabled(
                                "No live confidence yet (run CV first)");
                        }

                        ImGui::Spacing();
                        ImGui::Checkbox("Show Ghost Overlay",
                                        &roi->ghost_overlay);

                        ImGui::Separator();
                        ImGui::TextUnformatted("Capture template for state:");

                        static int stateValueBuf = 0;
                        ImGui::SetNextItemWidth(140);
                        ImGui::InputInt("State Integer Value",
                                        &stateValueBuf);
                        if (stateValueBuf < 0) stateValueBuf = 0;

                        if (ImGui::Button("Capture New State")) {
                            captureId    = roi->id;
                            captureState = stateValueBuf;
                        }

                        ImGui::Spacing();
                        ImGui::TextUnformatted("Mapped States:");

                        const auto states =
                            EnumerateCapturedStates(dir, roi->name);
                        if (states.empty()) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(none captured)");
                        } else {
                            for (size_t i = 0; i < states.size(); ++i) {
                                const int s = states[i];
                                ImGui::SameLine();
                                const bool isWinning =
                                    haveLive
                                    && liveIt->second.winningState == s;
                                if (isWinning) {
                                    ImGui::TextColored(
                                        ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                        "[%d]", s);
                                } else {
                                    ImGui::Text("[%d]", s);
                                }
                                ImGui::SameLine(0, 2);
                                char delLabel[32];
                                std::snprintf(delLabel,
                                    sizeof(delLabel), "x##del%d", s);
                                if (ImGui::SmallButton(delLabel)) {
                                    deleteStateRoi   = roi->id;
                                    deleteStateValue = s;
                                }
                            }
                        }
                        break;
                    }
                    // -----------------------------------------------
                    case fightsight::RoiType::Gradient: {
                        ImGui::TextWrapped(
                            "Pick a target colour and tolerate per "
                            "channel. cv::inRange filters HSV; Value = "
                            "%% of pixels inside the bounds.");
                        ImGui::Spacing();

                        float rgb[3];
                        HsvIntsToRgbFloats(
                            roi->base_hsv[0], roi->base_hsv[1],
                            roi->base_hsv[2], rgb);
                        if (ImGui::ColorEdit3("Base Target", rgb,
                            ImGuiColorEditFlags_NoInputs
                          | ImGuiColorEditFlags_PickerHueWheel)) {
                            RgbFloatsToHsvInts(rgb, roi->base_hsv);
                        }
                        ImGui::SameLine();
                        ImGui::Text("HSV %d,%d,%d",
                                    roi->base_hsv[0], roi->base_hsv[1],
                                    roi->base_hsv[2]);

                        if (haveLive) {
                            const int* m = liveIt->second.aux;
                            ImGui::Text("Mean HSV of crop: %d, %d, %d",
                                        m[0], m[1], m[2]);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Snap to mean")) {
                                roi->base_hsv[0] = m[0];
                                roi->base_hsv[1] = m[1];
                                roi->base_hsv[2] = m[2];
                            }
                        } else {
                            ImGui::TextDisabled("No live HSV yet");
                        }

                        ImGui::Spacing();
                        ImGui::Checkbox(
                            "Ignore Colour (Track Brightness Only)",
                            &roi->ignore_color);
                        ImGui::Checkbox(
                            "Drop Only (No Healing)", &roi->drop_only);
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!roi->drop_only);
                        if (ImGui::SmallButton("Reset##wm_g")) {
                            resetWatermarkId = roi->id;
                        }
                        ImGui::EndDisabled();
                        ImGui::Spacing();

                        ImGui::BeginDisabled(roi->ignore_color);
                        ImGui::SliderInt("Hue Tolerance",
                            &roi->hue_tolerance, 0, 90);
                        ImGui::SliderInt("Saturation Tolerance",
                            &roi->sat_tolerance, 0, 128);
                        ImGui::EndDisabled();
                        ImGui::SliderInt("Value / Lightness Tolerance",
                            &roi->val_tolerance, 0, 128);

                        int hLo, hHi, sLo, sHi, vLo, vHi;
                        if (roi->ignore_color) {
                            hLo = 0;   hHi = 179;
                            sLo = 0;   sHi = 255;
                        } else {
                            hLo = std::max(0,   roi->base_hsv[0] - roi->hue_tolerance);
                            hHi = std::min(179, roi->base_hsv[0] + roi->hue_tolerance);
                            sLo = std::max(0,   roi->base_hsv[1] - roi->sat_tolerance);
                            sHi = std::min(255, roi->base_hsv[1] + roi->sat_tolerance);
                        }
                        vLo = std::max(0,   roi->base_hsv[2] - roi->val_tolerance);
                        vHi = std::min(255, roi->base_hsv[2] + roi->val_tolerance);

                        ImGui::Spacing();
                        ImGui::TextUnformatted("Effective inRange bounds:");
                        const ImVec4 loC = HsvIntsToImVec4(hLo, sLo, vLo);
                        const ImVec4 hiC = HsvIntsToImVec4(hHi, sHi, vHi);
                        ImGui::ColorButton("##loSwatch", loC,
                            ImGuiColorEditFlags_NoTooltip,
                            ImVec2(36, 22));
                        ImGui::SameLine();
                        ImGui::Text("min HSV %3d,%3d,%3d", hLo, sLo, vLo);
                        ImGui::ColorButton("##hiSwatch", hiC,
                            ImGuiColorEditFlags_NoTooltip,
                            ImVec2(36, 22));
                        ImGui::SameLine();
                        ImGui::Text("max HSV %3d,%3d,%3d", hHi, sHi, vHi);

                        if (haveLive) {
                            ImGui::Spacing();
                            const float v = liveIt->second.value;
                            ImGui::Text("Live match: ");
                            ImGui::SameLine();
                            const ImVec4 vc =
                                (v >= roi->threshold)
                                  ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                  : ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
                            ImGui::TextColored(vc, "%.1f %%  (thr %.1f)",
                                               v, roi->threshold);
                        }
                        break;
                    }
                    // -----------------------------------------------
                    case fightsight::RoiType::Pixel: {
                        ImGui::TextWrapped(
                            "Mean BGR of crop compared to a target "
                            "colour. Value = similarity %% (100 = "
                            "identical, 0 = max distance).");
                        ImGui::Spacing();

                        ImGui::Checkbox(
                            "Drop Only (No Healing)", &roi->drop_only);
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!roi->drop_only);
                        if (ImGui::SmallButton("Reset##wm_p")) {
                            resetWatermarkId = roi->id;
                        }
                        ImGui::EndDisabled();
                        ImGui::Spacing();

                        float rgb[3] = {
                            roi->target_bgr[2] / 255.0f,
                            roi->target_bgr[1] / 255.0f,
                            roi->target_bgr[0] / 255.0f,
                        };
                        if (ImGui::ColorEdit3("Target", rgb,
                            ImGuiColorEditFlags_NoInputs
                          | ImGuiColorEditFlags_PickerHueWheel)) {
                            roi->target_bgr[2] = std::clamp(
                                int(rgb[0] * 255.0f), 0, 255);
                            roi->target_bgr[1] = std::clamp(
                                int(rgb[1] * 255.0f), 0, 255);
                            roi->target_bgr[0] = std::clamp(
                                int(rgb[2] * 255.0f), 0, 255);
                        }
                        ImGui::SameLine();
                        ImGui::Text("BGR %d,%d,%d",
                                    roi->target_bgr[0],
                                    roi->target_bgr[1],
                                    roi->target_bgr[2]);

                        if (haveLive) {
                            ImGui::Text("Live similarity: %.1f %%",
                                        liveIt->second.value);
                            const int* m = liveIt->second.aux;
                            const ImVec4 swatch(
                                m[2] / 255.0f,
                                m[1] / 255.0f,
                                m[0] / 255.0f, 1.0f);
                            ImGui::ColorButton("##actual", swatch,
                                ImGuiColorEditFlags_NoTooltip,
                                ImVec2(30, 18));
                            ImGui::SameLine();
                            ImGui::Text("Actual mean BGR %d,%d,%d",
                                        m[0], m[1], m[2]);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Pick from frame")) {
                                roi->target_bgr[0] = m[0];
                                roi->target_bgr[1] = m[1];
                                roi->target_bgr[2] = m[2];
                            }
                        } else {
                            ImGui::TextDisabled("No live colour yet");
                        }
                        break;
                    }
                    }
                }
            }
        }
        ImGui::End();
        } // if (show_inspector_window)

        // ----- Match Manager dashboard (Phase 12) -----------------------
        // Global state machine + group×state activation matrix. The CV
        // worker snapshots IsFilteringEnabled() + the active groups for
        // current_state at the top of each cycle; everything edited
        // here takes effect on the very next cycle.
        if (show_match_manager_window) {
        if (ImGui::Begin("Match Manager", &show_match_manager_window)) {
            const fightsight::MatchState cs = matchMgr.GetCurrentState();
            const char* csName = fightsight::MatchStateName(cs);

            // Big colored header for current state. WAITING = grey,
            // IN_MATCH = green, RESULT = orange.
            ImVec4 csColor;
            switch (cs) {
                case fightsight::MatchState::Waiting:
                    csColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); break;
                case fightsight::MatchState::InMatch:
                    csColor = ImVec4(0.4f, 1.0f, 0.5f, 1.0f); break;
                case fightsight::MatchState::Result:
                    csColor = ImVec4(1.0f, 0.7f, 0.3f, 1.0f); break;
            }
            ImGui::TextUnformatted("Current State:");
            ImGui::SameLine();
            ImGui::TextColored(csColor, "%s", csName);

            // Manual force buttons - useful for testing without waiting
            // for a real state-transition ROI to fire.
            ImGui::TextDisabled("Force state (debug):");
            ImGui::SameLine();
            if (ImGui::SmallButton("WAITING")) {
                matchMgr.SetCurrentState(fightsight::MatchState::Waiting);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("IN_MATCH")) {
                matchMgr.SetCurrentState(fightsight::MatchState::InMatch);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("RESULT")) {
                matchMgr.SetCurrentState(fightsight::MatchState::Result);
            }

            ImGui::Separator();

            // Master on/off. When this is OFF every ROI is processed
            // regardless of group - the gate is bypassed entirely so
            // existing profiles keep behaving the same.
            bool en = matchMgr.IsFilteringEnabled();
            if (ImGui::Checkbox(
                "Enable Match Manager filtering (gate ROIs by group)",
                &en)) {
                matchMgr.SetFilteringEnabled(en);
            }
            ImGui::TextDisabled(
                "  Off: every ROI runs every cycle. "
                "On: only ticked groups in the current state run.");

            ImGui::Separator();

            // ---- Group × State matrix ----
            // Collect unique group names from the live ROI set so the
            // matrix tracks renames / new groups without any extra UX.
            std::vector<std::string> groups;
            {
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                std::unordered_set<std::string> seen;
                for (const auto& r : roiManager.All()) {
                    const std::string g =
                        r.group_name.empty() ? "Default" : r.group_name;
                    if (seen.insert(g).second) groups.push_back(g);
                }
            }
            std::sort(groups.begin(), groups.end());

            if (groups.empty()) {
                ImGui::TextDisabled(
                    "(No groups defined - add ROIs first.)");
            } else if (ImGui::BeginTable("mm_groups", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
              | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Group",
                    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("WAITING",
                    ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("IN_MATCH",
                    ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("RESULT",
                    ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableHeadersRow();

                for (const auto& g : groups) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(g.c_str());
                    for (auto s : fightsight::kAllMatchStates) {
                        ImGui::TableNextColumn();
                        bool active = matchMgr.IsGroupActiveIn(s, g);
                        ImGui::PushID(static_cast<int>(s));
                        ImGui::PushID(g.c_str());
                        if (ImGui::Checkbox("##chk", &active)) {
                            matchMgr.SetGroupActiveIn(s, g, active);
                        }
                        ImGui::PopID();
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
        } // if (show_match_manager_window)

        // ---- Deferred ROI mutations (both panels write into these) ----
        // Runs unconditionally so headless mode + Inspector-only mode
        // still apply queued changes from the visible windows.
        if (copyId) {
            std::lock_guard<std::mutex> lk(roiManager.Mutex());
            roiManager.Copy(copyId);
        }
        if (pasteNewRequested) {
            std::lock_guard<std::mutex> lk(roiManager.Mutex());
            const int newId = roiManager.PasteNew();
            if (newId) {
                roiEditor.SetSelected(newId);
                selected_roi_id = newId;
            }
        }
        if (pasteMirroredRequested) {
            std::lock_guard<std::mutex> lk(roiManager.Mutex());
            const int canonW = processor.GetDisplayWidth() > 0
                ? processor.GetDisplayWidth() : 1920;
            const int newId = roiManager.PasteMirrored(canonW);
            if (newId) {
                roiEditor.SetSelected(newId);
                selected_roi_id = newId;
            }
        }
        if (resetWatermarkId) {
            cvWorker.ResetWatermark(resetWatermarkId);
        }
        if (deleteId) {
            {
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                roiManager.Remove(deleteId);
            }
            if (roiEditor.SelectedId() == deleteId) {
                roiEditor.ClearSelection();
            }
            if (selected_roi_id == deleteId) selected_roi_id = -1;
        }
        if (captureId) {
            fightsight::Roi snapshot;
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                if (const fightsight::Roi* r = roiManager.Find(captureId)) {
                    snapshot = *r;
                    found = true;
                }
            }
            if (found) {
                std::string result;
                if (CaptureRoiTemplate(processor, snapshot,
                                       profileMgr.ActiveTemplatesDir(),
                                       captureState,
                                       result)) {
                    cvWorker.InvalidateTemplate(captureId);
                    ghostCache.Invalidate(result);
                    lastCaptureMsg = "saved " + result;
                } else {
                    lastCaptureMsg = "FAILED: " + result;
                }
                lastCaptureAt = clk::now();
            }
        }
        if (deleteStateRoi) {
            fightsight::Roi snap;
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                if (const fightsight::Roi* r =
                    roiManager.Find(deleteStateRoi)) {
                    snap = *r;
                    found = true;
                }
            }
            if (found) {
                const std::string path =
                    profileMgr.ActiveTemplatesDir() + "/"
                    + fightsight::SanitizeName(snap.name)
                    + "_State_" + std::to_string(deleteStateValue)
                    + ".png";
                std::error_code ec;
                std::filesystem::remove(path, ec);
                cvWorker.InvalidateTemplate(deleteStateRoi);
                ghostCache.Invalidate(path);
                lastCaptureMsg = ec
                    ? ("delete failed: " + path)
                    : ("deleted " + path);
                lastCaptureAt = clk::now();
            }
        }

        // ----- Phase 12: Reference image load/clear (end of frame) -------
        // Fired AFTER ImGui::Render so the menu's frame is already
        // committed before the modal Win32 dialog steals focus.
        if (requestClearReferenceImage) {
            refImage.Release();
        }
        if (requestLoadReferenceImage) {
            const std::string path = OpenImageFileDialog(hwnd);
            if (!path.empty()) {
                if (LoadReferenceImage(path, g_pd3dDevice, refImage)) {
                    show_reference_image = true;
                } else {
                    ::MessageBoxW(hwnd,
                        L"Failed to load reference image. "
                        L"Make sure the file is a readable PNG/JPG/BMP/TGA.",
                        L"FightSight", MB_ICONWARNING);
                }
            }
        }
        // ----- Phase 12b: Capture current frame to /screen_shots/ -------
        // Reuses the toast UI plumbing already used by Capture-ROI so
        // the user sees the saved path (or the failure reason) in the
        // ROIs panel header for a few seconds.
        if (requestCaptureScreenshot) {
            std::string result;
            if (CaptureFullScreenshot(processor,
                                      profileMgr.ActiveTemplatesDir(),
                                      result)) {
                lastCaptureMsg = "screenshot saved " + result;
            } else {
                lastCaptureMsg = "screenshot FAILED: " + result;
            }
            lastCaptureAt = std::chrono::steady_clock::now();
        }

        // ----- Deferred profile actions (end of frame) --------------------
        if (requestSaveCurrentProfile) {
            std::lock_guard<std::mutex> lk(roiManager.Mutex());
            profileMgr.SaveProfile(profileMgr.ActiveProfile(), roiManager);
        }
        if (requestOpenProfilesFolder) {
            const auto wDir = std::filesystem::absolute(
                profileMgr.ProfilesDir()).wstring();
            ::ShellExecuteW(nullptr, L"open", wDir.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (!requestSwitchProfile.empty()
            && requestSwitchProfile != profileMgr.ActiveProfile()) {
            // Auto-save outgoing profile, swap roi list, point templates
            // dir at the new profile (which invalidates CvWorker's caches
            // + runtime state via SetTemplatesDir's m_dirtyAll flag).
            {
                std::lock_guard<std::mutex> lk(roiManager.Mutex());
                profileMgr.SaveProfile(
                    profileMgr.ActiveProfile(), roiManager);
                roiManager.All().clear();
                profileMgr.LoadProfile(requestSwitchProfile, roiManager);
            }
            roiEditor.ClearSelection();
            selected_roi_id = -1; // Phase 9: old selection no longer valid.
            cvWorker.SetTemplatesDir(profileMgr.ActiveTemplatesDir());
            // All previously cached ghost paths now point into a different
            // game's templates folder - release them so the next ghost
            // pass reloads from the active profile's files.
            ghostCache.Clear();
            config.Json()["active_profile"] = requestSwitchProfile;
        }

        // --- Render ---
        ImGui::Render();
        constexpr float clear[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // vsync ON -> ~60 FPS on a 60Hz panel
    }

    // --- Cleanup: stop worker threads BEFORE tearing down what they touch ---
    cvWorker.Stop();   // releases FrameProcessor + RoiManager + WsClient
    wsClient.Stop();   // joins Asio thread

    // Persist ROIs into the active profile, then app-wide settings into config.
    {
        std::lock_guard<std::mutex> lk(roiManager.Mutex());
        profileMgr.SaveProfile(profileMgr.ActiveProfile(), roiManager);
    }
    config.Json()["viewport"]["force_1080p"] = force1080p;
    config.Json()["websocket"]["host"]       = std::string(wsHostBuf);
    config.Json()["websocket"]["port"]       = wsPort;
    config.Json()["websocket"]["endpoint"]   = std::string(wsEndpointBuf);
    config.Json()["cv"]["target_hz"]         = cvWorker.GetTargetHz();
    config.Json()["active_profile"]          = profileMgr.ActiveProfile();
    config.Json()["match_manager"]           = matchMgr.ToJson();
    // Make sure no stale legacy "rois" field is written back to disk.
    if (config.Json().contains("rois")) config.Json().erase("rois");
    config.Save("config/config.json");

    refImage.Release();    // free the reference-image DX11 texture+SRV
    ghostCache.Shutdown(); // releases all DX11 textures before device dies
    processor.Shutdown();
    spout.Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ---------- DX11 helpers ----------
static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount         = 2;
    sd.BufferDesc.Width    = 0;
    sd.BufferDesc.Height   = 0;
    sd.BufferDesc.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags               = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage         = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow        = hWnd;
    sd.SampleDesc.Count    = 1;
    sd.SampleDesc.Quality  = 0;
    sd.Windowed            = TRUE;
    sd.SwapEffect          = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL outLevel{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevels, _countof(featureLevels),
        D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &outLevel, &g_pd3dDeviceContext);

    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
        backBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

// ---------- WndProc ----------
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0,
                (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // disable Alt menu
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
