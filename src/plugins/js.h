#pragma once

// A small C++ wrapper over QuickJS-ng for the plugin runtime: a runtime with a memory limit
// and a wall-clock deadline, contexts that evaluate ES modules, and RAII values.
// Destruction order: Values, then their Context, then the Runtime.

#include <atomic>
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
    std::string name;  // "TypeError", … ("" when what was thrown isn't an object)
    // Wording only: "stopped by a resource limit" rather than "threw". The host counts both kinds
    // of failure alike, so a wrong value here changes the message shown, never what is enforced.
    //  - Reliable (observed events): the deadline interrupt (uncatchable) and a refused allocation
    //    under the memory limit. A script cannot fake or hide them, even by catching the error.
    //  - Best effort: stack overflow. QuickJS reports it as an ordinary RangeError, with no host
    //    hook, so it is recognised by that exact message plus a trace of at least 8 frames (the
    //    engine's own overflow is raised deep). A script can evade this (Error.stackTraceLimit,
    //    catch and rethrow) or fake it (recurse 8+ deep, then throw that RangeError); it then
    //    reads as an ordinary error or a limit stop respectively.
    // Safety never rests on this flag: QuickJS's own stack check (JS_SetMaxStackSize) cannot be
    // disabled from script and always stops runaway recursion.
    bool limit = false;
};

enum class PromiseState { NotAPromise, Pending, Fulfilled, Rejected };

class Context;

// Memory shared by several runtimes on one thread (not thread-safe): an allocation that would take
// `used` past `limit` is refused, whichever runtime asks.
struct MemoryPool {
    size_t limit = 0;  // 0 = none
    size_t used = 0;
};

class Runtime {
public:
    // `pool`, if given, must outlive the runtime and be used from the runtime's thread only.
    explicit Runtime(size_t memoryLimit, MemoryPool* pool = nullptr);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Milliseconds on the steady clock (see nowMs()); 0 = no deadline. Scripts running past it
    // are aborted with an uncatchable error.
    void setDeadline(int64_t epochMs);
    JSRuntime* raw() { return rt_; }

    static int64_t nowMs();  // the clock the deadline is compared with
    bool deadlineHit() const { return hit_; }
    bool memoryLimitHit() const { return oom_; }
    bool poolLimitHit() const { return poolHit_; }
    // The host ran out of memory serving a script (a native's std::bad_alloc): counts as the limit.
    void noteOutOfMemory() { oom_ = true; }  // the refusal was the pool's (memoryLimitHit too)
    // Forget the limit events seen so far (the Context does this at the start of each operation).
    void clearLimitEvents() { hit_ = false; oom_ = false; poolHit_ = false; }

    // Re-anchors the stack-overflow check at the calling thread's stack. The host thread may
    // differ from the one that created the runtime; the Context does this before each operation.
    void anchorStack() { JS_UpdateStackTop(rt_); }

    // Any thread: stops the running script (and every later one) as if its deadline had passed.
    // For shutting down; there is no way back.
    void interruptNow() { abort_.store(true); }

private:
    static int interrupt(JSContext*, void* self);
    static void* allocate(void* self, size_t size);
    static void* allocateZeroed(void* self, size_t count, size_t size);
    static void* reallocate(void* self, void* ptr, size_t size);
    static void release(void* self, void* ptr);
    bool admit(size_t grow);
    void account(size_t add, size_t remove);  // this runtime's use and the pool's  // false (and records the event) if `grow` bytes would pass the limit
    JSRuntime* rt_ = nullptr;
    size_t limit_;
    size_t used_ = 0;
    MemoryPool* pool_;
    bool poolHit_ = false;
    int64_t deadline_ = 0;
    bool hit_ = false;  // the deadline interrupted a script
    std::atomic<bool> abort_{false};
    bool oom_ = false;  // an allocation was refused by the memory limit
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
    static Value fromOrderedJson(Context& cx, const nlohmann::ordered_json& j);  // keys keep their order
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
    bool isStackOverflow(JSValueConst ex, const JsError& e);
    void begin();  // start of an operation: anchor the stack, forget old limit events
    Runtime& rt_;
    JSContext* cx_;
    std::optional<JsError> error_;
};

}  // namespace zk::js
