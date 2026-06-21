# HIG Issues

This file tracks macOS Human Interface Guidelines and closely related Apple platform-convention issues for the current checkout.

Scope:

- current code only
- macOS-specific behavior only
- findings limited to behavior traceable to `src/main_osx.m`, `src/Info.plist`, and the small shared hooks added for the macOS host

Relevant Apple references validated for this review:

- `SMAppService`: <https://developer.apple.com/documentation/servicemanagement/smappservice>
- `SMAppService.Status.requiresApproval`: <https://developer.apple.com/documentation/servicemanagement/smappservice/status-swift.enum/requiresapproval>

## Fixed

### 1. Silent launch-at-login registration on launch

Status: fixed

Previous problem:

- `applicationDidFinishLaunching` automatically called `registerAndReturnError:`, enabling login-item behavior without explicit user action.

Implementation notes:

- `src/main_osx.m`: `applicationDidFinishLaunching` no longer registers the login item during startup.
- `src/main_osx.m`: `toggle_launch_at_login:` is now the only path that enables or disables launch at login.
- `src/main_osx.m`: `current_login_item_state` and `synchronize_menu_state` use the real `SMAppService` status as the source of truth for menu state.

### 2. No visible control surface for launch-at-login

Status: fixed

Previous problem:

- The menu only exposed `Quit` and `Skip image`, while login-item behavior had no visible control.

Implementation notes:

- `src/main_osx.m`: the status item menu now includes `Launch at Login`.
- `src/main_osx.m`: the menu also includes `Open Login Items Settings…`.
- `src/main_osx.m`: `menuWillOpen:` refreshes the checked state from the current `SMAppService` status whenever the menu opens.

### 3. Login-item failures were only logged

Status: fixed

Previous problem:

- Registration and unregistration failures were reduced to `NSLog`, which is not adequate feedback for an `LSUIElement` app.

Implementation notes:

- `src/main_osx.m`: `present_login_item_failure:` now shows user-facing alerts for enable/disable failures.
- `src/main_osx.m`: `present_login_item_approval_alert` explains the approval flow and can open Login Items settings directly.
- `src/main_osx.m`: the menu also shows a disabled warning item when the current `SMAppService` state requires approval or is unavailable.

### 4. The menu bar app was not an honest control surface

Status: fixed

Previous problem:

- The status menu did not expose the app’s main controls clearly enough for an agent-style app.

Implementation notes:

- `src/main_osx.m`: the menu now includes:
  - `WallpaperWorks` title item
  - `Next Wallpaper`
  - `Pause WallpaperWorks` / `Resume WallpaperWorks`
  - `Refresh Now`
  - `Launch at Login`
  - `Open Login Items Settings…`
  - `Clear Cache…`
  - `About WallpaperWorks`
  - `Quit WallpaperWorks`
- `src/main_osx.m`: `clear_cache:` now confirms before deleting cached wallpapers.
- `src/main_osx.m`: `show_first_run_explanation_if_needed` adds first-run transparency about downloads, caching, overlays, and menu-bar controls.

### 5. Permanent one-second redraw loop

Status: fixed

Previous problem:

- `MyDrawingView` kept a repeating one-second `NSTimer` running even though the visible time only changes once per minute.

Implementation notes:

- `src/main_osx.m`: the repeating per-view timer was removed.
- `src/main_osx.m`: redraws are now scheduled by:
  - `handle_minute_timer:`
  - `platform_scaled_background_updated`
  - `reconfigure_monitors`
  - `system_did_wake:`
  - user actions such as `refresh_now:` and `next_wallpaper:`
- `src/main_osx.m`: `MyDrawingView` tracks `last_rendered_background` and `last_rendered_minute` so redraws are minute-boundary and wallpaper-dirty driven.

### 6. Hardcoded English date strings on macOS

Status: fixed

Previous problem:

- The shared `app_loop()` fallback uses hardcoded English day and month names.

Implementation notes:

- `src/main.c`: `app_loop()` now accepts a small platform formatting hook.
- `src/main_osx.m`: `platform_format_time_and_date` formats time and date with `NSDateFormatter`, respecting the user locale and 12/24-hour preferences on macOS.
- Non-macOS hosts keep the existing shared fallback formatting.

### 7. Menu bar icons not adapting to light/dark appearance

Status: fixed

Previous problem:

- Status bar icon images were loaded without template rendering, which risks poor contrast in different menu bar appearances.

Implementation notes:

- `src/main_osx.m`: both status item images are now marked template images with `setTemplate:YES`.

## Still Open

### 1. Launch at Login depends on macOS 13 or later

Status: open

Notes:

- The app still declares `LSMinimumSystemVersion` 10.15 in `src/Info.plist`.
- The implementation uses `SMAppService`, which is only available on newer macOS versions.
- The current code handles this honestly by disabling the control and showing a warning, but full pre-macOS-13 launch-at-login support is not implemented.

### 2. Live bundle validation is blocked in this checkout by missing resources

Status: open

Notes:

- The macOS host changes were implemented in code, but full end-to-end bundle validation through `build.sh` is blocked in this checkout because the `resources/` inputs required by the existing build flow are missing.
- This is a validation limitation, not a remaining HIG defect in the implemented menu-bar behavior.
