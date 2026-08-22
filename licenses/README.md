# Release License Bundle

This directory is reserved for the exact third-party copyright, license, and notice files that correspond
to a published binary package.

Do not populate it from memory or from the direct dependency manifest alone. For each release:

1. Freeze the source commit, dependency baseline, triplet, build profile, and optional backends.
2. Build and package the final artifact.
3. Enumerate every executable, shared library, Qt plugin, codec, database, model, and provider binary.
4. Copy the matching upstream/vcpkg copyright and notice files into this directory.
5. Reconcile the assembled notice bundle against the final package after deployment tooling runs.

Source-only staging keeps this README but does not claim that an empty directory is a complete binary
notice bundle.
