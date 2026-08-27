#include "parent-watch.h"
#include <windows.h>

static DWORD WINAPI ParentWatchThread(LPVOID param)
{
  DWORD pid = (DWORD)(ULONG_PTR)param;
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h)
    return 0;

  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  ExitProcess(0);
  return 0;
}

void startParentWatch(DWORD pid)
{
  if (pid == 0)
    return;

  CreateThread(0, 0, ParentWatchThread, (LPVOID)(ULONG_PTR)pid, 0, 0);
}