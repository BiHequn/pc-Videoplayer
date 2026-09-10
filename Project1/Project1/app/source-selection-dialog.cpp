#include "source-selection-dialog.h"
#include "server-video-dialog.h"
#include "media/vod-downloader.h"

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {
constexpr wchar_t kWindowClassName[] = L"NativePlayerSourceSelection";

constexpr int kUrlEditId = 1001;
constexpr int kOpenFileButtonId = 1002;
constexpr int kPlayUrlButtonId = 1003;
constexpr int kCancelButtonId = 1004;
constexpr int kProgressBarId = 1005;
constexpr int kProgressTextId = 1006;
constexpr int kServerVideoButtonId = 1007;

constexpr UINT kDownloadProgressMessage = WM_APP + 1;
constexpr UINT kDownloadFinishedMessage = WM_APP + 2;

struct SourceDialogState {
    PlaybackSelection *selection = nullptr;
    HWND urlEdit = nullptr;
    HWND openFileButton = nullptr;
    HWND playUrlButton = nullptr;
    HWND cancelButton = nullptr;
    HWND serverVideoButton = nullptr;
    HWND progressBar = nullptr;
    HWND progressText = nullptr;
    HFONT font = nullptr;
    std::thread downloadThread;
    std::atomic_bool downloadCancelled{false};
    bool ownsFont = false;
    bool downloading = false;
    bool closeWhenFinished = false;
    bool accepted = false;
    bool finished = false;
};

std::string wideToUtf8(const wchar_t *text) {
    if (!text || text[0] == L'\0') return {};

    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

    if (size <= 1) return {};

    std::string result(static_cast<std::size_t>(size), '\0');

    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);

    result.pop_back();
    return result;
}

std::string selectLocalMediaFile(HWND owner) {
    wchar_t filePath[32768] = {};

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = filePath;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(filePath) / sizeof(filePath[0]));

    dialog.lpstrFilter = L"\u5A92\u4F53\u6587\u4EF6\0"
                         L"*.mp4;*.flv;*.m3u8;*.mkv;*.avi;*.mov;*.webm\0"
                         L"\u6240\u6709\u6587\u4EF6\0"
                         L"*.*\0";

    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) return {};

    return wideToUtf8(filePath);
}

