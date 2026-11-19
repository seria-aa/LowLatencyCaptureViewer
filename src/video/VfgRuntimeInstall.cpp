#include "video/VfgRuntimeInstall.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <objbase.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace llcv::vfg_runtime {
namespace {
namespace fs = std::filesystem;
constexpr auto Version = L"0.2.0.0-win-x64";
constexpr auto PackageHash = "5AAF6A42BC6B6DBBF52FCB714194C994A6893CBBF7ADA38BC2165A1F83E4A6FC";
struct Component { const wchar_t* name; const char* sha; };
constexpr Component files[] = {
    {L"cudart64_12.dll", "663B5FE65AA6C654B8B085A879F49DEB19F2CB033B8F2FF8DFFD06CDEFE0D1DB"},
    {L"NVCVImage.dll", "689A64586C804468127647B7ED24ACC36A9A9780502FFE95AD11AA4FE3286A39"},
    {L"nvngxruntime.dll", "D2DBD1993000A2EED2C7AD8CD417FFC738EA180003DAACF190E737121869D637"},
    {L"NVVideoEffects.dll", "C6B042FC57BDB6BEA88C9C52CB951582377D3F1643ACF52A52D197195A1EA8F4"},
    {L"nvVFXVideoFrameGeneration.dll", "270CF4FFF9329908F9770D306CB51B910EE04323AF5520CD02A98A2E22ABA795"}
};
struct Service {
    fs::path root;
    std::atomic<Stage> stage{Stage::Idle};
    std::atomic<unsigned> progress{0};
};
// The explicitly started worker owns no HWND and may finish after Settings
// closes. Process teardown is allowed to stop a transfer; only renamed,
// complete installations can be selected by the renderer.
Service& State() { static auto* s = new Service; return *s; }
void Check(bool okay, const char* what) { if (!okay) throw std::runtime_error(what); }
struct Internet { HINTERNET h; ~Internet() { if (h) WinHttpCloseHandle(h); } };
struct Handle { HANDLE h; ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct Work {
    fs::path path;
    ~Work() { if (!path.empty()) { std::error_code ec; fs::remove_all(path, ec); } }
};
struct Sha {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    ~Sha() { if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void Begin() {
        Check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0, "SHA provider");
        DWORD size = 0, copied = 0;
        Check(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &copied, 0) == 0, "SHA object length");
        object.resize(size);
        Check(BCryptCreateHash(alg, &hash, object.data(), size, nullptr, 0, 0) == 0, "SHA initialize");
    }
};
std::string Hash(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Check(in.is_open(), "open component");
    Sha sha;
    sha.Begin();
    std::vector<UCHAR> buffer(256 * 1024);
    while (in.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || in.gcount()) {
        Check(BCryptHashData(sha.hash, buffer.data(), static_cast<ULONG>(in.gcount()), 0) == 0, "SHA input");
    }
    Check(in.eof() && !in.bad(), "component read");
    std::array<UCHAR, 32> digest{};
    Check(BCryptFinishHash(sha.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0, "SHA finish");
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
bool PlainPath(const fs::path& path, bool directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}
bool MissingPath(const fs::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
bool HasFiles(const fs::path& path) {
    if (!PlainPath(path, true)) return false;
    for (const auto& f : files) if (!PlainPath(path / f.name, false)) return false;
    return true;
}
bool VerifyFiles(const fs::path& path) {
    if (!HasFiles(path)) return false;
    size_t entries = 0;
    for (const auto& item : fs::directory_iterator(path)) {
        (void)item;
        ++entries;
    }
    if (entries != std::size(files)) return false;
    for (const auto& f : files) {
        if (Hash(path / f.name) == f.sha) continue;
        std::fwprintf(stderr, L"[vfg-install] SHA-256 mismatch for %ls\n", f.name);
        return false;
    }
    return true;
}
fs::path CreateStage(const fs::path& store) {
    GUID id{};
    wchar_t guid[40]{};
    Check(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, guid, ARRAYSIZE(guid)) > 0, "stage identity");
    const auto stage = store / (std::wstring(L"stage-") + guid);
    Check(fs::create_directory(stage), "create unique stage");
    return stage;
}
Handle LockStore(const fs::path& store) {
    fs::create_directories(store);
    Check(PlainPath(store, true), "unsafe VFG store");
    const auto lock = store / L"install.lock";
    const auto handle = CreateFileW(lock.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "another download uses this component store");
    return Handle{handle};
}
void RemoveInterruptedStages(const fs::path& store) {
    const auto root = fs::canonical(store);
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().wstring();
        if (!name.starts_with(L"stage-") || name.size() != 44 || !PlainPath(entry.path(), true)) continue;
        GUID id{};
        wchar_t guid[40]{};
        if (FAILED(CLSIDFromString(name.c_str() + 6, &id)) ||
            !StringFromGUID2(id, guid, ARRAYSIZE(guid)) || name.substr(6) != guid) continue;
        const auto resolved = fs::canonical(entry.path());
        if (resolved.parent_path() != root || resolved.filename() != entry.path().filename()) continue;
        std::error_code ignored;
        fs::remove_all(resolved, ignored);
    }
}
std::wstring Quoted(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }
void Extract(const fs::path& wheel, const fs::path& destination) {
    std::array<wchar_t, 32768> system{};
    const auto n = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    Check(n > 0 && n < system.size(), "Windows system path");
    const auto tar = fs::path(system.data()) / L"tar.exe";
    std::wstring command = Quoted(tar) + L" -xf " + Quoted(wheel) + L" -C " + Quoted(destination);
    // Only these entries from the exact pinned archive, never a whole-wheel extract.
    for (const auto& f : files) command += L" nvvfx/libs/" + std::wstring(f.name);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    Check(CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, destination.c_str(), &si, &pi) != FALSE, "Windows archive extractor unavailable");
    const Handle process{pi.hProcess}, thread{pi.hThread};
    if (WaitForSingleObject(process.h, 120000) != WAIT_OBJECT_0) {
        TerminateProcess(process.h, 1);
        WaitForSingleObject(process.h, 5000);
        throw std::runtime_error("archive extraction timed out");
    }
    DWORD code = 1;
    Check(GetExitCodeProcess(process.h, &code) && code == 0, "archive extraction failed");
}
void VerifyArchive(const fs::path& wheel) {
    Check(PlainPath(wheel, false) && fs::file_size(wheel) == PackageBytes &&
          Hash(wheel) == PackageHash, "NVIDIA archive size or SHA-256 mismatch");
}
void Activate(const fs::path& wheel, const fs::path& work, const fs::path& store) {
    Extract(wheel, work);
    const auto payload = work / L"nvvfx" / L"libs";
    Check(VerifyFiles(payload), "extracted VFG component set did not verify");
    const auto final = store / Version;
    if (!MissingPath(final)) {
        Check(!VerifyFiles(final), "valid installation already exists");
        // Preserve an incomplete installation in the private store for diagnosis.
        // Restoration on rename failure leaves it as it was before retry.
        const auto backup = store / (L"invalid-" + work.filename().wstring());
        fs::rename(final, backup);
        try { fs::rename(payload, final); }
        catch (...) {
            std::error_code ignored;
            fs::rename(backup, final, ignored);
            throw;
        }
    } else {
        fs::rename(payload, final);
    }
}
void Download(const fs::path& wheel, Service& service, bool interruptOnceForTest) {
    const Internet session{WinHttpOpen(L"LLCV/2.3.2 VFG Installer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    Check(session.h && WinHttpSetTimeouts(session.h, 10000, 15000, 15000, 15000), "NVIDIA connection");
    DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    Check(WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls)), "NVIDIA TLS policy");
    const Internet connection{WinHttpConnect(session.h, PackageHost, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    Check(connection.h != nullptr, "NVIDIA host");
    std::ofstream out(wheel, std::ios::binary | std::ios::trunc);
    Check(out.is_open(), "open download destination");
    const auto deadline = GetTickCount64() + 20 * 60 * 1000;
    uint64_t received = 0;
    bool interruptedForTest = false;
    std::vector<char> buffer(128 * 1024);
    for (unsigned attempt = 0; received < PackageBytes && attempt < 3; ++attempt) {
        const Internet request{WinHttpOpenRequest(connection.h, L"GET", PackagePath, nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        Check(request.h != nullptr, "NVIDIA request");
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        Check(WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) &&
              WinHttpSetOption(request.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &auth, sizeof(auth)), "NVIDIA request policy");
        const auto offset = received;
        const auto range = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n" : std::wstring{};
        if (!WinHttpSendRequest(request.h, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
                                static_cast<DWORD>(range.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.h, nullptr)) continue;
        DWORD status = 0, bytes = sizeof(status);
        Check(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX) &&
              status == (offset ? 206u : 200u), "NVIDIA server did not honor the requested download");
        if (offset) {
            const auto expected = L"bytes " + std::to_wstring(offset) + L"-" +
                std::to_wstring(PackageBytes - 1) + L"/" + std::to_wstring(PackageBytes);
            wchar_t actual[96]{};
            bytes = sizeof(actual);
            Check(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
                                      actual, &bytes, WINHTTP_NO_HEADER_INDEX) &&
                  expected == actual, "NVIDIA response range mismatch");
            std::fprintf(stderr, "[vfg-install] resumed at %llu bytes\n", static_cast<unsigned long long>(offset));
        }
        while (received < PackageBytes) {
            Check(GetTickCount64() < deadline, "NVIDIA transfer timed out");
            DWORD n = 0;
            if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &n)) {
                const auto error = GetLastError();
                std::fprintf(stderr, "[vfg-install] WinHTTP read error %lu at %llu bytes\n", error,
                             static_cast<unsigned long long>(received));
                break; // Retry from the exact last byte written, not the beginning.
            }
            if (!n) break;
            Check(n <= PackageBytes - received, "NVIDIA transfer exceeds the pinned size");
            out.write(buffer.data(), n);
            Check(bool(out), "cannot write download");
            received += n;
            service.progress.store(static_cast<unsigned>(received * 100 / PackageBytes));
            if (interruptOnceForTest && !interruptedForTest && received >= 2 * 1024 * 1024 &&
                received < PackageBytes) {
                interruptedForTest = true;
                break;
            }
        }
    }
    out.close();
    Check(received == PackageBytes && bool(out), "download size mismatch");
    Check(!interruptOnceForTest || interruptedForTest, "download-resume fixture was not exercised");
}
void PublishReady(Service& service) noexcept {
    service.progress.store(100);
    service.stage.store(Stage::Ready);
}
void PublishFailure(Service& service) noexcept {
    service.progress.store(0);
    service.stage.store(Stage::Failed);
}
} // namespace

