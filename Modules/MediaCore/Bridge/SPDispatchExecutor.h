// macOS UI-thread executor for the media core: the main dispatch queue.
#pragma once

// Installs the executor unless one is already installed. Idempotent; call it
// before the media core posts its first task.
void SPInstallDispatchMainThreadExecutor(void);
