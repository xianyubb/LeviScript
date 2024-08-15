#include "API/EventAPI/EventAPI.h"

#include "ll/api/memory/Hook.h"

#include "mc/server/common/DedicatedServer.h"

LL_AUTO_TYPE_INSTANCE_HOOK(
    DedicatedServerHook,
    ll::memory::HookPriority::Normal,
    DedicatedServer,
    "??0DedicatedServer@@QEAA@XZ",
    void
) {
    EnableBeforeServerStartedListener();
    origin();
}
