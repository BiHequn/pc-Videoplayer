#include "api-client.h"

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {
std::wstring utf8ToWide(const std::string &value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

std::string readResponse(HINTERNET request) {
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        body += chunk;
    }
    return body;
}
}

LoginResult loginToApi(const ServerSettings &settings,
                       const std::string &username,
                       const std::string &password) {
    LoginResult result;
    HINTERNET session = WinHttpOpen(L"VideoPlayer1/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = L"无法初始化 HTTP 客户端。";
        return result;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, settings.host.c_str(),
                                          settings.apiPort, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        result.error = L"无法连接认证服务。";
        return result;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection, L"POST", L"/api/auth/login", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        result.error = L"无法创建登录请求。";
        return result;
    }

    nlohmann::json payload = {{"username", username}, {"password", password}};
    std::string body = payload.dump();
    const wchar_t headers[] = L"Content-Type: application/json\r\n";
    BOOL sent = WinHttpSendRequest(
        request, headers, static_cast<DWORD>(-1L), body.data(),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (sent) sent = WinHttpReceiveResponse(request, nullptr);

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (sent) {
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
    }
    std::string responseBody = sent ? readResponse(request) : std::string();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!sent) {
        result.error = L"登录请求失败，请检查 API 地址。";
        return result;
    }
    try {
        nlohmann::json response = nlohmann::json::parse(responseBody);
        if (status != 200) {
            result.error = utf8ToWide(response.value("detail", "登录失败"));
            return result;
        }
        result.username = response.value("username", "");
        result.accessToken = response.value("access_token", "");
        result.success = !result.username.empty() && !result.accessToken.empty();
        if (!result.success) result.error = L"认证服务返回了无效响应。";
    } catch (...) {
        result.error = L"无法解析认证服务响应。";
    }
    return result;
}
