#pragma once

namespace ls::backend::quickjs {

/// Register the QuickJS backend with the BackendRegistry under the manifest type
/// "lse-quickjs" (matching LegacyScriptEngine's QuickJS plugin type so existing
/// LSE plugins load unchanged). Called once when the mod loads.
void registerBackend();

/// The manifest type handled by this backend.
inline constexpr char const* kBackendType = "lse-quickjs";

} // namespace ls::backend::quickjs
