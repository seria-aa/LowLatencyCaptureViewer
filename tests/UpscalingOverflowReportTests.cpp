#include "UpscalingOverflowReport.h"
#include <stdexcept>
#include <string>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    using namespace llcv::upscaling::diagnostic;
    FILE* output = nullptr;
    if (tmpfile_s(&output) != 0 || !output) return 2;
    try {
        int notices = 0;
        bool errorNotice = false;
        auto notify = [&](const std::wstring&, bool failure) { ++notices; errorNotice = failure; };
        auto failedSave = []() -> std::filesystem::path { throw std::runtime_error("report write"); };
        auto saved = [] { return std::filesystem::path(L"isolated-test/report.txt"); };

        Require(FinishReport(0, true, failedSave, notify, output) == ReportSaveFailure,
            "headless save failure must return nonzero");
        Require(notices == 0, "headless save failure must never show a dialog");
        std::rewind(output);
        char buffer[512]{};
        const size_t bytes = std::fread(buffer, 1, sizeof(buffer), output);
        Require(std::string(buffer, bytes) == ReportSaveError, "headless save failure must write stderr text");
        Require(FinishReport(7, true, failedSave, notify, output) == 7,
            "save failure must preserve an existing viewer error");
        Require(FinishReport(0, true, saved, notify, output) == 0 && notices == 0,
            "headless successful save must remain silent and successful");
        Require(FinishReport(9, true, saved, notify, output) == 9,
            "successful save must preserve an existing viewer error");
        Require(FinishReport(0, false, failedSave, notify, output) == ReportSaveFailure && notices == 1 && errorNotice,
            "interactive save failure must notify and return nonzero");
        Require(FinishReport(0, false, saved, notify, output) == 0 && notices == 2 && !errorNotice,
            "interactive successful save must notify");
        std::fclose(output);
        std::puts("PASS: diagnostic report success/failure, no-dialog stderr/exit status, and viewer error preservation");
        return 0;
    } catch (const std::exception& error) {
        std::fclose(output);
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
