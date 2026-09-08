#pragma once

void startServer();
void handleClient(unsigned long long clientSocket, long long enqueuedAtUs = 0);
void requestServerShutdown() noexcept;
bool serverShutdownRequested() noexcept;
bool serverLifecycleStarted() noexcept;
