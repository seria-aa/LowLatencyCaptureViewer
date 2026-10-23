#pragma once
#include <mfidl.h>
#include <cstddef>
#include <cstdint>

namespace llcv::recording {
// Compressed bytes only. Writes own their queued copies; a successful Flush
// means they reached the underlying file. Physical failures are also published
// through RecordingStorageResult while recording. Capture pools are unchanged.
struct StorageLimits { size_t bytes=32*1024*1024,operations=4096; };
HRESULT BufferRecordingStorage(IMFByteStream* file,IMFByteStream* sibling,
    IMFByteStream** result,StorageLimits limits={});
// Periodic durability checks enqueue a coalesced flush. Flush()/Close() still
// wait for physical completion when the recording is finalized.
HRESULT RequestRecordingFlush(IMFByteStream* stream);
HRESULT RecordingStorageResult(IMFByteStream* stream);
// Space checks share the bounded storage worker with writes, never the encoder
// or capture callbacks. A pending request is coalesced; reads only copy the
// latest completed result. Split siblings share this cache, even after the
// original stream is retired. All requests in one recording use its folder.
struct StorageSpace { bool completed=false,known=false;uint64_t freeBytes=0; };
HRESULT RequestRecordingSpace(IMFByteStream* stream,const wchar_t* folder);
StorageSpace RecordingStorageSpace(IMFByteStream* stream);
#ifdef LLCV_RECORDING_STORAGE_SPACE_TEST
// This hook exists only in the dedicated storage regression executable.
using RecordingSpaceQuery=BOOL (*)(const wchar_t*,ULARGE_INTEGER*);
void SetRecordingSpaceQueryForTest(RecordingSpaceQuery query);
#endif
// Seal a completed split file in order, without delaying the next segment.
HRESULT RetireRecordingStorage(IMFByteStream* stream);
}
