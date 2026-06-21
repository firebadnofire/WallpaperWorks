# WallpaperWorks Dissection

This document is a code-oriented reference for how WallpaperWorks is put together today. It is intended to help future work start from the actual composition of the repository instead of rediscovering the same structure from scratch.

## 1. Program Purpose

WallpaperWorks downloads artwork from a remote image library, caches it locally, scales it per monitor, and renders the current time and date on top of the chosen image. The resulting composited image is then pushed into a platform-specific desktop background surface.

At a high level, the program is:

- One shared application core in `src/main.c`
- Three platform hosts in `src/main_win32.c`, `src/main_x11.c`, and `src/main_osx.m`
- A small header-only support layer for strings, arenas, images, fonts, networking, caching, and thread primitives
- A shell-based bootstrap and build flow in `configure.sh`, `build.sh`, and `build_installer.sh`

The shared core is not compiled as a separate translation unit. Instead, each platform entry point includes `src/main.c` directly after defining the platform-specific color layout and monitor/window types that the core expects.

## 2. Repository Shape

Top-level files with application logic:

- `README.md`: build instructions, mostly Windows-oriented
- `configure.sh`: stages third-party dependencies into `third_party/real`
- `build.sh`: generates embedded assets, selects the target platform, and builds the app
- `build_installer.sh`: wraps NSIS for the Windows installer
- `src/main.c`: shared wallpaper, download, scaling, and text rendering logic
- `src/main_win32.c`: Windows host
- `src/main_x11.c`: Linux/X11 host
- `src/main_osx.m`: macOS host
- `src/Info.plist`: macOS bundle metadata
- `src/installer.nsi`: NSIS installer definition

Shared support headers:

- `src/ds.h`: arena allocator, `s8` string type, file I/O, process helpers, numeric conversion
- `src/image.h`: image container, rescaling, compositing, simple PPM writing
- `src/font.h`: FreeType-backed text measurement and raster drawing
- `src/networking.h`: libcurl download wrapper
- `src/cache.h`: cache directory discovery and recursive deletion
- `src/threads.h`: semaphore and gate helpers on top of `pthread`
- `src/config.h`: positioning and size constants for the time/date overlay

Small utility and packaging files:

- `src/hex_dump.c`: converts `resources/font.ttf` into a generated C header
- `src/recs.rc`: Windows icon resource
- `scripts/bundle_dylibs.sh`: macOS production bundling helper
- `Entitlements.plist`, `product_definition.plist`: packaging support files

Vendored dependencies:

- `third_party/libwebp`
- `third_party/freetype`

Generated build outputs:

- `build/tmp/raw_font_buf.h`
- `build/tmp/app_name.h`
- `build/app_name.nsh`
- platform-specific binaries and bundles under `build/`

## 3. Build and Bootstrap Pipeline

### 3.1 `configure.sh`

`configure.sh` is the staging step for third-party code.

What it does:

- Detects the OS with `uname`
- Deletes and recreates `third_party/real`
- Copies vendored dependencies into `third_party/real`
- Builds the pieces that the main build expects

Platform behavior:

- Windows:
  - Downloads a prebuilt curl zip from `https://curl.se/windows/dl-8.15.0_7/...`
  - Unpacks it into `third_party/real/curl`
  - Copies and builds FreeType
  - Copies and builds libwebp with `make -f makefile.unix`
- Linux:
  - Copies libwebp and builds it
- macOS:
  - Copies libwebp and builds it with `-arch arm64 -arch x86_64`

Important characteristics:

- The script is destructive to `third_party/real`; local edits there are not durable
- curl is only staged on Windows
- FreeType is only explicitly built on Windows; Linux and macOS rely on system or package-managed FreeType at compile time

### 3.2 `build.sh`

`build.sh` is the main build entry point.

What it always does:

- Removes and recreates `build/` and `build/tmp/`
- Copies `resources/favicon.ico` into `build/`
- Copies `resources/font.ttf` into `build/tmp/`
- Builds `src/hex_dump.c`
- Generates `build/tmp/raw_font_buf.h` from the font file
- Generates app-name headers for C and NSIS
- Chooses build flags based on `development` vs `production`
- Detects the OS and selects the correct platform host

Generated artifacts consumed by the app:

- `raw_font_buf.h`: embeds the runtime font bytes into the program
- `app_name.h`: provides `APP_NAME`

Platform behavior:

