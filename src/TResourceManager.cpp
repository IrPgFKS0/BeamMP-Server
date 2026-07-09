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

#include "TResourceManager.h"
#include "Common.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fmt/core.h>
#include <ios>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <unordered_set>

namespace fs = std::filesystem;

// LAN: folders inside Resources/Client whose mods should NOT be served. Lets the
// host keep disabled mods around (e.g. removed_maps, unused) while organizing the
// active ones in named subfolders. A leading '.' or '_' also marks a folder as
// ignored (handy convention for staging).
static bool IsDisabledModDir(const std::string& Name) {
    if (Name.empty()) {
        return false;
    }
    if (Name.front() == '.' || Name.front() == '_') {
        return true;
    }
    std::string Lower = Name;
    std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return Lower == "unused" || Lower.rfind("removed", 0) == 0;
}

TResourceManager::TResourceManager() {
    Application::SetSubsystemStatus("ResourceManager", Application::Status::Starting);
    std::string Path = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client";
    if (!fs::exists(Path))
        fs::create_directories(Path);
    // Scan Resources/Client recursively so mods can be organized in named
    // subfolders. Clients still see/request a flat filename, so duplicate names
    // across folders are not allowed (first wins). Disabled folders are skipped.
    std::unordered_set<std::string> seen;
    std::vector<std::pair<size_t, std::string>> modSizes; // for the crash-risk "largest mods" hint
    fs::recursive_directory_iterator it(Path, fs::directory_options::skip_permission_denied), end;
    for (; it != end; ++it) {
        const auto& entry = *it;
        std::error_code ec;
        if (entry.is_directory(ec)) {
            if (IsDisabledModDir(entry.path().filename().string())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (entry.path().extension() != ".zip") {
            continue;
        }
        std::string FullPath(entry.path().string());
        std::replace(FullPath.begin(), FullPath.end(), '\\', '/');
        std::string FileName = entry.path().filename().string();
        if (!seen.insert(FileName).second) {
            beammp_warnf("Duplicate mod filename '{}' in Resources/Client; serving only the first, ignoring '{}'", FileName, FullPath);
            continue;
        }
        const auto Size = size_t(fs::file_size(entry.path()));
        mFileList += FullPath + ';';
        mTrimmedList += "/" + FileName + ';';
        mFileSizes += std::to_string(Size) + ';';
        mMaxModSize += Size;
        mModsLoaded++;
        modSizes.emplace_back(Size, FileName);
        mModPaths[FileName] = FullPath;

        // Startup audit: show each served mod and the subfolder it lives in, so
        // redundant/overlapping bundles are easy to spot. "." = directly in Client.
        std::error_code relEc;
        auto rel = fs::relative(entry.path().parent_path(), Path, relEc).generic_string();
        if (rel.empty() || relEc) {
            rel = ".";
        }
        beammp_infof("  [mod] {:<10} {} ({:.1f} MB)", rel + "/", FileName, double(Size) / (1024.0 * 1024.0));
    }

    if (mModsLoaded) {
        beammp_info("Loaded " + std::to_string(mModsLoaded) + " Mods");
        // Crash-risk heads-up. Per BeamNG's own error-code docs, exit code 0xC0000005 on map-load
        // "usually means ... the game exceeded the allocated budget" -- an internal engine resource
        // budget (mostly TEXTURES), NOT system RAM, and there is NO fixed mod-count limit (it is
        // resource-dependent). So we key the warning on TOTAL SIZE, not count. Tune MOD_WARN_GB to
        // just below the host's observed crash point.
        static constexpr double MOD_WARN_GB = 18.0;
        const double totalGB = double(mMaxModSize) / (1024.0 * 1024.0 * 1024.0);
        beammp_infof("Served mod set: {} mods, {:.1f} GB total", mModsLoaded, totalGB);
        if (totalGB > MOD_WARN_GB) {
            std::sort(modSizes.begin(), modSizes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string biggest;
            for (size_t i = 0; i < modSizes.size() && i < 3; ++i) {
                if (i) biggest += ", ";
                biggest += modSizes[i].second + " (" + std::to_string(modSizes[i].first / (1024 * 1024)) + " MB)";
            }
            beammp_warnf("Large mod set: {:.1f} GB across {} mods (over the {:.0f} GB heads-up "
                         "threshold). BeamNG can crash CLIENTS on map-load with exit code 0xC0000005 "
                         "(\"exceeded the allocated budget\") once the texture/resource volume crosses "
                         "an internal engine limit -- a base-game limit, NOT RAM. If clients crash "
                         "loading the map, remove the largest served mods and retry. Largest: {}",
                         totalGB, mModsLoaded, MOD_WARN_GB, biggest);
        }
    }

    Application::SetSubsystemStatus("ResourceManager", Application::Status::Good);
}

void TResourceManager::RefreshFiles() {
    std::unique_lock Lock(mModsMutex);
    mMods.clear();
    mModPaths.clear();

    std::string Path = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client";

    nlohmann::json modsDB;

    auto modsJsonPath = Path + "/mods.json";
    if (std::filesystem::exists(modsJsonPath)) {
        // BeamMP-Server#455: an EMPTY mods.json makes `stream >> modsDB` throw (parse error) every
        // startup; skip it (it will be regenerated) instead of erroring. Size check before parse.
        if (std::filesystem::file_size(modsJsonPath) > 0) {
            try {
                std::ifstream stream(modsJsonPath);

                stream >> modsDB;

                stream.close();
            } catch (const std::exception& e) {
                beammp_errorf("Failed to load mods.json: {}", e.what());
            }
        } else {
            beammp_warn("Mods database file (mods.json) is empty, ignoring it");
        }
    }

    std::unordered_set<std::string> seen;
    fs::recursive_directory_iterator it(Path, fs::directory_options::skip_permission_denied), end;
    for (; it != end; ++it) {
        const auto& entry = *it;
        std::error_code ec;
        if (entry.is_directory(ec)) {
            if (IsDisabledModDir(entry.path().filename().string())) {
                it.disable_recursion_pending();
            }
            continue;
        }

        std::string File(entry.path().string());
        std::replace(File.begin(), File.end(), '\\', '/');
        std::string FileName = entry.path().filename().string();

        if (FileName == "mods.json") {
            continue;
        }

        if (entry.path().extension() != ".zip") {
            beammp_warnf("'{}' is not a ZIP file and will be ignored", File);
            continue;
        }

        if (!seen.insert(FileName).second) {
            beammp_warnf("Duplicate mod filename '{}' in Resources/Client; serving only the first, ignoring '{}'", FileName, File);
            continue;
        }

        mModPaths[FileName] = File;

        if (modsDB.contains(entry.path().filename().string())) {
            auto& dbEntry = modsDB[entry.path().filename().string()];
            if (entry.last_write_time().time_since_epoch().count() > dbEntry["lastwrite"] || std::filesystem::file_size(File) != dbEntry["filesize"].get<size_t>()) {
                beammp_infof("File '{}' has been modified, rehashing", File);
            } else {
                dbEntry["exists"] = true;

                mMods.push_back(nlohmann::json {
                    { "file_name", std::filesystem::path(File).filename() },
                    { "file_size", std::filesystem::file_size(File) },
                    { "hash_algorithm", "sha256" },
                    { "hash", dbEntry["hash"] },
                    { "protected", dbEntry["protected"] } });

                beammp_debugf("Mod '{}' loaded from cache", File);

                continue;
            }
        }

        try {
            EVP_MD_CTX* mdctx;
            const EVP_MD* md;
            uint8_t sha256_value[EVP_MAX_MD_SIZE];
            md = EVP_sha256();
            if (md == nullptr) {
                throw std::runtime_error("EVP_sha256() failed");
            }

            mdctx = EVP_MD_CTX_new();
            if (mdctx == nullptr) {
                throw std::runtime_error("EVP_MD_CTX_new() failed");
            }
            if (!EVP_DigestInit_ex2(mdctx, md, NULL)) {
                EVP_MD_CTX_free(mdctx);
                throw std::runtime_error("EVP_DigestInit_ex2() failed");
            }

            std::ifstream stream(File, std::ios::binary);

            const size_t FileSize = std::filesystem::file_size(File);
            size_t Read = 0;
            std::vector<char> Data;
            while (Read < FileSize) {
                Data.resize(size_t(std::min<size_t>(FileSize - Read, 4096)));
                size_t RealDataSize = Data.size();
                stream.read(Data.data(), std::streamsize(Data.size()));
                if (stream.eof() || stream.fail()) {
                    RealDataSize = size_t(stream.gcount());
                }
                Data.resize(RealDataSize);
                if (RealDataSize == 0) {
                    break;
                }
                if (RealDataSize > 0 && !EVP_DigestUpdate(mdctx, Data.data(), Data.size())) {
                    EVP_MD_CTX_free(mdctx);
                    throw std::runtime_error("EVP_DigestUpdate() failed");
                }
                Read += RealDataSize;
            }
            unsigned int sha256_len = 0;
            if (!EVP_DigestFinal_ex(mdctx, sha256_value, &sha256_len)) {
                EVP_MD_CTX_free(mdctx);
                throw std::runtime_error("EVP_DigestFinal_ex() failed");
            }
            EVP_MD_CTX_free(mdctx);

            stream.close();

            std::string result;
            for (size_t i = 0; i < sha256_len; i++) {
                result += fmt::format("{:02x}", sha256_value[i]);
            }
            beammp_debugf("sha256('{}'): {}", File, result);
            mMods.push_back(nlohmann::json {
                { "file_name", std::filesystem::path(File).filename() },
                { "file_size", std::filesystem::file_size(File) },
                { "hash_algorithm", "sha256" },
                { "hash", result },
                { "protected", false } });

            modsDB[std::filesystem::path(File).filename().string()] = {
                { "lastwrite", entry.last_write_time().time_since_epoch().count() },
                { "hash", result },
                { "filesize", std::filesystem::file_size(File) },
                { "protected", false },
                { "exists", true }
            };

        } catch (const std::exception& e) {
            beammp_errorf("Sha256 hashing of '{}' failed: {}", File, e.what());
        }
    }

    for (auto dbIt = modsDB.begin(); dbIt != modsDB.end();) {
        if (!dbIt.value().contains("exists")) {
            dbIt = modsDB.erase(dbIt);
        } else {
            dbIt.value().erase("exists");
            ++dbIt;
        }
    }

    try {
        std::ofstream stream(Path + "/mods.json");

        stream << modsDB.dump(4);

        stream.close();
    } catch (std::exception& e) {
        beammp_error("Failed to update mod DB: " + std::string(e.what()));
    }
}

std::string TResourceManager::PathForMod(const std::string& FileName) {
    std::unique_lock Lock(mModsMutex);
    auto it = mModPaths.find(FileName);
    if (it != mModPaths.end()) {
        return it->second;
    }
    return "";
}

void TResourceManager::SetProtected(const std::string& ModName, bool Protected) {
    std::unique_lock Lock(mModsMutex);

    for (auto& mod : mMods) {
        if (mod["file_name"].get<std::string>() == ModName) {
            mod["protected"] = Protected;
            break;
        }
    }

    auto modsDBPath = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client/mods.json";

    if (std::filesystem::exists(modsDBPath)) {
        try {
            nlohmann::json modsDB;

            // BeamMP-Server#455: don't parse an EMPTY mods.json (throws); start from {} and rewrite.
            // Use separate in/out streams so a fresh (empty) file isn't left truncated on a read throw.
            if (std::filesystem::file_size(modsDBPath) > 0) {
                std::ifstream inStream(modsDBPath);
                inStream >> modsDB;
                inStream.close();
            } else {
                beammp_warn("Mods database file (mods.json) is empty, regenerating it");
            }

            if (modsDB.contains(ModName)) {
                modsDB[ModName]["protected"] = Protected;
            }

            std::ofstream outStream(modsDBPath);
            outStream << modsDB.dump(4);
            outStream.close();
        } catch (const std::exception& e) {
            beammp_errorf("Failed to update mods.json: {}", e.what());
        }
    }
}
