#include "update/UpdateChecker.h"
#include "update/UpdateTransport.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <memory>
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

std::wstring ReleaseNotesUrl(const std::wstring& version) {
    constexpr auto releases=L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases";
    size_t begin=(!version.empty() && (version[0]==L'v'||version[0]==L'V'))?1:0;
    size_t end=begin;
    for(unsigned part=0;part<3;++part) {
        const size_t first=end;
        while(end<version.size() && version[end]>=L'0' && version[end]<=L'9')++end;
        if(end==first || end-first>6)return releases;
        if(part<2) {
            if(end>=version.size() || version[end]!=L'.')return releases;
            ++end;
        }
    }
    if(end<version.size() && version[end]!=L'-')return releases;
    return std::wstring(releases)+L"/tag/v"+version.substr(begin,end-begin);
}

namespace detail {
namespace {

class InternetHandle {
public:
    InternetHandle(HINTERNET value, decltype(&WinHttpCloseHandle) close)
        : value_(value), close_(close) {}
    ~InternetHandle() { if (value_) close_(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET Get() const noexcept { return value_; }
private:
    HINTERNET value_ = nullptr;
    decltype(&WinHttpCloseHandle) close_;
};

struct CompletionState {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<DWORD> completed{0};
    std::atomic<DWORD> readBytes{0};
    // WinHTTP may still write here after cancellation returns to the UI.
    // This buffer and event survive until the request's final closing callback.
    std::array<char, 8192> buffer{};

    ~CompletionState() { if (event) CloseHandle(event); }
    void Prepare() {
        ResetEvent(event);
        completed.store(0, std::memory_order_relaxed);
        readBytes.store(0, std::memory_order_relaxed);
    }
};

// The owner and request callback each hold a reference. HANDLE_CLOSING is the
// last notification and cannot overlap other callbacks for the same request
// (WinHTTP contract); only then is the request's binding released.
using CallbackBinding = std::shared_ptr<CompletionState>;

void CALLBACK OnRequestStatus(HINTERNET, DWORD_PTR context, DWORD status,
                              void* information, DWORD length) {
    auto* binding = reinterpret_cast<CallbackBinding*>(context);
    if (!binding) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        delete binding;
        return;
    }
    auto& state = **binding;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        if (length > state.buffer.size() || (length && information != state.buffer.data())) {
            status = WINHTTP_CALLBACK_STATUS_REQUEST_ERROR;
        } else state.readBytes.store(length, std::memory_order_relaxed);
        break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        break;
    default:
        return;
    }
    state.completed.store(status, std::memory_order_release);
    SetEvent(state.event);
}

using Clock = std::chrono::steady_clock;

bool WaitForCompletion(CompletionState& state, DWORD expected,
                       const std::atomic<bool>* stop, Clock::time_point deadline) {
    while (!(stop && stop->load(std::memory_order_acquire)) && Clock::now() < deadline) {
        const DWORD status = state.completed.load(std::memory_order_acquire);
        if (status) return status == expected;
        const DWORD wait = WaitForSingleObject(state.event, 25);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) return false;
    }
    return false;
}

} // namespace

bool FetchReleaseJson(const wchar_t* userAgent, std::string& json,
                      const std::atomic<bool>* stop, const WinHttpApi& api,
                      DWORD deadlineMilliseconds) {
    json.clear();
    const auto cancelled = [stop]() { return stop && stop->load(std::memory_order_acquire); };
    if (!userAgent || !*userAgent || cancelled()) return false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(deadlineMilliseconds);
    InternetHandle session(api.open(userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC), api.close);
    if (!session.Get() || cancelled() ||
        !api.setTimeouts(session.Get(), 2500, 2500, 2500, 2500)) return false;
    InternetHandle connection(api.connect(session.Get(), L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0), api.close);
    if (!connection.Get() || cancelled()) return false;
    InternetHandle request(api.openRequest(connection.Get(), L"GET",
        L"/repos/seria-aa/LowLatencyCaptureViewer/releases/latest", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), api.close);
    if (!request.Get() || cancelled() || !api.addHeaders(request.Get(),
        L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L),
        WINHTTP_ADDREQ_FLAG_ADD)) return false;

    auto state = std::make_shared<CompletionState>();
    if (!state->event) return false;
    auto binding = std::make_unique<CallbackBinding>(state);
    const DWORD flags = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
        WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
        WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES;
    if (api.setCallback(request.Get(), &OnRequestStatus, flags, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK) return false;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(binding.get());
    if (!api.setOption(request.Get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        return false;
    binding.release(); // request's final HANDLE_CLOSING callback owns this reference

    state->Prepare();
    if (cancelled() || !api.send(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, context) ||
        !WaitForCompletion(*state, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, stop, deadline))
        return false;
    state->Prepare();
    if (cancelled() || !api.receive(request.Get(), nullptr) ||
        !WaitForCompletion(*state, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, stop, deadline))
        return false;
    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    if (cancelled() || !api.queryHeaders(request.Get(),
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) || statusCode != 200) return false;

    constexpr size_t kMaximumResponseBytes = 2 * 1024 * 1024;
    while (!cancelled()) {
        state->Prepare();
        if (!api.read(request.Get(), state->buffer.data(),
            static_cast<DWORD>(state->buffer.size()), nullptr) ||
            !WaitForCompletion(*state, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, stop, deadline))
            return false;
        const DWORD read = state->readBytes.load(std::memory_order_relaxed);
        if (!read) return !cancelled();
        if (read > kMaximumResponseBytes - json.size()) return false;
        json.append(state->buffer.data(), read);
    }
    return false;
}

} // namespace detail

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

bool FetchLatestRelease(const wchar_t* currentVersion, CheckResult& result,
                        const std::atomic<bool>* stop) {
    result = {};
    const auto cancelled = [stop]() {
        return stop && stop->load(std::memory_order_acquire);
    };
    if (!currentVersion || !*currentVersion || cancelled()) return false;

    std::wstring userAgent = L"LowLatencyCaptureViewer/";
    userAgent += currentVersion[0] == L'v' || currentVersion[0] == L'V'
        ? currentVersion + 1 : currentVersion;
    std::string json;
    const detail::WinHttpApi api;
    if (!detail::FetchReleaseJson(userAgent.c_str(), json, stop, api) || cancelled()) return false;

    return ParseLatestReleaseResponse(json, currentVersion, result);
}

bool ParseLatestReleaseResponse(const std::string& json,
                                const wchar_t* currentVersion,
                                CheckResult& result) {
    result = {};
    if (!currentVersion || !*currentVersion) return false;

    std::string tag;
    if (!ExtractJsonString(json, "tag_name", 0, tag)) return false;
    result.latestTag = Utf8ToWide(tag);
    if (result.latestTag.empty()) return false;
    result.success = true;
    result.newer = IsNewerReleaseTag(result.latestTag, currentVersion);
    if (!result.newer) {
        return true;
    }

    size_t cursor = 0;
    std::string assetUrl;
    constexpr wchar_t kOfficialAssetPrefix[] =
        L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/";
    while (ExtractJsonString(json, "browser_download_url", cursor,
                             assetUrl, &cursor)) {
        const std::wstring installerUrl = Utf8ToWide(assetUrl);
        if (installerUrl.rfind(kOfficialAssetPrefix, 0) == 0 &&
            installerUrl.ends_with(L"_Setup.exe")) {
            result.installerUrl = installerUrl;
            break;
        }
    }
    return true;
}

}  // namespace llcv::update
