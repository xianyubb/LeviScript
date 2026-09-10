#pragma once

// A self-contained header that mimics the shape of real LeviLamina headers so the
// generator can be exercised without pulling in the whole LL/MC include graph.
// Everything is defined inline so the generated bindings also link when compiled
// into a target (used by the end-to-end generator verification).

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace demo {

enum class Color : int { Red = 0, Green, Blue };

class Base {
public:
    Base()          = default;
    virtual ~Base() = default;

    [[nodiscard]] virtual std::string kind() const { return "Base"; }
    [[nodiscard]] int                 value() const { return mValue; }
    void                              setValue(int value) { mValue = value; }

    int mPublicField = 0;

protected:
    int mValue  = 0;
    int mHidden = 0;
};

class Derived : public Base {
public:
    Derived() = default;
    explicit Derived(std::string name) : mName(std::move(name)) {}

    [[nodiscard]] std::string        kind() const override { return "Derived"; }
    [[nodiscard]] std::string const& name() const { return mName; }
    void                             rename(std::string const& value) { mName = value; }

    static Derived* create(std::string const& name) { return new Derived(name); }

    // overloaded -> the generator must emit an explicit static_cast
    int compute(int x) { return x; }
    int compute(int x, int y) { return x + y; }

    [[nodiscard]] std::vector<float> samples() const { return {1.0F, 2.0F, 3.0F}; }
    [[nodiscard]] Color              color() const { return mColor; }

    // raw pointer parameter -> unbindable, must be skipped
    void takesRawPtr(int* ptr) { (void)ptr; }

private:
    std::string mName;
    Color       mColor = Color::Red;
};

template <typename T>
class Box {
public:
    Box() = default;

    [[nodiscard]] T get() const { return mValue; }
    void            set(T value) { mValue = value; }

    T mValue{};
};

// free functions
inline int         add(int a, int b) { return a + b; }
inline std::string greet(std::string const& name) { return "hello, " + name; }

// template function -> bound through an explicit instantiation in the config
template <typename T>
T identity(T value) {
    return value;
}

} // namespace demo
