#pragma once

namespace ClaimablePOIs::Diagnostics
{
	// Read once at startup. Safety checks and failure logs never depend on this.
	void Initialize() noexcept;
	[[nodiscard]] bool Enabled() noexcept;
}
