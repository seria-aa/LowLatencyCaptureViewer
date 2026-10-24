#pragma once
// Test binaries never resolve their default profiles to the real application
// directory, even when a fixture forgets to suppress saving. Normal runs retain
// the established paths. Diagnostic command-line runs opt into the same boundary.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <stdexcept>

namespace llcv::test_profile {
inline bool DiagnosticArgument(const wchar_t* argument) {
    return argument && (wcsncmp(argument,L"--smoke-test",12)==0 ||
        wcscmp(argument,L"--exclusive-probe")==0 ||
        wcscmp(argument,L"--exclusive-probe-all")==0);
}
// Defined once per executable: libraries cannot choose a different inline
// implementation based on their compile flags (ODR/COMDAT ambiguity).
bool Enabled();
inline const std::wstring& Directory() {
    static const std::wstring directory=[] {
        wchar_t temporary[32768]{};
        const DWORD count=GetTempPathW(32768,temporary);
        if(!count || count>=32768)throw std::runtime_error("No isolated test directory");
        const auto prefix=std::wstring(temporary)+L"LLCV-TestProfile-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        for(unsigned attempt=0;attempt<128;attempt++) {
            const auto path=prefix+L"-"+std::to_wstring(attempt);
            if(CreateDirectoryW(path.c_str(),nullptr))return path;
            if(GetLastError()!=ERROR_ALREADY_EXISTS)break;
        }
        // Never fall back to LocalAppData or the executable directory on failure.
        throw std::runtime_error("Cannot create isolated test directory");
    }();
    return directory;
}
inline std::wstring Canonical(const std::wstring& path) {
    wchar_t full[32768]{};const auto count=GetFullPathNameW(path.c_str(),32768,full,nullptr);
    return count && count<32768?std::wstring(full,count):std::wstring{};
}
inline bool Protected(const std::wstring& path) {
    if(!Enabled())return false;
    static const auto root=[] {
        PWSTR value=nullptr;std::wstring result;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&value)))
            result=Canonical(std::wstring(value)+L"\\LowLatencyCaptureViewer");
        CoTaskMemFree(value);return result;
    }();
    const auto full=Canonical(path);
    if(root.empty() || full.empty())return true;
    return _wcsicmp(full.c_str(),root.c_str())==0 ||
        (full.size()>root.size() && full[root.size()]==L'\\' &&
         _wcsnicmp(full.c_str(),root.c_str(),root.size())==0);
}
}
