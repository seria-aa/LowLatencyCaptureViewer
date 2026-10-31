#include "capture/DirectShowDevices.h"

#include <cstdio>

namespace {
int g_crossbarMessages = 0;
void CountMessage(const wchar_t*) { ++g_crossbarMessages; }

long FilterCount(IGraphBuilder* graph) {
    IEnumFilters* filters = nullptr;
    if (FAILED(graph->EnumFilters(&filters))) return -1;
    long count = 0;
    IBaseFilter* filter = nullptr;
    while (filters->Next(1, &filter, nullptr) == S_OK) {
        ++count;
        filter->Release();
    }
    filters->Release();
    return count;
}

IBaseFilter* AddFilter(IGraphBuilder* graph, const CLSID& id, const wchar_t* name) {
    IBaseFilter* filter = nullptr;
    if (FAILED(CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&filter))))
        return nullptr;
    if (FAILED(graph->AddFilter(filter, name))) {
        filter->Release();
        return nullptr;
    }
    return filter;
}
}  // namespace

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (condition) return;
        std::fprintf(stderr, "FAILED: %s\n", message);
        ++failures;
    };

    // Fail before enumeration: stale USB identity must never be reused.
    std::wstring stalePath = L"previous capture device";
    check(llcv::capture::FindVideoCaptureFilter({}, nullptr, nullptr, nullptr,
              nullptr, &stalePath) == E_POINTER && stalePath.empty(),
          "failed selection clears physical-device path without opening hardware");

    check(llcv::capture::RelatedCaptureAudioScore(
              L"AVerMedia HD Capture GC573", L"AVerMedia HD Capture GC573") ==
              1000,
          "identical normalized names");
    check(llcv::capture::RelatedCaptureAudioScore(
              L"Live Gamer 4K", L"Live Gamer 4K Audio") == 800,
          "one device name contains the other");
    check(llcv::capture::RelatedCaptureAudioScore(
              L"AVerMedia Live Gamer 4K GC573",
              L"AVerMedia GC573 Audio") >= 100,
          "model token matches separate audio filter");
    check(llcv::capture::RelatedCaptureAudioScore(
              L"Unrelated Video Device", L"Different USB Audio") == 0,
          "generic words do not create a false match");

    {
        const auto rejected = llcv::capture::AttachUpstreamCrossbar(nullptr, nullptr, nullptr, nullptr);
        check(rejected.result == E_POINTER && rejected.graphIntact &&
                  !llcv::capture::NeedsUpstreamCrossbar(nullptr),
              "crossbar setup rejects missing graph/filter without touching anything");
    }
    check(std::wstring(llcv::capture::PhysicalConnectorName(PhysConn_Video_YRYBY)) == L"component" &&
              std::wstring(llcv::capture::PhysicalConnectorName(PhysConn_Video_VideoDecoder)) ==
                  L"video decoder" &&
              std::wstring(llcv::capture::PhysicalConnectorName(0x7fff)) == L"vendor-defined",
          "crossbar connector names cover standard and vendor types");

    {
        struct MediumList {
            KSMULTIPLE_ITEM header;
            REGPINMEDIUM items[2];
        } list{};
        const GUID standard = {0x4747B320, 0x62CE, 0x11CF,
                               {0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}};
        const GUID vendor = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
        list.header.Size = sizeof(list);
        list.header.Count = 2;
        list.items[0] = {standard, 0, 0};
        list.items[1] = {vendor, 7, 0};
        REGPINMEDIUM medium{};
        check(llcv::capture::SelectPinMedium(&list.header, medium) &&
                  IsEqualGUID(medium.clsMedium, vendor) && medium.dw1 == 7,
              "medium selection skips the standard set and returns the device medium");
        list.header.Count = 3;
        check(!llcv::capture::SelectPinMedium(&list.header, medium),
              "a medium count beyond the reported size is rejected before reading");
        list.header.Count = 1;
        list.header.Size = sizeof(KSMULTIPLE_ITEM) - 1;
        check(!llcv::capture::SelectPinMedium(&list.header, medium),
              "a size smaller than the list header is rejected");
        list.header.Size = sizeof(list);
        list.header.Count = 1;
        check(!llcv::capture::SelectPinMedium(&list.header, medium) &&
                  !llcv::capture::SelectPinMedium(nullptr, medium),
              "standard-only and missing medium lists select nothing");
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IGraphBuilder* graph = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&graph)))) {
        // Output-only source, like a single-input HDMI capture filter.
        if (IBaseFilter* source = AddFilter(graph, CLSID_AsyncReader, L"output only")) {
            const long before = FilterCount(graph);
            g_crossbarMessages = 0;
            const auto skipped =
                llcv::capture::AttachUpstreamCrossbar(graph, source, L"test", CountMessage);
            check(skipped.result == S_FALSE && skipped.graphIntact &&
                      !llcv::capture::NeedsUpstreamCrossbar(source) &&
                      FilterCount(graph) == before && g_crossbarMessages == 0,
                  "filters without an input pin keep the graph unchanged and silent");
            source->Release();
        } else {
            check(false, "output-only test filter must be available");
        }
        // Unconnected input pin but no WDM crossbar: diagnostic only.
        if (IBaseFilter* tee = AddFilter(graph, CLSID_SmartTee, L"input pin")) {
            const long before = FilterCount(graph);
            g_crossbarMessages = 0;
            check(llcv::capture::NeedsUpstreamCrossbar(tee), "an unconnected input asks for a crossbar");
            const auto missing =
                llcv::capture::AttachUpstreamCrossbar(graph, tee, L"test", CountMessage);
            check(FAILED(missing.result) && missing.graphIntact &&
                      FilterCount(graph) == before && g_crossbarMessages == 1,
                  "a missing crossbar is logged once and leaves the graph intact");
            tee->Release();
        } else {
            check(false, "input-pin test filter must be available");
        }
        // Failure cleanup removes everything added after the snapshot and
        // keeps the original filters.
        std::vector<IBaseFilter*> before;
        const long original = FilterCount(graph);
        check(SUCCEEDED(llcv::capture::SnapshotGraphFilters(graph, before)) &&
                  static_cast<long>(before.size()) == original,
              "snapshot lists every original filter");
        IBaseFilter* first = AddFilter(graph, CLSID_SmartTee, L"added tee");
        IBaseFilter* second = AddFilter(graph, CLSID_AsyncReader, L"added source");
        check(first && second && FilterCount(graph) == original + 2, "test filters are added");
        check(SUCCEEDED(llcv::capture::RemoveFiltersAddedSince(graph, before)) &&
                  FilterCount(graph) == original && before.empty(),
              "cleanup restores the original filter set and releases the snapshot");
        std::vector<IBaseFilter*> missing;
        check(llcv::capture::SnapshotGraphFilters(nullptr, missing) == E_POINTER && missing.empty(),
              "a failed snapshot reports failure instead of an empty original graph");
        if (first) first->Release();
        if (second) second->Release();
        graph->Release();
    } else {
        check(false, "filter graph must be available");
    }
    if (SUCCEEDED(com)) CoUninitialize();

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("DirectShow device tests passed.");
    return 0;
}
