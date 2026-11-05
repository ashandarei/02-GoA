# Development notes

## Build setup

Use the build commands and requirements in the [README](../README.md). The DLL requires `Game__Shipping__Win64`; a generic `Release` configuration omits the required Unreal build definitions.

To reuse a dependency checkout, pass `-DUE4SS_SRC=C:/path/to/ue4ss-goa`. Configuration verifies the pinned revision and rejects changed tracked dependency files. CMake applies the required UE4SS settings and the patches in `cmake/patches` without modifying that checkout.

If Corrosion cannot resolve a Rustup toolchain, pass `-DRust_RESOLVE_RUSTUP_TOOLCHAINS=OFF` with absolute `-DRust_COMPILER=.../rustc.exe` and `-DRust_CARGO=.../cargo.exe` paths.

## Optional diagnostics

The default build is inactive during an ordinary KH0.2 launch. The KH3 client manages companion launches with `-KH02AssetCompanion`.

For standalone gameplay diagnostics:

1. Configure with `-DKH02_ENABLE_DIAGNOSTICS=ON` and rebuild.
2. Add `-KH02Diagnostics` to KH0.2's launch arguments through its store launcher.

Both opt-ins are required. Diagnostic mode provides startup loading, inspection, and profiling through a separate mailbox. It does not open the asset-reading service. Companion mode takes precedence if both launch flags are supplied.

Rebuild with diagnostics disabled before preparing a player release.

## Wine/Proton testing

Build the native tests on Windows using the README instructions, then run `build-tests/02_goa_tests.exe` with the intended Wine/Proton runner in a test prefix. It includes its C/C++ runtime and requires no game files.

Full companion launch, asset delivery, and playback still require in-game testing. Use the KH3 client's launch handling so the host and companion share the required Wine process and pipe namespace.