- Windows:
  - Uses `src/main_win32.c`
  - Links staged curl, staged libwebp, and built FreeType
  - Builds `build/recs.res` from `src/recs.rc`
  - Copies `libcurl-x64.dll` and `curl-ca-bundle.crt` into `build/`
- Linux:
  - Uses `src/main_x11.c`
  - Links system curl, system X11/Xinerama, system FreeType, and staged libwebp
  - Enables ASan and UBSan in development builds
- macOS:
  - Uses `src/main_osx.m`
  - Links `-lwebp`, system curl, FreeType via `pkg-config`, and Cocoa/ServiceManagement
  - Requests universal output with `-arch x86_64 -arch arm64`
  - Builds an app bundle at `build/WallpaperWorks.app`
  - Copies `favicon.icns`, status bar icons, CA bundle, and `src/Info.plist` into the bundle
  - Codesigns the app ad hoc in development builds
  - Runs `scripts/bundle_dylibs.sh` for production builds

Important build assumptions:

- The `resources/` directory is required but not tracked in the repository
- `build.sh` currently assumes all of these files exist:
  - `resources/favicon.ico`
  - `resources/font.ttf`
  - `resources/favicon.icns`
  - `resources/status_bar_icon_off.png`
  - `resources/status_bar_icon_on.png`
  - `resources/curl-ca-bundle.crt`
- The macOS branch in the current checkout links `-lwebp`, so it expects an install-visible libwebp rather than explicitly linking the staged static archive

### 3.3 `build_installer.sh`

This script only supports the Windows installer path.

It:

- Resolves an NSIS executable path
- Fails clearly if NSIS is missing
- Fails if `build/` does not exist
- Invokes `src/installer.nsi`

## 4. Shared Runtime Architecture

The shared runtime lives in `src/main.c`. It owns the program's behavior but not the platform windowing details.

### 4.1 Core data model

`src/main.c` defines:

- `Background`: wraps an `Image`
- `Monitor`: one screen-sized render target plus scaled wallpaper and two font instances
- `Context`: fixed-size monitor array, monitor count, and `skip_image` flag

Global state:

- `ctx`: process-wide monitor state
- `unscaled_background`: last downloaded full-size image
- `scaled_lock`: guards per-monitor scaled images
- `unscaled_lock`: guards the shared unscaled source image
- `needs_scaling`: semaphore used to wake the resize thread
- `not_paused`: gate used to stop work during sleep or equivalent pauses
- `font_lib`: shared FreeType library handle

This architecture assumes a single process-wide wallpaper source and up to `MAX_PLATFORM_MONITORS` monitors.

### 4.2 Image acquisition flow

`get_random_image()` is the fetch-and-cache function.

Remote source:

- Base URL: `https://infotoast.org/images`
- Metadata endpoints:
  - `/library.md5`
  - `/num.txt`
- Image payloads:
  - `/<random-number>.webp`

Fetch behavior:

- Determine the cache directory from `APP_NAME`
- Try to download the library MD5
- If the MD5 changed, delete and recreate the cache directory
- Try to download `num.txt` to learn the number of images
- Pick a random image number and try up to 10 downloads
- Save successfully downloaded images into the cache
- If network access fails, fall back to random cached files
- If neither network nor cache yields an image, the caller treats that as fatal

Important details:

- Cache invalidation is entire-directory, not per-file
- The image namespace is flat
- The cache fallback ignores `num.txt` and `library.md5`
- The code prints chosen image names to stdout/stderr during operation

### 4.3 Background thread

`background_thread()` is responsible for acquiring new source images.

Its loop:

- Wait on the pause gate
- Initialize curl once
- Download or retrieve one image
- Decode the WebP with `WebPDecodeRGBA`
- Convert the decoded buffer into the internal `Color` layout
- Sleep until the refresh timeout expires unless this is the first image or `skip_image` is requested
- Replace `unscaled_background`
- Signal `needs_scaling`

Timing behavior:

- The refresh interval is currently hardcoded to 60 seconds
- The wait loop checks `ctx.skip_image` during the sleep window

### 4.4 Resize thread

`resize_thread()` handles per-monitor scaling.

Its loop:

- Wait on the pause gate
- Wait for `needs_scaling`
- For each active monitor:
  - Copy the current unscaled background under lock
  - Rescale it to that monitor's screen size
  - Replace the monitor's `scaled_background`

This design isolates image scaling from the UI/render loops, but it rescales the entire source image separately for every monitor whenever a new source image arrives.

### 4.5 Startup barrier

`start()` creates the background and resize threads, loads fonts, and waits until every monitor has a scaled background before allowing the platform host's main render loop to continue.

