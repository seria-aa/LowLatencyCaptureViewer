#include "TestProfile.h"

namespace llcv::test_profile {
bool Enabled() {
#if defined(LLCV_TEST_PROFILE) || defined(LLCV_SPATIAL_PROTOTYPE) || defined(LLCV_HDR_FRAME_AUDIT) || defined(LLCV_VSR_EXPERIMENT) || defined(LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC)
    return true;
#else
    static const bool enabled=[] {
        int count=0;auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
        bool result=false;
        for(int i=1;arguments && i<count;i++)
            result|=DiagnosticArgument(arguments[i]);
        LocalFree(arguments);return result;
    }();
    return enabled;
#endif
}
}