void setControlFont(HWND control, HFONT font) {
    if (!control || !font) return;

    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

LRESULT CALLBACK sourceWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    SourceDialogState *state = reinterpret_cast<SourceDialogState *>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        CREATESTRUCTW *createInfo = reinterpret_cast<CREATESTRUCTW *>(lParam);

        state = static_cast<SourceDialogState *>(createInfo->lpCreateParams);

        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        return TRUE;
    }

    if (message == WM_CREATE) {
        state->font =
            CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        if (state->font) {
            state->ownsFont = true;
        } else {
            state->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        HWND label = CreateWindowExW(
            0, L"STATIC", L"\u8F93\u5165HLS\u70B9\u64AD\u6216HTTP-FLV\u70B9\u64AD\u5730\u5740\uFF1A",
            WS_CHILD | WS_VISIBLE, 20, 20, 500, 22, window, nullptr, GetModuleHandleW(nullptr), nullptr);

        state->urlEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 20, 48, 500, 28,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUrlEditId)), GetModuleHandleW(nullptr), nullptr);

        state->openFileButton = CreateWindowExW(
            0, L"BUTTON", L"\u6253\u5F00\u672C\u5730\u6587\u4EF6", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            20, 105, 150, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenFileButtonId)),
            GetModuleHandleW(nullptr), nullptr);

        state->playUrlButton = CreateWindowExW(0, L"BUTTON", L"\u64AD\u653E\u7F51\u7EDC\u5730\u5740",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 195, 105, 150, 34,
                                               window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlayUrlButtonId)),
                                               GetModuleHandleW(nullptr), nullptr);

        state->cancelButton = CreateWindowExW(
            0, L"BUTTON", L"\u53D6\u6D88", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 370, 105, 150, 34,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)), GetModuleHandleW(nullptr), nullptr);

        state->serverVideoButton = CreateWindowExW(
            0, L"BUTTON", L"\u670D\u52A1\u5668\u89C6\u9891", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            20, 150, 500, 34, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kServerVideoButtonId)),
            GetModuleHandleW(nullptr), nullptr);

        state->progressText =
            CreateWindowExW(0, L"STATIC", L"\u7B49\u5F85\u4E0B\u8F7D", WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 205,
                            500, 22, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgressTextId)),
                            GetModuleHandleW(nullptr), nullptr);

        state->progressBar =
            CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 20, 232, 500, 20, window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgressBarId)), GetModuleHandleW(nullptr),
                            nullptr);

        SendMessageW(state->progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        setControlFont(label, state->font);
        setControlFont(state->urlEdit, state->font);
        setControlFont(state->openFileButton, state->font);
        setControlFont(state->playUrlButton, state->font);
        setControlFont(state->cancelButton, state->font);
        setControlFont(state->serverVideoButton, state->font);
        setControlFont(state->progressText, state->font);

        SetFocus(state->urlEdit);
        return 0;
    }

    if (message == kDownloadProgressMessage && state) {
        int percent = static_cast<int>(wParam);

        SendMessageW(state->progressBar, PBM_SETPOS, percent, 0);

        std::wstring text = L"\u6B63\u5728\u4E0B\u8F7D\uFF1A" + std::to_wstring(percent) + L"%";

        SetWindowTextW(state->progressText, text.c_str());

        return 0;
    }

    if (message == kDownloadFinishedMessage && state) {
        std::unique_ptr<VodDownloadResult> result(reinterpret_cast<VodDownloadResult *>(lParam));

        if (state->downloadThread.joinable()) state->downloadThread.join();

        state->downloading = false;

        if (result && result->success && !state->downloadCancelled.load() && !state->closeWhenFinished) {
            state->selection->source = result->localPath;
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }

        if (state->closeWhenFinished) {
            DestroyWindow(window);
            return 0;
        }

        EnableWindow(state->urlEdit, TRUE);
        EnableWindow(state->openFileButton, TRUE);
        EnableWindow(state->playUrlButton, TRUE);
        EnableWindow(state->serverVideoButton, TRUE);
        EnableWindow(state->cancelButton, TRUE);

        SetWindowTextW(state->cancelButton, L"\u53D6\u6D88");

        SendMessageW(state->progressBar, PBM_SETPOS, 0, 0);
        SetWindowTextW(state->progressText, L"\u7B49\u5F85\u4E0B\u8F7D");

        if (!state->downloadCancelled.load()) {
            const wchar_t *messageText =
                result && !result->error.empty() ? result->error.c_str() : L"\u4E0B\u8F7D\u5931\u8D25\u3002";

            MessageBoxW(window, messageText, L"\u4E0B\u8F7D\u5931\u8D25", MB_OK | MB_ICONERROR);
        }

        state->downloadCancelled.store(false);
        SetFocus(state->urlEdit);
        return 0;
    }

    if (message == WM_COMMAND && state) {
        int commandId = LOWORD(wParam);

        if (commandId == kOpenFileButtonId) {
            std::string selected = selectLocalMediaFile(window);

            if (!selected.empty()) {
                state->selection->source = selected;
                state->accepted = true;
                DestroyWindow(window);
            }

            return 0;
        }

        if (commandId == kPlayUrlButtonId) {
            wchar_t url[32768] = {};

            GetWindowTextW(state->urlEdit, url, static_cast<int>(sizeof(url) / sizeof(url[0])));

            std::wstring urlText(url);

            if (urlText.empty() || urlText.find(L"://") == std::wstring::npos) {
                MessageBoxW(window,
                            L"\u8BF7\u8F93\u5165\u6709\u6548\u7684\u7F51\u7EDC\u5A92\u4F53\u5730\u5740\uFF0C\u4F8B"
                            L"\u5982\uFF1A\n"
                            L"https://example.com/video.m3u8",
                            L"\u5730\u5740\u65E0\u6548", MB_OK | MB_ICONWARNING);

                SetFocus(state->urlEdit);
                return 0;
            }

            std::string source = wideToUtf8(url);

            if (source.empty()) {
                MessageBoxW(window, L"\u7F51\u7EDC\u5730\u5740\u8F6C\u6362\u5931\u8D25\u3002",
                            L"\u6253\u5F00\u5931\u8D25", MB_OK | MB_ICONERROR);

                return 0;
            }

            state->selection->source.clear();
            state->selection->accessToken.clear();
            state->selection->serverHost.clear();
            state->selection->mediaPort = 0;
            state->selection->videoId = 0;
            state->downloadCancelled.store(false);
            state->closeWhenFinished = false;
            state->downloading = true;

            EnableWindow(state->urlEdit, FALSE);
            EnableWindow(state->openFileButton, FALSE);
            EnableWindow(state->playUrlButton, FALSE);
            EnableWindow(state->serverVideoButton, FALSE);

            SetWindowTextW(state->cancelButton, L"\u53D6\u6D88\u4E0B\u8F7D");

            SetWindowTextW(state->progressText, L"\u6B63\u5728\u4E0B\u8F7D\uFF1A0%");

            SendMessageW(state->progressBar, PBM_SETPOS, 0, 0);

            ShowWindow(state->progressText, SW_SHOW);
            ShowWindow(state->progressBar, SW_SHOW);

            state->downloadThread = std::thread([state, window, source]() {
                VodDownloadResult result = downloadVod(
                    source,
                    [window](int percent) {
                        PostMessageW(window, kDownloadProgressMessage, static_cast<WPARAM>(percent), 0);
                    },
                    state->downloadCancelled);

                auto *output = new VodDownloadResult(std::move(result));

                if (!PostMessageW(window, kDownloadFinishedMessage, 0, reinterpret_cast<LPARAM>(output))) {
                    delete output;
                }
            });

            return 0;
        }

        if (commandId == kServerVideoButtonId) {
            ServerVideoSelection selected;
            if (showServerVideoDialog(window, &selected)) {
                state->selection->source = selected.localPath;
                state->selection->accessToken = selected.accessToken;
                state->selection->serverHost = selected.settings.host;
                state->selection->mediaPort = selected.settings.mediaPort;
                state->selection->videoId = selected.videoId;
                state->accepted = true;
                DestroyWindow(window);
            }
            return 0;
        }

        if (commandId == kCancelButtonId) {
            if (state->downloading) {
                state->downloadCancelled.store(true);

                EnableWindow(state->cancelButton, FALSE);

                SetWindowTextW(state->progressText, L"\u6B63\u5728\u53D6\u6D88\u4E0B\u8F7D...");
            } else {
                DestroyWindow(window);
            }

            return 0;
        }
    }

    if (message == WM_CLOSE && state) {
        if (state->downloading) {
            state->closeWhenFinished = true;
            state->downloadCancelled.store(true);
            EnableWindow(state->cancelButton, FALSE);
            SetWindowTextW(state->progressText, L"\u6B63\u5728\u53D6\u6D88\u4E0B\u8F7D...");
        } else {
            DestroyWindow(window);
        }

        return 0;
    }

    if (message == WM_DESTROY) {
        if (state) {
            state->downloadCancelled.store(true);

            if (state->downloadThread.joinable()) {
                state->downloadThread.join();
            }

            if (state->ownsFont && state->font) {
                DeleteObject(state->font);
            }

            state->font = nullptr;
            state->ownsFont = false;
            state->finished = true;
        }

        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

bool showSourceSelectionDialog(PlaybackSelection *selection) {
    if (!selection) return false;

    *selection = {};

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS;

    if (!InitCommonControlsEx(&controls)) return false;

    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = sourceWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    constexpr int width = 560;
    constexpr int height = 315;

    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;

    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    SourceDialogState state;
    state.selection = selection;

    HWND window =
        CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClassName, L"Native Player - \u6253\u5F00\u5A92\u4F53",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, nullptr, nullptr, instance, &state);

    if (!window) return false;

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);

    MSG message = {};

    while (!state.finished) {
        BOOL result = GetMessageW(&message, nullptr, 0, 0);

        if (result <= 0) break;

        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    return state.accepted;
}
