// BeamMP, the BeamNG.drive multiplayer mod.
// Copyright (C) 2026 BeamMP Ltd., BeamMP team and contributors.
//
// BeamMP Ltd. can be contacted by electronic mail via contact@beammp.com.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "Common.h"
#include <condition_variable>
#include <string>
#include <sol/sol.hpp>

using TLuaStateId = std::string;

struct TDetachedLuaValue {
    using Array = std::vector<TDetachedLuaValue>;
    using Object = std::unordered_map<std::string, TDetachedLuaValue>;
    std::variant<std::monostate, bool, double, int, std::string, Array, Object> V;
};
std::ostream& operator<<(std::ostream& os, const TDetachedLuaValue& value);

struct TLuaResult {
    struct Snapshot {
        bool Error;
        bool Ready;
        std::string ErrorMessage;
        /// CAUTION: Accessing this object causes the Lua stack of the owning state
        /// to be accessed. Only call this from the owning Lua state. Otherwise,
        /// use `DetachedSnapshot`.
        sol::object Result;
        TLuaStateId StateId;
        std::string Function;
    };

    struct DetachedSnapshot {
        bool Error;
        bool Ready;
        std::string ErrorMessage;
        /// Serialized Lua result. Has no reference to the Lua state, and is safe to use
        /// from any thread that acquires it.
        TDetachedLuaValue Result;
        TLuaStateId StateId;
        std::string Function;
    };


    TLuaResult(TLuaStateId StateId, std::string FunctionName) : mStateId(StateId), mFunction(std::move(FunctionName)) {}

    /// Marks this result as success & sets it as ready, waking all threads waiting on it.
    void MarkReadySuccess(sol::object Res);
    /// Marks this result as erroneous & sets it as ready, waking all threads waiting on it.
    void MarkReadyError(sol::protected_function_result Res);
    /// Marks this result as erroneous & sets it as ready, waking all threads waiting on it.
    void MarkReadyError(std::string Res);
    /// Wait (suspend) until this result is ready. Use `GetSnapshot` to get the results.
    void WaitUntilReady();
    bool IsReady() const;
    bool IsError() const;
    TLuaStateId OwnerState() const;
    void SetOwnerState(TLuaStateId StateId);

    /// To get the result or error, while in the owning state, use this function.
    /// Pass your lua state ID so that the function can verify the safety of the access.
    /// If you have no state ID, use `GetDetachedSnapshot`.
    /// Accesses the Lua state directly when using the result.
    Snapshot GetSnapshot(TLuaStateId CallingState) const;
    /// Returns a snapshot with, if not an error, a serialized version of the result
    /// object. In contrast to `GetSnapshot`, this does not access the Lua stack when
    /// accessing `.Result`.
    DetachedSnapshot GetDetachedSnapshot() const;

private:
    mutable std::mutex mMutex {};
    bool mReady { false };
    bool mError;
    std::string mErrorMessage;
    sol::object mResult { sol::lua_nil };
    TDetachedLuaValue mDetachedResult;
    TLuaStateId mStateId;
    std::string mFunction;
    std::condition_variable mReadyCondition {};

    TDetachedLuaValue Freeze(const sol::object& o, int depth = 0);

    void SetErrorMessageFromResult(sol::protected_function_result& Res) {
        if (Res.valid()) {
            beammp_lua_errorf("Error was not an error");
        }
        if (Res.get_type() == sol::type::string) {
            mErrorMessage = Res.get<sol::error>().what();
        } else {
            mErrorMessage = "(unknown error; error object is not inspectable)";
        }
    }

    void MarkAsReady();
};
