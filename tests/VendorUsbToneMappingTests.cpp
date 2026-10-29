#include "capture/HardwareToneMapping.h"
#include "capture/ElgatoHidToneMapping.h"
#include <dshow.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <vidcap.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace llcv::capture;
using Format = llcv::settings::VideoPixelFormat;
static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
class FakeUvc final : public IKsControl, public IKsTopologyInfo {
public:
    ULONG refs=1, returnedBytes=20;
    DWORD nodes=2, matches=1;
    unsigned gets=0, sets=0, nodeCalls=0;
    HRESULT countHr=S_OK, typeHr=S_OK, getHr=S_OK, setHr=S_OK;
    bool exposeControl=true, exposeTopology=true;
    std::array<unsigned char,20> payload{};
    FakeUvc() { for (size_t i=0;i<payload.size();++i) payload[i]=static_cast<unsigned char>(i+30); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out=nullptr;
        if (id==IID_IUnknown || (id==__uuidof(IKsControl) && exposeControl)) *out=static_cast<IKsControl*>(this);
        else if (id==__uuidof(IKsTopologyInfo) && exposeTopology) *out=static_cast<IKsTopologyInfo*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE get_NumCategories(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Category(DWORD,GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_NumConnections(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_ConnectionInfo(DWORD,KSTOPOLOGY_CONNECTION*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_NodeName(DWORD,WCHAR*,DWORD,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateNodeInstance(DWORD,REFIID,void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_NumNodes(DWORD* count) override { *count=nodes; return countHr; }
    HRESULT STDMETHODCALLTYPE get_NodeType(DWORD,GUID* type) override { ++nodeCalls; *type=KSNODETYPE_DEV_SPECIFIC; return typeHr; }
    HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD,ULONG,LPVOID,ULONG,ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT,ULONG,LPVOID,ULONG,ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY property,ULONG propertyBytes,LPVOID data,ULONG bytes,ULONG* returned) override {
        const GUID expected={0xc835261b,0xff1c,0x4c9a,{0xb2,0xf7,0x93,0xc9,0x1f,0xcf,0xbe,0x77}};
        Check(propertyBytes==sizeof(KSP_NODE) && property->Set==expected && property->Id==11 && bytes==20,
            "exact AVer UVC GUID/ID/packet layout");
        const auto* node=reinterpret_cast<const KSP_NODE*>(property);
        Check(node->Reserved==0 && node->NodeId<nodes,"initialized topology request");
        if (property->Flags==(KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_TOPOLOGY)) {
            ++gets;
            if (node->NodeId>=matches) return E_PROP_ID_UNSUPPORTED;
            std::memcpy(data,payload.data(),(std::min)(static_cast<size_t>(returnedBytes),payload.size()));
            *returned=returnedBytes; return getHr;
        }
        Check(property->Flags==(KSPROPERTY_TYPE_SET|KSPROPERTY_TYPE_TOPOLOGY),"only GET/SET topology requests");
        ++sets;
        const auto* actual=static_cast<unsigned char*>(data);
        for (size_t i=0;i<20;++i)
            if (i!=15 && i!=17) Check(actual[i]==(i<returnedBytes ? payload[i] : 0),"preserve GET bytes; zero unread tail");
        Check(actual[15]==2 && actual[17]<=1,"only tone-mapping selector and enable changed");
        payload[17]=actual[17];
        *returned=0; return setHr;
    }
    ToneMappingResult Run(Format format=Format::P010) {
        auto result=ConfigureHardwareToneMapping(static_cast<IKsControl*>(this),L"USB Capture",format);
        Check(refs==1,"UVC COM references released");
        return result;
    }
};
static void TestUvc() {
    FakeUvc good;
    Check(good.Run().status==ToneMappingStatus::Applied && good.sets==1 && good.payload[17]==0,"UVC P010 OFF");
    Check(good.Run(Format::Nv12).status==ToneMappingStatus::Applied && good.sets==2 && good.payload[17]==1,"UVC SDR ON");
    for (auto rgb : {Format::Rgb24,Format::Rgb32}) {
        FakeUvc fixture;
        Check(fixture.Run(rgb).status==ToneMappingStatus::NotApplicable && fixture.sets==0 && fixture.gets==0,
            "RGB leaves vendor hardware tone mapping untouched");
    }
    for (auto bytes : {0ul,17ul,18ul,19ul,20ul,21ul,0xfffffffful}) {
        FakeUvc f; f.returnedBytes=bytes;
        const auto r=f.Run();
        Check((r.status==ToneMappingStatus::Applied)==(bytes>=18 && bytes<=20),"validate returned UVC length");
        Check(f.sets==(bytes>=18 && bytes<=20 ? 1u:0u),"invalid length never writes");
    }
    FakeUvc ambiguous; ambiguous.matches=2;
    Check(ambiguous.Run().status==ToneMappingStatus::AmbiguousDevice && !ambiguous.sets,"multiple matching extension nodes rejected");
    FakeUvc excessive; excessive.nodes=65;
    Check(excessive.Run().status==ToneMappingStatus::QueryFailed && !excessive.nodeCalls,"bounded topology walk");
    for (auto hr : {E_FAIL,S_FALSE,E_ACCESSDENIED}) {
        FakeUvc f; f.getHr=hr; Check(f.Run().status==ToneMappingStatus::QueryFailed && !f.sets,"GET failure prevents SET");
        FakeUvc c; c.countHr=hr; Check(c.Run().status==ToneMappingStatus::QueryFailed && !c.gets,"node count failure safe");
        FakeUvc n; n.typeHr=hr; Check(n.Run().status==ToneMappingStatus::QueryFailed && !n.gets,"node type failure safe");
        FakeUvc s; s.setHr=hr; Check(s.Run().status==ToneMappingStatus::SetFailed && s.sets==1,"failed SET not retried");
    }
    FakeUvc none; none.matches=0; Check(none.Run().status!=ToneMappingStatus::Applied && !none.sets,"unsupported UVC safe");
    FakeUvc missing; missing.exposeTopology=false; Check(missing.Run().status!=ToneMappingStatus::Applied && !missing.gets,"missing topology safe");
    FakeUvc repeated;
    for (int i=0;i<10000;++i) Check(repeated.Run(i%2 ? Format::Yuy2:Format::P010).status==ToneMappingStatus::Applied,"repeat UVC lifecycle");
    Check(repeated.sets==10000,"one UVC write per start");
}
class FakeHid final : public ElgatoHidAccess {
public:
    HidToneMappingDevice selected{{11,0,0,{0}},0x0fd9,0x0082,64,L"selected"};
    std::vector<HidToneMappingDevice> devices{selected};
    std::vector<uint8_t> last;
    HRESULT describeHr=S_OK, findHr=S_OK, writeHr=S_OK;
    unsigned describes=0, finds=0, writes=0;
    HRESULT DescribeSelected(std::wstring_view,HidToneMappingDevice& out) override { ++describes; out=selected; return describeHr; }
    HRESULT FindCandidates(const HidToneMappingDevice&,std::vector<HidToneMappingDevice>& out) override { ++finds; out=devices; return findHr; }
    HRESULT Write(const HidToneMappingDevice& device,std::span<const uint8_t> bytes) override {
        Check(SameCaptureContainer(selected,device),"write only selected container"); ++writes; last.assign(bytes.begin(),bytes.end()); return writeHr;
    }
};
static void TestHid() {
    constexpr auto path=LR"(\\?\usb#vid_0fd9&pid_0082&mi_00#selected)";
    for (unsigned short product : std::array<unsigned short,3>{0x006a,0x0082,0x008a}) {
        for (bool enable : {false,true}) {
            auto bytes=ElgatoToneMappingReport(product,64,enable);
            std::vector<uint8_t> expected = product==0x006a
                ? std::vector<uint8_t>{11,0x55,0x0a,1,static_cast<uint8_t>(enable)}
                : std::vector<uint8_t>{6,6,6,0x55,2,0x0a,static_cast<uint8_t>(enable)};
            expected.resize(64,0);
            Check(bytes==expected,"official Elgato report IDs/length/address/register/padding");
        }
    }
    Check(ElgatoToneMappingReport(0x0082,6,false).empty() &&
          ElgatoToneMappingReport(0x006a,4,false).empty() &&
          ElgatoToneMappingReport(0x0082,4097,false).empty() &&
          ElgatoToneMappingReport(0xffff,64,false).empty(),"unknown PID or invalid report length rejected");
    FakeHid good;
    for (int i=0;i<10000;++i) {
        const auto r=ConfigureElgatoHidToneMapping(path,i%2!=0,good);
        Check(r.status==ToneMappingStatus::Applied && good.last[6]==i%2,"repeat HID HDR/SDR policy");
    }
    Check(good.writes==10000,"one HID report per start");
    for (auto badPath : {L"",L"Elgato HD60 X",LR"(\\?\usb#vid_ffff&pid_0082#x)",LR"(\\?\usb#vid_0fd9&pid_ffff#x)",LR"(\\?\usb#vid_0fd9&pid_0082BAD)"}) {
        FakeHid f; Check(ConfigureElgatoHidToneMapping(badPath,false,f).status==ToneMappingStatus::NotApplicable &&
            !f.describes && !f.finds && !f.writes,"invalid identity causes no hardware calls");
    }
    FakeHid mismatch; mismatch.selected.container=GUID_NULL;
    Check(ConfigureElgatoHidToneMapping(path,false,mismatch).status==ToneMappingStatus::QueryFailed && !mismatch.finds,"missing selected container safe");
    FakeHid wrongPid; wrongPid.selected.product=0x006a;
    Check(ConfigureElgatoHidToneMapping(path,false,wrongPid).status==ToneMappingStatus::QueryFailed && !wrongPid.finds,"selected identity must agree with path");
    FakeHid other; other.devices[0].container.Data1=12;
    Check(ConfigureElgatoHidToneMapping(path,false,other).status==ToneMappingStatus::Unsupported && !other.writes,"same model on another USB port untouched");
    other.devices.push_back(other.selected);
    Check(ConfigureElgatoHidToneMapping(path,true,other).status==ToneMappingStatus::Applied && other.writes==1,"match exact physical container");
    FakeHid multiple; multiple.devices.push_back(multiple.selected);
    Check(ConfigureElgatoHidToneMapping(path,false,multiple).status==ToneMappingStatus::AmbiguousDevice && !multiple.writes,"ambiguous HID endpoints never guessed");
    for (auto hr : {E_FAIL,S_FALSE,E_ACCESSDENIED}) {
        FakeHid d; d.describeHr=hr; Check(ConfigureElgatoHidToneMapping(path,false,d).status==ToneMappingStatus::QueryFailed && !d.finds,"describe failure safe");
        FakeHid f; f.findHr=hr; Check(ConfigureElgatoHidToneMapping(path,false,f).status==ToneMappingStatus::QueryFailed && !f.writes,"discovery failure safe");
        FakeHid w; w.writeHr=hr; Check(ConfigureElgatoHidToneMapping(path,false,w).status==ToneMappingStatus::SetFailed && w.writes==1,"write failure not retried");
    }
    FakeHid wrongVendor; wrongVendor.devices[0].vendor=0xffff;
    Check(ConfigureElgatoHidToneMapping(path,false,wrongVendor).status==ToneMappingStatus::Unsupported && !wrongVendor.writes,
        "same container cannot override a vendor mismatch");
    FakeHid shortReport; shortReport.devices[0].reportBytes=6;
    Check(ConfigureElgatoHidToneMapping(path,false,shortReport).status==ToneMappingStatus::Unsupported && !shortReport.writes,
        "too-short device report never writes");
    FakeHid missingPath; missingPath.devices[0].path.clear();
    Check(ConfigureElgatoHidToneMapping(path,false,missingPath).status==ToneMappingStatus::QueryFailed && !missingPath.writes,
        "candidate identity without a path never writes");
    std::wstring embedded(path); embedded.push_back(L'\0'); embedded += L"unselected";
    FakeHid nul;
    Check(ConfigureElgatoHidToneMapping(embedded,false,nul).status==ToneMappingStatus::NotApplicable && !nul.describes,
        "embedded null path rejected before any device access");
}
int main() { TestUvc(); TestHid(); std::puts("VendorUsbToneMappingTests passed (fake COM/HID; no real devices)."); }
