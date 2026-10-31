#pragma once
// Included after the production controller by hardware-free integration tests.
inline void QueueExclusiveEndpointProbeResult(SettingsDialogState* state,
                                              ExclusiveEndpointProbeResult result) {
    if (!state->exclusiveWork)
        state->exclusiveWork = std::make_shared<ExclusiveScanWork>();
    std::lock_guard lock(state->exclusiveWork->mutex);
    state->exclusiveWork->pending.push_back(std::move(result));
}
