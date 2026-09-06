#pragma once
void sendReady();
void sendEnded();
void sendError(const char* code);
void startCloseWatcher();