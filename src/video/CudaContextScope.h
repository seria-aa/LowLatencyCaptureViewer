#pragma once
#include <cuda.h>

// Libraries may leave different CUDA contexts current on the calling thread.
// Keep each provider's borrowed context active only for its own calls.
class CudaContextScope {
    CUcontext previous_=nullptr;
    bool captured_=false,ready_=false;
public:
    explicit CudaContextScope(CUcontext desired=nullptr)noexcept {
        captured_=cuCtxGetCurrent(&previous_)==CUDA_SUCCESS;
        ready_=captured_&&(!desired||cuCtxSetCurrent(desired)==CUDA_SUCCESS);
    }
    ~CudaContextScope(){if(captured_)cuCtxSetCurrent(previous_);}
    explicit operator bool()const noexcept{return ready_;}
};
