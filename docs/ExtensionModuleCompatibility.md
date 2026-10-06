# Extension module compatibility (v1.1)

This core revision provides the SDK headers and client bindings used by the
Furioz420 module source snapshots. It starts from WarcraftXL `v1.1` commit
`60033ab125e3b23ef5e73f0767c44d6ea053148e` and ports the core-only
changes from the integrated WXL revision `87c277d`. No extension repository,
server module, game asset, or client binary is copied into this core change.

The public `v1.1` extension target previously searched only core `include/`
and `src/`. It now also searches its own `extensions/<name>/src/`, so includes
such as `ExtensionApi.hpp` and `client/Item/RetailItem.hpp` resolve from the
module that owns them. Its source glob excludes `test/`, `tests/`, and
`server/` translation units; the latter prevents Quest Marker's AzerothCore
source from being compiled into a Windows client DLL. Optional `target.cmake`
continues to let a module declare additional target settings.

The `wxl/*.h` API files belong to core's versioned extension SDK. Module
repositories must not vendor copies: a header-only workaround can compile a
DLL against an ABI or service implementation the running core does not have.
Some consumers also use `src/game`, `src/offsets`, and event contracts, so
matching header names alone are not a compatibility test.

## Build evidence

On Windows with MSVC Win32, the following targets built in a clean checkout
of this proposed core source with the corresponding owned-module publication
snapshots placed under `extensions/`:

`WarcraftXL`, `d3d9`, `wxl-patcher`, `wxl-character-creation-preview`,
`wxl-db2`, `wxl-modern-m2`, `wxl-modern-wmo`, `wxl-quest-marker`,
`wxl-radial-ping`, `wxl-retail-ui`, and `wxl-runtime`.

Builds establish source compatibility only. Before publishing installable
releases, pin each module workflow to the exact public core commit, run its
Win32 CI build, and smoke-test the resulting DLLs together in the client.
The integrated client acceptance does not automatically cover these separately
built artifacts.

Keep source-file copyright and license notices. This compatibility port is
derived from WarcraftXL core work and the integrated Furioz WXL development
history; module maintainers retain their own repository attribution.
