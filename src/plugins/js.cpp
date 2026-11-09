#include "plugins/js.h"

#include <chrono>
#include <cstdlib>
#include <utility>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace zk::js {

namespace {

std::string toStd(JSContext* cx, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(cx, &len, v);
    if (!s) return {};
    std::string out(s, len);
    JS_FreeCString(cx, s);
    return out;
}

size_t usableSize(const void* p) {
#if defined(__APPLE__)
    return malloc_size(p);
#else
    return malloc_usable_size(const_cast<void*>(p));
#endif
}

constexpr size_t kMaxStack = 512 * 1024;

}  // namespace

// ---- Runtime ----------------------------------------------------------------------------

int64_t Runtime::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The memory limit is enforced here rather than by JS_SetMemoryLimit, so a refused allocation is
// observed as an event (oom_) and a script cannot fake it by throwing a look-alike error.
bool Runtime::admit(size_t grow) {
    if (limit_ != 0 && used_ + grow > limit_) {
        oom_ = true;
        return false;
    }
    if (pool_ && pool_->limit != 0 && pool_->used + grow > pool_->limit) {
        oom_ = true;
        poolHit_ = true;
        return false;
    }
    return true;
}

void Runtime::account(size_t add, size_t remove) {
    used_ = used_ + add - remove;
    if (pool_) pool_->used = pool_->used + add - remove;
}

void* Runtime::allocate(void* self, size_t size) {
    auto* r = static_cast<Runtime*>(self);
    if (!r->admit(size)) return nullptr;
    void* p = std::malloc(size ? size : 1);
    if (p) r->account(usableSize(p), 0);
    return p;
}

void* Runtime::allocateZeroed(void* self, size_t count, size_t size) {
    auto* r = static_cast<Runtime*>(self);
    if (size != 0 && count > SIZE_MAX / size) return nullptr;
    if (!r->admit(count * size)) return nullptr;
    void* p = std::calloc(count ? count : 1, size ? size : 1);
    if (p) r->account(usableSize(p), 0);
    return p;
}

void Runtime::release(void* self, void* ptr) {
    if (!ptr) return;
    static_cast<Runtime*>(self)->account(0, usableSize(ptr));
    std::free(ptr);
}

void* Runtime::reallocate(void* self, void* ptr, size_t size) {
    auto* r = static_cast<Runtime*>(self);
    if (!ptr) return allocate(self, size);
    if (size == 0) {
        release(self, ptr);
        return nullptr;
    }
    const size_t old = usableSize(ptr);
    if (size > old && !r->admit(size - old)) return nullptr;
    void* p = std::realloc(ptr, size);
    if (!p) return nullptr;
    r->account(usableSize(p), old);
    return p;
}

Runtime::Runtime(size_t memoryLimit, MemoryPool* pool) : limit_(memoryLimit), pool_(pool) {
    static const JSMallocFunctions mf = {&Runtime::allocateZeroed, &Runtime::allocate, &Runtime::release,
                                         &Runtime::reallocate, &usableSize};
    rt_ = JS_NewRuntime2(&mf, this);
    JS_SetMaxStackSize(rt_, kMaxStack);
    JS_SetInterruptHandler(rt_, &Runtime::interrupt, this);
}

Runtime::~Runtime() {
    JS_FreeRuntime(rt_);
}

void Runtime::setDeadline(int64_t epochMs) {
    deadline_ = epochMs;
    hit_ = false;
}

int Runtime::interrupt(JSContext*, void* self) {
    auto* r = static_cast<Runtime*>(self);
    if (r->abort_.load(std::memory_order_relaxed) || (r->deadline_ != 0 && nowMs() > r->deadline_)) {
        r->hit_ = true;
        return 1;
    }
    return 0;
}

// ---- Value ------------------------------------------------------------------------------

Value::Value(const Value& o) : ctx_(o.ctx_), v_(o.ctx_ ? JS_DupValue(o.ctx_, o.v_) : o.v_) {}

Value::Value(Value&& o) noexcept : ctx_(o.ctx_), v_(o.v_) {
    o.ctx_ = nullptr;
    o.v_ = JS_UNDEFINED;
}

Value& Value::operator=(Value o) noexcept {
    std::swap(ctx_, o.ctx_);
    std::swap(v_, o.v_);
    return *this;
}

Value::~Value() {
    if (ctx_) JS_FreeValue(ctx_, v_);
}

Value Value::fromJson(Context& cx, const nlohmann::json& j) {
    // Invalid UTF-8 in strings becomes U+FFFD (strict dump() would throw).
    const std::string text = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return Value(cx.raw(), JS_ParseJSON(cx.raw(), text.data(), text.size(), "<json>"));
}

nlohmann::json Value::toJson() const {
    if (!ctx_ || JS_IsUndefined(v_) || JS_IsFunction(ctx_, v_)) return nullptr;
    JSValue s = JS_JSONStringify(ctx_, v_, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx_, JS_GetException(ctx_));
        return nullptr;
    }
    if (JS_IsUndefined(s)) return nullptr;
    const std::string text = toStd(ctx_, s);
    JS_FreeValue(ctx_, s);
    return nlohmann::json::parse(text, nullptr, false);  // discarded -> null
}

