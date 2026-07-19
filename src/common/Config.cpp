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

#include "common/Config.hpp"

#include <windows.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr const char* kConfigName = "WarcraftXL.cfg";

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const unsigned char ac = static_cast<unsigned char>(a[i]);
            const unsigned char bc = static_cast<unsigned char>(b[i]);
            if (std::tolower(ac) != std::tolower(bc)) return false;
        }
        return true;
    }

    std::string TrimDirectory(std::string path)
    {
        while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
        return path;
    }

    std::string DirectoryOf(std::string_view path)
    {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string_view::npos) return {};
        if (slash == 2 && path.size() >= 2 && path[1] == ':')
            return std::string(path.substr(0, 3));
        if (slash == 0) return std::string(path.substr(0, 1));
        return TrimDirectory(std::string(path.substr(0, slash)));
    }

    std::string FileNameOf(std::string_view path)
    {
        const size_t slash = path.find_last_of("\\/");
        return std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
    }

    bool IsAbsolute(std::string_view path)
    {
        return (!path.empty() && (path[0] == '\\' || path[0] == '/'))
            || (path.size() >= 2 && path[1] == ':');
    }

    std::string Join(std::string_view directory, std::string_view leaf)
    {
        if (directory.empty() || IsAbsolute(leaf)) return std::string(leaf);
        std::string result(directory);
        if (result.back() != '\\' && result.back() != '/') result.push_back('\\');
        result.append(leaf);
        return result;
    }

    bool IsFile(std::string_view path)
    {
        const DWORD attributes = GetFileAttributesA(std::string(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::string ProcessImagePath()
    {
        std::vector<char> path(32768, '\0');
        const DWORD length = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) return {};
        return std::string(path.data(), length);
    }

    std::string WorkingDirectory()
    {
        const DWORD required = GetCurrentDirectoryA(0, nullptr);
        if (required == 0) return {};
        std::vector<char> path(static_cast<size_t>(required) + 1, '\0');
        const DWORD length = GetCurrentDirectoryA(static_cast<DWORD>(path.size()), path.data());
        if (length == 0 || length >= path.size()) return {};
        return TrimDirectory(std::string(path.data(), length));
    }

    void AddUniqueDirectory(std::vector<std::string>& directories, std::string directory)
    {
        directory = TrimDirectory(std::move(directory));
        if (directory.empty()) return;
        for (const std::string& existing : directories)
            if (EqualsIgnoreCase(existing, directory)) return;
        directories.push_back(std::move(directory));
    }

    /**
     * @brief Builds the shared lookup order once.
     *
     * A discovered Wow.exe directory is always first. The executable directory, its parent, the
     * working directory, and its parent preserve standalone patcher/developer workflows when no client
     * installation can be identified. For the installed host this naturally resolves Utils\\.. first.
     */
    const std::vector<std::string>& SearchDirectories()
    {
        static const std::vector<std::string> directories = [] {
            const std::string image = ProcessImagePath();
            const std::string exeDirectory = DirectoryOf(image);
            const std::string cwd = WorkingDirectory();

            std::vector<std::string> probes;
            AddUniqueDirectory(probes, exeDirectory);
            AddUniqueDirectory(probes, DirectoryOf(exeDirectory));
            AddUniqueDirectory(probes, cwd);
            AddUniqueDirectory(probes, DirectoryOf(cwd));

            std::string clientRoot;
            if (EqualsIgnoreCase(FileNameOf(image), "Wow.exe"))
                clientRoot = exeDirectory;
            else
                for (const std::string& probe : probes)
                    if (IsFile(Join(probe, "Wow.exe"))) { clientRoot = probe; break; }

            std::vector<std::string> result;
            AddUniqueDirectory(result, clientRoot);
            if (clientRoot.empty())
                for (std::string& probe : probes) AddUniqueDirectory(result, std::move(probe));
            return result;
        }();
        return directories;
    }

    void DebugDiagnostic(const char* fmt, ...)
    {
        char body[768] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf(body, sizeof body, fmt, args);
        va_end(args);

        char line[896] = {};
        snprintf(line, sizeof line, "WarcraftXL config: %s\n", body);
        OutputDebugStringA(line);
    }

    void WarnOnce(std::string key, const char* fmt, ...)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> emitted;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!emitted.emplace(std::move(key)).second) return;
        }

        char body[768] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf(body, sizeof body, fmt, args);
        va_end(args);

        char line[896] = {};
        const int count = snprintf(line, sizeof line, "WarcraftXL config warning: %s\n", body);
        OutputDebugStringA(line);

        const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
        if (error && error != INVALID_HANDLE_VALUE && GetFileType(error) != FILE_TYPE_UNKNOWN)
        {
            DWORD written = 0;
            const DWORD bytes = static_cast<DWORD>(count > 0
                ? (std::min)(static_cast<size_t>(count), sizeof(line) - 1) : 0);
            if (bytes) WriteFile(error, line, bytes, &written, nullptr);
        }
    }

    struct ConfigFile
    {
        std::unordered_map<std::string, std::string> entries;
        std::string path;
    };

    /**
     * @brief The optional user config file, parsed once per process.
     *
     * Plain "KEY=value" lines, '#' comments, spaces trimmed. The Wow.exe directory is authoritative;
     * executable/CWD fallbacks retain standalone developer and patcher workflows.
     */
    const ConfigFile& CfgFile()
    {
        static const ConfigFile config = [] {
            ConfigFile result;
            FILE* f = nullptr;
            for (const std::string& directory : SearchDirectories())
            {
                const std::string candidate = Join(directory, kConfigName);
                if (fopen_s(&f, candidate.c_str(), "rb") == 0 && f)
                {
                    result.path = candidate;
                    break;
                }
                if (IsFile(candidate))
                {
                    WarnOnce(candidate + "|open", "cannot open %s", candidate.c_str());
                    result.path = candidate;
                    return result;
                }
            }
            if (!f) return result;

            char line[2048];
            uint32_t lineNumber = 0;
            while (fgets(line, sizeof line, f))
            {
                ++lineNumber;
                if (!std::strchr(line, '\n') && !feof(f))
                {
                    int ch = 0;
                    while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
                    WarnOnce(result.path + "|long-line|" + std::to_string(lineNumber),
                             "%s:%u line exceeds %zu bytes and was ignored",
                             result.path.c_str(), lineNumber, sizeof(line) - 1);
                    continue;
                }

                char* text = line;
                if (lineNumber == 1 && std::strlen(text) >= 3
                    && static_cast<unsigned char>(text[0]) == 0xEF
                    && static_cast<unsigned char>(text[1]) == 0xBB
                    && static_cast<unsigned char>(text[2]) == 0xBF)
                    text += 3;
                while (*text == ' ' || *text == '\t') ++text;
                if (*text == '#' || *text == ';' || *text == '\r' || *text == '\n' || *text == '\0')
                    continue;
                char* eq = std::strchr(text, '=');
                if (!eq)
                {
                    WarnOnce(result.path + "|missing-equals|" + std::to_string(lineNumber),
                             "%s:%u has no '=' and was ignored", result.path.c_str(), lineNumber);
                    continue;
                }
                char* keyEnd = eq;
                while (keyEnd > text && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) --keyEnd;
                if (keyEnd == text)
                {
                    WarnOnce(result.path + "|empty-key|" + std::to_string(lineNumber),
                             "%s:%u has an empty key and was ignored", result.path.c_str(), lineNumber);
                    continue;
                }
                char* value = eq + 1;
                while (*value == ' ' || *value == '\t') ++value;
                char* valueEnd = value + std::strlen(value);
                while (valueEnd > value && (valueEnd[-1] == '\n' || valueEnd[-1] == '\r'
                                         || valueEnd[-1] == ' '  || valueEnd[-1] == '\t')) --valueEnd;
                const std::string key(text, keyEnd);
                if (!result.entries.emplace(key, std::string(value, valueEnd)).second)
                    WarnOnce(result.path + "|duplicate|" + key,
                             "%s:%u repeats %s; the first value remains active",
                             result.path.c_str(), lineNumber, key.c_str());
            }
            fclose(f);
            DebugDiagnostic("loaded %zu entries from %s", result.entries.size(), result.path.c_str());
            return result;
        }();
        return config;
    }

    enum class ReadResult
    {
        Missing,
        Value,
        Invalid,
    };

    /**
     * @brief Resolves a knob's raw value: environment first, then the WarcraftXL.cfg file.
     * @return Value when copied, Invalid when an explicit value does not fit, otherwise Missing.
     */
    ReadResult ReadValue(const char* name, char* buf, DWORD cap)
    {
        if (!name || !*name || !buf || cap == 0) return ReadResult::Missing;

        const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
        if (required > 0)
        {
            if (required > cap)
            {
                WarnOnce(std::string(name) + "|environment-too-long",
                         "%s from the environment is too long (%lu bytes; limit %lu) and was ignored",
                         name, static_cast<unsigned long>(required - 1),
                         static_cast<unsigned long>(cap - 1));
                return ReadResult::Invalid;
            }
            const DWORD copied = GetEnvironmentVariableA(name, buf, cap);
            if (copied > 0 && copied < cap) return ReadResult::Value;
            return ReadResult::Invalid;
        }

        const auto& cfg = CfgFile();
        const auto it = cfg.entries.find(name);
        if (it == cfg.entries.end() || it->second.empty()) return ReadResult::Missing;
        if (it->second.size() + 1 > cap)
        {
            WarnOnce(std::string(name) + "|config-too-long",
                     "%s in %s is too long (%zu bytes; limit %lu) and was ignored",
                     name, cfg.path.c_str(), it->second.size(), static_cast<unsigned long>(cap - 1));
            return ReadResult::Invalid;
        }
        std::memcpy(buf, it->second.c_str(), it->second.size() + 1);
        return ReadResult::Value;
    }

    bool SentinelExists(const char* file)
    {
        if (!file || !*file) return false;
        if (IsAbsolute(file)) return IsFile(file);
        for (const std::string& directory : SearchDirectories())
            if (IsFile(Join(directory, file))) return true;
        return false;
    }

    bool ParseUnsigned(const char* name, const char* raw, uint64_t& parsed)
    {
        const char* begin = raw;
        while (*begin && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
        if (!*begin || *begin == '-')
        {
            WarnOnce(std::string(name ? name : "?") + "|invalid-number",
                     "%s has invalid unsigned value '%s'; using its fallback",
                     name ? name : "?", raw ? raw : "");
            return false;
        }

        errno = 0;
        char* end = nullptr;
        const uint64_t value = std::strtoull(begin, &end, 10);
        while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (end == begin || !end || *end != '\0' || errno == ERANGE)
        {
            WarnOnce(std::string(name ? name : "?") + "|invalid-number",
                     "%s has invalid unsigned value '%s'; using its fallback",
                     name ? name : "?", raw ? raw : "");
            return false;
        }
        parsed = value;
        return true;
    }
}

