// Environment/flag-file configuration helpers shared by every WarcraftXL binary.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief One truthiness convention, one bounds convention, for every knob in the project.
 *
 * Replaces the ~10 hand-rolled copies of env parsing (GameHooks, StorageHook, ModuleInstall,
 * host, profilers) whose bounds had silently diverged. Values are read at call time; callers
 * that want caching wrap the call in a function-local static, as before.
 */
namespace wxl::config
{
    /**
     * @brief Interprets a raw string as a boolean.
     *
     * Common forms are accepted case-insensitively (0/no/false/off and 1/yes/true/on). For backward
     * compatibility, other values retain the historical leading 0/n/f=false, anything-else=true rule.
     * @param raw       value to interpret, may be null.
     * @param fallback  result when raw is null or empty.
     */
    bool Truthy(const char* raw, bool fallback);

    /**
     * @brief Copies a knob's raw string value into buf: environment first, then WarcraftXL.cfg.
     * @param name  knob name (the WXL_* key).
     * @param buf   receives the NUL-terminated value.
     * @param cap   buffer capacity.
     * @return true when a non-empty value was found and fits.
     */
    bool Raw(const char* name, char* buf, size_t cap);

    /**
     * @brief Reads a boolean environment variable.
     * @param name      environment variable name.
     * @param fallback  result when the variable is absent or empty.
     */
    bool Env(const char* name, bool fallback);

    /**
     * @brief Feature toggle: an environment/config value and a .disable sentinel, defaulting to ON.
     *
     * Resolution follows the project-wide priority exactly: environment > WarcraftXL.cfg > sentinel
     * > default. Consequently, any explicit truthy value enables the feature even when an old sentinel
     * remains beside Wow.exe.
     * @param envName      environment/config key.
     * @param disableFile  sentinel file name whose presence disables, may be null.
     * @return true when the feature is enabled.
     */
    bool Flag(const char* envName, const char* disableFile);

    /**
     * @brief Reads an unsigned environment variable, clamped into [minValue, maxValue].
     * @param name      environment variable name.
     * @param fallback  result when absent or unparsable.
     */
    uint64_t U64(const char* name, uint64_t fallback, uint64_t minValue, uint64_t maxValue);

    /** @brief U32 variant of U64. */
    uint32_t U32(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue);

    /**
     * @brief Reads a byte size from an MB env var, then a KB env var, then a default.
     *
     * A candidate outside [minKb, maxKb] (after unit conversion) is REJECTED — the next source is
     * tried — matching the historical MB→KB→default cascades in GameHooks/StorageHook.
     * @param envMb     environment variable holding megabytes, may be null.
     * @param envKb     environment variable holding kilobytes, may be null.
     * @param defBytes  default when both are absent or out of range.
     * @param minKb     inclusive lower bound in KB.
     * @param maxKb     inclusive upper bound in KB.
     * @return the resolved size in bytes.
     */
    uint32_t BytesMbKb(const char* envMb, const char* envKb, uint32_t defBytes,
                       uint32_t minKb, uint32_t maxKb);
}
