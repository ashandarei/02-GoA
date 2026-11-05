# 02-GoA

02-GoA is the Kingdom Hearts 0.2 companion DLL for the Garden of Assemblage integration in the Archipelago KH3 client. It provides assets, movies, and audio from a local KH0.2 installation to KH3.

Supported KH0.2 installations are the Epic Games Store and Steam versions. You must own and install the games; this project does not include game assets or executables.

Version 2.0 uses asset protocol 3. After the game engine mounts its paks, the
companion discovers an immutable index of supported file paths and sizes in
memory and shares it with its owning KH3 process. It does not read or create
an asset catalog file. Compatibility edits are calculated by the matching KH3
runtime when packages are requested. Use both updated DLLs together with
matching asset protocol support.

## Installation

Use the Archipelago KH3 client to select your KH0.2 installation and install the companion. The client installs the required UE4SS loader and configuration, then launches the companion when needed.

The companion DLL is installed at:

```text
KINGDOM HEARTS 0.2 Birth by Sleep/
  Binaries/Win64/Mods/KH02APProbeCpp/dlls/main.dll
```

On Linux with Wine/Proton, use the KH3 client's launch handling for your selected stores.

## Build

Requirements: Windows x64, Visual Studio 2022 with MSVC **14.38**, Windows SDK, CMake 3.22+, Ninja, Git, and Rust/Cargo. UE4SS may require additional tools documented in its pinned source.

From an x64 Native Tools command prompt using MSVC 14.38:

```powershell
git clone https://github.com/ashandarei/ue4ss-goa.git vendor/ue4ss-goa
git -C vendor/ue4ss-goa checkout --detach 06dd053d3e537ec2b477ee76337661b15642d012
git -C vendor/ue4ss-goa submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 -DKH02_ENABLE_DIAGNOSTICS=OFF
cmake --build build --target KH02APProbeCpp
```

Output: `build/src/probe/main.dll`. Building does not install the DLL into a game.

See [development notes](docs/development.md) for dependency setup, build troubleshooting, and optional diagnostics.

## Tests

The native tests use synthetic inputs and compile without UE4SS or game files:

```powershell
cmake -S tests -B build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

These tests cover native utilities and transport behavior. Full integration testing requires the installed games.

## License

Licensed under the MIT License; see [LICENSE](LICENSE). Dependency licenses and attribution are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`.

This project is not affiliated with the game publishers.
