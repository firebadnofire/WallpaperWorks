#include <Cocoa/Cocoa.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGDisplayConfiguration.h>
#include <CoreGraphics/CGWindowLevel.h>
#include <Security/Security.h>
#include <ServiceManagement/SMAppService.h>
#include <TargetConditionals.h>

#include <dispatch/dispatch.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "ds.h"

static bool platform_format_time_and_date(Arena *perm, time_t now, s8 *time_str, s8 *date_str);
static void platform_scaled_background_updated(int monitor_i);

#define PLATFORM_FORMAT_TIME_AND_DATE(perm, now, time_str, date_str) \
    platform_format_time_and_date((perm), (now), (time_str), (date_str))
#define PLATFORM_SCALED_BACKGROUND_UPDATED(monitor_i) \
    platform_scaled_background_updated((monitor_i))

static void activate_agent_app(void) {
    if (NSApp) [NSApp activateIgnoringOtherApps:YES];
}

static NSModalResponse run_alert(
    NSString *title,
    NSString *message,
    NSString *default_button,
    NSString *alternate_button
) {
    __block NSModalResponse response = NSAlertFirstButtonReturn;

    void (^present_alert)(void) = ^{
        activate_agent_app();

        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:title];
        [alert setInformativeText:message];
        [alert addButtonWithTitle:default_button ?: @"OK"];
        if (alternate_button) [alert addButtonWithTitle:alternate_button];
        response = [alert runModal];
        [alert release];
    };

    if ([NSThread isMainThread]) present_alert();
    else dispatch_sync(dispatch_get_main_queue(), present_alert);

    return response;
}

static void show_message_alert(NSString *title, NSString *message) {
    (void) run_alert(title, message, @"OK", nil);
}

static void fatal_alertf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    NSString *message = [[[NSString alloc]
        initWithFormat:[NSString stringWithUTF8String:fmt]
        arguments:args] autorelease];

    va_end(args);

    show_message_alert(@"WallpaperWorks Error", message);
}

#define err(...) do { \
    fatal_alertf(__VA_ARGS__); \
    exit(1); \
} while (0)

#define warning(...) do { \
    fprintf(stderr, "Warning: "); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while (0)

typedef struct {
    uint8_t c[4];
} Color;

typedef enum {
    COLOR_R,
    COLOR_G,
    COLOR_B,
    COLOR_A,
} PlatformColorEnum;

#define IMAGE_IMPL
#include "image.h"

#define MAX_PLATFORM_MONITORS 100
#include "main.c"

@class AppDelegate;

@interface MyDrawingView : NSView {
    CGContextRef bitmap_ctx;
    unsigned char *buf;
    size_t w, h, monitor_index;
    Color *last_rendered_background;
    time_t last_rendered_minute;
    BOOL force_redraw;
}

- (void) setup;
- (void) cleanup_bitmap_ctx;
- (void) draw_buf;
- (void) request_redraw;
- (void) reset_render_state;

@end

static size_t wins_len = 0;
static NSWindow *wins[MAX_PLATFORM_MONITORS] = {0};
static MyDrawingView *views[MAX_PLATFORM_MONITORS] = {0};
static _Atomic bool user_paused = false;
static AppDelegate *app_delegate_instance = nil;

static bool app_is_user_paused(void) {
    return atomic_load(&user_paused);
}

static void request_redraw_for_monitor(NSUInteger monitor_index);
static void request_redraw_all_views(void);
static void make_win_bg(NSWindow *win);
static void reconfigure_screens(bool first_time);

@implementation MyDrawingView

- (id) initWithFrame : (NSRect) frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        monitor_index = MAX_PLATFORM_MONITORS;
        [self setup];
    }
    return self;
}

- (void) setup {
    [self cleanup_bitmap_ctx];

    w = (size_t) [self bounds].size.width;
    h = (size_t) [self bounds].size.height;

    buf = calloc(h * w * 4, sizeof(unsigned char));

    monitor_index = atomic_fetch_add(&ctx.monitors_len, 1);
    [self reset_render_state];

    ctx.monitors[monitor_index].screen = (Image) {
        .buf = (Color *) buf,
        .alloc_w = (int) w,
        .w = (int) w,
        .h = (int) h,
    };

    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    bitmap_ctx = CGBitmapContextCreate(
        buf,
        w, h,
        8, 4 * w,
        color_space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
    );
    CGColorSpaceRelease(color_space);
}

