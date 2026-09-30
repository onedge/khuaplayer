// macOS executors for the media core: the main dispatch queue for UI-thread
// work (sp::mainThread()) and GCD global queues, serial queues and dispatch
// sources for background work (sp::backgroundTasks()).
#pragma once

// Installs both unless already installed. Idempotent; call it before the
// media core posts its first task.
void SPInstallDispatchExecutors(void);
