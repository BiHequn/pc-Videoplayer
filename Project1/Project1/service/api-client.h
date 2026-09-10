#pragma once

#include "server-settings.h"

#include <string>

struct LoginResult {
    bool success = false;
    std::string username;
    std::string accessToken;
    std::wstring error;
};

LoginResult loginToApi(const ServerSettings &settings,
                       const std::string &username,
                       const std::string &password);