- (void) cleanup_bitmap_ctx {
    if (bitmap_ctx) {
        CGContextRelease(bitmap_ctx);
        bitmap_ctx = 0;
    }

    free(buf);
    buf = 0;

    if (monitor_index < MAX_PLATFORM_MONITORS) {
        ctx.monitors[monitor_index].screen.buf = 0;
    }
}

- (void) dealloc {
    [self cleanup_bitmap_ctx];
    [super dealloc];
}

- (void) reset_render_state {
    last_rendered_background = 0;
    last_rendered_minute = -1;
    force_redraw = YES;
}

- (void) request_redraw {
    force_redraw = YES;
    [self setNeedsDisplay:YES];
}

- (void) draw_buf {
    if (!buf) return;
    if (gate_is_closed(&not_paused)) return;

    ctx.monitors[monitor_index].screen = (Image) {
        .buf = (Color *) buf,
        .alloc_w = (int) w,
        .w = (int) w,
        .h = (int) h,
    };

    time_t now = time(0);
    time_t minute = now / 60;

    pthread_mutex_lock(&scaled_lock);
        Color *background = ctx.monitors[monitor_index].scaled_background.img.buf;
    pthread_mutex_unlock(&scaled_lock);

    bool wallpaper_dirty = force_redraw || last_rendered_background != background;
    bool minute_dirty = last_rendered_minute != minute;

    if (!wallpaper_dirty && !minute_dirty) return;

    app_loop(monitor_index, wallpaper_dirty, now);

    last_rendered_background = background;
    last_rendered_minute = minute;
    force_redraw = NO;
}

- (void) drawRect : (NSRect) dirtyRect {
    (void) dirtyRect;

    [self draw_buf];

    CGImageRef image = CGBitmapContextCreateImage(bitmap_ctx);
    CGContextRef screen_ctx = (CGContextRef) [[NSGraphicsContext currentContext] CGContext];
    if (image) {
        CGContextDrawImage(screen_ctx, CGRectMake(0, 0, w, h), image);
        CGImageRelease(image);
    }
}

@end

static void request_redraw_for_monitor(NSUInteger monitor_index) {
    if (monitor_index >= wins_len) return;
    if (views[monitor_index]) [views[monitor_index] request_redraw];
}

static void request_redraw_all_views(void) {
    for (NSUInteger i = 0; i < wins_len; i++) {
        request_redraw_for_monitor(i);
    }
}

static void make_win_bg(NSWindow *win) {
    [win setStyleMask:NSWindowStyleMaskBorderless];
    [win setOpaque:NO];
    [win setBackgroundColor:[NSColor clearColor]];
    [win setHasShadow:NO];
    [win setLevel:kCGDesktopWindowLevel - 1];
    [win setCollectionBehavior:
        NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorStationary
    ];
    [win setFrame:[[win screen] frame] display:YES];
    [win setMovable:NO];
}

static void reconfigure_screens(bool first_time) {
    atomic_store(&ctx.monitors_len, 0);

    for (size_t i = 0; i < wins_len; i++) {
        if (wins[i]) {
            [wins[i] close];
            wins[i] = nil;
        }
        if (views[i]) {
            [views[i] cleanup_bitmap_ctx];
            [views[i] removeFromSuperview];
            views[i] = nil;
        }
    }

    NSArray<NSScreen *> *screens = [NSScreen screens];
    wins_len = (size_t) [screens count];

    for (size_t i = 0; i < wins_len; i++) {
        NSRect frame = [screens[i] frame];
        wins[i] = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:NSWindowStyleMaskBorderless
            backing:NSBackingStoreBuffered
            defer:NO
        ];
        make_win_bg(wins[i]);

        views[i] = [[MyDrawingView alloc] initWithFrame:frame];
        [wins[i] setContentView:views[i]];
        [wins[i] orderFrontRegardless];
    }

    if (first_time) {
        start();
        request_redraw_all_views();
    } else {
        make_fonts();
        my_semaphore_increment(&needs_scaling);
    }
}