PromiseState Value::promiseState() const {
    if (!ctx_) return PromiseState::NotAPromise;
    switch (JS_PromiseState(ctx_, v_)) {
        case JS_PROMISE_PENDING: return PromiseState::Pending;
        case JS_PROMISE_FULFILLED: return PromiseState::Fulfilled;
        case JS_PROMISE_REJECTED: return PromiseState::Rejected;
        default: return PromiseState::NotAPromise;
    }
}

bool Value::isUndefined() const {
    return !ctx_ || JS_IsUndefined(v_);
}

Value Value::get(const char* property) const {
    if (!ctx_ || !JS_IsObject(v_)) return {};
    JSValue r = JS_GetPropertyStr(ctx_, v_, property);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx_, JS_GetException(ctx_));
        return {};
    }
    return Value(ctx_, r);
}

// ---- Context ----------------------------------------------------------------------------

Context::Context(Runtime& rt) : rt_(rt), cx_(JS_NewContext(rt.raw())) {}

Context::~Context() {
    JS_FreeContext(cx_);
}

void Context::begin() {
    rt_.anchorStack();
    rt_.clearLimitEvents();
}

bool Context::isStackOverflow(JSValueConst ex, const JsError& e) {
    if (e.message != "Maximum call stack size exceeded" || !JS_IsError(ex)) return false;
    JSValue name = JS_GetPropertyStr(cx_, ex, "name");
    const bool range = toStd(cx_, name) == "RangeError";
    JS_FreeValue(cx_, name);
    // The engine's own overflow is raised at depth, so its (capped) trace is full of frames; a
    // look-alike thrown by hand from shallow code is not.
    int frames = 0;
    for (size_t at = e.stack.find("    at "); at != std::string::npos; at = e.stack.find("    at ", at + 1)) ++frames;
    return range && frames >= 8;
}

void Context::recordException() {
    JSValue ex = JS_GetException(cx_);
    if (error_) {  // keep the first error; just release this one
        JS_FreeValue(cx_, ex);
        return;
    }
    JsError e;
    if (JS_IsObject(ex)) {
        JSValue msg = JS_GetPropertyStr(cx_, ex, "message");
        e.message = JS_IsUndefined(msg) ? toStd(cx_, ex) : toStd(cx_, msg);
        JS_FreeValue(cx_, msg);
        JSValue nm = JS_GetPropertyStr(cx_, ex, "name");
        if (JS_IsString(nm)) e.name = toStd(cx_, nm);
        JS_FreeValue(cx_, nm);
        JSValue st = JS_GetPropertyStr(cx_, ex, "stack");
        if (!JS_IsUndefined(st)) e.stack = toStd(cx_, st);
        JS_FreeValue(cx_, st);
    } else {
        e.message = toStd(cx_, ex);
    }
    // Limit stops are told from script throws by events the engine host observed: the deadline
    // interrupt (uncatchable), a refused allocation, or a real stack overflow (the engine's
    // RangeError, thrown while the call stack was deep).
    e.limit = (rt_.deadlineHit() && JS_IsUncatchableError(ex)) || rt_.memoryLimitHit() || isStackOverflow(ex, e);
    JS_FreeValue(cx_, ex);
    error_ = std::move(e);
}

Value Context::evalModule(const std::string& source, const std::string& name) {
    begin();
    JSValue fn = JS_Eval(cx_, source.c_str(), source.size(), name.c_str(),
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) {
        recordException();
        return {};
    }
    JSModuleDef* mod = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(fn));
    JSValue done = JS_EvalFunction(cx_, fn);  // consumes fn; a promise in this QuickJS
    if (JS_IsException(done)) {
        recordException();
        return {};
    }
    Value promise(cx_, done);
    if (promise.promiseState() == PromiseState::Rejected) {
        JSValue reason = JS_PromiseResult(cx_, promise.raw());
        JS_Throw(cx_, reason);
        recordException();
        return {};
    }
    return Value(cx_, JS_GetModuleNamespace(cx_, mod));
}

Value Context::call(const Value& fn, std::vector<Value> args) {
    begin();
    std::vector<JSValue> raw;
    raw.reserve(args.size());
    for (const Value& a : args) raw.push_back(a.raw());
    JSValue r = JS_Call(cx_, fn.raw(), JS_UNDEFINED, static_cast<int>(raw.size()), raw.data());
    if (JS_IsException(r)) {
        recordException();
        return {};
    }
    return Value(cx_, r);
}

void Context::runJobs() {
    begin();
    for (;;) {
        JSContext* jc = nullptr;
        int rc = JS_ExecutePendingJob(rt_.raw(), &jc);
        if (rc == 0) break;
        if (rc < 0) {
            if (jc == cx_) recordException();
            else if (jc) JS_FreeValue(jc, JS_GetException(jc));
            if (rt_.deadlineHit()) break;  // an interrupted job: do not spin on the rest
        }
    }
}

std::optional<JsError> Context::takeError() {
    auto e = std::move(error_);
    error_.reset();
    return e;
}

}  // namespace zk::js
