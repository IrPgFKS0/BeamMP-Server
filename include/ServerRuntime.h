// BeamMP, the BeamNG.drive multiplayer mod.
// Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
// Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class TServer;
class TNetwork;

// Arguments passed to the server entry point. Moved out of main.cpp so the whole server
// RUNTIME lives in the linkable library (PRJ_SOURCES), letting another binary embed it --
// specifically the LAN single-box host, where the launcher runs the server in-process and
// talks to it over an in-memory channel instead of loopback sockets. main.cpp now only
// builds this struct and calls BeamMPServerMain().
struct MainArguments {
    int argc {};
    char** argv {};
    std::vector<std::string_view> List;
    std::string InvokedAs;
};

// The full server startup + run loop (formerly the body of main()). BLOCKS until the server
// is shut down (Application shutdown handler / signal handler set it going). When embedding,
// call this on a dedicated thread.
int BeamMPServerMain(MainArguments Arguments);

// Optional embed hook. Invoked exactly once, ON THE SERVER THREAD, right after TServer and
// TNetwork are constructed and wired together but BEFORE the run loop begins. The combined-
// host launcher sets this to capture the live TServer/TNetwork so it can register its
// in-memory ("virtual", socketless) local client. Left unset for the standalone exe -> no-op.
void SetServerReadyHook(std::function<void(TServer&, TNetwork&)> Hook);