This is the shared runtime's readiness barrier.

### 4.6 Per-frame composition

`app_loop()` performs the final composition for one monitor.

It:

- Exits early if the pause gate is closed
- Rebuilds the wallpaper frame if needed
- Builds the current `HH:MM` time string
- Builds a `Day, Month Date` string
- Copies the wallpaper frame into the monitor screen buffer
- Measures and draws the time text
- Measures and draws the date text

The actual overlay layout is controlled by `src/config.h`:

- `time_x`, `time_y`, `time_size`, `time_shadow_x`, `time_shadow_y`
- `date_x`, `date_y`, `date_size`, `date_shadow_x`, `date_shadow_y`

The current rendering strategy repaints the whole target rather than incrementally updating only the changed text region.

Important current-state note:

- `src/main.c` currently defines `app_loop(int monitor_i, bool wallpaper_dirty, time_t now)`
- `src/main_x11.c` calls that three-argument form
- `src/main_win32.c` and `src/main_osx.m` still call the older one-argument form

That means the shared/runtime contract has drifted across platform hosts in the current checkout.

## 5. Shared Support Layer

### 5.1 `src/ds.h`

This is the program's small utility runtime.

Major responsibilities:

- Basic typedefs
- `Arena` allocator
- `s8` string/slice type
- File reads and writes
- String concatenation and hashing
- Numeric conversion
- Shell command execution via `popen`

Observations:

- It is header-only and implementation-enabled by `#define DS_IMPL`
- Arena allocation falls back to `calloc` when passed `NULL`
- Some error values are encoded by returning negative `s8.len`

This file underpins most of the rest of the codebase.

### 5.2 `src/image.h`

This is the image manipulation layer.

Major responsibilities:

- `Image` structure with slice semantics
- Pixel addressing with `img_at()`
- Allocation and copying
- Bilinear scaling
- Alpha blending
- Image compositing
- PPM file writing

Important design points:

- `Image` carries `alloc_w` so sub-images can share backing memory
- Out-of-bounds pixel access returns a pointer to `Image.none` instead of asserting
- Rescaling is done in software and is CPU-bound

### 5.3 `src/font.h`

This file wraps FreeType for text rasterization.

Major responsibilities:

- Initialize FreeType
- Load the embedded font from memory
- Measure text bounds
- Draw glyphs into an `Image`
- Draw optional shadowed text

The main application uses two font instances per monitor:

- time font
- date font

Both are derived from the same embedded font bytes, sized relative to the monitor's smaller dimension.

### 5.4 `src/networking.h`

This is a very thin libcurl wrapper.

Major responsibilities:

- Collect response bytes into an arena-backed `s8`
- Apply the request timeout
- Set `CURLOPT_CAINFO` on Windows and macOS
- Return payload bytes plus HTTP status code

Security and transport notes:

- Certificate validation is delegated to libcurl and the chosen CA bundle
- The code does not explicitly pin TLS versions or cipher suites
- The code does not implement retries or backoff beyond higher-level image-selection retries

### 5.5 `src/cache.h`

This file manages the cache directory.

Major responsibilities:

- Derive a per-app cache path
- Create parent and app cache directories if missing
- Recursively delete the cache tree with `nftw`

Platform cache roots:

- macOS: `$HOME/Library/Caches`
- Linux: `$XDG_CACHE_HOME` or `$HOME/.cache`
- Windows: `$HOME/.cache`

Notable behavior:

- Cache creation is idempotent enough for repeated runs
- Cache invalidation is destructive and unconditional when `library.md5` changes

### 5.6 `src/threads.h`

This file defines two simple synchronization helpers:

- `Semaphore`: counts pending work items
- `Gate`: pause/resume barrier

The shared runtime uses them as:

- `needs_scaling`: work queue trigger
- `not_paused`: global pause switch

## 6. Platform Hosts

Each platform file performs two jobs:

- Define platform-native types, constants, and rendering details
- Include `src/main.c` so the shared logic can bind to that platform

This inclusion model is unusual but central to understanding the codebase.

### 6.1 Windows host: `src/main_win32.c`

Responsibilities:

- Discover desktop-parenting windows such as `Progman`, `WorkerW`, and `SHELLDLL_DefView`
- Create one hidden/background window per monitor
- Attach windows into the desktop window hierarchy
- Draw pixels with `StretchDIBits`
- Show a system tray icon and popup menu
- Register the app to auto-run at login via `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`

Main loop behavior:

