#pragma once

// A small C++ wrapper over QuickJS-ng for the plugin runtime: a runtime with a memory limit
// and a wall-clock deadline, contexts that evaluate ES modules, and RAII values.
// Destruction order: Values, then their Context, then the Runtime.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quickjs.h"

namespace zk::js {

struct JsError {
    std::string message, stack;
    bool limit = false;  // stopped by the memory limit or the deadline, not thrown by the script
};

enum class PromiseState { NotAPromise, Pending, Fulfilled, Rejected };

class Context;

class Runtime {
public:
    explicit Runtime(size_t memoryLimit);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Milliseconds on the steady clock (see nowMs()); 0 = no deadline. Scripts running past it
    // are aborted with an uncatchable error.
    void setDeadline(int64_t epochMs);
    JSRuntime* raw() { return rt_; }

    static int64_t nowMs();  // the clock the deadline is compared with
    bool deadlineHit() const { return hit_; }
    void clearDeadlineHit() { hit_ = false; }

private:
    static int interrupt(JSContext*, void* self);
    JSRuntime* rt_;
    int64_t deadline_ = 0;
    bool hit_ = false;
};

// Owns one JSValue reference; copying duplicates it. Tied to the Context that made it.
class Value {
public:
    Value() = default;
    Value(JSContext* ctx, JSValue v) : ctx_(ctx), v_(v) {}  // takes ownership of `v`
    Value(const Value& o);
    Value(Value&& o) noexcept;
    Value& operator=(Value o) noexcept;
    ~Value();

    static Value fromJson(Context& cx, const nlohmann::json& j);
    nlohmann::json toJson() const;  // undefined and functions become null

    bool isPromise() const { return promiseState() != PromiseState::NotAPromise; }
    PromiseState promiseState() const;
    bool isUndefined() const;
    Value get(const char* property) const;  // undefined when absent or not an object

    JSValue raw() const { return v_; }
    JSContext* ctx() const { return ctx_; }

private:
    JSContext* ctx_ = nullptr;
    JSValue v_ = JS_UNDEFINED;
};

class Context {
public:
    explicit Context(Runtime& rt);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Compiles and evaluates `source` as a module and returns its namespace object. On failure
    // returns undefined and records an error for takeError().
    Value evalModule(const std::string& source, const std::string& name);
    // Calls `fn` with undefined as `this`. On a throw returns undefined and records the error.
    Value call(const Value& fn, std::vector<Value> args);
    // Drains the promise job queue; an exception in a job is recorded.
    void runJobs();
    // The first error recorded since the last call, if any.
    std::optional<JsError> takeError();

    JSContext* raw() { return cx_; }
    Runtime& runtime() { return rt_; }

private:
    void recordException();
    Runtime& rt_;
    JSContext* cx_;
    std::optional<JsError> error_;
};

}  // namespace zk::js
