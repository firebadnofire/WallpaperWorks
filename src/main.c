#define FONT_IMPL
#include "font.h"

#define CACHE_IMPL
#include "cache.h"

#define NETWORKING_IMPL
#include "networking.h"

#define THREADS_IMPL
#include "threads.h"

#include "../third_party/libwebp/src/webp/decode.h"

#include "recs.h"
#include "../build/tmp/raw_font_buf.h"
#include "../build/tmp/app_name.h"

#include <dirent.h>

int usleep(useconds_t usec);

typedef struct {
    Image img;
} Background;

typedef struct {
    Image screen;
    Background scaled_background;
    Background wallpaper_frame;
    FFont time_font;
    FFont date_font;
} Monitor;

typedef struct {
    Monitor monitors[MAX_PLATFORM_MONITORS];
    _Atomic ssize monitors_len;
    _Atomic bool skip_image;
} Context;

#include "config.h"

#ifndef PLATFORM_FORMAT_TIME_AND_DATE
#define PLATFORM_FORMAT_TIME_AND_DATE(perm, now, time_str, date_str) false
#endif

#ifndef PLATFORM_SCALED_BACKGROUND_UPDATED
#define PLATFORM_SCALED_BACKGROUND_UPDATED(monitor_i) do { \
    (void) (monitor_i); \
} while (0)
#endif

pthread_mutex_t scaled_lock = PTHREAD_MUTEX_INITIALIZER;
Background unscaled_background = {0};
pthread_mutex_t unscaled_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

