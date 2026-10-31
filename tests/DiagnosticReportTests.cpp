#include "diagnostics/DiagnosticReport.h"
#include <fstream>
#include <cstdio>
#include <map>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace llcv::diagnostics;
static void Check(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
static void TestDevicePrivacy() {
    const std::wstring native =
        L"\\\\?\\usb#vid_0fd9&pid_006a#PRIVATE_SERIAL_ABC#{01234567-89ab-cdef-0123-456789abcdef}\\global";
    ReportRequest request;
    request.deviceIds = {L"@device:pnp:" + native,
                         L"{0.0.0.00000000}.{12345678-1234-1234-abcd-123456789abc}"};
    ReportDevicePrivacy privacy(request.deviceIds);
    request.redactions.insert(request.redactions.end(), privacy.Rules().begin(),
                              privacy.Rules().end());
    const auto text = privacy.Apply(report_detail::Redact(
        L"selected=@device:pnp:" + native + L"\npath=" + native +
            L"\nold=\\\\?\\usb#vid_1234&pid_5678#OLD_PRIVATE_SERIAL#{01234567-89ab-cdef-0123-456789abcdef}" +
            L"\ninstance=USB\\VID_1234&PID_5678\\OLD_PRIVATE_SERIAL\nendpoint={0.0.1.00000000}.{abcdef01-1234-1234-abcd-123456789abc}" +
            L"\nsubtype={3231564E-0000-0010-8000-00AA00389B71}",
        request));
    Check(text.find(L"PRIVATE_SERIAL") == std::wstring::npos &&
              text.find(L"abcdef01") == std::wstring::npos,
          "known and historical USB/endpoint unique identities are masked");
    Check(text.find(L"selected=<DEVICE_1>") != std::wstring::npos &&
              text.find(L"path=<DEVICE_1>") != std::wstring::npos,
          "moniker and symbolic-path variants share a report-local alias");
    Check(text.find(L"vid_1234&pid_5678") != std::wstring::npos &&
              text.find(L"{3231564E-0000-0010-8000-00AA00389B71}") != std::wstring::npos,
          "VID/PID, interface classes and media-subtype GUIDs needed for diagnosis remain intact");
    Check(privacy.Apply(L"USB\\VID_1234&PID_5678\\OLD_PRIVATE_SERIAL").find(L"<DEVICE_3>") !=
              std::wstring::npos,
          "device aliases stay consistent across multiple files in one report");
    const std::wstring truncated =
        L"USB\\VID_1234&PID_5678\nfile=C:\\diagnostic\\file.txt\n"
        L"\\\\?\\usb#vid_1234&pid_5678\nother#detail#{01234567-89ab-cdef-0123-456789abcdef}";
    Check(privacy.Apply(truncated) == truncated,
          "truncated device IDs cannot consume unrelated diagnostic text on later lines");
    request.redactions = {{L"C:\\Users\\ТЕСТ", L"<USER>"}};
    Check(report_detail::Redact(L"c:/users/тест/log", request) == L"<USER>/log",
          "Unicode Windows paths are masked without locale-dependent case matching");
}
static void Write(const fs::path& file, const std::string& data) {
    std::ofstream out(file, std::ios::binary);
    out.write(data.data(), data.size());
    Check(!!out, "write test fixture");
}
static std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
static uint32_t Number(const std::string& data, size_t offset, int size) {
    Check(offset + size <= data.size(), "ZIP field in bounds");
    uint32_t result = 0;
    for (int i = 0; i < size; ++i)
        result |= uint32_t(static_cast<unsigned char>(data[offset + i])) << (8 * i);
    return result;
}
static std::map<std::string, std::string> Extract(const std::string& zip) {
    std::map<std::string, std::string> files;
    size_t offset = 0;
    while (Number(zip, offset, 4) == 0x04034b50) {
        Check(Number(zip, offset + 8, 2) == 0, "standard STORE compression");
        const auto size = Number(zip, offset + 18, 4), nameSize = Number(zip, offset + 26, 2),
                   extra = Number(zip, offset + 28, 2);
        const auto begin = offset + 30 + nameSize + extra;
        Check(begin + size <= zip.size(), "ZIP entry bytes in bounds");
        const auto name = zip.substr(offset + 30, nameSize), bytes = zip.substr(begin, size);
        Check(report_detail::Crc(bytes) == Number(zip, offset + 14, 4), "ZIP data CRC");
        Check(files.emplace(name, bytes).second, "unique archive names");
        offset = begin + size;
    }
    Check(Number(zip, offset, 4) == 0x02014b50, "central directory present");
    Check(Number(zip, zip.size() - 22, 4) == 0x06054b50, "complete end record present");
    Check(Number(zip, zip.size() - 12, 2) == files.size(),
          "end record entry count matches local entries");
    return files;
}
static ReportResult Fixture(const fs::path& root) {
    fs::create_directories(root / L"logs");
    const std::wstring settings =
        L"\xfeff[Settings]\r\nName=한글\r\nPath=C:\\Users\\PrivateName\\file.ini\r\n";
    Write(root / L"settings.ini",
          std::string(reinterpret_cast<const char*>(settings.data()), settings.size() * 2));
    Write(root / L"recording-prototype.ini",
          "[Recording]\r\nFolder=D:\\PrivateRecordings\r\nCodec=1\r\n");
    for (int i = 0; i < 7; ++i) {
        const auto path =
            root / L"logs" / (L"LowLatencyCapture_20261031_12000" + std::to_wstring(i) + L".log");
        Write(path, "[capture] C:/Users/PrivateName/log " + std::to_string(i) + "\r\n");
        fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(7 - i));
    }
    Write(root / L"logs" / L"private.txt", "DO NOT INCLUDE");
    Write(root / L"video.mp4", "DO NOT INCLUDE");
    ReportRequest request{
        root.wstring(),
        L"LLCV test\r\nStartup stage: connect graph\r\nC:\\Users\\PrivateName\\config",
        {{L"C:\\Users\\PrivateName", L"<USER>"},
         {L"D:\\PrivateRecordings", L"<RECORDING_FOLDER>"}}};
    request.deviceIds = {L"@device:pnp:private-test-device"};
    request.summary +=
        L"\r\nCurrent device: @device:pnp:private-test-device\r\nUnknown path: \\\\?\\usb#vid_1234&pid_5678#ZIP_PRIVATE_SERIAL#{01234567-89ab-cdef-0123-456789abcdef}";
    std::atomic<bool> stop = false;
    const auto originalSettings = Read(root / L"settings.ini"),
               originalRecording = Read(root / L"recording-prototype.ini");
    const auto priority = GetThreadPriority(GetCurrentThread());
    const auto result = CreateDiagnosticReport(request, stop);
    Check(GetThreadPriority(GetCurrentThread()) == priority, "export restores thread priority");
    Check(SUCCEEDED(result.status) && result.logs == 5 && result.settings == 2,
          "bounded recent logs and both settings bundled");
    Check(Read(root / L"settings.ini") == originalSettings &&
              Read(root / L"recording-prototype.ini") == originalRecording,
          "source settings unchanged");
    const auto zip = Read(result.path);
    Check(zip.size() < 12 * 1024 * 1024, "bounded ZIP size");
    const auto files = Extract(zip);
    Check(files.size() == 9, "only approved entries are included");
    Check(files.at("settings.ini").find(report_detail::Utf8(L"한글")) != std::string::npos,
          "UTF-16 settings decoded as UTF-8");
    Check(files.at("recording.ini").find("<RECORDING_FOLDER>") != std::string::npos,
          "recording folder masked");
    Check(files.at("logs/log-1.txt").find("log 6") != std::string::npos &&
              files.at("logs/log-5.txt").find("log 2") != std::string::npos,
          "five newest logs in chronological priority");
    for (const auto& [name, bytes] : files) {
        Check(bytes.find("PrivateName") == std::string::npos &&
                  bytes.find("DO NOT INCLUDE") == std::string::npos &&
                  bytes.find("private-test-device") == std::string::npos &&
                  bytes.find("ZIP_PRIVATE_SERIAL") == std::string::npos,
              "profile paths masked and unrelated files excluded");
        Check(!name.starts_with("/") && name.find("..") == std::string::npos,
              "safe fixed ZIP entry names");
    }
    Check(fs::exists(result.path) && !fs::exists(result.path + L".partial"),
          "only finalized ZIP is exposed");
    stop = true;
    const auto cancelled = CreateDiagnosticReport(request, stop);
    Check(cancelled.status == HRESULT_FROM_WIN32(ERROR_CANCELLED) && cancelled.path.empty(),
          "pre-cancelled request does no work");
    return result;
}
int wmain(int argc, wchar_t** argv) {
    TestDevicePrivacy();
    PWSTR local = nullptr;
    Check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)),
          "resolve protected application root for negative test");
    const auto protectedRoot = std::wstring(local) + L"\\LowLatencyCaptureViewer";
    CoTaskMemFree(local);
    std::atomic<bool> protectStop = false;
    Check(CreateDiagnosticReport({protectedRoot, L"must not read real settings", {}}, protectStop)
                  .status == E_ACCESSDENIED,
          "test exports cannot read or write the real application data directory");
    Check(report_detail::Crc("123456789") == 0xcbf43926, "standard ZIP CRC vector");
    const auto partial = report_detail::Decode("complete log line\r\n\xe2\x82");
    Check(partial.starts_with(L"complete log line\r\n") &&
              partial.find(L"incomplete UTF-8") != std::wstring::npos,
          "a live log's incomplete last character does not discard all earlier diagnostics");
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fixture") {
        const auto result = Fixture(argv[2]);
        std::wprintf(L"%s\n", result.path.c_str());
        return 0;
    }
    wchar_t temp[32768]{};
    Check(GetTempPathW(32768, temp) > 0, "temporary directory available");
    const fs::path root =
        fs::path(temp) / (L"LLCV-report-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(root), "unique test directory created");
    const auto first = Fixture(root), second = Fixture(root);
    Check(first.path != second.path && fs::exists(first.path),
          "repeated requests never overwrite a report");
    std::atomic<bool> stop = false;
    const auto missing =
        CreateDiagnosticReport({(root / L"missing").wstring(), L"no logs", {}}, stop);
    Check(SUCCEEDED(missing.status) && missing.logs == 0 && missing.settings == 0,
          "missing logs/settings reported without fake data");
    fs::create_directories(root / L"oversized" / L"logs");
    Write(root / L"oversized" / L"settings.ini", std::string(256 * 1024 + 1, 's'));
    Write(root / L"oversized" / L"logs" / L"LowLatencyCapture_20261031_123456.log",
          std::string(3 * 1024 * 1024, 'l'));
    const auto oversized =
        CreateDiagnosticReport({(root / L"oversized").wstring(), L"bounded reads", {}}, stop);
    Check(SUCCEEDED(oversized.status) && oversized.settings == 0 && oversized.logs == 0,
          "oversized sources are omitted before reading their contents");
    const auto oversizedEntries = Extract(Read(oversized.path));
    Check(oversizedEntries.size() == 2 &&
              oversizedEntries.at("report.txt").find("too large") != std::string::npos,
          "omissions are explicit and review instructions remain present");
    Write(root / L"blocked", "file not a folder");
    const auto failure =
        CreateDiagnosticReport({(root / L"blocked").wstring(), L"write failure", {}}, stop);
    Check(FAILED(failure.status) && failure.path.empty() &&
              Read(root / L"blocked") == "file not a folder",
          "write failure preserves existing data");
    fs::remove_all(root); // Exact, uniquely created test fixture, never a user profile.
    std::puts(
        "PASS: bounded ZIP, privacy masking, Unicode, cancellation, safe failures, unchanged source settings.");
}