typedef NS_ENUM(NSInteger, WallpaperLoginItemState) {
    WallpaperLoginItemStateUnavailable = 0,
    WallpaperLoginItemStateDisabled,
    WallpaperLoginItemStateEnabled,
    WallpaperLoginItemStateRequiresApproval,
    WallpaperLoginItemStateNotFound,
};

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
- (void) applicationDidFinishLaunching : (NSNotification *) notification;
- (void) reconfigure_monitors;
- (void) system_will_sleep : (NSNotification *) notification;
- (void) system_did_wake : (NSNotification *) notification;
- (void) next_wallpaper : (id) sender;
- (void) toggle_pause : (id) sender;
- (void) refresh_now : (id) sender;
- (void) toggle_launch_at_login : (id) sender;
- (void) open_login_items_settings : (id) sender;
- (void) clear_cache : (id) sender;
- (void) show_about : (id) sender;
- (void) menuWillOpen : (NSMenu *) menu;
- (void) handle_minute_timer : (NSTimer *) timer;
@property (nonatomic, retain) NSStatusItem *status_item;
@property (nonatomic, retain) NSImage *status_on_img;
@property (nonatomic, retain) NSImage *status_off_img;
@property (nonatomic, retain) NSMenu *status_menu;
@property (nonatomic, retain) NSMenuItem *title_item;
@property (nonatomic, retain) NSMenuItem *warning_item;
@property (nonatomic, retain) NSMenuItem *next_wallpaper_item;
@property (nonatomic, retain) NSMenuItem *pause_item;
@property (nonatomic, retain) NSMenuItem *refresh_item;
@property (nonatomic, retain) NSMenuItem *launch_at_login_item;
@property (nonatomic, retain) NSMenuItem *open_login_items_settings_item;
@property (nonatomic, retain) NSMenuItem *clear_cache_item;
@property (nonatomic, retain) NSMenuItem *about_item;
@property (nonatomic, retain) NSMenuItem *quit_item;
@property (nonatomic, retain) NSTimer *minute_timer;
@property (nonatomic, assign) BOOL system_sleeping;
@end

@implementation AppDelegate

- (NSMenuItem *) add_menu_item_with_title:(NSString *)title action:(SEL)action key:(NSString *)key {
    NSMenuItem *item = [[NSMenuItem alloc]
        initWithTitle:title
        action:action
        keyEquivalent:key ?: @""
    ];
    [item setTarget:self];
    [self.status_menu addItem:item];
    return item;
}

- (WallpaperLoginItemState) current_login_item_state {
    if (@available(macOS 13.0, *)) {
        SMAppService *service = [SMAppService mainAppService];
        switch ([service status]) {
        case SMAppServiceStatusNotRegistered:
            return WallpaperLoginItemStateDisabled;
        case SMAppServiceStatusEnabled:
            return WallpaperLoginItemStateEnabled;
        case SMAppServiceStatusRequiresApproval:
            return WallpaperLoginItemStateRequiresApproval;
        case SMAppServiceStatusNotFound:
            return WallpaperLoginItemStateNotFound;
        default:
            return WallpaperLoginItemStateDisabled;
        }
    }

    return WallpaperLoginItemStateUnavailable;
}

- (void) invalidate_minute_timer {
    [self.minute_timer invalidate];
    self.minute_timer = nil;
}

- (void) schedule_next_minute_redraw {
    [self invalidate_minute_timer];
    if (app_is_user_paused() || self.system_sleeping) return;

    NSTimeInterval now = [[NSDate date] timeIntervalSince1970];
    NSTimeInterval interval = 60.0 - fmod(now, 60.0);
    if (interval < 0.1) interval = 60.0;

    self.minute_timer = [NSTimer scheduledTimerWithTimeInterval:
        interval
        target:self
        selector:@selector(handle_minute_timer:)
        userInfo:nil
        repeats:NO
    ];
}

- (void) handle_minute_timer : (NSTimer *) timer {
    (void) timer;
    request_redraw_all_views();
    [self schedule_next_minute_redraw];
}

