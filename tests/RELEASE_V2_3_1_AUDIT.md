# LLCV v2.3.1 release checks

Branch: `release/v2.3.1`, based on the v2.3.0 merge. This patch changes VFG
preference persistence; the VFG runtime, supported GPUs, and recording formats
are unchanged.

- Built x64 Release with recording and the same optional VFG configuration as
  v2.3.0. Matched the registered test names against the previous 203-test
  build, including the independent FFmpeg/ffprobe recording checks. All 203
  distinct tests passed: 182 base tests and 21 additional recording tests,
  with no failures in either run.
- The release interpolation policy test uses a disposable profile. It checks
  fresh defaults, toggling off before F2, accepting the settings without editing
  the checkbox, toggling back on, restoring quality and wait mode, disabling
  the checkbox, retaining opt-in on an unavailable GPU, and preserving an
  unrelated INI key.
- The final portable ZIP was extracted and passed the release package validator.
  Its 99 file hashes (including multiplicities) match the Setup build manifest.
  The viewer and Setup report version 2.3.1. No real user profile or installed
  application was used. Fresh installation and in-place upgrade were not tested.

| File | SHA-256 |
| --- | --- |
| Viewer EXE | `332B608E4C58F575238D2943C293292EFF336F641D15022DE6A9012C6A4EAA68` |
| Setup | `3BB32F361F5ACDF283FAD86FFD458C9803362E7576C752DDBF1C07A39B070983` |
| Portable ZIP | `BC85B2BFA8B05F1368B221209EDAB3C8D046991E02E2878067DF391E377FCF05` |
