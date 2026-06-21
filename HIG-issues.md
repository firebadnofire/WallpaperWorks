# HIG Issues

This file tracks macOS Human Interface Guidelines and closely related Apple platform-convention issues visible in the current checkout.

Scope:

- Current code only
- macOS-specific behavior only
- Findings are limited to issues that can be traced directly to `src/main_osx.m` or `src/Info.plist`

Relevant Apple references validated for this review:

- `SMAppService`: <https://developer.apple.com/documentation/servicemanagement/smappservice>
- `SMAppService.Status.requiresApproval`: <https://developer.apple.com/documentation/servicemanagement/smappservice/status-swift.enum/requiresapproval>

## 1. Silent login-item registration on launch

Status: open

Evidence:

- [src/main_osx.m](/Users/william/git/WallpaperWorks/src/main_osx.m#L297) calls `[[SMAppService mainAppService] registerAndReturnError:&error]` during `applicationDidFinishLaunching`
- [src/Info.plist](/Users/william/git/WallpaperWorks/src/Info.plist#L54) sets `LSUIElement=true`, so the app is hidden from the Dock and behaves like an agent app

Why this is a HIG/platform-convention problem:

- The app enables a background/login behavior immediately at launch, before the user has explicitly opted in.
- Because the app is an agent app, that side effect is easy to miss; the user may not realize the app has enrolled itself to relaunch automatically.
- Apple’s current login-item API surface explicitly models user approval and Login Items settings interaction. From that, it is reasonable to infer that immediate silent registration is not the intended user experience.

Recommended fix:

- Ask for consent before first registration.
- Persist the user’s decision.
- Offer a visible `Launch at Login` control in the status item menu or a settings surface.
- If the service requires approval, direct the user to Login Items settings instead of failing silently.

## 2. No user-facing way to turn launch-at-login off

Status: open

Evidence:

- [src/main_osx.m](/Users/william/git/WallpaperWorks/src/main_osx.m#L317) implements `disable_login_item`
- The status item menu created in [src/main_osx.m](/Users/william/git/WallpaperWorks/src/main_osx.m#L258) only exposes `Quit` and `Skip image`

Why this matters:

- Even if automatic registration were acceptable, users still need an obvious way to inspect and reverse the behavior.
- In an `LSUIElement` app, the status item menu is the primary visible control surface, so omitting a toggle there makes the background behavior harder to understand and harder to undo.

Recommended fix:

- Add a checked `Launch at Login` menu item bound to the current `SMAppService` status.
- Keep the menu state in sync with the real registration state.

## 3. Login-item registration failures are effectively invisible

Status: open

Evidence:

- [src/main_osx.m](/Users/william/git/WallpaperWorks/src/main_osx.m#L301) logs registration failure with `NSLog`, followed by `// TODO: show an alert or something`
- [src/Info.plist](/Users/william/git/WallpaperWorks/src/Info.plist#L54) keeps the app hidden from the Dock

Why this matters:

- In a hidden agent app, `NSLog` is not an adequate user-facing failure path.
- If registration fails or requires approval, the user receives no clear explanation or next step.

Recommended fix:

- Surface failures with a native alert or status item state change.
- If approval is needed, point the user to Login Items settings.
