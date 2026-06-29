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

#include "Common.h"
#include <nlohmann/json.hpp>
#include <unordered_map>

class TResourceManager {
public:
    TResourceManager();

    [[nodiscard]] size_t MaxModSize() const { return mMaxModSize; }
    [[nodiscard]] std::string FileList() const { return mFileList; }
    [[nodiscard]] std::string TrimmedList() const { return mTrimmedList; }
    [[nodiscard]] std::string FileSizes() const { return mFileSizes; }
    [[nodiscard]] int ModsLoaded() const { return mModsLoaded; }
    [[nodiscard]] nlohmann::json GetMods() const { return mMods; }

    // LAN: mods may live in named subfolders of Resources/Client (for
    // organization). Clients still request a bare filename, so resolve it to the
    // real on-disk path. Returns "" if the mod is not known.
    [[nodiscard]] std::string PathForMod(const std::string& FileName);

    void RefreshFiles();
    void SetProtected(const std::string& ModName, bool Protected);

private:
    size_t mMaxModSize = 0;
    std::string mFileSizes;
    std::string mFileList;
    std::string mTrimmedList;
    int mModsLoaded = 0;

    std::mutex mModsMutex;
    nlohmann::json mMods = nlohmann::json::array();
    // filename (e.g. "foo.zip") -> full path (may be inside a subfolder)
    std::unordered_map<std::string, std::string> mModPaths;
};
