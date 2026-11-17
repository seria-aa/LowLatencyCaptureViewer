#pragma once

namespace llcv::capture {

// What to do when the capture audio branch cannot be built before the graph
// runs. A prompt needs a user and a window that can show above the output.
enum class AudioFailureAction { ReportFailure, AskVideoOnly };
constexpr AudioFailureAction OnCaptureAudioFailure(bool unattended, bool exclusiveOutput) {
    return unattended || exclusiveOutput ? AudioFailureAction::ReportFailure
                                         : AudioFailureAction::AskVideoOnly;
}

struct VideoOnlyAnswer {
    // Yes: detach the audio branch, close the audio output and run video.
    bool startVideoOnly = false;
    bool closeAudioOutput = false;
    // No: back to Settings without a second, generic failure dialog.
    bool returnToSettingsQuietly = false;
};
constexpr VideoOnlyAnswer AfterVideoOnlyPrompt(bool accepted) {
    return accepted ? VideoOnlyAnswer{true, true, false} : VideoOnlyAnswer{false, false, true};
}

}  // namespace llcv::capture
