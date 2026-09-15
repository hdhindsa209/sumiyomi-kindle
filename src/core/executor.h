#pragma once
#include <functional>

namespace sumi {

// Where blocking work runs and how its results come back (design doc §3.1).
// Worker is the real one; tests use InlineExecutor to run everything synchronously.
class Executor {
public:
    virtual ~Executor() = default;
    virtual void submit(std::function<void()> job) = 0;   // background (or inline)
    virtual void post(std::function<void()> fn) = 0;      // back on the UI thread
};

// Runs jobs and posts immediately, on the calling thread. For tests.
class InlineExecutor final : public Executor {
public:
    void submit(std::function<void()> job) override { job(); }
    void post(std::function<void()> fn) override { fn(); }
};

} // namespace sumi
