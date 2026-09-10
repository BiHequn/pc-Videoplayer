#include "server-video-dialog.h"

#include "service/api-client.h"
#include "service/media-client.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
constexpr wchar_t kClassName[] = L"NativePlayerServerVideo";
constexpr int kHostEdit = 2001;
constexpr int kApiPortEdit = 2002;
constexpr int kMediaPortEdit = 2003;
constexpr int kUserEdit = 2004;
constexpr int kPasswordEdit = 2005;
constexpr int kLoginButton = 2006;
constexpr int kVideoList = 2007;
constexpr int kPlayButton = 2008;
constexpr int kRefreshButton = 2009;
constexpr int kCancelButton = 2010;
constexpr int kProgress = 2011;
constexpr int kStatusText = 2012;
constexpr UINT kDownloadProgress = WM_APP + 20;
constexpr UINT kDownloadFinished = WM_APP + 21;
constexpr UINT kSessionFinished = WM_APP + 22;

struct DownloadOutput {
    MediaResult result;
    std::string path;
    int videoId = 0;
};

struct SessionOutput {
    MediaResult result;
    ServerSettings settings;
    std::string accessToken;
    std::vector<RemoteVideoInfo> videos;
};

struct DialogState {
    ServerVideoSelection *selection = nullptr;
    HWND hostEdit = nullptr;
    HWND apiPortEdit = nullptr;
    HWND mediaPortEdit = nullptr;
    HWND userEdit = nullptr;
    HWND passwordEdit = nullptr;
    HWND loginButton = nullptr;
    HWND videoList = nullptr;
    HWND playButton = nullptr;
    HWND refreshButton = nullptr;
    HWND cancelButton = nullptr;
    HWND progress = nullptr;
    HWND statusText = nullptr;
    HFONT font = nullptr;
    bool ownsFont = false;
    bool finished = false;
    bool accepted = false;
    bool downloading = false;
    bool sessionRunning = false;
    bool closeWhenFinished = false;
    std::atomic_bool cancelled{false};
    std::thread worker;
    std::vector<RemoteVideoInfo> videos;
    std::string accessToken;
    ServerSettings settings;
};

std::string wideToUtf8(const std::wstring &value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0,
                                     nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string &value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

fs::path settingsPath() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, path)))
        return fs::path(path) / L"VideoPlayer1" / L"settings.json";
    return fs::temp_directory_path() / L"VideoPlayer1" / L"settings.json";
}

void loadSettings(ServerSettings *settings) {
    try {
        std::ifstream input(settingsPath());
        if (!input) return;
        nlohmann::json value;
        input >> value;
        settings->host = utf8ToWide(value.value("host", "192.168.62.129"));
        settings->apiPort = value.value("api_port", 8000);
        settings->mediaPort = value.value("media_port", 8888);
    } catch (...) {
    }
}

void saveSettings(const ServerSettings &settings) {
    try {
        fs::path path = settingsPath();
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        output << nlohmann::json{{"host", wideToUtf8(settings.host)},
                                 {"api_port", settings.apiPort},
                                 {"media_port", settings.mediaPort}}.dump(2);
    } catch (...) {
    }
}

std::wstring readText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    if (length) GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

unsigned short parsePort(HWND control, unsigned short fallback) {
    std::wstring text = readText(control);
    wchar_t *end = nullptr;
    unsigned long value = wcstoul(text.c_str(), &end, 10);
    return end && *end == L'\0' && value > 0 && value <= 65535
               ? static_cast<unsigned short>(value)
               : fallback;
}

void setFont(HWND control, HFONT font) {
    if (control && font) SendMessageW(control, WM_SETFONT,
                                      reinterpret_cast<WPARAM>(font), TRUE);
}