Semaphore needs_scaling = {
    .cond = PTHREAD_COND_INITIALIZER,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

Gate not_paused  = {
    .cond = PTHREAD_COND_INITIALIZER,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

Context ctx = {0};

FFontLib font_lib = {0};

Color color(u8 r, u8 g, u8 b, u8 a) {
    return (Color) {
        .c[COLOR_R] = r,
        .c[COLOR_G] = g,
        .c[COLOR_B] = b,
        .c[COLOR_A] = a,
    };
}

// TODO: this code sucks
s8 get_random_image(Arena *perm, CURL *curl, s8 cache_dir_name) {
    s8 base = s8("https://infotoast.org/images");
    bool network_mode = true;

    s8 cache_dir = get_or_make_cache_dir(perm, cache_dir_name);
    bool cache_support = true;
    if (cache_dir.len <= 0) {
        cache_support = false;
        warning("Could not get system cache directory. Disabling cache support.");
    }

    if (cache_support) {
        s8 md5_file_name = s8("/library.md5");
        s8 url = s8_newcat(perm, base, md5_file_name);
        s8 path = s8_newcat(perm, cache_dir, md5_file_name);

        DownloadResponse response = download(perm, curl, url);
        network_mode = response.data.len != 0 && response.code == 200;

        if (network_mode) {
            s8 new_md5sum = response.data;
            s8 old_md5sum = s8_read_file(perm, path);

            if (!s8_equals(new_md5sum, old_md5sum)) {
                cache_dir = remake_cache_dir(perm, cache_dir_name);
                if (cache_dir.len <= 0) {
                    cache_support = false;
                    warning(
                        "Could not get system cache directory. "
                        "Disabling cache support."
                    );
                }
            }

            if (cache_support) s8_write_to_file(path, new_md5sum);
        }
    }

    u64 n = 0;
    {
        s8 url = s8_newcat(perm, base, s8("/num.txt"));

        DownloadResponse response = download(perm, curl, url);
        network_mode = response.data.len != 0 && response.code == 200;

        if (network_mode) n = s8_to_u64(response.data);
    }

    s8 img_data = {0};
    if (network_mode) {
        int times_tried = 0;
try_downloading_another_one:
        times_tried += 1;
        if (times_tried >= 10) goto pick_random_downloaded_image;

        s8 a0 = s8_copy(perm, s8("/"));
        s8 al = u64_to_s8(perm, rand() % (n + 1), 0);
        s8 name = s8_masscat(*perm, a0, al);

        s8_print(name);
        printf("\n");

        s8 url = s8_newcat(perm, base, s8_newcat(perm, name, s8(".webp")));
        s8 path = s8_newcat(perm, cache_dir, name);

        if (cache_support) img_data = s8_read_file(perm, path);

        // File was not found
        if (img_data.len <= 0) {
            if (!network_mode) goto pick_random_downloaded_image;

            DownloadResponse response = download(perm, curl, url);
            if (response.code != 200) goto try_downloading_another_one;

            img_data = response.data;
            network_mode = img_data.len > 0 && response.code == 200;

            if (!network_mode) goto pick_random_downloaded_image;
            if (cache_support) s8_write_to_file(path, img_data);
        }
    } else pick_random_downloaded_image: {
        if (!cache_support) return (s8) {0};

        struct {
            char (*buf)[255];
            ssize len;
        } files = {0};

        {
            DIR *dirp = opendir((char *) s8_newcat(perm, cache_dir, s8("\0")).buf);
            if (!dirp) return (s8) {0};

            while (true) {
                struct dirent *d = readdir(dirp);
                if (!d) break;
                if (!strcmp(d->d_name, ".") ||
                    !strcmp(d->d_name, "..") ||
                    !strcmp(d->d_name, "num.txt") ||
                    !strcmp(d->d_name, "library.md5") ||
                    0) continue;
                char (*entry)[255] = new(perm, *files.buf, 1);
                files.buf = files.buf ? files.buf : entry;
                memmove(*entry, d->d_name, arrlen(*entry));
                // printf("%s\n", files.buf[files.len]);
                files.len += 1;
            }

            if (!files.len) return (s8) {0};
            closedir(dirp);
        }

        while (true) {
            ssize number = rand() % files.len;
            char *name = files.buf[number];
            printf("'%s'\n", name);

            s8 a0 = s8_copy(perm, cache_dir);
                s8_copy(perm, s8("/"));
            s8 al = s8_copy(perm, (s8) { .buf = (u8 *) name, .len = strlen(name)});
            s8 path = s8_masscat(*perm, a0, al);

            img_data = s8_read_file(perm, path);
            printf("Offline image: ");
            s8_print(path);
            printf("\n");
            break;
        }
    }

    return img_data;
}

void *background_thread(void *arg) {
    (void) arg;
    // TODO: make the memory management around this cleaner
    srand(time(0));

    Arena perm = new_arena(1 * GiB);

    CURL *curl = curl_easy_init(); 
    if (!curl) err("Failed to initialize libcurl.");

#ifdef _WIN32
    curl_easy_setopt(curl, CURLOPT_CAINFO, "./curl-ca-bundle.crt");
#endif

    int timeout_s = 60;
    bool initial = true;

    while (true) {
        gate_wait(&not_paused);

        Arena scratch = perm;

        time_t initial_time = time(0);
        Image decoded = {0};

        pthread_mutex_lock(&cache_lock);
        s8 img_data = get_random_image(&scratch, curl, s8(APP_NAME));
        pthread_mutex_unlock(&cache_lock);
        if (!img_data.buf) {
            err(
                "No images are saved in cache. "
                "You have to connect to the internet to run this application."
            );
        }

        // TODO: check this
        WebPGetInfo(
            img_data.buf, img_data.len,
            &decoded.w, &decoded.h
        );
        printf("Decoding image of size %dx%d: \n", decoded.w, decoded.h);

        Image decoded_tmp = {
            .alloc_w = decoded.w,
            .w = decoded.w,
            .h = decoded.h,
        };
        decoded_tmp.buf = (Color *) WebPDecodeRGBA(
            img_data.buf, img_data.len,
            &decoded.w, &decoded.h
        );

        decoded = new_img(0, decoded);

        for (int x = 0; x < decoded.w; x++) {
            for (int y = 0; y < decoded.h; y++) {
                Color d = *img_at(&decoded_tmp, x, y);
                *img_at(&decoded, x, y) = color(d.c[0], d.c[1], d.c[2], 255);
            }
        }

        WebPFree(decoded_tmp.buf);

        int wait = timeout_s - (time(0) - initial_time);
        for (int i = 0; i < 10 * wait && !initial && !atomic_load(&ctx.skip_image); i++) {
            usleep(1000000 / 10);
            gate_wait(&not_paused);
        }

        pthread_mutex_lock(&unscaled_lock);
            if (atomic_load(&ctx.skip_image)) atomic_store(&ctx.skip_image, false);
            free(unscaled_background.img.buf);
            unscaled_background = (Background) { .img = decoded, };
            my_semaphore_increment(&needs_scaling);
        pthread_mutex_unlock(&unscaled_lock);

        initial = false;
    }

    curl_easy_cleanup(curl);
}

void *resize_thread(void *arg) {
    (void) arg;

    while (true) {
        gate_wait(&not_paused);
        my_semaphore_wait(&needs_scaling);

        for (int i = 0; i < atomic_load(&ctx.monitors_len); i++) {
            pthread_mutex_lock(&unscaled_lock);
                Image img = rescale_img(
                    0,
                    unscaled_background.img,
                    ctx.monitors[i].screen.w,
                    ctx.monitors[i].screen.h
                );
            pthread_mutex_unlock(&unscaled_lock);

            pthread_mutex_lock(&scaled_lock);
                free(ctx.monitors[i].scaled_background.img.buf);
                ctx.monitors[i].scaled_background = (Background) {0};
                ctx.monitors[i].scaled_background.img = img;
            pthread_mutex_unlock(&scaled_lock);

            PLATFORM_SCALED_BACKGROUND_UPDATED(i);
        }
    }
}

void make_fonts() {
    font_lib = init_ffont();

    for (int i = 0; i < atomic_load(&ctx.monitors_len); i++) {
        Monitor *monitor = &ctx.monitors[i];
        int min_dim = monitor->screen.w < monitor->screen.h ?
                      monitor->screen.w :
                      monitor->screen.h;

        load_font(&monitor->time_font, font_lib, (u8 *) raw_font_buf, raw_font_buf_len);
        FT_Set_Pixel_Sizes(
            monitor->time_font.face,
            min_dim * time_size,
            min_dim * time_size
        );

        load_font(&monitor->date_font, font_lib, (u8 *) raw_font_buf, raw_font_buf_len);
        FT_Set_Pixel_Sizes(
            monitor->date_font.face,
            min_dim * date_size,
            min_dim * date_size
        );
    }
}

void rebuild_wallpaper_frame(int monitor_i) {
    Monitor *monitor = &ctx.monitors[monitor_i];

    if (monitor->wallpaper_frame.img.w != monitor->screen.w ||
        monitor->wallpaper_frame.img.h != monitor->screen.h) {
        free(monitor->wallpaper_frame.img.buf);
        monitor->wallpaper_frame.img = new_img(0, monitor->screen);
    }

    for (int x = 0; x < monitor->wallpaper_frame.img.w; x++) {
        for (int y = 0; y < monitor->wallpaper_frame.img.h; y++) {
            *img_at(&monitor->wallpaper_frame.img, x, y) = color(0, 0, 0, 255);
        }
    }

    pthread_mutex_lock(&scaled_lock);
        place_img(monitor->wallpaper_frame.img, monitor->scaled_background.img, 0, 0, 0);
    pthread_mutex_unlock(&scaled_lock);
}

void start() {
    {
        pthread_t thread = 0;
        if (pthread_create(&thread, 0, background_thread, 0)) {
            err("Failed to create background thread.");
        }
    }

    {
        pthread_t thread = 0;
        if (pthread_create(&thread, 0, resize_thread, 0)) {
            err("Failed to create resize thread.");
        }
    }

    // TODO: update the font sizes whenever the screen resizes

    make_fonts();

    while (true) {
        bool stop = true;
        for (int i = 0; i < atomic_load(&ctx.monitors_len); i++) {
            pthread_mutex_lock(&scaled_lock);
                stop = ctx.monitors[i].scaled_background.img.buf != 0;
            pthread_mutex_unlock(&scaled_lock);
            if (!stop) break;
        }
        if (stop) break;
        usleep(1000000 / 20);
    }
}

void app_loop(int monitor_i, bool wallpaper_dirty, time_t now) {
    if (gate_is_closed(&not_paused)) return;

    Monitor *monitor = &ctx.monitors[monitor_i];

    if (wallpaper_dirty) rebuild_wallpaper_frame(monitor_i);

    new_static_arena(scratch, 500);

    s8 time_str = {0}, date_str = {0};
    if (!PLATFORM_FORMAT_TIME_AND_DATE(&scratch, now, &time_str, &date_str)) {
        time_t ftime = now;
        struct tm *lt = localtime(&ftime);

        {
            s8 a0 = u64_to_s8(&scratch, lt->tm_hour, 2);
                    s8_copy(&scratch, s8(":"));
            s8 al = u64_to_s8(&scratch, lt->tm_min, 2);
            time_str = s8_masscat(scratch, a0, al);
        }

        {
            s8 months[] = {
                s8("January"), s8("February"), s8("March"), s8("April"),
                s8("May"), s8("June"), s8("July"), s8("August"),
                s8("September"), s8("October"), s8("November"), s8("December"),
            };
            s8 days[] = {
                s8("Sunday"), s8("Monday"), s8("Tuesday"), s8("Wednesday"),
                s8("Thursday"), s8("Friday"), s8("Saturday"),
            };

            s8 a0 = s8_copy(&scratch, days[lt->tm_wday]);
                    s8_copy(&scratch, s8(", "));
                    s8_copy(&scratch, months[lt->tm_mon]);
                    s8_copy(&scratch, s8(" "));
            s8 al = u64_to_s8(&scratch, lt->tm_mday, 0);
            date_str = s8_masscat(scratch, a0, al);
        }
    }

    place_img(monitor->screen, monitor->wallpaper_frame.img, 0, 0, 0);

    Image time_bound = get_bound_of_text(&monitor->time_font, time_str);
    time_bound = draw_text_shadow(
        monitor->screen,
        &monitor->time_font,
        time_str,
        monitor->screen.w - 1 - monitor->screen.w * time_x - time_bound.w,
        monitor->screen.h - 1 - monitor->screen.h * time_y,
        255, 255, 255,
        time_shadow_x * monitor->screen.w, time_shadow_y * monitor->screen.h,
        0, 0, 0,
        true
    );
    Image date_bound = get_bound_of_text(&monitor->date_font, date_str);
    date_bound = draw_text_shadow(
        monitor->screen,
        &monitor->date_font,
        date_str,
        monitor->screen.w - 1 - monitor->screen.w * date_x - date_bound.w,
        monitor->screen.h - 1 - monitor->screen.h * date_y,
        255, 255, 255,
        date_shadow_x * monitor->screen.w, date_shadow_y * monitor->screen.h,
        0, 0, 0,
        true
    );
}
