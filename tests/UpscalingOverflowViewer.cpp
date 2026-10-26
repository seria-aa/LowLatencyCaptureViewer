// Private viewer entry point: full UI with isolated profiles and exit report.
// No entry point/source from this file is linked into the release application.
#define wWinMain OriginalViewerWinMain
#include "../src/main.cpp"
#undef wWinMain
#include <filesystem>
#include "UpscalingOverflowReport.h"

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
#ifdef LLCV_RECORDING
    const int probe=llcv::recording::RunEncoderProbeChild(command,[] {return llcv::recording::ProbeEncoders();});
    if(probe>=0)return probe;
#endif
    if(!llcv::test_profile::Enabled())return 3; // Fail closed if build isolation is lost.
    llcv::upscaling::SetOverflowSamplingEnabled(CommandLineOptionValue(L"--overflow-sampling")!=L"off");
    if(CommandLineOptionValue(L"--overflow-live")==L"yes")show=SW_SHOWNORMAL;
    const int result=OriginalViewerWinMain(instance,previous,command,show);
    return llcv::upscaling::diagnostic::FinishReport(result,
        CommandLineOptionValue(L"--overflow-no-dialog")==L"yes", [] {
        wchar_t executable[32768]{};const DWORD length=GetModuleFileNameW(nullptr,executable,32768);
        if(!length||length>=32768)throw std::runtime_error("executable path");
        const auto root=std::filesystem::path(executable).parent_path()/L"overflow-results";
        std::filesystem::create_directories(root);
        SYSTEMTIME time{};GetLocalTime(&time);wchar_t name[128]{};
        swprintf_s(name,L"overflow-%04u%02u%02u-%02u%02u%02u-%03u-%lu.txt",time.wYear,time.wMonth,time.wDay,
            time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetCurrentProcessId());
        const auto path=root/name;
        const auto text=llcv::upscaling::FormatOverflowReport(llcv::upscaling::ReadOverflowCounts());
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("report file");
        DWORD written=0;const bool saved=WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr)&&written==text.size();
        CloseHandle(file);if(!saved)throw std::runtime_error("report write");
        return path;
    }, [](const std::wstring& message, bool failure) {
        MessageBoxW(nullptr,message.c_str(),L"LLCV · 업스케일링 진단",
            MB_OK|(failure?MB_ICONWARNING:MB_ICONINFORMATION));
    });
}