- (void) show_first_run_explanation_if_needed {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSString *key = @"FirstRunExplanationShown";

    if ([defaults boolForKey:key]) return;

    [defaults setBool:YES forKey:key];

    show_message_alert(
        @"Welcome to WallpaperWorks",
        @"WallpaperWorks downloads wallpapers from infotoast.org, caches them locally, overlays the time and date, and updates your desktop background. Controls are available from the menu bar."
    );
}

- (void) present_login_item_approval_alert {
    NSModalResponse response = run_alert(
        @"Launch at Login Needs Approval",
        @"macOS requires you to approve WallpaperWorks in Login Items settings before launch at login can finish enabling.",
        @"Open Settings",
        @"Not Now"
    );

    if (response == NSAlertFirstButtonReturn) {
        [self open_login_items_settings:nil];
    }
}

- (void) present_login_item_failure:(NSString *)operation error:(NSError *)error {
    NSString *details = [error localizedDescription] ?: @"Unknown error.";
    NSString *message = [NSString stringWithFormat:@"%@\n\n%@", operation, details];
    show_message_alert(@"Launch at Login", message);
}

- (void) synchronize_menu_state {
    self.pause_item.title = app_is_user_paused() ?
        @"Resume WallpaperWorks" :
        @"Pause WallpaperWorks";

    WallpaperLoginItemState state = [self current_login_item_state];

    self.warning_item.hidden = YES;
    self.warning_item.title = @"";

    switch (state) {
    case WallpaperLoginItemStateEnabled:
        self.launch_at_login_item.enabled = YES;
        self.launch_at_login_item.state = NSControlStateValueOn;
        self.open_login_items_settings_item.enabled = YES;
        break;
    case WallpaperLoginItemStateDisabled:
        self.launch_at_login_item.enabled = YES;
        self.launch_at_login_item.state = NSControlStateValueOff;
        self.open_login_items_settings_item.enabled = YES;
        break;
    case WallpaperLoginItemStateRequiresApproval:
        self.launch_at_login_item.enabled = YES;
        self.launch_at_login_item.state = NSControlStateValueMixed;
        self.open_login_items_settings_item.enabled = YES;
        self.warning_item.hidden = NO;
        self.warning_item.title = @"Launch at Login needs approval in Login Items settings.";
        break;
    case WallpaperLoginItemStateNotFound:
        self.launch_at_login_item.enabled = NO;
        self.launch_at_login_item.state = NSControlStateValueOff;
        self.open_login_items_settings_item.enabled = YES;
        self.warning_item.hidden = NO;
        self.warning_item.title = @"Launch at Login could not be configured in this build.";
        break;
    case WallpaperLoginItemStateUnavailable:
    default:
        self.launch_at_login_item.enabled = NO;
        self.launch_at_login_item.state = NSControlStateValueOff;
        self.open_login_items_settings_item.enabled = NO;
        self.warning_item.hidden = NO;
        self.warning_item.title = @"Launch at Login requires macOS 13 or later.";
        break;
    }
}

- (void) build_status_menu {
    self.status_menu = [[NSMenu alloc] initWithTitle:@""];
    self.status_menu.delegate = self;

    self.title_item = [[NSMenuItem alloc] initWithTitle:@"WallpaperWorks" action:nil keyEquivalent:@""];
    self.title_item.enabled = NO;
    [self.status_menu addItem:self.title_item];

    self.warning_item = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    self.warning_item.enabled = NO;
    self.warning_item.hidden = YES;
    [self.status_menu addItem:self.warning_item];

    [self.status_menu addItem:[NSMenuItem separatorItem]];

    self.next_wallpaper_item = [self add_menu_item_with_title:@"Next Wallpaper" action:@selector(next_wallpaper:) key:@"n"];
    self.pause_item = [self add_menu_item_with_title:@"Pause WallpaperWorks" action:@selector(toggle_pause:) key:@"p"];
    self.refresh_item = [self add_menu_item_with_title:@"Refresh Now" action:@selector(refresh_now:) key:@"r"];

    [self.status_menu addItem:[NSMenuItem separatorItem]];

    self.launch_at_login_item = [self add_menu_item_with_title:@"Launch at Login" action:@selector(toggle_launch_at_login:) key:@""];
    self.open_login_items_settings_item = [self add_menu_item_with_title:@"Open Login Items Settings…" action:@selector(open_login_items_settings:) key:@""];

    [self.status_menu addItem:[NSMenuItem separatorItem]];

    self.clear_cache_item = [self add_menu_item_with_title:@"Clear Cache…" action:@selector(clear_cache:) key:@""];
    self.about_item = [self add_menu_item_with_title:@"About WallpaperWorks" action:@selector(show_about:) key:@""];

    [self.status_menu addItem:[NSMenuItem separatorItem]];

    self.quit_item = [[NSMenuItem alloc]
        initWithTitle:@"Quit WallpaperWorks"
        action:@selector(terminate:)
        keyEquivalent:@"q"
    ];
    [self.quit_item setTarget:NSApp];
    [self.status_menu addItem:self.quit_item];

    self.status_item.menu = self.status_menu;
}

