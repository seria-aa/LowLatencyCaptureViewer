#include "update/UpdateChecker.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace llcv::update {
namespace {

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return result;
}

bool ExtractJsonString(const std::string& json, const char* key,
                       size_t searchFrom, std::string& value,
                       size_t* nextPosition = nullptr) {
    if (!key) return false;
    const std::string marker = std::string("\"") + key + "\"";
    const size_t markerPos = json.find(marker, searchFrom);
    if (markerPos == std::string::npos) return false;
    size_t cursor = json.find(':', markerPos + marker.size());
    if (cursor == std::string::npos) return false;
    ++cursor;
    while (cursor < json.size() &&
           (json[cursor] == ' ' || json[cursor] == '\t' ||
            json[cursor] == '\r' || json[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor >= json.size() || json[cursor] != '"') return false;
    ++cursor;
    value.clear();
    bool escaped = false;
    for (; cursor < json.size(); ++cursor) {
        const char ch = json[cursor];
        if (escaped) {
            // Release tags and GitHub asset URLs do not contain escaped
            // unicode, but preserve the common JSON escapes safely.
            switch (ch) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            if (nextPosition) *nextPosition = cursor + 1;
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

std::vector<int> ParseReleaseTag(const std::wstring& input) {
    std::vector<int> parts;
    size_t index = 0;
    while (index < input.size() &&
           (input[index] == L'v' || input[index] == L'V' ||
            input[index] == L' ')) {
        ++index;
    }
    while (index < input.size()) {
        while (index < input.size() && !iswdigit(input[index])) ++index;
        if (index >= input.size()) break;
        int value = 0;
        while (index < input.size() && iswdigit(input[index])) {
            value = (std::min)(value * 10 + (input[index] - L'0'), 1000000);
            ++index;
        }
        parts.push_back(value);
    }
    return parts;
}

}  // namespace

bool IsNewerReleaseTag(const std::wstring& latestTag,
                       const std::wstring& currentTag) {
    const auto latest = ParseReleaseTag(latestTag);
    const auto current = ParseReleaseTag(currentTag);
    const size_t count = (std::max)(latest.size(), current.size());
    for (size_t i = 0; i < count; ++i) {
        const int lhs = i < latest.size() ? latest[i] : 0;
        const int rhs = i < current.size() ? current[i] : 0;
        if (lhs != rhs) return lhs > rhs;
    }
    return false;
}

bool FetchLatestRelease(const wchar_t* currentVersion, CheckResult& result) {
    result = {};
    if (!currentVersion || !*currentVersion) return false;

    std::wstring userAgent = L"LowLatencyCaptureViewer/";
    userAgent += currentVersion[0] == L'v' || currentVersion[0] == L'V'
        ? currentVersion + 1 : currentVersion;
    HINTERNET session = WinHttpOpen(
        userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 2500, 2500, 2500, 2500);
    HINTERNET connection = WinHttpConnect(
        session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection, L"GET",
        L"/repos/seria-aa/LowLatencyCaptureViewer/releases/latest", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    WinHttpAddRequestHeaders(
        request, L"Accept: application/vnd.github+json\r\n",
        static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);
    const bool sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
        0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    if (!sent) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::string json;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) ||
            read == 0) {
            break;
        }
        chunk.resize(read);
        json += chunk;
        if (json.size() > 2 * 1024 * 1024) break;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    std::string tag;
    if (!ExtractJsonString(json, "tag_name", 0, tag)) return false;
    result.latestTag = Utf8ToWide(tag);
    result.success = !result.latestTag.empty();
    if (result.latestTag.empty() ||
        !IsNewerReleaseTag(result.latestTag, currentVersion)) {
        return true;
    }

    size_t cursor = 0;
    std::string assetUrl;
    while (ExtractJsonString(json, "browser_download_url", cursor,
                             assetUrl, &cursor)) {
        if (assetUrl.find("_Setup.exe") != std::string::npos) break;
        assetUrl.clear();
    }
    const std::wstring installerUrl = Utf8ToWide(assetUrl);
    constexpr wchar_t kOfficialAssetPrefix[] =
        L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/";
    if (installerUrl.empty() ||
        installerUrl.rfind(kOfficialAssetPrefix, 0) != 0) {
        return true;
    }
    result.installerUrl = installerUrl;
    result.newer = true;
    return true;
}

}  // namespace llcv::update
