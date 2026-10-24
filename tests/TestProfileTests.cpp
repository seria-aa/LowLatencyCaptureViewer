#include "settings/TestProfile.h"
#include "settings/SettingsStore.h"
#include "recording/RecordingOptions.h"
#include <cstdio>
#include <cstdlib>
static void Check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    using namespace llcv;
    Check(test_profile::Enabled(),"test binary always isolated");
    for(const auto* arg:{L"--smoke-test",L"--smoke-test-audio-only",L"--exclusive-probe",L"--exclusive-probe-all"})
        Check(test_profile::DiagnosticArgument(arg),"normal executable diagnostic arguments isolate profiles");
    for(const auto* arg:{L"",L"--settings-tab",L"--recording",L"C:\\smoke-test\\viewer.exe"})
        Check(!test_profile::DiagnosticArgument(arg),"ordinary runs retain established profile behavior");
    const auto folder=test_profile::Directory();
    Check(GetFileAttributesW(folder.c_str())!=INVALID_FILE_ATTRIBUTES,"unique temporary root exists");
    Check(!test_profile::Protected(folder+L"\\settings.ini"),"isolated profile permitted");
    const auto recording=recording::ProfilePath();
    Check(recording==folder+L"\\recording-prototype.ini","recording profile uses same isolated root");
    Check(recording::DefaultFolder()==folder+L"\\recordings","default recordings never use real Videos folder");
    settings::AppSettings settings;settings.volumePercent=75;settings.cmaaNisEnabled=true;
    unsigned long error=0;
    Check(settings::SaveToIni(folder+L"\\settings.ini",settings,&error),"saving allowed even without suppression");
    Check(settings::LoadFromIni(folder+L"\\settings.ini").settings.volumePercent==75,"isolated settings round trip");
    recording::Options options;options.audioVolumePercent=42;
    Check(recording::SaveOptions(options) && recording::LoadOptions().audioVolumePercent==42,"isolated recording round trip");
    PWSTR local=nullptr;Check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)),"real root identification");
    const auto real=std::wstring(local)+L"\\LowLatencyCaptureViewer\\";CoTaskMemFree(local);
    Check(test_profile::Protected(real+L"settings.ini"),"real settings recognized");
    Check(test_profile::Protected(real+L"logs\\..\\settings.ini"),"normalized traversal recognized");
    Check(settings::LoadFromIni(real+L"settings.ini").settings.volumePercent==settings::AppSettings{}.volumePercent,"real viewer profile is not loaded by test");
    Check(recording::LoadOptions(real+L"recording-prototype.ini").audioVolumePercent==recording::Options{}.audioVolumePercent,"real recording profile is not loaded by test");
    Check(!settings::SaveToIni(real+L"settings.ini",settings,&error) && error==ERROR_ACCESS_DENIED,"real settings write rejected before opening file");
    Check(!settings::MigrateLegacyPcmQueueTarget(real+L"settings.ini"),"real migration rejected");
    Check(!settings::StartupReviewRequired(real+L"settings.ini",L"v-test"),"test does not inspect real startup marker");
    Check(!settings::MarkStartupSettingsShown(real+L"settings.ini",L"v-test",&error) && error==ERROR_ACCESS_DENIED,
        "startup marker cannot modify real settings in test builds");
    Check(!recording::SaveOptions(options,real+L"recording-prototype.ini"),"real recording write rejected");
    std::printf("PASS: isolated defaults and explicit real-profile access denial. root=%ls\n",folder.c_str());
}