namespace wxl::config
{
    bool Truthy(const char* raw, bool fallback)
    {
        if (!raw || !*raw) return fallback;
        std::string_view value(raw);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        if (value.empty()) return fallback;

        if (EqualsIgnoreCase(value, "0") || EqualsIgnoreCase(value, "no")
            || EqualsIgnoreCase(value, "false") || EqualsIgnoreCase(value, "off")
            || EqualsIgnoreCase(value, "disable") || EqualsIgnoreCase(value, "disabled"))
            return false;
        if (EqualsIgnoreCase(value, "1") || EqualsIgnoreCase(value, "yes")
            || EqualsIgnoreCase(value, "true") || EqualsIgnoreCase(value, "on")
            || EqualsIgnoreCase(value, "enabled"))
            return true;

        const char c = value.front();
        return !(c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F');
    }

    bool Raw(const char* name, char* buf, size_t cap)
    {
        return ReadValue(name, buf, static_cast<DWORD>(cap)) == ReadResult::Value;
    }

    bool Env(const char* name, bool fallback)
    {
        char value[16] = {};
        if (ReadValue(name, value, sizeof value) != ReadResult::Value) return fallback;
        return Truthy(value, fallback);
    }

    bool Flag(const char* envName, const char* disableFile)
    {
        char value[16] = {};
        const ReadResult result = ReadValue(envName, value, sizeof value);
        if (result == ReadResult::Value) return Truthy(value, true);
        // An invalid explicit value is ignored. Preserve the remaining priority chain instead of
        // accidentally forcing the feature on and bypassing an existing .disable sentinel.
        if (SentinelExists(disableFile)) return false;
        return true;
    }

    uint64_t U64(const char* name, uint64_t fallback, uint64_t minValue, uint64_t maxValue)
    {
        char value[32] = {};
        if (ReadValue(name, value, sizeof value) != ReadResult::Value) return fallback;
        uint64_t parsed = 0;
        if (!ParseUnsigned(name, value, parsed)) return fallback;
        if (parsed < minValue)
        {
            WarnOnce(std::string(name) + "|clamped-low",
                     "%s=%llu is below %llu and was clamped",
                     name, static_cast<unsigned long long>(parsed),
                     static_cast<unsigned long long>(minValue));
            return minValue;
        }
        if (parsed > maxValue)
        {
            WarnOnce(std::string(name) + "|clamped-high",
                     "%s=%llu is above %llu and was clamped",
                     name, static_cast<unsigned long long>(parsed),
                     static_cast<unsigned long long>(maxValue));
            return maxValue;
        }
        return parsed;
    }

    uint32_t U32(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue)
    {
        return static_cast<uint32_t>(U64(name, fallback, minValue, maxValue));
    }

    uint32_t BytesMbKb(const char* envMb, const char* envKb, uint32_t defBytes,
                       uint32_t minKb, uint32_t maxKb)
    {
        char value[32] = {};
        if (ReadValue(envMb, value, sizeof value) == ReadResult::Value)
        {
            uint64_t mb = 0;
            if (ParseUnsigned(envMb, value, mb) && mb <= UINT64_MAX / 1024ull)
            {
                const uint64_t kb = mb * 1024ull;
                if (kb >= minKb && kb <= maxKb)
                    return static_cast<uint32_t>(kb * 1024ull);
            }
        }
        if (ReadValue(envKb, value, sizeof value) == ReadResult::Value)
        {
            uint64_t kb = 0;
            if (ParseUnsigned(envKb, value, kb) && kb >= minKb && kb <= maxKb)
                return static_cast<uint32_t>(kb * 1024ull);
        }
        return defBytes;
    }
}
