#include "claude_files.h"

#include "strutil.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>

namespace pm::gui::cf {

namespace {

fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
        if (raw)
            CoTaskMemFree(raw);
        return {};
    }
    fs::path out(raw);
    CoTaskMemFree(raw);
    return out;
}

} // namespace

fs::path userProfile()
{
    return knownFolder(FOLDERID_Profile);
}

fs::path localAppData()
{
    return knownFolder(FOLDERID_LocalAppData);
}

fs::path claudeHome()
{
    const fs::path profile = userProfile();
    return profile.empty() ? fs::path() : profile / ".claude";
}

std::string readShared(const fs::path& file, std::int64_t maxBytes)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        return {};
    }

    const std::int64_t from = size.QuadPart > maxBytes ? size.QuadPart - maxBytes : 0;
    LARGE_INTEGER      seek{};
    seek.QuadPart = from;
    if (from > 0 && !SetFilePointerEx(h, seek, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return {};
    }

    const std::size_t want = static_cast<std::size_t>(size.QuadPart - from);
    std::string       text(want, '\0');

    std::size_t have = 0;
    while (have < want) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(want - have, 1u << 20));
        DWORD       got   = 0;
        if (!ReadFile(h, text.data() + have, chunk, &got, nullptr) || got == 0)
            break;
        have += got;
    }
    CloseHandle(h);
    text.resize(have);

    // Starting partway in lands mid-record. That first fragment is not a line.
    if (from > 0) {
        const std::size_t nl = text.find('\n');
        text = (nl == std::string::npos) ? std::string() : text.substr(nl + 1);
    }
    return text;
}

std::string readSharedHead(const fs::path& file, std::int64_t maxBytes)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        return {};
    }

    const std::size_t want =
        static_cast<std::size_t>(std::min<std::int64_t>(size.QuadPart, maxBytes));
    std::string text(want, '\0');

    std::size_t have = 0;
    while (have < want) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(want - have, 1u << 20));
        DWORD       got   = 0;
        if (!ReadFile(h, text.data() + have, chunk, &got, nullptr) || got == 0)
            break;
        have += got;
    }
    CloseHandle(h);
    text.resize(have);
    return text;
}

bool writerHoldsOpen(const fs::path& file)
{
    // Share mode zero is the whole test. GENERIC_READ rather than write access
    // so this never needs permission it would not otherwise have, and never
    // truncates or touches the file if the open does succeed.
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return false;
    }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

std::int64_t ticksToUnixMs(unsigned long long ticks)
{
    if (ticks < static_cast<unsigned long long>(kFileTimeUnixEpoch))
        return 0;
    return (static_cast<std::int64_t>(ticks) - kFileTimeUnixEpoch) / 10000;
}

std::int64_t fileWriteTimeMs(const fs::path& file)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data))
        return 0;

    ULARGE_INTEGER t{};
    t.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    t.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return ticksToUnixMs(t.QuadPart);
}

std::string projectSlug(const fs::path& cwd)
{
    std::string s = narrow(cwd.wstring());
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/'))
        s.pop_back();

    for (char& c : s) {
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                        || (c >= '0' && c <= '9');
        if (!alnum)
            c = '-';
    }
    return s;
}

std::int64_t iso8601Ms(const std::string& text)
{
    // "2026-09-17T18:18:53.979Z". The fractional part and the zone are both
    // optional here; everything up to the seconds is not.
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, milli = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute,
                    &second)
        != 6)
        return 0;

    if (const std::size_t dot = text.find('.'); dot != std::string::npos) {
        // Take exactly three digits, padding a shorter fraction rather than
        // reading it as fewer milliseconds than it is.
        int scale = 100;
        for (std::size_t i = dot + 1; i < text.size() && scale > 0; ++i) {
            if (text[i] < '0' || text[i] > '9')
                break;
            milli += (text[i] - '0') * scale;
            scale /= 10;
        }
    }

    SYSTEMTIME st{};
    st.wYear         = static_cast<WORD>(year);
    st.wMonth        = static_cast<WORD>(month);
    st.wDay          = static_cast<WORD>(day);
    st.wHour         = static_cast<WORD>(hour);
    st.wMinute       = static_cast<WORD>(minute);
    st.wSecond       = static_cast<WORD>(second);
    st.wMilliseconds = static_cast<WORD>(milli);

    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft))
        return 0;

    ULARGE_INTEGER t{};
    t.LowPart  = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return ticksToUnixMs(t.QuadPart);
}

// ------------------------------------------------------------------- fields

std::string stringField(const json::Value* object, const char* key)
{
    if (!object)
        return {};
    const json::Value* v = object->find(key);
    return (v && v->type == json::Value::Type::String) ? v->string : std::string();
}

std::string anyString(const json::Value* object, const char* key)
{
    if (!object)
        return {};
    const json::Value* v = object->find(key);
    if (!v || v->isNull())
        return {};
    if (v->type == json::Value::Type::String)
        return v->string;
    return json::dump(*v);
}

std::int64_t intField(const json::Value* object, const char* key)
{
    if (!object)
        return 0;
    const json::Value* v = object->find(key);
    if (!v || v->type != json::Value::Type::Number)
        return 0;
    return v->isInteger ? v->integer : static_cast<std::int64_t>(v->number);
}

unsigned long long ticksField(const json::Value* object, const char* key)
{
    if (!object)
        return 0;
    const json::Value* v = object->find(key);
    if (!v)
        return 0;

    if (v->type == json::Value::Type::String) {
        char*                    end = nullptr;
        const unsigned long long n   = std::strtoull(v->string.c_str(), &end, 10);
        return (end && *end == '\0') ? n : 0;
    }
    if (v->type == json::Value::Type::Number && v->isInteger && v->integer > 0)
        return static_cast<unsigned long long>(v->integer);
    return 0;
}

// ------------------------------------------------------------- still running

bool processStillOurs(unsigned long pid, unsigned long long recordedTicks,
                      std::int64_t startedAtMs)
{
    if (pid == 0)
        return false;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return false;

    FILETIME created{}, exited{}, kernel{}, user{};
    const bool got = GetProcessTimes(proc, &created, &exited, &kernel, &user) != 0;
    CloseHandle(proc);

    // The handle opened, so something is running under that pid. Without a
    // creation time there is nothing left to check it against.
    if (!got)
        return true;

    ULARGE_INTEGER createdTicks{};
    createdTicks.LowPart  = created.dwLowDateTime;
    createdTicks.HighPart = created.dwHighDateTime;

    if (recordedTicks != 0)
        return createdTicks.QuadPart == recordedTicks;

    // No recorded ticks, so fall back on when the record says it started. A pid
    // reissued after that process exited belongs to one created later than the
    // record was written. The slack covers the second or so between the process
    // starting and Claude Code writing the file.
    if (startedAtMs > 0) {
        constexpr std::int64_t kSlackMs = 60 * 1000;
        return ticksToUnixMs(createdTicks.QuadPart) <= startedAtMs + kSlackMs;
    }
    return true;
}

} // namespace pm::gui::cf
