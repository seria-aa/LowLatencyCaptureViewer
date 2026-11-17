#pragma once
#include "recording/RecordingOptions.h"
#include "recording/RecordingArchive.h"
#include <mfidl.h>
#include <mfreadwrite.h>

namespace llcv::recording {
inline bool UsesFragmentedContainer(Codec codec) {
    return codec == Codec::H264 || codec == Codec::Hevc || codec == Codec::Av1;
}
// Start both encoded tracks at zero using bounded raw silence/first-image
// prefixes. Some Windows readers discard a fragmented MP4 track's initial
// edit-list delay even when the muxer correctly preserves its PTS/DTS.
inline bool RequiresZeroOriginTimeline(Codec codec) { return UsesFragmentedContainer(codec); }
// Continuous, crash-tolerant MP4 without encoder restarts. Standard builds use
// MF encoders and a packet-only hybrid muxer for all codecs: fragmented while
// recording, with a conventional sample index on successful finalization.
// There is no silent fallback if the selected codec/container fails.
// The caller must initialize COM / Media Foundation and configure its streams.
HRESULT CreateRecordingWriter(IMFByteStream* bytes, IMFAttributes* attributes,
                              IMFSinkWriter** writer,
                              Codec codec = Codec::H264, bool preserveColor = false);
// Creates a new file atomically; existing recordings are never overwritten.
// Without splitting the returned stream remains open after Finalize for flush.
// With splitting it describes only part 001; flush/close through archive instead.
HRESULT CreateRecordingFile(const wchar_t* path, IMFAttributes* attributes,
                            IMFByteStream** bytes, IMFSinkWriter** writer,
                            Codec codec = Codec::H264,const Options* options=nullptr,
                            std::shared_ptr<RecordingArchive>* archive=nullptr,
                            bool preserveColor=false);
// Call only after the writer/file are closed. A bounded structural sanity check
// of both encoded streams, not a full-file decode or a power-loss guarantee.
HRESULT ValidateCompletedRecording(const wchar_t* path);
} // namespace llcv::recording
