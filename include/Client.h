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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "BoostAliases.h"
#include "Common.h"
#include "Compat.h"
#include "VehicleData.h"

class TServer;

#ifdef BEAMMP_WINDOWS
// for socklen_t
#include <WS2tcpip.h>
#endif // WINDOWS

struct TConnection final {
    ip::tcp::socket Socket;
    ip::tcp::endpoint SockAddr;
};

// In-memory transport for a "virtual" (socketless) client. On the combined host (--combined),
// the host's OWN client lives in the same process as the server and talks to it over these
// queues instead of loopback TCP/UDP -- removing the host self-socket entirely. The server side
// (TNetwork) pushes to / drains the *toClient* / *fromClient* queues; the launcher's host-mode
// loop owns the opposite ends. A normal networked client (incl. LAN2) has mLink == nullptr and
// uses the real socket path unchanged.
struct InMemoryLink final {
    // server -> client: TNetwork::TCPSend / UDP send_to push here; the launcher drains + feeds the game.
    std::mutex ToClientMtx;
    std::condition_variable ToClientCv;
    std::queue<std::vector<uint8_t>> ToClientTCP;
    std::queue<std::vector<uint8_t>> ToClientUDP;
    // client -> server: the launcher pushes game data here; TNetwork::TCPRcv / UDP dispatch drain.
    std::mutex FromClientMtx;
    std::condition_variable FromClientCv;
    std::queue<std::vector<uint8_t>> FromClientTCP;
    std::queue<std::vector<uint8_t>> FromClientUDP;
    std::atomic<bool> Closed { false };
};

class TClient final {
public:
    using TSetOfVehicleData = std::vector<TVehicleData>;

    struct TVehicleDataLockPair {
        TSetOfVehicleData* VehicleData;
        std::unique_lock<std::mutex> Lock;
    };

    TClient(TServer& Server, ip::tcp::socket&& Socket);
    TClient(const TClient&) = delete;
    ~TClient();
    TClient& operator=(const TClient&) = delete;

    void AddNewCar(int Ident, const nlohmann::json& Data);
    void SetCarData(int Ident, const nlohmann::json& Data);
    void SetCarPosition(int Ident, const std::string& Data);
    TVehicleDataLockPair GetAllCars();
    void SetName(const std::string& Name) { mName = Name; }
    void SetRoles(const std::string& Role) { mRole = Role; }
    void SetIdentifier(const std::string& key, const std::string& value) { mIdentifiers[key] = value; }
    nlohmann::json GetCarData(int Ident);
    std::string GetCarPositionRaw(int Ident);
    void SetUDPAddr(const ip::udp::endpoint& Addr) { mUDPAddress = Addr; }
    void SetTCPSock(ip::tcp::socket&& CSock) { mSocket = std::move(CSock); }
    // Returns true only for the thread that actually performs socket shutdown/close.
    [[nodiscard]] bool Disconnect(std::string_view Reason);
    bool IsDisconnected() const {
        return mDisconnectState.load(std::memory_order_acquire) != EDisconnectState::Connected;
    }
    // locks
    void DeleteCar(int Ident);
    [[nodiscard]] const std::unordered_map<std::string, std::string>& GetIdentifiers() const { return mIdentifiers; }
    [[nodiscard]] const ip::udp::endpoint& GetUDPAddr() const { return mUDPAddress; }
    [[nodiscard]] ip::udp::endpoint& GetUDPAddr() { return mUDPAddress; }
    [[nodiscard]] ip::tcp::socket& GetTCPSock() { return mSocket; }
    [[nodiscard]] const ip::tcp::socket& GetTCPSock() const { return mSocket; }
    // In-memory (virtual/socketless) client -- combined host's own client. mLink != nullptr =>
    // virtual: TNetwork uses the InMemoryLink queues instead of mSocket/mUDPSock. Normal clients
    // (incl. LAN2) leave mLink null and use the real socket path unchanged.
    [[nodiscard]] bool IsVirtual() const { return mLink != nullptr; }
    [[nodiscard]] InMemoryLink* Link() const { return mLink.get(); }
    void SetInMemoryLink(std::unique_ptr<InMemoryLink> Link) { mLink = std::move(Link); }
    [[nodiscard]] std::string GetRoles() const { return mRole; }
    [[nodiscard]] std::string GetName() const { return mName; }
    void SetUnicycleID(int ID) { mUnicycleID = ID; }
    void SetID(int ID) { mID = ID; }
    [[nodiscard]] int GetOpenCarID() const;
    [[nodiscard]] int GetCarCount() const;
    void ClearCars();
    [[nodiscard]] int GetID() const { return mID; }
    [[nodiscard]] int GetUnicycleID() const { return mUnicycleID; }
    [[nodiscard]] bool IsUDPConnected() const { return mIsUDPConnected; }
    [[nodiscard]] bool IsSynced() const { return mIsSynced; }
    [[nodiscard]] bool IsSyncing() const { return mIsSyncing; }
    [[nodiscard]] bool IsGuest() const { return mIsGuest; }
    void SetIsGuest(bool NewIsGuest) { mIsGuest = NewIsGuest; }
    void SetIsSynced(bool NewIsSynced) { mIsSynced = NewIsSynced; }
    void SetIsSyncing(bool NewIsSyncing) { mIsSyncing = NewIsSyncing; }
    // Seamless map switch: while a client is loading the new level (between the
    // server's onMapChange broadcast and the client's map-ready ack) it is
    // "transitioning"; the server fences its stale old-map vehicle/position packets.
    [[nodiscard]] bool IsTransitioning() const { return mTransitioning.load(std::memory_order_acquire); }
    void SetTransitioning(bool V) { mTransitioning.store(V, std::memory_order_release); }
    [[nodiscard]] uint32_t GetMapGeneration() const { return mMapGeneration.load(std::memory_order_acquire); }
    void SetMapGeneration(uint32_t G) { mMapGeneration.store(G, std::memory_order_release); }
    void EnqueuePacket(const std::vector<uint8_t>& Packet);
    [[nodiscard]] std::queue<std::vector<uint8_t>>& MissedPacketQueue() { return mPacketsSync; }
    [[nodiscard]] const std::queue<std::vector<uint8_t>>& MissedPacketQueue() const { return mPacketsSync; }
    [[nodiscard]] size_t MissedPacketQueueSize() const { return mPacketsSync.size(); }
    [[nodiscard]] std::mutex& MissedPacketQueueMutex() const { return mMissedPacketsMutex; }
    [[nodiscard]] std::mutex& TCPSendMutex() const { return mTCPSendMutex; }
    void SetIsUDPConnected(bool NewIsConnected) { mIsUDPConnected = NewIsConnected; }
    [[nodiscard]] TServer& Server() const;
    void UpdatePingTime();
    int SecondsSinceLastPing();
    void SetMagic(std::vector<uint8_t> magic) { mMagic = std::move(magic); }
    [[nodiscard]] const std::vector<uint8_t>& GetMagic() const { return mMagic; }

private:
    enum class EDisconnectState {
        Connected,
        Disconnecting,
        Disconnected
    };