- (void) applicationDidFinishLaunching : (NSNotification *) notification {
    (void) notification;

    app_delegate_instance = self;

    self.status_item = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSVariableStatusItemLength
    ];

    self.status_on_img = [NSImage imageNamed:@"status_bar_icon_on"];
    [self.status_on_img setSize:NSMakeSize(22, 22)];
    [self.status_on_img setTemplate:YES];

    self.status_off_img = [NSImage imageNamed:@"status_bar_icon_off"];
    [self.status_off_img setSize:NSMakeSize(22, 22)];
    [self.status_off_img setTemplate:YES];

    [[self.status_item button] setImage:self.status_off_img];
    [[self.status_item button] setAlternateImage:self.status_on_img];
    [[self.status_item button] setToolTip:@"WallpaperWorks"];

    [self build_status_menu];
    [self synchronize_menu_state];

    [[NSNotificationCenter defaultCenter]
        addObserver:self
        selector:@selector(reconfigure_monitors)
        name:NSApplicationDidChangeScreenParametersNotification
        object:nil
    ];

    [[NSWorkspace sharedWorkspace].notificationCenter addObserver:
        self
        selector:@selector(system_will_sleep:)
        name:NSWorkspaceWillSleepNotification
        object:nil
    ];

    [[NSWorkspace sharedWorkspace].notificationCenter addObserver:
        self
        selector:@selector(system_did_wake:)
        name:NSWorkspaceDidWakeNotification
        object:nil
    ];

    [self show_first_run_explanation_if_needed];

    reconfigure_screens(true);
    [self schedule_next_minute_redraw];
}

- (void) menuWillOpen : (NSMenu *) menu {
    (void) menu;
    [self synchronize_menu_state];
}

- (void) reconfigure_monitors {
    reconfigure_screens(false);
    [self schedule_next_minute_redraw];
}

- (void) system_will_sleep : (NSNotification *) notification {
    (void) notification;
    self.system_sleeping = YES;
    [self invalidate_minute_timer];
    gate_close(&not_paused);
}

- (void) system_did_wake : (NSNotification *) notification {
    (void) notification;
    self.system_sleeping = NO;
    if (!app_is_user_paused()) gate_open(&not_paused);
    reconfigure_screens(false);
    request_redraw_all_views();
    [self schedule_next_minute_redraw];
}

- (void) next_wallpaper : (id) sender {
    (void) sender;
    atomic_store(&ctx.skip_image, true);
}

- (void) toggle_pause : (id) sender {
    (void) sender;

    if (app_is_user_paused()) {
        atomic_store(&user_paused, false);
        if (!self.system_sleeping) gate_open(&not_paused);
        request_redraw_all_views();
        [self schedule_next_minute_redraw];
    } else {
        atomic_store(&user_paused, true);
        [self invalidate_minute_timer];
        gate_close(&not_paused);
    }

    [self synchronize_menu_state];
}

- (void) refresh_now : (id) sender {
    (void) sender;
    request_redraw_all_views();
    [self schedule_next_minute_redraw];
}

