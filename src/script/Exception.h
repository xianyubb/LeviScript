#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace ls::script {

/// Exception type used across the abstraction boundary.
///
/// Native callbacks throw it to report errors; the active backend catches it at
/// the JS/native boundary and turns it into a real script exception (with stack).
/// Conversely, when a script call fails the backend throws it into C++.
class ScriptException : public std::runtime_error {
public:
    explicit ScriptException(std::string message, std::string stack = {})
    : std::runtime_error(message), mStack(std::move(stack)) {}

    [[nodiscard]] std::string const& stack() const noexcept { return mStack; }

    /// Message plus stack, formatted for logging.
    [[nodiscard]] std::string fullMessage() const {
        if (mStack.empty()) {
            return what();
        }
        return std::string(what()) + "\n" + mStack;
    }

private:
    std::string mStack;
};

} // namespace ls::script
