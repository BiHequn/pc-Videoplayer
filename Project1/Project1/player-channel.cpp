#include "player-channel.h"
#include "pip-window.h"

#include <iostream>
#include <string>
#include <windows.h>

static DWORD WINAPI StdinThread(LPVOID)
{
  std::string line;
  while (std::getline(std::cin, line))
  {
    if (line.find("\"command\":\"close\"") != std::string::npos)
    {
      requestPipWindowClose();
      break;
    }
  }
  return 0;
}

void sendReady()
{
  std::cout << "{\"version\":1,\"event\":\"ready\"}" << std::endl;
}

void startCloseWatcher()
{
  CreateThread(0, 0, StdinThread, 0, 0, 0);
}