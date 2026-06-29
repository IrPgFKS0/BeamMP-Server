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

#include "TServer.h"
#include "Client.h"
#include "Common.h"
#include "CustomAssert.h"
#include "TLuaEngine.h"
#include "TNetwork.h"
#include "TPPSMonitor.h"
#include <TLuaPlugin.h>
#include <algorithm>
#include <any>
#include <optional>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "LuaAPI.h"

#undef GetObject // Fixes Windows

#include "Json.h"

static std::optional<std::pair<int, int>> GetPidVid(const std::string& str) {
    auto IDSep = str.find('-');
    // H1: with no '-', substr(0,npos)=whole string and substr(npos+1)=substr(0)=whole string,
    // so both pid and vid become the same all-digit string and the packet is misrouted to a
    // plausible-looking {N,N}. Require the separator.
    if (IDSep == std::string::npos) {
        return std::nullopt;
    }
    std::string pid = str.substr(0, IDSep);
    std::string vid = str.substr(IDSep + 1);
    if (pid.empty() || vid.empty()) {
        return std::nullopt; // e.g. "-0" / "0-": the digit check below is vacuously true on ""
    }

    if (pid.find_first_not_of("0123456789") == std::string::npos && vid.find_first_not_of("0123456789") == std::string::npos) {
        try {
            int PID = stoi(pid);
            int VID = stoi(vid);
            return { { PID, VID } };
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}
TEST_CASE("GetPidVid") {
    SUBCASE("Valid singledigit") {
        const auto MaybePidVid = GetPidVid("0-1");
        CHECK(MaybePidVid);
        auto [pid, vid] = MaybePidVid.value();

        CHECK_EQ(pid, 0);
        CHECK_EQ(vid, 1);
    }
    SUBCASE("Valid doubledigit") {
        const auto MaybePidVid = GetPidVid("10-12");
        CHECK(MaybePidVid);
        auto [pid, vid] = MaybePidVid.value();

        CHECK_EQ(pid, 10);
        CHECK_EQ(vid, 12);
    }
    SUBCASE("Valid doubledigit 2") {
        const auto MaybePidVid = GetPidVid("10-2");
        CHECK(MaybePidVid);
        auto [pid, vid] = MaybePidVid.value();

        CHECK_EQ(pid, 10);
        CHECK_EQ(vid, 2);
    }
    SUBCASE("Valid doubledigit 3") {
        const auto MaybePidVid = GetPidVid("33-23");
        CHECK(MaybePidVid);
        auto [pid, vid] = MaybePidVid.value();

        CHECK_EQ(pid, 33);
        CHECK_EQ(vid, 23);
    }
    SUBCASE("Valid doubledigit 4") {
        const auto MaybePidVid = GetPidVid("3-23");
        CHECK(MaybePidVid);
        auto [pid, vid] = MaybePidVid.value();

        CHECK_EQ(pid, 3);
        CHECK_EQ(vid, 23);
    }
    SUBCASE("Empty string") {
        const auto MaybePidVid = GetPidVid("");
        CHECK(!MaybePidVid);
    }
    SUBCASE("Invalid separator") {
        const auto MaybePidVid = GetPidVid("0x0");
        CHECK(!MaybePidVid);
    }
    SUBCASE("Missing pid") {
        const auto MaybePidVid = GetPidVid("-0");
        CHECK(!MaybePidVid);
    }
    SUBCASE("Missing vid") {
        const auto MaybePidVid = GetPidVid("0-");
        CHECK(!MaybePidVid);
    }
    SUBCASE("Invalid pid") {
        const auto MaybePidVid = GetPidVid("x-0");
        CHECK(!MaybePidVid);
    }
    SUBCASE("Invalid vid") {
        const auto MaybePidVid = GetPidVid("0-x");
        CHECK(!MaybePidVid);
    }
}
TServer::TServer(const std::vector<std::string_view>& Arguments) {
    beammp_info("BeamMP LAN Server v" + Application::ServerVersionString());
    Application::SetSubsystemStatus("Server", Application::Status::Starting);
    Application::SetSubsystemStatus("Server", Application::Status::Good);
}

void TServer::RemoveClient(const std::weak_ptr<TClient>& WeakClientPtr) {
    auto LockedClientPtr = WeakClientPtr.lock();
    if (!LockedClientPtr) {
        return;
    }
    TClient& Client = *LockedClientPtr;
    beammp_debug("removing client " + Client.GetName() + " (" + std::to_string(ClientCount()) + ")");
    Client.ClearCars();
    WriteLock Lock(mClientsMutex);
    mClients.erase(LockedClientPtr);
}

void TServer::ForEachClient(const std::function<bool(std::weak_ptr<TClient>)>& Fn) {
    decltype(mClients) Clients;
    {
        ReadLock lock(mClientsMutex);
        Clients = mClients;
    }
    for (auto& Client : Clients) {
        if (!Fn(Client)) {
            break;
        }
    }
}

size_t TServer::ClientCount() const {
    ReadLock Lock(mClientsMutex);
    return mClients.size();
}

// Seamless map switch: may this client use the in-game /map command? Admins = the
// configured General/AdminName, plus (optionally) loopback clients on a single-box host.
static bool IsMapAdmin(const TClient& c) {
    if (Application::Settings.getAsBool(Settings::Key::General_AllowLoopbackAdmin)) {
        const auto& ids = c.GetIdentifiers();
        auto it = ids.find("ip");
        if (it != ids.end()) {
            const std::string& ip = it->second;
            if (ip == "::1" || ip.rfind("127.", 0) == 0) {
                return true;
            }
        }
    }
    const std::string AdminName = Application::Settings.getAsString(Settings::Key::General_AdminName);
    return !AdminName.empty() && c.GetName() == AdminName;
}

// Seamless map switch: handle "/map <name|path>" typed in chat (admin-gated). Accepts a
// short level name (expanded to /levels/<name>/info.json) or a full level path. Returns
// true if the message was a /map command (so the caller doesn't broadcast it as chat).
static void HandleMapChatCommand(TClient& c, const std::string& Message, TNetwork& Network) {
    auto Reply = [&](const std::string& Text) {
        (void)Network.Respond(c, StringToVector("C:Server: " + Text), true);
    };
    auto isSp = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    std::string Arg;
    auto SpacePos = Message.find(' ');
    if (SpacePos != std::string::npos) {
        Arg = Message.substr(SpacePos + 1);
    }
    while (!Arg.empty() && isSp(Arg.front())) {
        Arg.erase(Arg.begin());
    }
    while (!Arg.empty() && isSp(Arg.back())) {
        Arg.pop_back();
    }
    // "/map" or "/map list": ask the requesting client to print its available maps
    // (the client enumerates core_levels.getList(), which includes synced MODDED maps,
    // and marks them). Informational, so it is NOT admin-gated.
    if (Arg.empty() || Arg == "list") {
        (void)Network.Respond(c, StringToVector("E:mapList:"), true);
        return;
    }
    // "/map <name|path>": the actual switch is admin-gated.
    if (!IsMapAdmin(c)) {
        beammp_infof("Player '{}' ({}) tried /map without permission", c.GetName(), c.GetID());
        Reply("You are not allowed to change the map. (type /map for the list)");
        return;
    }
    const std::string MapPath = (Arg.find('/') == std::string::npos) ? ("/levels/" + Arg + "/info.json") : Arg;
    const uint32_t Gen = c.Server().ChangeMap(MapPath);
    beammp_infof("Player '{}' ({}) changed map to '{}' (generation {})", c.GetName(), c.GetID(), MapPath, Gen);
    Network.SendToAll(nullptr, StringToVector("C:Server: " + c.GetName() + " switched the map to " + MapPath), true, true);
}

void TServer::GlobalParser(const std::weak_ptr<TClient>& Client, std::vector<uint8_t>&& Packet, TPPSMonitor& PPSMonitor, TNetwork& Network, bool udp) {
    constexpr std::string_view ABG = "ABG:";
    if (Packet.size() >= ABG.size() && std::equal(Packet.begin(), Packet.begin() + ABG.size(), ABG.begin(), ABG.end())) {
        Packet.erase(Packet.begin(), Packet.begin() + ABG.size());
        try {
            Packet = DeComp(Packet);
        } catch (const InvalidDataError& ) {
            if (auto LockedClient = Client.lock()) {
                beammp_errorf("Failed to decompress packet from client {}. The client sent invalid data and will now be disconnected.", LockedClient->GetID());
                Network.ClientKick(*LockedClient, "Sent invalid compressed packet (this is likely a bug on your end)");
            }
            return;
        } catch (const std::runtime_error& e) {
            if (auto LockedClient = Client.lock()) {
                beammp_errorf("Failed to decompress packet from client {}: {}. The server might be out of RAM! The client will now be disconnected.", LockedClient->GetID(), e.what());
                Network.ClientKick(*LockedClient, "Decompression failed (likely a server-side problem)");
            }
            return;
        }
    }
    if (Packet.empty()) {
        return;
    }

    auto LockedClient = Client.lock();
    if (!LockedClient) {
        return;
    }

    std::any Res;
    char Code = Packet.at(0);

    std::string StringPacket(reinterpret_cast<const char*>(Packet.data()), Packet.size());

    // V to Y
    if (Code <= 89 && Code >= 86) {
        // Seamless map switch: drop a transitioning client's stale old-map vehicle
        // data so it isn't rebroadcast to peers who already loaded the new map.
        if (LockedClient->IsTransitioning())
            return;
        int PID = -1;
        int VID = -1;

        auto pidVidPart = StringPacket.substr(3);
        auto MaybePidVid = GetPidVid(pidVidPart.substr(0, pidVidPart.find(':')));
        if (MaybePidVid) {
            std::tie(PID, VID) = MaybePidVid.value();
        }

        if (PID == -1 || VID == -1 || PID != LockedClient->GetID()) {
            return;
        }

        PPSMonitor.IncrementInternalPPS();
        Network.SendToAll(LockedClient.get(), Packet, false, false);
        return;
    }
    switch (Code) {
    case 'H': // initial connection
        if (udp) {
            beammp_debugf("Received 'H' packet over UDP from client '{}' ({}), ignoring it", LockedClient->GetName(), LockedClient->GetID());
            return;
        }
        if (!Network.SyncClient(Client)) {
            // TODO handle
        }
        return;
    case 'p':
        if (!Network.Respond(*LockedClient, StringToVector("p"), false)) {
            // failed to send
            Network.DisconnectClient(*LockedClient, "Failed to send ping");
        } else {
            Network.UpdatePlayer(*LockedClient);
        }
        return;
    case 'O':
        if (udp) {
            beammp_debugf("Received 'O' packet over UDP from client '{}' ({}), ignoring it", LockedClient->GetName(), LockedClient->GetID());
            return;
        }
        // Seamless map switch: ignore old-map spawn/edit/delete events from a client
        // that is still transitioning (it will re-spawn after sending its map-ready ack).
        if (LockedClient->IsTransitioning())
            return;
        if (Packet.size() > 1000) {
            beammp_debug(("Received data from: ") + LockedClient->GetName() + (" Size: ") + std::to_string(Packet.size()));
        }
        ParseVehicle(*LockedClient, StringPacket, Network);
        return;
    case 'C': {
        if (udp) {
            beammp_debugf("Received 'C' packet over UDP from client '{}' ({}), ignoring it", LockedClient->GetName(), LockedClient->GetID());
            return;
        }
        if (Packet.size() < 4 || std::find(Packet.begin() + 3, Packet.end(), ':') == Packet.end())
            break;
        const auto PacketAsString = std::string(reinterpret_cast<const char*>(Packet.data()), Packet.size());
        std::string Message = "";
        const auto ColonPos = PacketAsString.find(':', 3);
        if (ColonPos != std::string::npos && ColonPos + 2 < PacketAsString.size()) {
            Message = PacketAsString.substr(ColonPos + 2);
        }
        if (Message.empty()) {
            beammp_debugf("Empty chat message received from '{}' ({}), ignoring it", LockedClient->GetName(), LockedClient->GetID());
            return;
        }
        if (Message.size() > 500) {
           beammp_debugf("Chat message too long from '{}' ({}), ignoring it", LockedClient->GetName(), LockedClient->GetID());
           return;
        }
        // Seamless map switch: intercept the "/map <name>" chat command (admin-gated) so it
        // isn't broadcast/logged as a normal chat message. The console `map` stays the fallback.
        if (Message == "/map" || Message.rfind("/map ", 0) == 0) {
            HandleMapChatCommand(*LockedClient, Message, Network);
            return;
        }
        auto Futures = LuaAPI::MP::Engine->TriggerEvent("onChatMessage", "", LockedClient->GetID(), LockedClient->GetName(), Message);
        TLuaEngine::WaitForAll(Futures);
        LogChatMessage(LockedClient->GetName(), LockedClient->GetID(), PacketAsString.substr(PacketAsString.find(':', 3) + 1));
        bool Rejected = std::any_of(Futures.begin(), Futures.end(),
            [](const std::shared_ptr<TLuaResult>& Elem) {
                auto Snapshot = Elem->GetDetachedSnapshot();
                if (Snapshot.Error) {
                    return false;
                }
                const int* MaybeInt = std::get_if<int>(&Snapshot.Result.V);
                if (MaybeInt == nullptr) {
                    return false;
                }
                return bool(*MaybeInt);
            });
        if (!Rejected) {
            std::string SanitizedPacket = fmt::format("C:{}: {}", LockedClient->GetName(), Message);
            Network.SendToAll(nullptr, StringToVector(SanitizedPacket), true, true);
        }
        auto PostFutures = LuaAPI::MP::Engine->TriggerEvent("postChatMessage", "", !Rejected, LockedClient->GetID(), LockedClient->GetName(), Message);
        LuaAPI::MP::Engine->ReportErrors(PostFutures);
        return;
    }
    case 'E':
        if (udp) {
            beammp_debugf("Received 'E' packet over UDP from client '{}' ({}), ignoring it", LockedClient->GetName(), LockedClient->GetID());
            return;
        }
        HandleEvent(*LockedClient, StringPacket);
        return;
    case 'N':
        Network.SendToAll(LockedClient.get(), Packet, false, true);
        return;
    case 'B': // LAN fork: networked weapon-explosion sync -- relay the owner's authoritative
              // blast to all OTHER clients (reliable/TCP); the sender already applied it locally.
        Network.SendToAll(LockedClient.get(), Packet, false, true);
        return;
    case 'M': // seamless map switch: client map-ready ack -> "Mr<generation>"
        if (Packet.size() >= 2 && Packet.at(1) == 'r') {
            uint32_t Gen = 0;
            try {
                Gen = static_cast<uint32_t>(std::stoul(StringPacket.substr(2)));
            } catch (const std::exception&) {
                return;
            }
            // Only clear the fence once the client has loaded the CURRENT map
            // (ignore acks for a superseded generation after a rapid re-switch).
            if (Gen == mMapGeneration.load(std::memory_order_acquire)) {
                LockedClient->SetMapGeneration(Gen);
                LockedClient->SetTransitioning(false);
                beammp_debugf("Client '{}' ({}) ready on map generation {}", LockedClient->GetName(), LockedClient->GetID(), Gen);
            }
        }
        return;
    case 'Z': { // position packet
        // Seamless map switch: drop a transitioning client's stale old-map positions.
        if (LockedClient->IsTransitioning())
            return;
        PPSMonitor.IncrementInternalPPS();

        int PID = -1;
        int VID = -1;

        auto pidVidPart = StringPacket.substr(3);
        auto MaybePidVid = GetPidVid(pidVidPart.substr(0, pidVidPart.find(':')));
        if (MaybePidVid) {
            std::tie(PID, VID) = MaybePidVid.value();
        }

        if (PID == -1 || VID == -1 || PID != LockedClient->GetID()) {
            return;
        }

        Network.SendToAll(LockedClient.get(), Packet, false, false);
        HandlePosition(*LockedClient, StringPacket);
        return;
    }
    default:
        return;
    }
}

uint32_t TServer::ChangeMap(const std::string& MapPath) {
    // Persist the new map so any late joiner receives it via the normal handshake.
    Application::Settings.set(Settings::Key::General_Map, MapPath);
    // Bump the session generation: every connected client must now load this map and
    // ack it ("Mr<gen>") before its vehicle/position packets are accepted again.
    const uint32_t Gen = ++mMapGeneration;
    ForEachClient([&](std::weak_ptr<TClient> ClientPtr) -> bool {
        if (auto c = ClientPtr.lock()) {
            c->ClearCars();
            c->SetMapGeneration(Gen);
            // Only fence clients already in the session: they have old-map vehicles and
            // run the coordinated transition (ack required). A client still doing its
            // initial handshake isn't fenced -- it receives the new map via the normal
            // join (we already updated General/Map) and would never send a map-ready ack.
            c->SetTransitioning(c->IsSynced());
        }
        return true;
    });
    // Coordinated push over the reliable channel; the client mod's onMapChange handler
    // (MPGameNetwork AddEventHandler) loads the level without a reconnect/Lua reload.
    const std::string Packet = "E:onMapChange:" + MapPath + ":" + std::to_string(Gen);
    if (LuaAPI::MP::Engine) {
        LuaAPI::MP::Engine->Network().SendToAll(nullptr, StringToVector(Packet), true, true);
    }
    beammp_infof("Map change to '{}' (generation {}) requested for {} client(s)", MapPath, Gen, ClientCount());
    return Gen;
}

void TServer::HandleEvent(TClient& c, const std::string& RawData) {
    // E:Name:Data
    // Data is allowed to have ':'
    if (RawData.size() < 2) {
        beammp_debugf("Client '{}' ({}) tried to send an empty event, ignoring", c.GetName(), c.GetID());
        return;
    }
    auto NameDataSep = RawData.find(':', 2);
    if (NameDataSep == std::string::npos) {
        // C3: without the ':', substr(NameDataSep+1) wraps to substr(0) and the whole raw packet
        // is fired into Lua handlers as the event data with a garbled name. Drop it.
        beammp_warn("received event in invalid format (missing ':'), got: '" + RawData + "'");
        return;
    }
    std::string Name = RawData.substr(2, NameDataSep - 2);
    std::string Data = RawData.substr(NameDataSep + 1);

    std::vector<std::string> exclude = {"onInit", "onFileChanged","onVehicleDeleted","onConsoleInput","onPlayerAuth","postPlayerAuth", "onPlayerDisconnect",
    "onPlayerConnecting","onPlayerJoining","onPlayerJoin","onChatMessage","postChatMessage","onVehicleSpawn","postVehicleSpawn","onVehicleEdited", "postVehicleEdited",
    "onVehicleReset","onVehiclePaintChanged","onShutdown"};

    if (std::ranges::find(exclude, Name) != exclude.end()) {
        beammp_debugf("Excluded event triggered by client '{}' ({}): '{}', ignoring.", c.GetName(), c.GetID(), Name);
        return;
    }
    LuaAPI::MP::Engine->ReportErrors(LuaAPI::MP::Engine->TriggerEvent(Name, "", c.GetID(), Data));
}

bool TServer::IsUnicycle(TClient& c, const std::string& CarJson) {
    try {
        auto Car = nlohmann::json::parse(CarJson);
        const std::string jbm = "jbm";
        if (Car.contains(jbm) && Car[jbm].is_string() && Car[jbm] == "unicycle") {
            return true;
        }
    } catch (const std::exception& e) {
        beammp_warn("Failed to parse vehicle data as json for client " + std::to_string(c.GetID()) + ": '" + CarJson + "'.");
    }
    return false;
}

bool TServer::ShouldSpawn(TClient& c, const std::string& CarJson, int ID) {
    if (IsUnicycle(c, CarJson) && c.GetUnicycleID() < 0) {
        c.SetUnicycleID(ID);
        return true;
    } else {
        return c.GetCarCount() < Application::Settings.getAsInt(Settings::Key::General_MaxCars);
    }
}

void TServer::ParseVehicle(TClient& c, const std::string& Pckt, TNetwork& Network) {
    if (Pckt.length() < 6)
        return;
    std::string Packet = Pckt;
    char Code = Packet.at(1);
    int PID = -1;
    int VID = -1;
    std::string Data = Packet.substr(3), pid, vid;
    switch (Code) { // Spawned Destroyed Switched/Moved NotFound Reset
    case 's':
        beammp_tracef("got 'Os' packet: '{}' ({})", Packet, Packet.size());
        if (Data.at(0) == '0') {
            int CarID = c.GetOpenCarID();
            beammp_debugf("'{}' created a car with ID {}", c.GetName(), CarID);

            std::string CarJson = Packet.substr(5);
            Packet = "Os:" + c.GetRoles() + ":" + c.GetName() + ":" + std::to_string(c.GetID()) + "-" + std::to_string(CarID) + ":" + CarJson;
            auto Futures = LuaAPI::MP::Engine->TriggerEvent("onVehicleSpawn", "", c.GetID(), CarID, Packet.substr(3));
            TLuaEngine::WaitForAll(Futures);
            bool ShouldntSpawn = std::any_of(Futures.begin(), Futures.end(),
                [](const std::shared_ptr<TLuaResult>& Result) {
                    auto Snapshot = Result->GetDetachedSnapshot();
                    if (Snapshot.Error) {
                        return false;
                    }
                    const int* MaybeInt = std::get_if<int>(&Snapshot.Result.V);
                    if (MaybeInt == nullptr) {
                        return false;
                    }
                    return *MaybeInt != 0;
                });

            bool SpawnConfirmed = false;
            auto CarJsonDoc = nlohmann::json::parse(CarJson, nullptr, false);
            if (ShouldSpawn(c, CarJson, CarID) && !ShouldntSpawn && !CarJsonDoc.is_discarded()) {
                c.AddNewCar(CarID, CarJsonDoc);
                Network.SendToAll(nullptr, StringToVector(Packet), true, true);
                SpawnConfirmed = true;
            } else {
                if (!Network.Respond(c, StringToVector(Packet), true)) {
                    // TODO: handle
                }
                std::string Destroy = "Od:" + std::to_string(c.GetID()) + "-" + std::to_string(CarID);
                LuaAPI::MP::Engine->ReportErrors(LuaAPI::MP::Engine->TriggerEvent("onVehicleDeleted", "", c.GetID(), CarID));
                if (!Network.Respond(c, StringToVector(Destroy), true)) {
                    // TODO: handle
                }
                beammp_debugf("{} (force : car limit/lua) removed ID {}", c.GetName(), CarID);
                SpawnConfirmed = false;
            }
            auto PostFutures = LuaAPI::MP::Engine->TriggerEvent("postVehicleSpawn", "", SpawnConfirmed, c.GetID(), CarID, Packet.substr(3));
            // the post event is not cancellable so we dont wait for it
            LuaAPI::MP::Engine->ReportErrors(PostFutures);
        }
        return;
    case 'c': {
        beammp_trace(std::string(("got 'Oc' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        auto MaybePidVid = GetPidVid(Data.substr(0, Data.find(':', 1)));
        if (MaybePidVid) {
            std::tie(PID, VID) = MaybePidVid.value();
        }
        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            auto Futures = LuaAPI::MP::Engine->TriggerEvent("onVehicleEdited", "", c.GetID(), VID, Packet.substr(3));
            TLuaEngine::WaitForAll(Futures);
            bool ShouldntAllow = std::any_of(Futures.begin(), Futures.end(),
                [](const std::shared_ptr<TLuaResult>& Result) {
                    auto Snapshot = Result->GetDetachedSnapshot();
                    if (Snapshot.Error) {
                        return false;
                    }
                    const int* MaybeInt = std::get_if<int>(&Snapshot.Result.V);
                    if (MaybeInt == nullptr) {
                        return false;
                    }
                    return *MaybeInt != 0;
                });

            auto FoundPos = Packet.find('{');
            FoundPos = FoundPos == std::string::npos ? 0 : FoundPos; // attempt at sanitizing this
            bool Allowed = false;
            if ((c.GetUnicycleID() != VID || IsUnicycle(c, Packet.substr(FoundPos)))
                && !ShouldntAllow) {
                Network.SendToAll(&c, StringToVector(Packet), false, true);
                Apply(c, VID, Packet);
                Allowed = true;
            } else {
                if (c.GetUnicycleID() == VID) {
                    c.SetUnicycleID(-1);
                }
                std::string Destroy = "Od:" + std::to_string(c.GetID()) + "-" + std::to_string(VID);
                Network.SendToAll(nullptr, StringToVector(Destroy), true, true);
                LuaAPI::MP::Engine->ReportErrors(LuaAPI::MP::Engine->TriggerEvent("onVehicleDeleted", "", c.GetID(), VID));
                c.DeleteCar(VID);
                Allowed = false;
            }

            auto PostFutures = LuaAPI::MP::Engine->TriggerEvent("postVehicleEdited", "", Allowed, c.GetID(), VID, Packet.substr(3));
            // the post event is not cancellable so we dont wait for it
            LuaAPI::MP::Engine->ReportErrors(PostFutures);
        }
        return;
    }
    case 'd': {
        beammp_trace(std::string(("got 'Od' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        auto MaybePidVid = GetPidVid(Data.substr(0, Data.find(':', 1)));
        if (MaybePidVid) {
            std::tie(PID, VID) = MaybePidVid.value();
        }
        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            if (c.GetUnicycleID() == VID) {
                c.SetUnicycleID(-1);
            }
            Network.SendToAll(nullptr, StringToVector(Packet), true, true);
            // TODO: should this trigger on all vehicle deletions?
            LuaAPI::MP::Engine->ReportErrors(LuaAPI::MP::Engine->TriggerEvent("onVehicleDeleted", "", c.GetID(), VID));
            c.DeleteCar(VID);
            beammp_debug(c.GetName() + (" deleted car with ID ") + std::to_string(VID));
        }
        return;
    }
    case 'r': {
        beammp_trace(std::string(("got 'Or' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        auto MaybePidVid = GetPidVid(Data.substr(0, Data.find(':', 1)));
        if (MaybePidVid) {
            std::tie(PID, VID) = MaybePidVid.value();
        }

        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            auto BracketPos = Data.find('{');
            if (BracketPos == std::string::npos) {
                beammp_debugf("Invalid 'Or' packet body from client {}", c.GetID());
                return;
            }

            Data = Data.substr(BracketPos);
            LuaAPI::MP::Engine->ReportErrors(LuaAPI::MP::Engine->TriggerEvent("onVehicleReset", "", c.GetID(), VID, Data));
            Network.SendToAll(&c, StringToVector(Packet), false, true);
        }
        return;
    }
    case 't': {
        beammp_trace(std::string(("got 'Ot' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        auto MaybePidVid = GetPidVid(Data.substr(0, Data.find(':', 1)));
        if (MaybePidVid) {
            std::tie(PID, VID) = MaybePidVid.value();
        }
        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            Network.SendToAll(&c, StringToVector(Packet), false, true);
        }
        return;
    }
    case 'm': {
        Network.SendToAll(&c, StringToVector(Packet), false, true);
        return;
    }
    case 'p': {
        beammp_trace(std::string(("got 'Op' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        auto MaybePidVid = GetPidVid(Data.substr(0, Data.find(':', 1)));
        if (MaybePidVid) {
            std::tie(PID, VID) = MaybePidVid.value();
        }

        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            auto BracketPos = Data.find('[');
            if (BracketPos == std::string::npos) {
                beammp_debugf("Invalid 'Op' packet body from client {}", c.GetID());
                return;
            }

            Data = Data.substr(BracketPos);

            LuaAPI::MP::Engine->ReportErrors(LuaAPI::MP::Engine->TriggerEvent("onVehiclePaintChanged", "", c.GetID(), VID, Data));
            Network.SendToAll(&c, StringToVector(Packet), false, true);

            auto CarData = c.GetCarData(VID);
            if (CarData == nlohmann::detail::value_t::null)
                return;

            if (CarData.contains("vcf") && CarData.at("vcf").is_object())
                if (CarData.at("vcf").contains("paints") && CarData.at("vcf").at("paints").is_array()) {
                    CarData.at("vcf")["paints"] = nlohmann::json::parse(Data);
                    c.SetCarData(VID, CarData);
                }

        }
        return;
    }
    default:
        beammp_trace(std::string(("possibly not implemented: '") + Packet + ("' (") + std::to_string(Packet.size()) + (")")));
        return;
    }
}

void TServer::Apply(TClient& c, int VID, const std::string& pckt) {
    auto FoundPos = pckt.find('{');
    if (FoundPos == std::string::npos) {
        beammp_error("Malformed packet received, no '{' found");
        return;
    }

    std::string Packet = pckt.substr(FoundPos);
    nlohmann::json VD = c.GetCarData(VID);
    if (VD == nlohmann::detail::value_t::null) {
        beammp_error("Tried to apply change to vehicle that does not exist");
        return;
    }

    nlohmann::json Pack = nlohmann::json::parse(Packet, nullptr, false);

    if (Pack.is_discarded()) {
        beammp_error("Could not get active vehicle config!");
        return;
    }

    c.SetCarData(VID, Pack);
}

void TServer::InsertClient(const std::shared_ptr<TClient>& NewClient) {
    beammp_debug("inserting client (" + std::to_string(ClientCount()) + ")");
    WriteLock Lock(mClientsMutex); // TODO why is there 30+ threads locked here
    (void)mClients.insert(NewClient);
}

struct PidVidData {
    int PID;
    int VID;
    std::string Data;
};

static std::optional<PidVidData> ParsePositionPacket(const std::string& Packet) {
    if (Packet.size() < 3) {
        // invalid packet
        return std::nullopt;
    }
    // Zp:PID-VID:DATA
    std::string withoutCode = Packet.substr(3);

    // parse veh ID
    if (auto DataBeginPos = withoutCode.find('{'); DataBeginPos != std::string::npos && DataBeginPos != 0) {
        // separator is :{, so position of { minus one
        auto PidVidOnly = withoutCode.substr(0, DataBeginPos - 1);
        auto MaybePidVid = GetPidVid(PidVidOnly);
        if (MaybePidVid) {
            int PID = -1;
            int VID = -1;
            // FIXME: check that the VID and PID are valid, so that we don't waste memory
            std::tie(PID, VID) = MaybePidVid.value();

            std::string Data = withoutCode.substr(DataBeginPos);
            return PidVidData {
                .PID = PID,
                .VID = VID,
                .Data = Data,
            };
        } else {
            // invalid packet
            return std::nullopt;
        }
    }
    // invalid packet
    return std::nullopt;
}

TEST_CASE("ParsePositionPacket") {
    const auto TestData = R"({"tim":10.428000331623,"vel":[-2.4171722121385e-05,-9.7184734153252e-06,-7.6420763232237e-06],"rot":[-0.0001296154171915,0.0031575385950029,0.98994906610295,0.14138903660382],"rvel":[5.3640324636461e-05,-9.9824529946024e-05,5.1664064641372e-05],"pos":[-0.27281248907838,-0.20515357944633,0.49695488960431],"ping":0.032999999821186})";
    SUBCASE("All the pids and vids") {
        for (int pid = 0; pid < 100; ++pid) {
            for (int vid = 0; vid < 100; ++vid) {
                std::optional<PidVidData> MaybeRes = ParsePositionPacket(fmt::format("Zp:{}-{}:{}", pid, vid, TestData));
                CHECK(MaybeRes.has_value());
                CHECK_EQ(MaybeRes.value().PID, pid);
                CHECK_EQ(MaybeRes.value().VID, vid);
                CHECK_EQ(MaybeRes.value().Data, TestData);
            }
        }
    }
}

void TServer::HandlePosition(TClient& c, const std::string& Packet) {
    if (auto Parsed = ParsePositionPacket(Packet); Parsed.has_value()) {
        c.SetCarPosition(Parsed.value().VID, Parsed.value().Data);
    }
}