    void InsertVehicle(int ID, const std::string& Data);

    TServer& mServer;
    // Atomic: written by the UDP/auth threads, read by Looper/TCPClient/PPSMonitor/SendToAll.
    std::atomic<bool> mIsUDPConnected { false };
    std::atomic<bool> mIsSynced { false };
    std::atomic<bool> mIsSyncing { false };
    mutable std::mutex mMissedPacketsMutex;
    // Serializes concurrent boost::asio::write() on this client's TCP socket (Looper,
    // GlobalParser/Respond, Lua callbacks, PPSMonitor all send) -- interleaved writes
    // would corrupt the 4-byte length framing and desync/disconnect the client.
    mutable std::mutex mTCPSendMutex;
    std::queue<std::vector<uint8_t>> mPacketsSync;
    std::unordered_map<std::string, std::string> mIdentifiers;
    bool mIsGuest = false;
    std::atomic<bool> mTransitioning { false };
    std::atomic<uint32_t> mMapGeneration { 0 };
    mutable std::mutex mVehicleDataMutex;
    mutable std::mutex mVehiclePositionMutex;
    TSetOfVehicleData mVehicleData;
    SparseArray<std::string> mVehiclePosition;
    std::string mName = "Unknown Client";
    // Once disconnect starts, this client is terminal and its socket must be treated as dead.
    std::atomic<EDisconnectState> mDisconnectState { EDisconnectState::Connected };
    ip::tcp::socket mSocket;
    ip::udp::endpoint mUDPAddress {};
    // Non-null only for a virtual (socketless) client -- the combined host's own client (--combined).
    std::unique_ptr<InMemoryLink> mLink;
    int mUnicycleID = -1;
    std::string mRole;
    std::string mDID;
    int mID = -1;
    std::chrono::time_point<std::chrono::high_resolution_clock> mLastPingTime = std::chrono::high_resolution_clock::now();
    std::vector<uint8_t> mMagic;
};

std::optional<std::weak_ptr<TClient>> GetClient(class TServer& Server, int ID);
