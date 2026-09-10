#pragma once

#include <string>

namespace ct {

struct D; // forward

struct C {
    std::string cname();
    D*          getD(); // transitive reference: closure must also pull in D
};

} // namespace ct