void setBusy(DialogState *state, bool busy) {
    EnableWindow(state->hostEdit, !busy);
    EnableWindow(state->apiPortEdit, !busy);
    EnableWindow(state->mediaPortEdit, !busy);
    EnableWindow(state->userEdit, !busy);
    EnableWindow(state->passwordEdit, !busy);
    EnableWindow(state->loginButton, !busy);
    EnableWindow(state->videoList, !busy);
    EnableWindow(state->playButton, !busy);
    EnableWindow(state->refreshButton, !busy);
    SetWindowTextW(state->cancelButton,
                   busy ? (state->sessionRunning ? L"取消请求" : L"取消下载") : L"关闭");
    ShowWindow(state->progress, busy ? SW_SHOW : SW_HIDE);
}

void renderVideos(DialogState *state) {
    SendMessageW(state->videoList, LB_RESETCONTENT, 0, 0);
    for (const RemoteVideoInfo &video : state->videos) {
        std::wstring label = utf8ToWide(video.filename) + L"  (" +
            std::to_wstring(video.filesize / 1024) + L" KB, 播放 " +
            std::to_wstring(video.playCount) + L")";
        SendMessageW(state->videoList, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
    }
    SetWindowTextW(state->statusText,
                   state->videos.empty() ? L"服务器暂无视频" : L"请选择视频后下载并播放");
}

void startSessionRequest(HWND window, DialogState *state, bool loginRequired) {
    ServerSettings settings;
    settings.host = readText(state->hostEdit);
    settings.apiPort = parsePort(state->apiPortEdit, 8000);
    settings.mediaPort = parsePort(state->mediaPortEdit, 8888);
    std::string username = wideToUtf8(readText(state->userEdit));
    std::string password = wideToUtf8(readText(state->passwordEdit));
    if (settings.host.empty() || (loginRequired && (username.empty() || password.empty()))) {
        MessageBoxW(window, L"请填写服务器、用户名和密码。", L"登录失败",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    std::string existingToken = state->accessToken;
    state->accessToken.clear();
    state->videos.clear();
    renderVideos(state);
    state->downloading = true;
    state->sessionRunning = true;
    state->cancelled.store(false);
    setBusy(state, true);
    SetWindowTextW(state->statusText, loginRequired ? L"正在登录..." : L"正在刷新...");
    state->worker = std::thread([window, settings, username, password,
                                 existingToken, loginRequired] {
        std::unique_ptr<SessionOutput> output(new SessionOutput());
        try {
            output->settings = settings;
            if (loginRequired) {
                LoginResult login = loginToApi(settings, username, password);
                if (!login.success) {
                    output->result.error = login.error;
                } else {
                    output->accessToken = login.accessToken;
                }
            } else {
                output->accessToken = existingToken;
            }
            if (!output->accessToken.empty()) {
                MediaClient media;
                output->result = media.connectAndAuthenticate(settings, output->accessToken);
                if (output->result.success)
                    output->result = media.listVideos(&output->videos);
            } else if (output->result.error.empty()) {
                output->result.error = L"登录状态已失效，请重新登录。";
            }
        } catch (...) {
            output->result.success = false;
            output->result.error = L"服务器请求发生未知错误。";
        }
        if (!PostMessageW(window, kSessionFinished, 0,
                          reinterpret_cast<LPARAM>(output.get()))) return;
        output.release();
    });
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    DialogState *state = reinterpret_cast<DialogState *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        state = static_cast<DialogState *>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    if (message == WM_CREATE) {
        state->font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE,
                                  L"Microsoft YaHei UI");
        state->ownsFont = state->font != nullptr;
        if (!state->font) state->font = reinterpret_cast<HFONT>(
            GetStockObject(DEFAULT_GUI_FONT));
        HINSTANCE instance = GetModuleHandleW(nullptr);
        HWND labels[] = {
            CreateWindowExW(0, L"STATIC", L"服务器", WS_CHILD | WS_VISIBLE,
                            20, 18, 65, 24, window, nullptr, instance, nullptr),
            CreateWindowExW(0, L"STATIC", L"API端口", WS_CHILD | WS_VISIBLE,
                            300, 18, 65, 24, window, nullptr, instance, nullptr),
            CreateWindowExW(0, L"STATIC", L"媒体端口", WS_CHILD | WS_VISIBLE,
                            440, 18, 70, 24, window, nullptr, instance, nullptr),
            CreateWindowExW(0, L"STATIC", L"用户名", WS_CHILD | WS_VISIBLE,
                            20, 58, 65, 24, window, nullptr, instance, nullptr),
            CreateWindowExW(0, L"STATIC", L"密码", WS_CHILD | WS_VISIBLE,
                            300, 58, 45, 24, window, nullptr, instance, nullptr)};
        state->hostEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"192.168.62.129",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 85, 15, 200, 27,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHostEdit)), instance, nullptr);
        state->apiPortEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8000",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 365, 15, 60, 27,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApiPortEdit)), instance, nullptr);
        state->mediaPortEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8888",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 515, 15, 60, 27,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMediaPortEdit)), instance, nullptr);
        state->userEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 85, 55, 200, 27,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUserEdit)), instance, nullptr);
        state->passwordEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD, 350, 55, 225, 27,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPasswordEdit)), instance, nullptr);
        state->loginButton = CreateWindowExW(0, L"BUTTON", L"登录并刷新",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 20, 96, 120, 32,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLoginButton)), instance, nullptr);
        state->refreshButton = CreateWindowExW(0, L"BUTTON", L"刷新列表",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 155, 96, 100, 32,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)), instance, nullptr);
        state->playButton = CreateWindowExW(0, L"BUTTON", L"下载并播放",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 270, 96, 120, 32,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlayButton)), instance, nullptr);
        state->cancelButton = CreateWindowExW(0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 475, 96, 100, 32,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButton)), instance, nullptr);
        state->videoList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY,
            20, 145, 555, 225, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVideoList)),
            instance, nullptr);
        state->statusText = CreateWindowExW(0, L"STATIC", L"请登录服务器",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 382, 555, 22, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusText)), instance, nullptr);
        state->progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD, 20, 412, 555, 20, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgress)), instance, nullptr);
        SendMessageW(state->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SetWindowTextW(state->hostEdit, state->settings.host.c_str());
        SetWindowTextW(state->apiPortEdit,
                       std::to_wstring(state->settings.apiPort).c_str());
        SetWindowTextW(state->mediaPortEdit,
                       std::to_wstring(state->settings.mediaPort).c_str());
        for (HWND label : labels) setFont(label, state->font);
        HWND controls[] = {state->hostEdit, state->apiPortEdit, state->mediaPortEdit,
                           state->userEdit, state->passwordEdit, state->loginButton,
                           state->refreshButton, state->playButton, state->cancelButton,
                           state->videoList, state->statusText};
        for (HWND control : controls) setFont(control, state->font);
        SetFocus(state->userEdit);
        return 0;
    }
    if (message == kDownloadProgress && state) {
        int percent = static_cast<int>(wParam);
        SendMessageW(state->progress, PBM_SETPOS, percent, 0);
        std::wstring status = L"正在下载：" + std::to_wstring(percent) + L"%";
        SetWindowTextW(state->statusText, status.c_str());
        return 0;
    }
    if (message == kDownloadFinished && state) {
        std::unique_ptr<DownloadOutput> output(
            reinterpret_cast<DownloadOutput *>(lParam));
        if (state->worker.joinable()) state->worker.join();
        state->downloading = false;
        state->sessionRunning = false;
        if (state->closeWhenFinished) {
            DestroyWindow(window);
            return 0;
        }
        setBusy(state, false);
        if (output && output->result.success && !state->cancelled.load()) {
            state->selection->localPath = output->path;
            state->selection->videoId = output->videoId;
            state->selection->settings = state->settings;
            state->selection->accessToken = state->accessToken;
            state->accepted = true;
            DestroyWindow(window);
        } else if (!state->cancelled.load()) {
            MessageBoxW(window,
                        output ? output->result.error.c_str() : L"下载失败。",
                        L"下载失败", MB_OK | MB_ICONERROR);
        }
        state->cancelled.store(false);
        return 0;
    }
    if (message == kSessionFinished && state) {
        std::unique_ptr<SessionOutput> output(reinterpret_cast<SessionOutput *>(lParam));
        if (state->worker.joinable()) state->worker.join();
        state->downloading = false;
        if (state->closeWhenFinished) {
            DestroyWindow(window);
            return 0;
        }
        setBusy(state, false);
        if (state->cancelled.load()) {
            setBusy(state, false);
            SetWindowTextW(state->statusText, L"请求已取消");
            state->cancelled.store(false);
        } else if (output && output->result.success) {
            state->settings = output->settings;
            state->accessToken = std::move(output->accessToken);
            state->videos = std::move(output->videos);
            saveSettings(state->settings);
            renderVideos(state);
        } else {
            MessageBoxW(window, output ? output->result.error.c_str() : L"服务器请求失败。",
                        L"服务器请求失败", MB_OK | MB_ICONERROR);
            SetWindowTextW(state->statusText, L"请重新登录服务器");
        }
        return 0;
    }
    if (message == WM_COMMAND && state) {
        int id = LOWORD(wParam);
        if (id == kLoginButton) {
            startSessionRequest(window, state, true);
            return 0;
        }
        if (id == kRefreshButton) {
            if (state->accessToken.empty())
                MessageBoxW(window, L"请先登录服务器。", L"提示", MB_OK | MB_ICONWARNING);
            else
                startSessionRequest(window, state, false);
            return 0;
        }
        if (id == kPlayButton ||
            (id == kVideoList && HIWORD(wParam) == LBN_DBLCLK)) {
            int index = static_cast<int>(SendMessageW(state->videoList,
                                                      LB_GETCURSEL, 0, 0));
            if (state->accessToken.empty() || index < 0 ||
                index >= static_cast<int>(state->videos.size())) {
                MessageBoxW(window, L"请先登录并选择服务器视频。", L"提示",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            int videoId = state->videos[static_cast<std::size_t>(index)].id;
            state->cancelled.store(false);
            state->downloading = true;
            state->sessionRunning = false;
            setBusy(state, true);
            SetWindowTextW(state->statusText, L"正在下载：0%");
            SendMessageW(state->progress, PBM_SETPOS, 0, 0);
            ServerSettings settings = state->settings;
            std::string accessToken = state->accessToken;
            state->worker = std::thread([state, window, settings, accessToken, videoId] {
                auto *output = new DownloadOutput();
                output->videoId = videoId;
                try {
                    MediaClient media;
                    output->result = media.connectAndAuthenticate(settings, accessToken);
                    if (output->result.success)
                        output->result = media.downloadVideo(
                            videoId,
                            [window](int percent) {
                                PostMessageW(window, kDownloadProgress,
                                             static_cast<WPARAM>(percent), 0);
                            },
                            state->cancelled, &output->path);
                } catch (...) {
                    output->result.error = L"下载线程发生未知错误。";
                }
                if (!PostMessageW(window, kDownloadFinished, 0,
                                  reinterpret_cast<LPARAM>(output))) delete output;
            });
            return 0;
        }
        if (id == kCancelButton) {
            if (state->downloading) {
                state->cancelled.store(true);
                EnableWindow(state->cancelButton, FALSE);
                SetWindowTextW(state->statusText, L"正在取消下载...");
            } else DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE && state) {
        if (state->downloading) {
            state->closeWhenFinished = true;
            state->cancelled.store(true);
            EnableWindow(state->cancelButton, FALSE);
            SetWindowTextW(state->statusText, L"正在取消下载...");
        } else DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        if (state) {
            state->cancelled.store(true);
            if (state->worker.joinable()) state->worker.join();
            if (state->ownsFont && state->font) DeleteObject(state->font);
            state->finished = true;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

bool showServerVideoDialog(HWND owner, ServerVideoSelection *selection) {
    if (!selection) return false;
    *selection = {};
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    if (!InitCommonControlsEx(&controls)) return false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    DialogState state;
    state.selection = selection;
    loadSettings(&state.settings);
    constexpr int width = 615;
    constexpr int height = 485;
    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"VideoPlayer1 - 服务器视频",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
        width, height, owner, nullptr, instance, &state);
    if (!window) return false;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    MSG message{};
    while (!state.finished) {
        BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) break;
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    return state.accepted;
}
