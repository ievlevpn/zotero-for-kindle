#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <pthread.h>

#include <chrono>
#include <optional>
#include <functional>

#include "plugins/js.h"

using namespace zk::js;

TEST_CASE("js: a module evaluates and exports a function") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule("export function add(a, b) { return a + b; }", "m.js");
    REQUIRE_FALSE(cx.takeError().has_value());
    Value fn = ns.get("add");
    Value r = cx.call(fn, {Value::fromJson(cx, 2), Value::fromJson(cx, 40)});
    REQUIRE_FALSE(cx.takeError().has_value());
    CHECK(r.toJson() == nlohmann::json(42));
}

TEST_CASE("js: JSON round-trips") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule("export const echo = (x) => x;", "m.js");
    nlohmann::json in = {{"a", 1}, {"b", {1, 2, 3}}, {"s", "привет"}, {"n", nullptr}, {"f", 1.5}, {"o", {{"k", true}}}};
    Value r = cx.call(ns.get("echo"), {Value::fromJson(cx, in)});
    CHECK(r.toJson() == in);
}

TEST_CASE("js: an infinite loop stops at the deadline with limit=true") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule("export function spin() { while (true) {} }", "m.js");
    using namespace std::chrono;
    auto nowMs = [] { return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); };
    const auto t0 = steady_clock::now();
    rt.setDeadline(nowMs() + 200);
    cx.call(ns.get("spin"), {});
    const auto took = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK(err->limit);
    CHECK(took < 1000);
    rt.setDeadline(0);
}

TEST_CASE("js: a memory bomb stops at the limit") {
    Runtime rt(4 << 20);
    Context cx(rt);
    Value ns = cx.evalModule("export function bomb() { return new Array(1e8).fill(1); }", "m.js");
    REQUIRE_FALSE(cx.takeError().has_value());
    cx.call(ns.get("bomb"), {});
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK(err->limit);
}

TEST_CASE("js: an exception keeps its stack") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule("function inner() { throw new Error('boom'); }\nexport function outer() { inner(); }", "m.js");
    cx.call(ns.get("outer"), {});
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK(err->message.find("boom") != std::string::npos);
    CHECK(err->stack.find("inner") != std::string::npos);
    CHECK(err->stack.find("m.js") != std::string::npos);
    CHECK_FALSE(err->limit);
    CHECK_FALSE(cx.takeError().has_value());
}

TEST_CASE("js: a syntax error in a module is reported") {
    Runtime rt(16 << 20);
    Context cx(rt);
    cx.evalModule("export function (", "bad.js");
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK_FALSE(err->limit);
    CHECK_FALSE(err->message.empty());
}

TEST_CASE("js: promise jobs run on runJobs()") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule(
        "export let got = 0;\n"
        "export const p = Promise.resolve(7).then(v => { got = v; return v * 2; });\n"
        "export const pending = new Promise(() => {});\n"
        "export function read() { return got; }",
        "m.js");
    REQUIRE_FALSE(cx.takeError().has_value());
    Value p = ns.get("p");
    CHECK(p.isPromise());
    CHECK(p.promiseState() == PromiseState::Pending);
    CHECK(ns.get("pending").promiseState() == PromiseState::Pending);
    CHECK(cx.call(ns.get("read"), {}).toJson() == nlohmann::json(0));
    cx.runJobs();
    CHECK_FALSE(cx.takeError().has_value());
    CHECK(p.promiseState() == PromiseState::Fulfilled);
    CHECK(cx.call(ns.get("read"), {}).toJson() == nlohmann::json(7));
    CHECK_FALSE(ns.get("read").isPromise());
}

TEST_CASE("js: a script throwing a look-alike out-of-memory error is not a limit stop") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule(
        "export function fake() { throw new Error('out of memory'); }\n"
        "export function fakeStack() { throw new RangeError('Maximum call stack size exceeded'); }",
        "m.js");
    cx.call(ns.get("fake"), {});
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK_FALSE(err->limit);
    cx.call(ns.get("fakeStack"), {});
    err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK_FALSE(err->limit);
}

TEST_CASE("js: a script that catches an out-of-memory error cannot hide that it was a limit stop") {
    Runtime rt(4 << 20);
    Context cx(rt);
    Value ns = cx.evalModule(
        "export function sneaky() { try { new Array(1e8).fill(1); } catch (e) {} throw new Error('later'); }", "m.js");
    cx.call(ns.get("sneaky"), {});
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK(err->limit);
}

TEST_CASE("js: after a deadline stop an ordinary throw is not a limit stop") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule(
        "export function spin() { while (true) {} }\n"
        "export function bad() { throw new Error('plain'); }\n"
        "queueMicrotask(() => { throw new Error('in a job'); });",
        "m.js");
    rt.setDeadline(Runtime::nowMs() + 50);
    cx.call(ns.get("spin"), {});
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK(err->limit);
    // No setDeadline here: the old deadline stays in the past, the flag must not stick.
    cx.call(ns.get("bad"), {});
    err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK(err->message == "plain");
    CHECK_FALSE(err->limit);
    cx.runJobs();
    err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK_FALSE(err->limit);
    rt.setDeadline(0);
}

TEST_CASE("js: runaway recursion is a limit stop and does not crash") {
    Runtime rt(16 << 20);
    Context cx(rt);
    Value ns = cx.evalModule("export function f() { return f() + 1; }", "m.js");
    cx.call(ns.get("f"), {});
    auto err = cx.takeError();
    REQUIRE(err.has_value());
    CHECK(err->limit);
}

TEST_CASE("js: recursion is caught when the runtime is used from another thread") {
    Runtime rt(16 << 20);
    std::optional<Context> cx;
    cx.emplace(rt);
    Value ns = cx->evalModule("export function f() { return f() + 1; }", "m.js");
    Value fn = ns.get("f");
    bool limit = false, got = false;
    std::function<void()> work = [&] {
        cx->call(fn, {});
        auto err = cx->takeError();
        got = err.has_value();
        limit = got && err->limit;
    };
    // A thread with a 4 MB stack (the default of a secondary thread can be smaller than the engine's limit).
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 << 20);
    pthread_t th;
    REQUIRE(pthread_create(&th, &attr, [](void* p) -> void* { (*static_cast<std::function<void()>*>(p))(); return nullptr; }, &work) == 0);
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
    CHECK(got);
    CHECK(limit);
    fn = Value();
    ns = Value();
    cx.reset();
}
