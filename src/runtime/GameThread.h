#pragma once

namespace GameThread
{
	// Binds engine mutations to the thread running Skyrim's main-loop hook.
	// Returns false if that hook unexpectedly migrates to another thread.
	[[nodiscard]] bool Bind();

	// True only on the bound main-loop thread. Safe to call from any thread.
	[[nodiscard]] bool IsAuthoritative();

	// Like IsAuthoritative, but logs the first refused a_operation.
	[[nodiscard]] bool Require(const char* a_operation);
}