void Initialize(fs::path appData) {
    auto& s = State();
    s.root = std::move(appData) / L"vfg-runtime";
    if (MissingPath(s.root)) return;
    if (!PlainPath(s.root, true)) { PublishFailure(s); return; }
    const auto final = s.root / Version;
    if (MissingPath(final)) return; // A new user has nothing to repair.
    // Even a complete-looking directory is untrusted. Hash it off the UI and
    // capture threads and do not advertise availability until verification ends.
    s.stage.store(Stage::Verifying);
    try {
        std::thread([&s, final] {
            // Opening an existing installation must not compete for normal
            // CPU and disk priority with playback that does not use VFG.
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            try {
                if (VerifyFiles(final)) { PublishReady(s); return; }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[vfg-install] %s\n", e.what());
            } catch (...) {}
            PublishFailure(s);
        }).detach();
    } catch (...) {
        PublishFailure(s);
    }
}
Status GetStatus() noexcept { return {State().stage.load(), State().progress.load()}; }
bool StartDownload(bool interruptOnceForTest) noexcept {
    auto& s = State();
    auto old = s.stage.load();
    do { if (!CanStartDownload(old) || s.root.empty()) return false; }
    while (!s.stage.compare_exchange_weak(old, Stage::Downloading));
    s.progress.store(0);
    try {
        std::thread([&s, interruptOnceForTest] {
            try {
                {
                    const auto lock = LockStore(s.root);
                    RemoveInterruptedStages(s.root);
                    const auto final = s.root / Version;
                    if (!VerifyFiles(final)) {
                        Check(fs::space(s.root).available >= 700ull * 1024 * 1024, "insufficient disk space for VFG");
                        const Work work{CreateStage(s.root)};
                        const auto wheel = work.path / L"nvidia-vfx.whl";
                        Download(wheel, s, interruptOnceForTest);
                        s.stage.store(Stage::Verifying);
                        VerifyArchive(wheel);
                        s.stage.store(Stage::Installing);
                        Activate(wheel, work.path, s.root);
                    }
                }
                PublishReady(s);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[vfg-install] %s\n", e.what());
                PublishFailure(s);
            } catch (...) { PublishFailure(s); }
        }).detach();
        return true;
    } catch (...) {
        PublishFailure(s);
        return false;
    }
}
fs::path VerifiedDirectory() {
    const auto& s = State();
    // Ready is terminal for this process; neither the root nor its selected
    // version changes after successful verification.
    return s.stage.load() == Stage::Ready ? s.root / Version : fs::path{};
}
void InstallLocalArchiveForTest(const fs::path& archive, const fs::path& store) {
    const auto lock = LockStore(store);
    if (VerifyFiles(store / Version)) return;
    VerifyArchive(archive);
    const Work work{CreateStage(store)};
    Activate(archive, work.path, store);
}
} // namespace llcv::vfg_runtime
