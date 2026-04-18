#include "TLuaResult.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

void TLuaResult::MarkReadySuccess(sol::object Res) {
    std::unique_lock Lock(mMutex);
    mError = false;
    mResult = Res;
    mDetachedResult = Freeze(Res);

    MarkAsReady();
}

void TLuaResult::MarkReadyError(sol::protected_function_result Res) {
    std::unique_lock Lock(mMutex);
    mError = true;
    SetErrorMessageFromResult(Res);

    MarkAsReady();
}

void TLuaResult::MarkReadyError(std::string Res) {
    std::unique_lock Lock(mMutex);
    mError = true;
    mErrorMessage = std::move(Res);

    MarkAsReady();
}

bool TLuaResult::IsReady() const {
    std::unique_lock Lock(mMutex);
    return mReady;
}
bool TLuaResult::IsError() const {
    std::unique_lock Lock(mMutex);
    return mError;
}

TLuaStateId TLuaResult::OwnerState() const {
    std::unique_lock Lock(mMutex);
    // copy
    return mStateId;
}
void TLuaResult::SetOwnerState(TLuaStateId StateId) {
    std::unique_lock Lock(mMutex);
    mStateId = std::move(StateId);
}

TLuaResult::Snapshot TLuaResult::GetSnapshot(TLuaStateId CallingState) const {
    std::unique_lock Lock(mMutex);
    if (CallingState != mStateId) {
        throw std::logic_error("Tried to get snapshot from non-owning state (use a detached snapshot instead)");
    }
    Snapshot snapshot {
        .Error = mError,
        .Ready = mReady,
        .ErrorMessage = mErrorMessage,
        .Result = mResult,
        .StateId = mStateId,
        .Function = mFunction,
    };
    return snapshot;
}
TLuaResult::DetachedSnapshot TLuaResult::GetDetachedSnapshot() const {
    std::unique_lock Lock(mMutex);
    DetachedSnapshot snapshot {
        .Error = mError,
        .Ready = mReady,
        .ErrorMessage = mErrorMessage,
        .Result = mDetachedResult,
        .StateId = mStateId,
        .Function = mFunction,
    };
    return snapshot;
}

TDetachedLuaValue TLuaResult::Freeze(const sol::object& o, int depth) {
    if (depth > 64)
        throw std::runtime_error("max depth (64) reached");
    switch (o.get_type()) {
    case sol::type::lua_nil:
        return { { std::monostate { } } };
    case sol::type::boolean:
        return { { o.as<bool>() } };
    case sol::type::number: {
        if (o.is<int>()) {
            return { { o.as<int>() } };
        } else {
            return { { o.as<double>() } };
        }
    }
    case sol::type::string:
        return { { o.as<std::string>() } };
    case sol::type::table: {
        TDetachedLuaValue::Object out;
        for (auto&& [k, v] : o.as<sol::table>()) {
            if (!k.is<std::string>())
                continue; // no numeric-key handling, don't need it
            out.emplace(k.as<std::string>(), Freeze(v, depth + 1));
        }
        return { { std::move(out) } };
    }
    default:
        throw std::runtime_error("unsupported Lua type for cross-thread snapshot");
    }
}
void TLuaResult::MarkAsReady() {
    mReady = true;
    mReadyCondition.notify_all();
}

void TLuaResult::WaitUntilReady() {
    std::unique_lock Lock(mMutex);
    while (!mReady)
        mReadyCondition.wait_for(Lock, std::chrono::milliseconds(50),
            [this] {
                return mReady;
            });
}

TEST_CASE("TLuaResult MarkReadyError(string) marks ready and wakes waiters") {
    TLuaResult result("state_a", "fn_a");
    std::atomic<bool> waiterDone { false };

    auto waiter = std::thread([&] {
        result.WaitUntilReady();
        waiterDone.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(waiterDone.load(std::memory_order_acquire));

    result.MarkReadyError(std::string("boom"));
    waiter.join();

    CHECK(result.IsReady());
    const auto snapshot = result.GetDetachedSnapshot();
    CHECK(snapshot.Ready);
    CHECK(snapshot.Error);
    CHECK(snapshot.ErrorMessage == "boom");
    CHECK(snapshot.StateId == "state_a");
    CHECK(snapshot.Function == "fn_a");
}

TEST_CASE("TLuaResult GetSnapshot enforces owner state id") {
    TLuaResult result("owner_state", "fn_owner");
    sol::state lua;
    lua.open_libraries(sol::lib::base);
    result.MarkReadySuccess(sol::make_object(lua.lua_state(), std::string("ok")));

    CHECK_NOTHROW(result.GetSnapshot("owner_state"));
    CHECK_THROWS_AS(result.GetSnapshot("different_state"), std::logic_error);
}

TEST_CASE("TLuaResult detached snapshot freezes nested string-keyed tables") {
    TLuaResult result("state_table", "fn_table");
    sol::state lua;
    lua.open_libraries(sol::lib::base);

    auto outer = lua.create_table();
    auto inner = lua.create_table();
    inner["k"] = std::string("v");

    outer["flag"] = true;
    outer["msg"] = std::string("hello");
    outer["inner"] = inner;
    outer[1] = std::string("ignored_numeric_key");

    result.MarkReadySuccess(sol::make_object(lua.lua_state(), outer));
    const auto detached = result.GetDetachedSnapshot();

    CHECK(detached.Ready);
    CHECK_FALSE(detached.Error);
    const auto* object = std::get_if<TDetachedLuaValue::Object>(&detached.Result.V);
    REQUIRE(object != nullptr);

    CHECK(object->contains("flag"));
    CHECK(object->contains("msg"));
    CHECK(object->contains("inner"));
    CHECK_FALSE(object->contains("1"));

    const auto* flag = std::get_if<bool>(&object->at("flag").V);
    REQUIRE(flag != nullptr);
    CHECK(*flag);

    const auto* msg = std::get_if<std::string>(&object->at("msg").V);
    REQUIRE(msg != nullptr);
    CHECK(*msg == "hello");

    const auto* innerObj = std::get_if<TDetachedLuaValue::Object>(&object->at("inner").V);
    REQUIRE(innerObj != nullptr);
    REQUIRE(innerObj->contains("k"));
    const auto* innerValue = std::get_if<std::string>(&innerObj->at("k").V);
    REQUIRE(innerValue != nullptr);
    CHECK(*innerValue == "v");
}

TEST_CASE("TLuaResult MarkReadySuccess throws on unsupported Lua function value") {
    TLuaResult result("state_fn", "fn_fn");
    sol::state lua;
    lua.open_libraries(sol::lib::base);
    lua["f"] = [] { return 1; };
    const sol::table globals = lua.globals();
    const sol::protected_function fn = globals.get<sol::protected_function>("f");
    const sol::object fnObj = sol::make_object(lua.lua_state(), fn);

    CHECK_THROWS_AS(result.MarkReadySuccess(fnObj), std::runtime_error);
    CHECK_FALSE(result.IsReady());
}