- (void) toggle_launch_at_login : (id) sender {
    (void) sender;

    SMAppService *service = nil;
    if (@available(macOS 13.0, *)) {
        service = [SMAppService mainAppService];
    } else {
        show_message_alert(
            @"Launch at Login",
            @"Launch at Login requires macOS 13 or later."
        );
        return;
    }
    WallpaperLoginItemState state = [self current_login_item_state];
    NSError *error = nil;
    BOOL success = NO;

    switch (state) {
    case WallpaperLoginItemStateEnabled:
        success = [service unregisterAndReturnError:&error];
        if (!success) {
            [self present_login_item_failure:@"Could not disable Launch at Login." error:error];
        }
        break;
    case WallpaperLoginItemStateDisabled:
        success = [service registerAndReturnError:&error];
        if (!success) {
            [self present_login_item_failure:@"Could not enable Launch at Login." error:error];
        }
        break;
    case WallpaperLoginItemStateRequiresApproval:
        [self present_login_item_approval_alert];
        break;
    case WallpaperLoginItemStateNotFound:
        show_message_alert(
            @"Launch at Login",
            @"WallpaperWorks could not find its login item service in this build."
        );
        break;
    case WallpaperLoginItemStateUnavailable:
    default:
        show_message_alert(
            @"Launch at Login",
            @"Launch at Login requires macOS 13 or later."
        );
        break;
    }

    [self synchronize_menu_state];

    if ([self current_login_item_state] == WallpaperLoginItemStateRequiresApproval) {
        [self present_login_item_approval_alert];
    }
}

- (void) open_login_items_settings : (id) sender {
    (void) sender;

    if (@available(macOS 13.0, *)) {
        [SMAppService openSystemSettingsLoginItems];
    } else {
        show_message_alert(
            @"Login Items Settings",
            @"Opening Login Items settings requires macOS 13 or later."
        );
    }
}

- (void) clear_cache : (id) sender {
    (void) sender;

    NSModalResponse response = run_alert(
        @"Clear Cached Wallpapers?",
        @"This deletes WallpaperWorks' cached wallpapers. Your current wallpaper stays visible until the next refresh or download.",
        @"Clear Cache",
        @"Cancel"
    );

    if (response != NSAlertFirstButtonReturn) return;

    Arena perm = new_arena(4 * KiB);
    s8 cache_dir = {0};

    pthread_mutex_lock(&cache_lock);
        cache_dir = remake_cache_dir(&perm, s8(APP_NAME));
    pthread_mutex_unlock(&cache_lock);

    if (cache_dir.len <= 0) {
        show_message_alert(
            @"Clear Cache",
            @"WallpaperWorks could not recreate its cache directory."
        );
    }

    free(perm.buf);
}

- (void) show_about : (id) sender {
    (void) sender;
    activate_agent_app();
    [NSApp orderFrontStandardAboutPanel:nil];
}

@end

static void platform_scaled_background_updated(int monitor_i) {
    dispatch_async(dispatch_get_main_queue(), ^{
        request_redraw_for_monitor((NSUInteger) monitor_i);
        [app_delegate_instance schedule_next_minute_redraw];
    });
}

static bool platform_format_time_and_date(Arena *perm, time_t now, s8 *time_str, s8 *date_str) {
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:now];

    NSString *time_text = [NSDateFormatter
        localizedStringFromDate:date
        dateStyle:NSDateFormatterNoStyle
        timeStyle:NSDateFormatterShortStyle
    ];

    NSDateFormatter *date_formatter = [[[NSDateFormatter alloc] init] autorelease];
    [date_formatter setLocalizedDateFormatFromTemplate:@"EEEE MMMM d"];
    NSString *date_text = [date_formatter stringFromDate:date];

    if (!time_text || !date_text) return false;

    const char *time_utf8 = [time_text UTF8String];
    const char *date_utf8 = [date_text UTF8String];
    if (!time_utf8 || !date_utf8) return false;

    *time_str = s8_copy(perm, (s8) {
        .buf = (u8 *) time_utf8,
        .len = (ssize) strlen(time_utf8),
    });
    *date_str = s8_copy(perm, (s8) {
        .buf = (u8 *) date_utf8,
        .len = (ssize) strlen(date_utf8),
    });

    return true;
}

int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    NSApplication *app = [NSApplication sharedApplication];

    AppDelegate *app_delegate = [[AppDelegate alloc] init];
    [app_delegate autorelease];
    [app setDelegate:app_delegate];

    [NSApp run];
    [pool drain];
    return 0;
}
