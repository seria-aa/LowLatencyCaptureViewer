// Full app/UI test, isolated at compile time. Never load/import real user settings.
#define wWinMain OriginalViewerWinMain
#include "../src/main.cpp"
#undef wWinMain

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    if(!llcv::test_profile::Enabled())return 3;
#ifdef LLCV_RECORDING
    const int probe=llcv::recording::RunEncoderProbeChild(command,[] {return llcv::recording::ProbeEncoders();});
    if(probe>=0)return probe;
#endif
    llcv::settings::AppSettings preview;
    preview.checkForUpdates=false;preview.skipStartupSettings=false;
    preview.cmaaNisEnabled=true;preview.vsrEnabled=false;preview.cmaaPreserve=false;
    preview.preferredUpscaler=llcv::settings::UpscalingMethod::CmaaNis;
    preview.pixelPerfect=false;preview.displayMatchesCapture=false;
    preview.videoPreset=VideoPreset::R2560x1440;preview.capturePreset=VideoPreset::R1920x1080;
    preview.videoFrameRate=60;preview.pixelFormat=VideoPixelFormat::Nv12;
    const auto path=llcv::test_profile::Directory()+L"\\settings.ini";
    unsigned long error=0;
    if(!llcv::settings::SaveToIni(path,preview,&error))return 4;
    return OriginalViewerWinMain(instance,previous,command,show);
}
