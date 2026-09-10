#pragma once

#include "service/server-settings.h"

#include <windows.h>
#include <string>

struct ServerVideoSelection {
    std::string localPath;
    std::string accessToken;
    ServerSettings settings;
    int videoId = 0;
};

bool showServerVideoDialog(HWND owner, ServerVideoSelection *selection);
