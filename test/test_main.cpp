// BeamMP, the BeamNG.drive multiplayer mod.
// Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
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

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Common.h>

int main(int argc, char** argv) {
    doctest::Context context;
    // This binary owns its own lifetime. Application::GracefullyShutdown (which the TLuaEngine test
    // calls to stop the engine's threads) ends with std::_Exit(0) in STANDALONE mode (the upstream
    // PR #501 port) -- inside doctest that terminated the run at test case 23 of 36: the 13 later
    // cases never executed, no summary was printed, and the exit code was 0, i.e. "all passed".
    // Embedded mode means exactly "this process is not the server's to end", which is the harness's
    // situation, and it gates only shutdown/signals/console -- none of which the tests exercise --
    // so reuse the author's mechanism rather than add a test-only knob.
    Application::SetEmbedded(true);

    // Application::InitializeConsole();

    context.applyCommandLine(argc, argv);

    int res = context.run(); // run

    if (context.shouldExit()) // important - query flags (and --exit) rely on the user doing this
        return res; // propagate the result of the tests

    int client_stuff_return_code = 0;
    // your program - if the testing framework is integrated in your production code

    return res + client_stuff_return_code; // the result from doctest is propagated here as well
}
