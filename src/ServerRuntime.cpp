// BeamMP, the BeamNG.drive multiplayer mod.
// Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
// Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Server runtime: the full startup + run loop, moved verbatim out of main.cpp so it lives in
// the linkable library (PRJ_SOURCES) and can be embedded in-process by the LAN launcher.
// Behaviour for the standalone exe is unchanged (main.cpp just calls BeamMPServerMain()).

#include "ServerRuntime.h"

#include "ArgsParser.h"
#include "Client.h"
#include "Common.h"
#include "Http.h"
#include "LuaAPI.h"
#include "Settings.h"
#include "SignalHandling.h"
#include "TConfig.h"
#include "THeartbeatThread.h"
#include "TLuaEngine.h"
#include "TNetwork.h"
#include "TPPSMonitor.h"
#include "TPluginMonitor.h"
#include "TResourceManager.h"
#include "TServer.h"

#include <cstdint>
#include <iostream>
#include <thread>

static const std::string sCommandlineArguments = R"(
USAGE:
    BeamMP-Server [arguments]

ARGUMENTS:
    --help
                        Displays this help and exits.
    --port=1234
                        Sets the server's listening TCP and
                        UDP port. Overrides ENV and ServerConfig.
    --config=/path/to/ServerConfig.toml
                        Absolute or relative path to the
                        Server Config file, including the
                        filename. For paths and filenames with
                        spaces, put quotes around the path.
    --working-directory=/path/to/folder
                        Sets the working directory of the Server.
                        All paths are considered relative to this,
                        including the path given in --config.
    --version
                        Prints version info and exits.

EXAMPLES:
    BeamMP-Server --config=../MyWestCoastServerConfig.toml
        Runs the BeamMP-Server and uses the server config file
        which is one directory above it and is named
        'MyWestCoastServerConfig.toml'.
)";

// Embed hook (see ServerRuntime.h). Unset for the standalone server.
static std::function<void(TServer&, TNetwork&)> sServerReadyHook;
void SetServerReadyHook(std::function<void(TServer&, TNetwork&)> Hook) {
    sServerReadyHook = std::move(Hook);
}