- Create one `Win` per monitor
- Bind each `Win` buffer into `ctx.monitors[i].screen`
- Start the shared runtime
- For each iteration:
  - Refresh monitor screen bindings
  - Run `app_loop()`
  - Wait for timer or UI events
  - Handle tray menu commands
  - Blit the composed pixels to the window

User-visible controls:

- Tray icon
- "Skip image"
- "Quit"

Important traits:

- Auto-start is implicit on Windows startup after first run
- The platform code uses event queues instead of calling shared logic from the window callback
- Errors intentionally crash by writing through a null pointer in the `err` macro
- The host currently calls an older one-argument `app_loop()` form and has drifted from the shared core's current signature

### 6.2 Linux/X11 host: `src/main_x11.c`

Responsibilities:

- Open the X display
- Manage a root pixmap for wallpaper rendering
- Query monitors through Xinerama
- Create root-backed or desktop-type windows
- Draw with `XPutImage` and `XCopyArea`
- Detect monitor topology changes and rebuild monitor windows

Main loop behavior:

- Enumerate monitors
- Create one `Win` per monitor and bind it into `ctx`
- Start the shared runtime
- Re-render when:
  - the minute changes
  - the scaled background pointer changes
  - the monitor topology changes
- Sleep using `select()` on the X connection until the next minute or an event

Important traits:

- This host is more topology-aware than the Windows and macOS hosts
- It redraws only when time or background changes, not on a fixed timer cadence
- It assumes Xinerama is present and active

### 6.3 macOS host: `src/main_osx.m`

Responsibilities:

- Build a Cocoa app
- Create a borderless window and `MyDrawingView` per screen
- Use Core Graphics bitmap contexts backed by the shared `Image` buffers
- Maintain an `NSStatusItem` menu
- React to screen changes and sleep/wake notifications
- Register the app as a login item through `SMAppService`

Main objects:

- `MyDrawingView`: owns the screen-sized bitmap buffer and per-view redraw timer
- `AppDelegate`: owns the menu bar status item and lifecycle callbacks

Startup behavior:

- Create the status bar item with "Quit" and "Skip image"
- Register notification observers for screen changes and sleep/wake
- Attempt login-item registration
- Build windows for all current screens
- Start the shared runtime on the first configuration pass

Rendering behavior:

- Each `MyDrawingView` allocates a buffer and writes it into `ctx.monitors`
- A repeating `NSTimer` marks the view dirty every second
- `drawRect:` calls `app_loop()` and then draws the bitmap context into the current graphics context
- The host currently calls an older one-argument `app_loop()` form and has drifted from the shared core's current signature

Important traits:

- `LSUIElement` is enabled in `src/Info.plist`, so the app is agent-style and hidden from the Dock
- Login-item registration is automatic in the current checkout, not gated by explicit user consent
- The host currently redraws once per second, even though only the minute and image changes materially affect output
- `disable_login_item` exists but is not wired into the UI

HIG note:

- See [HIG-issues.md](/Users/william/git/WallpaperWorks/HIG-issues.md) for the dedicated macOS HIG/platform-convention review of the current login-item and agent-app behavior.

## 7. Asset and Bundle Composition

### 7.1 Required runtime/build assets

The repository expects manually supplied resources outside source control.

Consumed by `build.sh`:

- `resources/font.ttf`
- `resources/favicon.ico`

Consumed on macOS:

- `resources/favicon.icns`
- `resources/status_bar_icon_off.png`
- `resources/status_bar_icon_on.png`
- `resources/curl-ca-bundle.crt`

Generated from those inputs:

- `build/tmp/raw_font_buf.h`
- `build/favicon.ico`

### 7.2 macOS bundle metadata

`src/Info.plist` declares:

- bundle name, executable name, identifier, and versions
- hardened runtime
- app sandbox
- network client entitlement
- `LSMinimumSystemVersion` of `10.15`
- `LSUIElement=true`

The bundle is assembled under:

- `build/WallpaperWorks.app/Contents/MacOS/WallpaperWorks`
- `build/WallpaperWorks.app/Contents/Resources/...`

### 7.3 Windows installer

`src/installer.nsi` packages:

- the built executable
- the rest of the build directory
- install/uninstall registry keys
- a Start Menu shortcut

It uses `build/favicon.ico` for the installer icon and `build/app_name.nsh` for the app name macro.

## 8. Control Flow Summary

From process start to wallpaper display:

1. The platform host starts and prepares native windows/views.
2. The platform host populates `ctx.monitors[i].screen`.
3. The platform host includes and calls into the shared runtime via `start()`.
4. `start()` launches:
   - one background image acquisition thread
   - one resize thread
