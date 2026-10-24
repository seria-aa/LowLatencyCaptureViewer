# Test profile isolation

All CMake test and private-preview executables opt into `LLCV_TEST_PROFILE`.
Each process gets a newly created `%TEMP%/LLCV-TestProfile-<pid>-<tick>-<attempt>`
directory. Viewer settings, recording settings, logs, and the default test
recording folder resolve there. No real or legacy profile is imported.

The regular application retains its established profile paths. Its
`--smoke-test*` and `--exclusive-probe` / `--exclusive-probe-all` runs use the
same temporary-profile policy. Private spatial/HDR/VSR/recovery builds are
isolated even when opened without test arguments.

`TestProfile.cpp` is compiled once per executable; shared libraries must not
decide isolation through a macro-dependent inline function. The settings
stores also reject explicit access to the real application data directory
during isolated runs. Save suppression remains an additional fixture guard,
not the boundary on which isolation depends.

Rebuild old test/preview binaries before running them. Do not test by launching
the regular packaged application without diagnostic arguments. Read logs from
the child process's isolated directory, never a stale real-profile log. Tests
requiring settings must create synthetic fixtures or receive an explicit copy
in their temporary directory; they must not modify the source profile.

`TestProfileTests` verifies isolated viewer/recording round trips without save
suppression, production diagnostic argument classification, and rejection of
explicit real-profile reads, writes, and migration. Check the real settings
and recording-profile hashes before and after hardware validation as a second
independent guard. Temporary test directories are kept for diagnosis.