int BeamMPServerMain(MainArguments Arguments) {
    setlocale(LC_ALL, "C");
    ArgsParser Parser;
    Parser.RegisterArgument({ "help" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "version" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "config" }, ArgsParser::HAS_VALUE);
    Parser.RegisterArgument({ "port" }, ArgsParser::HAS_VALUE);
    Parser.RegisterArgument({ "working-directory" }, ArgsParser::HAS_VALUE);
    Parser.Parse(Arguments.List);
    if (!Parser.Verify()) {
        return 1;
    }
    if (Parser.FoundArgument({ "help" })) {
        Application::Console().WriteRaw(sCommandlineArguments);
        return 0;
    }
    if (Parser.FoundArgument({ "version" })) {
        Application::Console().WriteRaw("BeamMP-Server v" + Application::ServerVersionString());
        return 0;
    }

    std::string ConfigPath = "ServerConfig.toml";
    if (Parser.FoundArgument({ "config" })) {
        auto MaybeConfigPath = Parser.GetValueOfArgument({ "config" });
        if (MaybeConfigPath.has_value()) {
            ConfigPath = MaybeConfigPath.value();
            beammp_info("Custom config requested via commandline arguments: '" + ConfigPath + "'");
        }
    }
    if (Parser.FoundArgument({ "working-directory" })) {
        auto MaybeWorkingDirectory = Parser.GetValueOfArgument({ "working-directory" });
        if (MaybeWorkingDirectory.has_value()) {
            beammp_info("Custom working directory requested via commandline arguments: '" + MaybeWorkingDirectory.value() + "'");
            try {
                fs::current_path(fs::path(MaybeWorkingDirectory.value()));
            } catch (const std::exception& e) {
                beammp_errorf("Could not set working directory to '{}': {}", MaybeWorkingDirectory.value(), e.what());
            }
        }
    }

    TConfig Config(ConfigPath);

    if (Config.Failed()) {
        beammp_info("Closing in 10 seconds");
        // loop to make it possible to ctrl+c instead
        for (size_t i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 1;
    }

    // override port if provided via arguments
    if (Parser.FoundArgument({ "port" })) {
        auto Port = Parser.GetValueOfArgument({ "port" });
        if (Port.has_value()) {
            auto P = int(std::strtoul(Port.value().c_str(), nullptr, 10));
            if (P == 0 || P < 0 || P > UINT16_MAX) {
                beammp_errorf("Custom port requested via --port is invalid: '{}'", Port.value());
                return 1;
            } else {
                Application::Settings.set(Settings::Key::General_Port, P);
                beammp_info("Custom port requested via commandline arguments: " + Port.value());
            }
        }
    }

    Config.PrintDebug();

    Application::InitializeConsole();
    Application::Console().StartLoggingToFile();

    Application::SetSubsystemStatus("Main", Application::Status::Starting);

    if (!Application::IsEmbedded()) {
        // In combined-host mode the launcher owns the process + its console-ctrl/signal handling.
        // The server's handler calls GracefullyShutdown (which can std::exit on a repeat), so it must
        // not be installed here or a Ctrl event meant for the launcher would take the whole thing down.
        SetupSignalHandlers();
    }

    Settings settings {};
    beammp_infof("Server name set in new impl: {}", settings.getAsString(Settings::Key::General_Name));

    bool Shutdown = false;
    Application::RegisterShutdownHandler([&Shutdown] {
        beammp_info("If this takes too long, you can press Ctrl+C repeatedly to force a shutdown.");
        Application::SetSubsystemStatus("Main", Application::Status::ShuttingDown);
        Shutdown = true;
    });

    TServer Server(Arguments.List);

    RegisterThread("Main");

    beammp_trace("Running in debug mode on a debug build");
    TResourceManager ResourceManager;
    ResourceManager.RefreshFiles();
    TPPSMonitor PPSMonitor(Server);
    THeartbeatThread Heartbeat(ResourceManager, Server);
    TNetwork Network(Server, PPSMonitor, ResourceManager);

    auto LuaEngine = std::make_shared<TLuaEngine>();
    LuaEngine->SetServer(&Server);
    Application::Console().InitializeLuaConsole(*LuaEngine);
    LuaEngine->SetNetwork(&Network);
    PPSMonitor.SetNetwork(Network);
    Application::CheckForUpdates();

    // Embed point: the server is now fully wired but the run loop hasn't started. The
    // combined-host launcher uses this to register its in-memory virtual client on Network.
    if (sServerReadyHook) {
        sServerReadyHook(Server, Network);
    }

    TPluginMonitor PluginMonitor(fs::path(Application::Settings.getAsString(Settings::Key::General_ResourceFolder)) / "Server", LuaEngine);

    Application::RegisterShutdownHandler([] {
        auto Futures = LuaAPI::MP::Engine->TriggerEvent("onShutdown", "");
        TLuaEngine::WaitForAll(Futures, std::chrono::seconds(5));
    });
    Application::RegisterShutdownHandler([&Server, &Network] {
        beammp_debug("Kicking all players due to shutdown");
        Server.ForEachClient([&Network](std::weak_ptr<TClient> client) -> bool {
            if (!client.expired()) {
                Network.ClientKick(*client.lock(), "Server shutdown");
            }
            return true;
        });
    });

    RegisterThread("Main(Waiting)");

    std::set<std::string> IgnoreSubsystems {
        "UpdateCheck" // Ignore as not to confuse users (non-vital system)
    };

    bool FullyStarted = false;
    while (!Shutdown) {
        if (!FullyStarted) {
            FullyStarted = true;
            bool WithErrors = false;
            std::string SystemsBadList {};
            auto Statuses = Application::GetSubsystemStatuses();
            for (const auto& NameStatusPair : Statuses) {
                if (NameStatusPair.first == "Main") {
                    continue;
                }

                if (IgnoreSubsystems.count(NameStatusPair.first) > 0) {
                    continue; // ignore
                }
                if (NameStatusPair.second == Application::Status::Starting) {
                    FullyStarted = false;
                } else if (NameStatusPair.second == Application::Status::Bad) {
                    SystemsBadList += NameStatusPair.first + ", ";
                    WithErrors = true;
                }
            }
            // remove ", "
            SystemsBadList = SystemsBadList.substr(0, SystemsBadList.size() - 2);
            if (FullyStarted) {
                Application::SetSubsystemStatus("Main", Application::Status::Good);

                if (!WithErrors) {
                    beammp_info("ALL SYSTEMS STARTED SUCCESSFULLY, EVERYTHING IS OKAY");
                } else {
                    beammp_error("STARTUP NOT SUCCESSFUL, SYSTEMS " + SystemsBadList + " HAD ERRORS. THIS MAY OR MAY NOT CAUSE ISSUES.");
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Application::SetSubsystemStatus("Main", Application::Status::Shutdown);
    beammp_info("Shutdown.");
    return 0;
}