5. The background thread fetches or loads a WebP image and updates `unscaled_background`.
6. The resize thread scales that image for each active monitor.
7. The platform host's render loop or redraw callback calls `app_loop()`.
8. `app_loop()` composites:
   - the scaled wallpaper
   - the current time
   - the current date
9. The platform host blits the final pixel buffer to the desktop surface.

## 9. External Dependencies and Remote Touchpoints

Build-time and runtime dependencies visible in the current checkout:

- libcurl
- libwebp
- FreeType
- pthreads
- Windows APIs, X11/Xinerama, or Cocoa/CoreGraphics/ServiceManagement depending on platform

Remote URLs in the current codebase:

- `https://infotoast.org/images`
- `https://curl.se/windows/dl-8.15.0_7/...`
- README references to GitHub, ImageMagick, NSIS, and an icon conversion site

Operational implications:

- Runtime wallpaper acquisition depends on `infotoast.org`
- Windows bootstrap depends on curl's hosted zip distribution
- There is no local mirror or fallback source defined in the repo

## 10. Notable Design Choices and Risks

### 10.1 Inclusion-based shared core

The platform files include `src/main.c` directly instead of linking against a separate compiled library. That keeps shared code in one place, but it also means:

- platform files must define the right types and macros before inclusion
- compile errors can be harder to localize
- platform-specific assumptions can leak into the shared layer

### 10.2 Header-only implementation pattern

Several support files are both declarations and implementations. This keeps the build simple, but it tightly couples inclusion order and macro definitions.

### 10.3 Fixed-size monitor model

The program assumes `MAX_PLATFORM_MONITORS` and process-global monitor state. Dynamic monitor counts above that limit are not supported.

### 10.4 Coarse cache invalidation

When the remote MD5 changes, the cache directory is deleted and rebuilt wholesale. There is no partial reconciliation.

### 10.5 Render/update efficiency

- macOS redraws every second even though the displayed minute changes only once per minute
- The resize thread rescales the full image for every monitor after each image change
- Full-screen compositing is redone in software

### 10.6 Security posture

Positive traits:

- libcurl uses CA bundles on Windows and macOS
- macOS bundle metadata enables hardened runtime and network client entitlement

Gaps:

- No explicit TLS version floor is set in code
- No certificate pinning
- Remote content is trusted if HTTPS and CA validation succeed
- The runtime does not verify content type or image size before decoding

### 10.7 Platform behavior divergence

Each host behaves differently:

- Windows implicitly installs login startup via the registry
- macOS implicitly attempts login-item registration
- X11 has no comparable auto-start path in this repo

That divergence matters for any future feature meant to be "cross-platform".

### 10.8 Current cross-platform drift

The current checkout is not behaviorally aligned across hosts:

- `src/main.c` expects `wallpaper_dirty` and `now` to be provided to `app_loop()`
- `src/main_x11.c` was updated to that model
- `src/main_win32.c` and `src/main_osx.m` were not

Any future work touching the shared render loop should treat this mismatch as an existing integration fault, not as a documentation ambiguity.

## 11. Practical Orientation for Future Changes

If the change is about:

- wallpaper fetch, caching, timing, or text overlay:
  - start with `src/main.c`
- custom strings, file helpers, or arena behavior:
  - start with `src/ds.h`
- scaling, blending, or pixel operations:
  - start with `src/image.h`
- text layout or font rendering:
  - start with `src/font.h`
- network failures or CA handling:
  - start with `src/networking.h`
- cache location or invalidation:
  - start with `src/cache.h`
- pause/resume or background coordination:
  - start with `src/threads.h`
- Windows desktop attachment or tray behavior:
  - start with `src/main_win32.c`
- Linux wallpaper integration or monitor refresh:
  - start with `src/main_x11.c`
- macOS windows, status item, or login-item behavior:
  - start with `src/main_osx.m` and `src/Info.plist`
- build or packaging failures:
  - start with `configure.sh`, `build.sh`, and `build_installer.sh`

## 12. Short Version

WallpaperWorks is a C program with a shared compositing/runtime core embedded directly into three platform-specific hosts. It downloads WebP images from a fixed remote source, caches them locally, rescales them per monitor, overlays time/date text with FreeType, and pushes the finished pixels into the desktop background using native windowing APIs. The build is shell-driven, expects manually supplied assets, and relies on a small set of header-only support modules rather than a large framework stack.
