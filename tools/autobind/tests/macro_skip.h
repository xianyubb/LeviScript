#pragma once

// Mimic LeviLamina's API markers. LLAPI (plain) stays bindable; LLNDAPI
// ([[nodiscard]] API) and MCNAPI (MC native-binary symbols) must be skipped.
#define LLAPI
#define LLNDAPI [[nodiscard]]
#define MCNAPI

namespace mtest {

// free functions
LLAPI int keepFunc(int x);          // bind
LLNDAPI int dropNodiscard(int x);   // skip: LLNDAPI
MCNAPI int dropMcNative(int x);     // skip: MCNAPI

struct Keep {
    LLAPI int keepMethod(int x);    // bind
    LLNDAPI int dropMethodNd();     // skip: LLNDAPI
    MCNAPI int dropMethodMc();      // skip: MCNAPI
    int field = 0;                  // bind (data member)
};

MCNAPI struct Drop {                // skip whole class: MCNAPI
    int x = 0;
};

} // namespace mtest
