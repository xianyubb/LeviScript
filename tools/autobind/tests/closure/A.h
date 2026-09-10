#pragma once

// Closure test (headers named after their class, like real LL/MC): A references B
// (by pointer) and C (by reference); C references D. With "closure": true the
// generator must locate B.h / C.h / D.h and bind B, C, D transitively, so A's and
// C's referencing methods become bindable.
namespace ct {

struct B; // forward
struct C; // forward

struct A {
    int   value();
    B*    getB();
    void  useC(C& c);
};

} // namespace ct
