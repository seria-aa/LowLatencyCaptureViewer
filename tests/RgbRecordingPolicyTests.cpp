#include "recording/RgbRecordingPolicy.h"
#include "recording/RecordingDeliveryPolicy.h"
#include "recording/RecordingStatusPresentation.h"
#include <cstdio>
#include <limits>
using namespace llcv::recording;
static int failures=0;
static void Expect(bool ok,const char* why){if(!ok){++failures;printf("FAIL %s\n",why);}}
int main(){
    for(int width:{1280,1920,2560,3840})for(int step:{3,4}){
        const int height=width*9/16;
        const uint64_t raw=uint64_t(width)*height*step;
        const auto count=RgbBufferCount(raw,kRgbCaptureBudget);
        Expect(count>=4 && count<=16 && raw*count<=kRgbCaptureBudget,"raw buffers are bounded with a viable queue");
        for(int outHeight:{720,1080,height}){
            if(outHeight>height)continue;
            const int outWidth=width*outHeight/height;
            const uint64_t bytes=uint64_t(outWidth)*outHeight*3/2;
            const auto output=RgbBufferCount(bytes,kRgbOutputBudget);
            Expect(output==16 && raw*count+bytes*output<=kRgbCaptureBudget+kRgbOutputBudget,
                "preserve encoder lease capacity within the combined budget");
        }
    }
    Expect(RgbBufferCount(0,kRgbCaptureBudget)==0 &&
        RgbBufferCount(kRgbCaptureBudget+1,kRgbCaptureBudget)==0,"invalid frame sizes are rejected");
    Expect(RecordingDeliveryPolicy(false).videoBuffers==16 &&
        RecordingDeliveryPolicy(true).videoBuffers==8,"existing YUV/MJPEG budgets unchanged");
    RgbPreflight p{7,8,1000.0/60,6};
    Expect(RgbHasHeadroom(p),"ordinary 1080p conversion accepted");
    p={31,32,1000.0/60,6};Expect(!RgbHasHeadroom(p),"native 4K60 conversion rejected");
    p={11.6,12.5,1000.0/60,6};Expect(RgbHasHeadroom(p),"4K to 1080p uses the selected output route");
    p.meanMs=p.budgetMs*.75;p.maximumMs=p.budgetMs;Expect(RgbHasHeadroom(p),"exact safety boundary");
    p.meanMs+=.001;Expect(!RgbHasHeadroom(p),"insufficient encoding headroom");
    p={7,17,1000.0/60,6};Expect(!RgbHasHeadroom(p),"deadline spike rejected");
    p={7,8,1000.0/60,2};Expect(!RgbHasHeadroom(p),"insufficient samples");
    p={std::numeric_limits<double>::quiet_NaN(),8,16,6};Expect(!RgbHasHeadroom(p),"NaN rejected");
    Snapshot s;s.phase=Phase::Failed;s.result=RgbRecordingTooSlow;s.path=L"must-not-open.mp4";
    s.rgbConversionMs=31;s.rgbBudgetMs=1000.0/60;
    for(bool english:{false,true}){
        const auto view=presentation::Build(s,english);
        Expect(!view.canOpenVideo && !view.canOpenFolder &&
            view.description.find(english?L"No recording file":L"파일을 만들지")!=std::wstring::npos,
            "preflight rejection has translated no-file guidance");
    }
    printf("RGB budget/preflight policy failures=%d\n",failures);return failures?1:0;
}
