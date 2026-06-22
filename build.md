# Building WallpaperWorks

This document describes the current build flow in this repository as of June 22, 2026.

## Overview

The repository uses two scripts:

- `./configure.sh` prepares bundled third-party dependencies under `third_party/real/`
- `./build.sh [development|production]` builds the application

Important behavior:

- `./configure.sh` deletes and recreates `third_party/real/` every run
- `./build.sh` deletes and recreates `build/` every run
- Omitting the build type defaults to `development`

## Required local resources

The build scripts expect a local `resources/` directory that is not committed to the repository.

Create `resources/` in the project root and provide these files:

### Required on all platforms

- `resources/font.ttf`
- `resources/favicon.ico`

### Additional files required on macOS

- `resources/favicon.icns`
- `resources/status_bar_icon_off.png`
- `resources/status_bar_icon_on.png`
- `resources/curl-ca-bundle.crt`

If any of these files are missing, `./build.sh` will fail during the initial `cp` steps.

## Platform notes

### macOS

Validated on this host:

- OS: macOS
- Architecture: `arm64`
- Compiler: Apple clang from `/usr/bin/gcc`
- `pkg-config freetype2`: available

The macOS build path in `build.sh` currently does all of the following:

- builds `build/WallpaperWorks.app`
- compiles `src/main_osx.m`
- uses `pkg-config --libs freetype2`
- links with `-lcurl`, `-lwebp`, Cocoa, and ServiceManagement
- signs development builds with ad-hoc signing via `codesign --force --sign -`

Current caveat:

- `./configure.sh` was run on this `arm64` macOS host and failed while linking `third_party/libwebp` example binaries
- Observed failure: `archive member 'example_util.o' not a mach-o file`
- Because of that failure, the full macOS build is not currently verified end-to-end from a clean checkout

Build commands:

```sh
mkdir -p resources
# add the required resource files listed above

./configure.sh
./build.sh development
```

Expected output if the build succeeds:

- app bundle: `build/WallpaperWorks.app`
- executable inside bundle: `build/WallpaperWorks.app/Contents/MacOS/WallpaperWorks`

For an optimized build:

```sh
./build.sh production
```

Production macOS builds additionally run `scripts/bundle_dylibs.sh` to bundle dependent dynamic libraries into the app.

### Linux

This path was not executed in this session. The steps below are derived from the current scripts.

Required tools and libraries inferred from `build.sh` and `configure.sh`:

- `cc`
- `make`
- `pkg-config`
- `curl`
- `freetype2`
- X11 development files
- Xinerama development files

Build commands:

```sh
mkdir -p resources
# add resources/font.ttf and resources/favicon.ico

./configure.sh
./build.sh development
```

Expected output if the build succeeds:

- executable: `build/WallpaperWorks`

Linux development builds also enable `-fsanitize=address,undefined`.

### Windows

This path was not executed in this session. The steps below are derived from the current scripts and existing README.

Required tools inferred from the repository:

- Git
- `w64devkit`
- `unzip`
- NSIS if you want to build the installer

Build commands:

```sh
git clone https://github.com/Aaron-Speedy/WallpaperWorks
cd WallpaperWorks

mkdir resources
# add resources/font.ttf and resources/favicon.ico

./configure.sh
./build.sh development
```

Expected output if the build succeeds:

- executable: `build/WallpaperWorks.exe`

To build the installer after a successful app build:

```sh
./build_installer.sh
```

Expected installer output:

- `build/WallpaperWorksInstaller.exe`

## Troubleshooting

### `resources/` is missing

Symptom:

- `cp: resources/...: No such file or directory`

Cause:

- the repository does not include the required local asset files

Fix:

- create `resources/` and add the required files before running `./build.sh`

### macOS `./configure.sh` fails in `third_party/libwebp`

Symptom observed on this host:

- `archive member 'example_util.o' not a mach-o file`

Impact:

- the dependency bootstrap does not complete from a clean checkout on the tested `arm64` macOS environment

Status:

- documented here as a current repo issue; not fixed in this change

## Verification status for this document

Verified directly in this session:

- repository scripts: `configure.sh`, `build.sh`, `build_installer.sh`
- current README build instructions
- host architecture: `arm64`
- `pkg-config freetype2` availability on this host
- absence of a committed `resources/` directory
- current macOS `./configure.sh` failure from a clean `third_party/real/` bootstrap

Not verified directly in this session:

- Linux build completion
- Windows build completion
- successful end-to-end macOS app build from the current checkout
