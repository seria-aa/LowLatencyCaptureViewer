#pragma once

// Private diagnostic exit handling, shared with its isolated regression test.
#include <cstdio>
#include <filesystem>
#include <string>

namespace llcv::upscaling::diagnostic {
inline constexpr int ReportSaveFailure = 4;
inline constexpr const char* ReportSaveError =
    "LLCV: could not save the upscaling diagnostic report; check the executable folder's write permissions.\n";

template<class SaveReport, class Notify>
int FinishReport(int viewerResult, bool noDialog, SaveReport saveReport,
                 Notify notify, FILE* errors = stderr) {
    try {
        const std::filesystem::path path = saveReport();
        if (!noDialog) {
            notify(L"진단 결과를 저장했습니다. 실제 앱 설정은 사용하지 않았습니다.\n\n" + path.wstring(), false);
        }
    } catch (...) {
        if (noDialog) {
            std::fputs(ReportSaveError, errors);
            std::fflush(errors);
        } else {
            notify(L"진단 결과 파일을 저장하지 못했습니다. 실행 파일 폴더의 쓰기 권한을 확인하세요.", true);
        }
        // Do not hide a prior viewer failure, or report a successful exit when
        // the only requested diagnostic artifact could not be written.
        return viewerResult != 0 ? viewerResult : ReportSaveFailure;
    }
    return viewerResult;
}
}
