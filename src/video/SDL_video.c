/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

/* The high-level video driver subsystem */

#include "SDL_sysvideo.h"
#include "SDL_egl_c.h"
#include "SDL_blit.h"
#include "SDL_pixels_c.h"
#include "SDL_rect_c.h"
#include "SDL_video_c.h"
#include "../events/SDL_events_c.h"
#include "../timer/SDL_timer_c.h"

#include <SDL3/SDL_syswm.h>

#if SDL_VIDEO_OPENGL
#include <SDL3/SDL_opengl.h>
#endif /* SDL_VIDEO_OPENGL */

#if SDL_VIDEO_OPENGL_ES && !SDL_VIDEO_OPENGL
#include <SDL3/SDL_opengles.h>
#endif /* SDL_VIDEO_OPENGL_ES && !SDL_VIDEO_OPENGL */

/* GL and GLES2 headers conflict on Linux 32 bits */
#if SDL_VIDEO_OPENGL_ES2 && !SDL_VIDEO_OPENGL
#include <SDL3/SDL_opengles2.h>
#endif /* SDL_VIDEO_OPENGL_ES2 && !SDL_VIDEO_OPENGL */

#if !SDL_VIDEO_OPENGL
#ifndef GL_CONTEXT_RELEASE_BEHAVIOR_KHR
#define GL_CONTEXT_RELEASE_BEHAVIOR_KHR 0x82FB
#endif
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __LINUX__
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Available video drivers */
static VideoBootStrap *bootstrap[] = {
#if SDL_VIDEO_DRIVER_COCOA
    &COCOA_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_WAYLAND
    &Wayland_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_X11
    &X11_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_VIVANTE
    &VIVANTE_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_WINDOWS
    &WINDOWS_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_WINRT
    &WINRT_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_HAIKU
    &HAIKU_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_UIKIT
    &UIKIT_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_ANDROID
    &Android_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_PS2
    &PS2_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_PSP
    &PSP_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_VITA
    &VITA_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_N3DS
    &N3DS_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_KMSDRM
    &KMSDRM_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_RISCOS
    &RISCOS_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_RPI
    &RPI_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_EMSCRIPTEN
    &Emscripten_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_OFFSCREEN
    &OFFSCREEN_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_NGAGE
    &NGAGE_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_DUMMY
    &DUMMY_bootstrap,
#if SDL_INPUT_LINUXEV
    &DUMMY_evdev_bootstrap,
#endif
#endif
    NULL
};

#define CHECK_WINDOW_MAGIC(window, retval)                              \
    if (!_this) {                                                       \
        SDL_UninitializedVideo();                                       \
        return retval;                                                  \
    }                                                                   \
    if (!(window) || (window)->magic != &_this->window_magic) {         \
        SDL_SetError("Invalid window");                                 \
        return retval;                                                  \
    }

#define CHECK_DISPLAY_MAGIC(display, retval)                            \
    if (!display) {                                                     \
        return retval;                                                  \
    }                                                                   \

#if defined(__MACOS__) && defined(SDL_VIDEO_DRIVER_COCOA)
/* Support for macOS fullscreen spaces */
extern SDL_bool Cocoa_IsWindowInFullscreenSpace(SDL_Window *window);
extern SDL_bool Cocoa_SetWindowFullscreenSpace(SDL_Window *window, SDL_bool state);
#endif

/* Convenience functions for reading driver flags */
static SDL_bool ModeSwitchingEmulated(_THIS)
{
    return !!(_this->quirk_flags & VIDEO_DEVICE_QUIRK_MODE_SWITCHING_EMULATED);
}

static SDL_bool DisableUnsetFullscreenOnMinimize(_THIS)
{
    return !!(_this->quirk_flags & VIDEO_DEVICE_QUIRK_DISABLE_UNSET_FULLSCREEN_ON_MINIMIZE);
}

/* Support for framebuffer emulation using an accelerated renderer */

#define SDL_WINDOWTEXTUREDATA "_SDL_WindowTextureData"

typedef struct
{
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    void *pixels;
    int pitch;
    int bytes_per_pixel;
} SDL_WindowTextureData;

static Uint32 SDL_DefaultGraphicsBackends(SDL_VideoDevice *_this)
{
#if (SDL_VIDEO_OPENGL && __MACOS__) || (__IOS__ && !TARGET_OS_MACCATALYST) || __ANDROID__
    if (_this->GL_CreateContext != NULL) {
        return SDL_WINDOW_OPENGL;
    }
#endif
#if SDL_VIDEO_METAL && (TARGET_OS_MACCATALYST || __MACOS__ || __IOS__)
    if (_this->Metal_CreateView != NULL) {
        return SDL_WINDOW_METAL;
    }
#endif
    return 0;
}

static int SDL_CreateWindowTexture(SDL_VideoDevice *_this, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    SDL_RendererInfo info;
    SDL_WindowTextureData *data = SDL_GetWindowData(window, SDL_WINDOWTEXTUREDATA);
    int i;
    int w, h;

    SDL_GetWindowSizeInPixels(window, &w, &h);

    if (data == NULL) {
        SDL_Renderer *renderer = NULL;
        const char *hint = SDL_GetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
        const SDL_bool specific_accelerated_renderer = (hint && *hint != '0' && *hint != '1' &&
                                                        SDL_strcasecmp(hint, "true") != 0 &&
                                                        SDL_strcasecmp(hint, "false") != 0 &&
                                                        SDL_strcasecmp(hint, "software") != 0);

        /* Check to see if there's a specific driver requested */
        if (specific_accelerated_renderer) {
            renderer = SDL_CreateRenderer(window, hint, 0);
            if (renderer == NULL || (SDL_GetRendererInfo(renderer, &info) == -1)) {
                if (renderer) {
                    SDL_DestroyRenderer(renderer);
                }
                return SDL_SetError("Requested renderer for " SDL_HINT_FRAMEBUFFER_ACCELERATION " is not available");
            }
            /* if it was specifically requested, even if SDL_RENDERER_ACCELERATED isn't set, we'll accept this renderer. */
        } else {
            const int total = SDL_GetNumRenderDrivers();
            for (i = 0; i < total; ++i) {
                const char *name = SDL_GetRenderDriver(i);
                if (name && (SDL_strcmp(name, "software") != 0)) {
                    renderer = SDL_CreateRenderer(window, name, 0);
                    if (renderer && (SDL_GetRendererInfo(renderer, &info) == 0) && (info.flags & SDL_RENDERER_ACCELERATED)) {
                        break; /* this will work. */
                    }
                    if (renderer) { /* wasn't accelerated, etc, skip it. */
                        SDL_DestroyRenderer(renderer);
                        renderer = NULL;
                    }
                }
            }
            if (renderer == NULL) {
                return SDL_SetError("No hardware accelerated renderers available");
            }
        }

        SDL_assert(renderer != NULL); /* should have explicitly checked this above. */

        /* Create the data after we successfully create the renderer (bug #1116) */
        data = (SDL_WindowTextureData *)SDL_calloc(1, sizeof(*data));
        if (data == NULL) {
            SDL_DestroyRenderer(renderer);
            return SDL_OutOfMemory();
        }
        SDL_SetWindowData(window, SDL_WINDOWTEXTUREDATA, data);

        data->renderer = renderer;
    } else {
        if (SDL_GetRendererInfo(data->renderer, &info) == -1) {
            return -1;
        }
    }

    /* Free any old texture and pixel data */
    if (data->texture) {
        SDL_DestroyTexture(data->texture);
        data->texture = NULL;
    }
    SDL_free(data->pixels);
    data->pixels = NULL;

    /* Find the first format without an alpha channel */
    *format = info.texture_formats[0];

    for (i = 0; i < (int)info.num_texture_formats; ++i) {
        if (!SDL_ISPIXELFORMAT_FOURCC(info.texture_formats[i]) &&
            !SDL_ISPIXELFORMAT_ALPHA(info.texture_formats[i])) {
            *format = info.texture_formats[i];
            break;
        }
    }

    data->texture = SDL_CreateTexture(data->renderer, *format,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      w, h);
    if (!data->texture) {
        /* codechecker_false_positive [Malloc] Static analyzer doesn't realize allocated `data` is saved to SDL_WINDOWTEXTUREDATA and not leaked here. */
        return -1; /* NOLINT(clang-analyzer-unix.Malloc) */
    }

    /* Create framebuffer data */
    data->bytes_per_pixel = SDL_BYTESPERPIXEL(*format);
    data->pitch = (((w * data->bytes_per_pixel) + 3) & ~3);

    {
        /* Make static analysis happy about potential SDL_malloc(0) calls. */
        const size_t allocsize = (size_t)h * data->pitch;
        data->pixels = SDL_malloc((allocsize > 0) ? allocsize : 1);
        if (!data->pixels) {
            return SDL_OutOfMemory();
        }
    }

    *pixels = data->pixels;
    *pitch = data->pitch;

    /* Make sure we're not double-scaling the viewport */
    SDL_SetRenderViewport(data->renderer, NULL);

    return 0;
}

static SDL_VideoDevice *_this = NULL;
static SDL_atomic_t SDL_messagebox_count;

static int SDL_UpdateWindowTexture(SDL_VideoDevice *unused, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_WindowTextureData *data;
    SDL_Rect rect;
    void *src;
    int w, h;

    SDL_GetWindowSizeInPixels(window, &w, &h);

    data = SDL_GetWindowData(window, SDL_WINDOWTEXTUREDATA);
    if (data == NULL || !data->texture) {
        return SDL_SetError("No window texture data");
    }

    /* Update a single rect that contains subrects for best DMA performance */
    if (SDL_GetSpanEnclosingRect(w, h, numrects, rects, &rect)) {
        src = (void *)((Uint8 *)data->pixels +
                       rect.y * data->pitch +
                       rect.x * data->bytes_per_pixel);
        if (SDL_UpdateTexture(data->texture, &rect, src, data->pitch) < 0) {
            return -1;
        }

        if (SDL_RenderTexture(data->renderer, data->texture, NULL, NULL) < 0) {
            return -1;
        }

        SDL_RenderPresent(data->renderer);
    }
    return 0;
}

static void SDL_DestroyWindowTexture(SDL_VideoDevice *unused, SDL_Window *window)
{
    SDL_WindowTextureData *data;

    data = SDL_SetWindowData(window, SDL_WINDOWTEXTUREDATA, NULL);
    if (data == NULL) {
        return;
    }
    if (data->texture) {
        SDL_DestroyTexture(data->texture);
    }
    if (data->renderer) {
        SDL_DestroyRenderer(data->renderer);
    }
    SDL_free(data->pixels);
    SDL_free(data);
}

int SDL_SetWindowTextureVSync(SDL_Window *window, int vsync)
{
    SDL_WindowTextureData *data;

    data = SDL_GetWindowData(window, SDL_WINDOWTEXTUREDATA);
    if (data == NULL) {
        return -1;
    }
    if (data->renderer == NULL ) {
        return -1;
    }
    return SDL_SetRenderVSync(data->renderer, vsync);
}

static int SDLCALL cmpmodes(const void *A, const void *B)
{
    const SDL_DisplayMode *a = (const SDL_DisplayMode *)A;
    const SDL_DisplayMode *b = (const SDL_DisplayMode *)B;
    if (a->screen_w != b->screen_w) {
        return b->screen_w - a->screen_w;
    } else if (a->screen_h != b->screen_h) {
        return b->screen_h - a->screen_h;
    } else if (a->pixel_w != b->pixel_w) {
        return b->pixel_w - a->pixel_w;
    } else if (a->pixel_h != b->pixel_h) {
        return b->pixel_h - a->pixel_h;
    } else if (SDL_BITSPERPIXEL(a->format) != SDL_BITSPERPIXEL(b->format)) {
        return SDL_BITSPERPIXEL(b->format) - SDL_BITSPERPIXEL(a->format);
    } else if (SDL_PIXELLAYOUT(a->format) != SDL_PIXELLAYOUT(b->format)) {
        return SDL_PIXELLAYOUT(b->format) - SDL_PIXELLAYOUT(a->format);
    } else if (a->refresh_rate != b->refresh_rate) {
        return (int)(b->refresh_rate * 100) - (int)(a->refresh_rate * 100);
    }
    return 0;
}

static int SDL_UninitializedVideo(void)
{
    return SDL_SetError("Video subsystem has not been initialized");
}

int SDL_GetNumVideoDrivers(void)
{
    return SDL_arraysize(bootstrap) - 1;
}

const char *SDL_GetVideoDriver(int index)
{
    if (index >= 0 && index < SDL_GetNumVideoDrivers()) {
        return bootstrap[index]->name;
    }
    return NULL;
}

/*
 * Initialize the video and event subsystems -- determine native pixel format
 */
int SDL_VideoInit(const char *driver_name)
{
    SDL_VideoDevice *video;
    SDL_bool init_events = SDL_FALSE;
    SDL_bool init_keyboard = SDL_FALSE;
    SDL_bool init_mouse = SDL_FALSE;
    SDL_bool init_touch = SDL_FALSE;
    int i = 0;

    /* Check to make sure we don't overwrite '_this' */
    if (_this != NULL) {
        SDL_VideoQuit();
    }

#if !SDL_TIMERS_DISABLED
    SDL_InitTicks();
#endif

    /* Start the event loop */
    if (SDL_InitSubSystem(SDL_INIT_EVENTS) < 0) {
        goto pre_driver_error;
    }
    init_events = SDL_TRUE;
    if (SDL_InitKeyboard() < 0) {
        goto pre_driver_error;
    }
    init_keyboard = SDL_TRUE;
    if (SDL_InitMouse() < 0) {
        goto pre_driver_error;
    }
    init_mouse = SDL_TRUE;
    if (SDL_InitTouch() < 0) {
        goto pre_driver_error;
    }
    init_touch = SDL_TRUE;

    /* Select the proper video driver */
    video = NULL;
    if (driver_name == NULL) {
        driver_name = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    }
    if (driver_name != NULL && *driver_name != 0) {
        const char *driver_attempt = driver_name;
        while (driver_attempt != NULL && *driver_attempt != 0 && video == NULL) {
            const char *driver_attempt_end = SDL_strchr(driver_attempt, ',');
            size_t driver_attempt_len = (driver_attempt_end != NULL) ? (driver_attempt_end - driver_attempt)
                                                                     : SDL_strlen(driver_attempt);

            for (i = 0; bootstrap[i]; ++i) {
                if ((driver_attempt_len == SDL_strlen(bootstrap[i]->name)) &&
                    (SDL_strncasecmp(bootstrap[i]->name, driver_attempt, driver_attempt_len) == 0)) {
                    video = bootstrap[i]->create();
                    break;
                }
            }

            driver_attempt = (driver_attempt_end != NULL) ? (driver_attempt_end + 1) : NULL;
        }
    } else {
        for (i = 0; bootstrap[i]; ++i) {
            video = bootstrap[i]->create();
            if (video != NULL) {
                break;
            }
        }
    }
    if (video == NULL) {
        if (driver_name) {
            SDL_SetError("%s not available", driver_name);
            goto pre_driver_error;
        }
        SDL_SetError("No available video device");
        goto pre_driver_error;
    }

    /* From this point on, use SDL_VideoQuit to cleanup on error, rather than
    pre_driver_error. */
    _this = video;
    _this->name = bootstrap[i]->name;
    _this->next_object_id = 1;
    _this->thread = SDL_ThreadID();

    /* Set some very sane GL defaults */
    _this->gl_config.driver_loaded = 0;
    _this->gl_config.dll_handle = NULL;
    SDL_GL_ResetAttributes();

    _this->current_glwin_tls = SDL_TLSCreate();
    _this->current_glctx_tls = SDL_TLSCreate();

    /* Initialize the video subsystem */
    if (_this->VideoInit(_this) < 0) {
        SDL_VideoQuit();
        return -1;
    }

    /* Make sure some displays were added */
    if (_this->num_displays == 0) {
        SDL_VideoQuit();
        return SDL_SetError("The video driver did not add any displays");
    }

    /* Disable the screen saver by default. This is a change from <= 2.0.1,
       but most things using SDL are games or media players; you wouldn't
       want a screensaver to trigger if you're playing exclusively with a
       joystick, or passively watching a movie. Things that use SDL but
       function more like a normal desktop app should explicitly reenable the
       screensaver. */
    if (!SDL_GetHintBoolean(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, SDL_FALSE)) {
        SDL_DisableScreenSaver();
    }

    /* If we don't use a screen keyboard, turn on text input by default,
       otherwise programs that expect to get text events without enabling
       UNICODE input won't get any events.

       Actually, come to think of it, you needed to call SDL_EnableUNICODE(1)
       in SDL 1.2 before you got text input events.  Hmm...
     */
    if (!SDL_HasScreenKeyboardSupport()) {
        SDL_StartTextInput();
    }

    /* We're ready to go! */
    return 0;

pre_driver_error:
    SDL_assert(_this == NULL);
    if (init_touch) {
        SDL_QuitTouch();
    }
    if (init_mouse) {
        SDL_QuitMouse();
    }
    if (init_keyboard) {
        SDL_QuitKeyboard();
    }
    if (init_events) {
        SDL_QuitSubSystem(SDL_INIT_EVENTS);
    }
    return -1;
}

const char *SDL_GetCurrentVideoDriver(void)
{
    if (_this == NULL) {
        SDL_UninitializedVideo();
        return NULL;
    }
    return _this->name;
}

SDL_VideoDevice *SDL_GetVideoDevice(void)
{
    return _this;
}

SDL_bool SDL_IsVideoContextExternal(void)
{
    return SDL_GetHintBoolean(SDL_HINT_VIDEO_EXTERNAL_CONTEXT, SDL_FALSE);
}

SDL_bool SDL_OnVideoThread(void)
{
    return (_this && SDL_ThreadID() == _this->thread) ? SDL_TRUE : SDL_FALSE;
}

static void SDL_FinalizeDisplayMode(SDL_DisplayMode *mode)
{
    /* Make sure all the fields are set up correctly */
    if (mode->display_scale <= 0.0f) {
        if (mode->screen_w == 0 && mode->screen_h == 0) {
            mode->screen_w = mode->pixel_w;
            mode->screen_h = mode->pixel_h;
        }
        if (mode->pixel_w == 0 && mode->pixel_h == 0) {
            mode->pixel_w = mode->screen_w;
            mode->pixel_h = mode->screen_h;
        }
        if (mode->screen_w > 0) {
            mode->display_scale = (float)mode->pixel_w / mode->screen_w;
        }
    } else {
        if (mode->screen_w == 0 && mode->screen_h == 0) {
            mode->screen_w = (int)SDL_floorf(mode->pixel_w / mode->display_scale);
            mode->screen_h = (int)SDL_floorf(mode->pixel_h / mode->display_scale);
        }
        if (mode->pixel_w == 0 && mode->pixel_h == 0) {
            mode->pixel_w = (int)SDL_ceilf(mode->screen_w * mode->display_scale);
            mode->pixel_h = (int)SDL_ceilf(mode->screen_h * mode->display_scale);
        }
    }

    /* Make sure the screen width, pixel width, and display scale all match */
    if (mode->display_scale != 0.0f) {
        SDL_assert(mode->display_scale > 0.0f);
        SDL_assert(SDL_fabsf(mode->screen_w - (mode->pixel_w / mode->display_scale)) < 1.0f);
        SDL_assert(SDL_fabsf(mode->screen_h - (mode->pixel_h / mode->display_scale)) < 1.0f);
    }
}

SDL_DisplayID SDL_AddBasicVideoDisplay(const SDL_DisplayMode *desktop_mode)
{
    SDL_VideoDisplay display;

    SDL_zero(display);
    if (desktop_mode) {
        SDL_memcpy(&display.desktop_mode, desktop_mode, sizeof(display.desktop_mode));
    }
    return SDL_AddVideoDisplay(&display, SDL_FALSE);
}

SDL_DisplayID SDL_AddVideoDisplay(const SDL_VideoDisplay *display, SDL_bool send_event)
{
    SDL_VideoDisplay *displays, *new_display;
    SDL_DisplayID id = 0;

    displays = (SDL_VideoDisplay *)SDL_malloc((_this->num_displays + 1) * sizeof(*displays));
    if (displays) {
        int i;

        if (_this->displays) {
            /* The display list may contain self-referential pointers to the desktop mode. */
            SDL_memcpy(displays, _this->displays, _this->num_displays * sizeof(*displays));
            for (i = 0; i < _this->num_displays; ++i) {
                if (displays[i].current_mode == &_this->displays[i].desktop_mode) {
                    displays[i].current_mode = &displays[i].desktop_mode;
                }
            }

            SDL_free(_this->displays);
        }

        _this->displays = displays;
        id = _this->next_object_id++;
        new_display = &displays[_this->num_displays++];

        SDL_memcpy(new_display, display, sizeof(*new_display));
        new_display->id = id;
        new_display->device = _this;
        if (display->name) {
            new_display->name = SDL_strdup(display->name);
        } else {
            char name[32];

            SDL_itoa(id, name, 10);
            new_display->name = SDL_strdup(name);
        }

        new_display->desktop_mode.displayID = id;
        new_display->current_mode = &new_display->desktop_mode;
        SDL_FinalizeDisplayMode(&new_display->desktop_mode);

        for (i = 0; i < new_display->num_fullscreen_modes; ++i) {
            new_display->fullscreen_modes[i].displayID = id;
        }

        if (send_event) {
            SDL_SendDisplayEvent(new_display, SDL_EVENT_DISPLAY_CONNECTED, 0);
        }
    } else {
        SDL_OutOfMemory();
    }
    return id;
}

void SDL_DelVideoDisplay(SDL_DisplayID displayID, SDL_bool send_event)
{
    SDL_VideoDisplay *display;
    int display_index = SDL_GetDisplayIndex(displayID);
    if (display_index < 0) {
        return;
    }

    display = &_this->displays[display_index];

    if (send_event) {
        SDL_SendDisplayEvent(display, SDL_EVENT_DISPLAY_DISCONNECTED, 0);
    }

    SDL_free(display->name);
    SDL_ResetFullscreenDisplayModes(display);
    SDL_free(display->desktop_mode.driverdata);
    display->desktop_mode.driverdata = NULL;
    SDL_free(display->driverdata);
    display->driverdata = NULL;

    if (display_index < (_this->num_displays - 1)) {
        SDL_memmove(&_this->displays[display_index], &_this->displays[display_index + 1], (_this->num_displays - display_index - 1) * sizeof(_this->displays[display_index]));
    }
    --_this->num_displays;
}

SDL_DisplayID *SDL_GetDisplays(int *count)
{
    int i;
    SDL_DisplayID *displays;

    displays = (SDL_DisplayID *)SDL_malloc((_this->num_displays + 1) * sizeof(*displays));
    if (displays) {
        if (count) {
            *count = _this->num_displays;
        }

        for (i = 0; i < _this->num_displays; ++i) {
            displays[i] = _this->displays[i].id;
        }
        displays[i] = 0;
    } else {
        if (count) {
            *count = 0;
        }

        SDL_OutOfMemory();
    }
    return displays;
}

SDL_VideoDisplay *SDL_GetVideoDisplay(SDL_DisplayID displayID)
{
    int display_index;

    display_index = SDL_GetDisplayIndex(displayID);
    if (display_index < 0) {
        return NULL;
    }
    return &_this->displays[display_index];
}

SDL_VideoDisplay *SDL_GetVideoDisplayForWindow(SDL_Window *window)
{
    return SDL_GetVideoDisplay(SDL_GetDisplayForWindow(window));
}

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
    if (!_this || _this->num_displays == 0) {
        SDL_UninitializedVideo();
        return 0;
    }
    return _this->displays[0].id;
}

int SDL_GetDisplayIndex(SDL_DisplayID displayID)
{
    int display_index;

    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }

    for (display_index = 0; display_index < _this->num_displays; ++display_index) {
        if (displayID == _this->displays[display_index].id) {
            return display_index;
        }
    }
    return SDL_SetError("Invalid display");
}

SDL_DisplayData *SDL_GetDisplayDriverData(SDL_DisplayID displayID)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    CHECK_DISPLAY_MAGIC(display, NULL);

    return display->driverdata;
}

SDL_DisplayData *SDL_GetDisplayDriverDataForWindow(SDL_Window *window)
{
    return SDL_GetDisplayDriverData(SDL_GetDisplayForWindow(window));
}

const char *SDL_GetDisplayName(SDL_DisplayID displayID)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    CHECK_DISPLAY_MAGIC(display, NULL);

    return display->name;
}

int SDL_GetDisplayBounds(SDL_DisplayID displayID, SDL_Rect *rect)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    CHECK_DISPLAY_MAGIC(display, -1);

    if (rect == NULL) {
        return SDL_InvalidParamError("rect");
    }

    if (_this->GetDisplayBounds) {
        if (_this->GetDisplayBounds(_this, display, rect) == 0) {
            return 0;
        }
    }

    /* Assume that the displays are left to right */
    if (displayID == SDL_GetPrimaryDisplay()) {
        rect->x = 0;
        rect->y = 0;
    } else {
        SDL_GetDisplayBounds(_this->displays[SDL_GetDisplayIndex(displayID) - 1].id, rect);
        rect->x += rect->w;
    }
    rect->w = display->current_mode->screen_w;
    rect->h = display->current_mode->screen_h;
    return 0;
}

static int ParseDisplayUsableBoundsHint(SDL_Rect *rect)
{
    const char *hint = SDL_GetHint(SDL_HINT_DISPLAY_USABLE_BOUNDS);
    return hint && (SDL_sscanf(hint, "%d,%d,%d,%d", &rect->x, &rect->y, &rect->w, &rect->h) == 4);
}

int SDL_GetDisplayUsableBounds(SDL_DisplayID displayID, SDL_Rect *rect)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    CHECK_DISPLAY_MAGIC(display, -1);

    if (rect == NULL) {
        return SDL_InvalidParamError("rect");
    }

    if (displayID == SDL_GetPrimaryDisplay() && ParseDisplayUsableBoundsHint(rect)) {
        return 0;
    }

    if (_this->GetDisplayUsableBounds) {
        if (_this->GetDisplayUsableBounds(_this, display, rect) == 0) {
            return 0;
        }
    }

    /* Oh well, just give the entire display bounds. */
    return SDL_GetDisplayBounds(displayID, rect);
}

SDL_DisplayOrientation SDL_GetDisplayOrientation(SDL_DisplayID displayID)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    CHECK_DISPLAY_MAGIC(display, SDL_ORIENTATION_UNKNOWN);

    return display->orientation;
}

static const SDL_DisplayMode *SDL_GetFullscreenModeMatch(const SDL_DisplayMode *mode)
{
    const SDL_DisplayMode **modes;
    SDL_DisplayMode fullscreen_mode;

    if (mode->screen_w <= 0 || mode->screen_h <= 0) {
        /* Use the desktop mode */
        return NULL;
    }

    SDL_memcpy(&fullscreen_mode, mode, sizeof(fullscreen_mode));
    if (fullscreen_mode.displayID == 0) {
        fullscreen_mode.displayID = SDL_GetPrimaryDisplay();
    }
    SDL_FinalizeDisplayMode(&fullscreen_mode);

    mode = NULL;

    modes = SDL_GetFullscreenDisplayModes(fullscreen_mode.displayID, NULL);
    if (modes) {
        int i;

        /* Search for an exact match */
        if (!mode) {
            for (i = 0; modes[i]; ++i) {
                if (SDL_memcmp(&fullscreen_mode, modes[i], sizeof(fullscreen_mode)) == 0) {
                    mode = modes[i];
                    break;
                }
            }
        }

        /* Search for a mode with the same characteristics */
        if (!mode) {
            for (i = 0; modes[i]; ++i) {
                if (cmpmodes(&fullscreen_mode, modes[i]) == 0) {
                    mode = modes[i];
                    break;
                }
            }
        }

        SDL_free((void *)modes);
    }
    return mode;
}

SDL_bool SDL_AddFullscreenDisplayMode(SDL_VideoDisplay *display, const SDL_DisplayMode *mode)
{
    SDL_DisplayMode *modes;
    SDL_DisplayMode new_mode;
    int i, nmodes;

    /* Finalize the mode for the display */
    SDL_memcpy(&new_mode, mode, sizeof(new_mode));
    new_mode.displayID = display->id;
    SDL_FinalizeDisplayMode(&new_mode);

    /* Make sure we don't already have the mode in the list */
    modes = display->fullscreen_modes;
    nmodes = display->num_fullscreen_modes;
    for (i = 0; i < nmodes; ++i) {
        if (cmpmodes(&new_mode, &modes[i]) == 0) {
            return SDL_FALSE;
        }
    }

    /* Go ahead and add the new mode */
    if (nmodes == display->max_fullscreen_modes) {
        modes = (SDL_DisplayMode *)SDL_malloc((display->max_fullscreen_modes + 32) * sizeof(*modes));
        if (modes == NULL) {
            return SDL_FALSE;
        }

        if (display->fullscreen_modes) {
            /* Copy the list and update the current mode pointer, if necessary. */
            SDL_memcpy(modes, display->fullscreen_modes, nmodes * sizeof(*modes));
            for (i = 0; i < nmodes; ++i) {
                if (display->current_mode == &display->fullscreen_modes[i]) {
                    display->current_mode = &modes[i];
                }
            }

            SDL_free(display->fullscreen_modes);
        }

        display->fullscreen_modes = modes;
        display->max_fullscreen_modes += 32;
    }
    SDL_memcpy(&modes[display->num_fullscreen_modes++], &new_mode, sizeof(new_mode));

    /* Re-sort video modes */
    SDL_qsort(display->fullscreen_modes, display->num_fullscreen_modes,
              sizeof(SDL_DisplayMode), cmpmodes);

    return SDL_TRUE;
}

void SDL_ResetFullscreenDisplayModes(SDL_VideoDisplay *display)
{
    int i;

    for (i = display->num_fullscreen_modes; i--;) {
        SDL_free(display->fullscreen_modes[i].driverdata);
        display->fullscreen_modes[i].driverdata = NULL;
    }
    SDL_free(display->fullscreen_modes);
    display->fullscreen_modes = NULL;
    display->num_fullscreen_modes = 0;
    display->max_fullscreen_modes = 0;
    display->current_mode = &display->desktop_mode;
}

const SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID displayID, int *count)
{
    int i;
    const SDL_DisplayMode **modes;
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    if (count) {
        *count = 0;
    }

    CHECK_DISPLAY_MAGIC(display, NULL);

    if (display->num_fullscreen_modes == 0 && _this->GetDisplayModes) {
        _this->GetDisplayModes(_this, display);
    }

    modes = (const SDL_DisplayMode **)SDL_malloc((display->num_fullscreen_modes + 1) * sizeof(*modes));
    if (modes) {
        if (count) {
            *count = display->num_fullscreen_modes;
        }

        for (i = 0; i < display->num_fullscreen_modes; ++i) {
            modes[i] = &display->fullscreen_modes[i];
        }
        modes[i] = NULL;
    } else {
        if (count) {
            *count = 0;
        }

        SDL_OutOfMemory();
    }
    return modes;
}

const SDL_DisplayMode *SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID displayID, int w, int h, float refresh_rate)
{
    const SDL_DisplayMode **modes;
    const SDL_DisplayMode *mode, *closest = NULL;
    float aspect_ratio;
    int i;

    if (h > 0) {
        aspect_ratio = (float)w / h;
    } else {
        aspect_ratio = 1.0f;
    }

    if (!refresh_rate) {
        mode = SDL_GetDesktopDisplayMode(displayID);
        if (mode) {
            refresh_rate = mode->refresh_rate;
        }
    }

    modes = SDL_GetFullscreenDisplayModes(displayID, NULL);
    if (modes) {
        for (i = 0; modes[i]; ++i) {
            mode = modes[i];

            if (w > mode->pixel_w) {
                /* Out of sorted modes large enough here */
                break;
            }
            if (h > mode->pixel_h) {
                /* Wider, but not tall enough, due to a different aspect ratio.
                 * This mode must be skipped, but closer modes may still follow */
                continue;
            }
            if (closest) {
                float current_aspect_ratio = (float)mode->pixel_w / mode->pixel_h;
                float closest_aspect_ratio = (float)closest->pixel_w / closest->pixel_h;
                if (SDL_fabsf(aspect_ratio - closest_aspect_ratio) < SDL_fabsf(aspect_ratio - current_aspect_ratio)) {
                    /* The mode we already found has a better aspect ratio match */
                    continue;
                }

                if (SDL_fabsf(closest->refresh_rate - refresh_rate) < SDL_fabsf(mode->refresh_rate - refresh_rate)) {
                    /* We already found a mode and the new mode is further from our
                     * refresh rate target */
                    continue;
                }
            }

            closest = mode;
        }
        SDL_free((void *)modes);
    }
    return closest;
}

void SDL_SetDesktopDisplayMode(SDL_VideoDisplay *display, const SDL_DisplayMode *mode)
{
    SDL_memcpy(&display->desktop_mode, mode, sizeof(*mode));
    display->desktop_mode.displayID = display->id;
    SDL_FinalizeDisplayMode(&display->desktop_mode);
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID displayID)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    CHECK_DISPLAY_MAGIC(display, NULL);

    return &display->desktop_mode;
}

void SDL_SetCurrentDisplayMode(SDL_VideoDisplay *display, const SDL_DisplayMode *mode)
{
    display->current_mode = mode;
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID displayID)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(displayID);

    CHECK_DISPLAY_MAGIC(display, NULL);

    /* Make sure our mode list is updated */
    if (display->num_fullscreen_modes == 0 && _this->GetDisplayModes) {
        _this->GetDisplayModes(_this, display);
    }

    return display->current_mode;
}

static int SDL_SetDisplayModeForDisplay(SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    /* Mode switching is being emulated per-window; nothing to do and cannot fail. */
    if (ModeSwitchingEmulated(_this)) {
        return 0;
    }

    if (!mode) {
        mode = &display->desktop_mode;
    }

    if (mode == display->current_mode) {
        return 0;
    }

    /* Actually change the display mode */
    if (_this->SetDisplayMode) {
        int result;

        _this->setting_display_mode = SDL_TRUE;
        result = _this->SetDisplayMode(_this, display, mode);
        _this->setting_display_mode = SDL_FALSE;
        if (result < 0) {
            return -1;
        }
    }

    SDL_SetCurrentDisplayMode(display, mode);

    return 0;
}

/**
 * If x, y are outside of rect, snaps them to the closest point inside rect
 * (between rect->x, rect->y, inclusive, and rect->x + w, rect->y + h, exclusive)
 */
static void SDL_GetClosestPointOnRect(const SDL_Rect *rect, SDL_Point *point)
{
    const int right = rect->x + rect->w - 1;
    const int bottom = rect->y + rect->h - 1;

    if (point->x < rect->x) {
        point->x = rect->x;
    } else if (point->x > right) {
        point->x = right;
    }

    if (point->y < rect->y) {
        point->y = rect->y;
    } else if (point->y > bottom) {
        point->y = bottom;
    }
}

static SDL_DisplayID GetDisplayForRect(int x, int y, int w, int h)
{
    int i, dist;
    SDL_DisplayID closest = 0;
    int closest_dist = 0x7FFFFFFF;
    SDL_Point closest_point_on_display;
    SDL_Point delta;
    SDL_Point center;
    center.x = x + w / 2;
    center.y = y + h / 2;

    if (_this) {
        for (i = 0; i < _this->num_displays; ++i) {
            SDL_VideoDisplay *display = &_this->displays[i];
            SDL_Rect display_rect;
            SDL_GetDisplayBounds(display->id, &display_rect);

            /* Check if the window is fully enclosed */
            if (SDL_GetRectEnclosingPoints(&center, 1, &display_rect, NULL)) {
                return display->id;
            }

            /* Snap window center to the display rect */
            closest_point_on_display = center;
            SDL_GetClosestPointOnRect(&display_rect, &closest_point_on_display);

            delta.x = center.x - closest_point_on_display.x;
            delta.y = center.y - closest_point_on_display.y;
            dist = (delta.x * delta.x + delta.y * delta.y);
            if (dist < closest_dist) {
                closest = display->id;
                closest_dist = dist;
            }
        }
    }

    if (closest == 0) {
        SDL_SetError("Couldn't find any displays");
    }

    return closest;
}

SDL_DisplayID SDL_GetDisplayForPoint(const SDL_Point *point)
{
    return GetDisplayForRect(point->x, point->y, 1, 1);
}

SDL_DisplayID SDL_GetDisplayForRect(const SDL_Rect *rect)
{
    return GetDisplayForRect(rect->x, rect->y, rect->w, rect->h);
}

SDL_DisplayID SDL_GetDisplayForWindowCoordinate(int coordinate)
{
    SDL_DisplayID displayID = 0;

    if (SDL_WINDOWPOS_ISUNDEFINED(coordinate) ||
        SDL_WINDOWPOS_ISCENTERED(coordinate)) {
        displayID = (coordinate & 0xFFFF);
        /* 0 or invalid */
        if (displayID == 0 || SDL_GetDisplayIndex(displayID) < 0) {
            displayID = SDL_GetPrimaryDisplay();
        }
    }
    return displayID;
}

static SDL_DisplayID SDL_GetDisplayForWindowPosition(SDL_Window *window)
{
    SDL_DisplayID displayID = 0;

    CHECK_WINDOW_MAGIC(window, 0);

    if (_this->GetDisplayForWindow) {
        displayID = _this->GetDisplayForWindow(_this, window);
    }

    /* A backend implementation may fail to get a display for the window
     * (for example if the window is off-screen), but other code may expect it
     * to succeed in that situation, so we fall back to a generic position-
     * based implementation in that case. */
    if (!displayID) {
        displayID = SDL_GetDisplayForWindowCoordinate(window->windowed.x);
    }
    if (!displayID) {
        displayID = SDL_GetDisplayForWindowCoordinate(window->windowed.y);
    }
    if (!displayID) {
        displayID = GetDisplayForRect(window->x, window->y, window->w, window->h);
    }
    if (!displayID) {
        /* Use the primary display for a window if we can't find it anywhere else */
        displayID = SDL_GetPrimaryDisplay();
    }
    return displayID;
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
    SDL_DisplayID displayID = 0;

    CHECK_WINDOW_MAGIC(window, 0);

    /* An explicit fullscreen display overrides all */
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        displayID = window->current_fullscreen_mode.displayID;
    }

    if (!displayID && _this->GetDisplayForWindow) {
        displayID = _this->GetDisplayForWindow(_this, window);
    }

    /* A backend implementation may fail to get a display for the window
     * (for example if the window is off-screen), but other code may expect it
     * to succeed in that situation, so we fall back to a generic position-
     * based implementation in that case. */
    if (!displayID) {
        displayID = SDL_GetDisplayForWindowCoordinate(window->windowed.x);
    }
    if (!displayID) {
        displayID = SDL_GetDisplayForWindowCoordinate(window->windowed.y);
    }
    if (!displayID) {
        displayID = GetDisplayForRect(window->x, window->y, window->w, window->h);
    }
    if (!displayID) {
        /* Use the primary display for a window if we can't find it anywhere else */
        displayID = SDL_GetPrimaryDisplay();
    }
    return displayID;
}

void SDL_CheckWindowDisplayChanged(SDL_Window *window)
{
    SDL_DisplayID displayID = SDL_GetDisplayForWindowPosition(window);

    if (displayID != window->last_displayID) {
        int i, display_index;

        /* Sanity check our fullscreen windows */
        display_index = SDL_GetDisplayIndex(displayID);
        for (i = 0; i < _this->num_displays; ++i) {
            SDL_VideoDisplay *display = &_this->displays[i];

            if (display->fullscreen_window == window) {
                if (display_index != i) {
                    if (display_index < 0) {
                        display_index = i;
                    } else {
                        SDL_VideoDisplay *new_display = &_this->displays[display_index];

                        /* The window was moved to a different display */
                        if (new_display->fullscreen_window &&
                            new_display->fullscreen_window != window) {
                            /* Uh oh, there's already a fullscreen window here; minimize it */
                            SDL_MinimizeWindow(new_display->fullscreen_window);
                        }
                        new_display->fullscreen_window = window;
                        display->fullscreen_window = NULL;
                    }
                }
                break;
            }
        }

        SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_DISPLAY_CHANGED, (int)displayID, 0);
    }
}

#if __WINRT__
extern Uint32 WINRT_DetectWindowFlags(SDL_Window *window);
#endif

static void SDL_RestoreMousePosition(SDL_Window *window)
{
    float x, y;

    if (window == SDL_GetMouseFocus()) {
        SDL_GetMouseState(&x, &y);
        SDL_WarpMouseInWindow(window, x, y);
    }
}

static int SDL_UpdateFullscreenMode(SDL_Window *window, SDL_bool fullscreen)
{
    SDL_VideoDisplay *display = NULL;
    SDL_DisplayMode *mode = NULL;

    CHECK_WINDOW_MAGIC(window, -1);

    window->fullscreen_exclusive = SDL_FALSE;

    /* if we are in the process of hiding don't go back to fullscreen */
    if (window->is_hiding && fullscreen) {
        goto done;
    }

    display = SDL_GetVideoDisplayForWindow(window);

    if (fullscreen) {
        mode = (SDL_DisplayMode *)SDL_GetWindowFullscreenMode(window);
        if (mode != NULL) {
            window->fullscreen_exclusive = SDL_TRUE;
        }
    }

#if defined(__MACOS__) && defined(SDL_VIDEO_DRIVER_COCOA)
    /* if the window is going away and no resolution change is necessary,
       do nothing, or else we may trigger an ugly double-transition
     */
    if (SDL_strcmp(_this->name, "cocoa") == 0) { /* don't do this for X11, etc */
        if (window->is_destroying && !window->last_fullscreen_exclusive_display) {
            window->fullscreen_exclusive = SDL_FALSE;
            goto done;
        }

        /* If we're switching between a fullscreen Space and exclusive fullscreen, we need to get back to normal first. */
        if (fullscreen && !window->last_fullscreen_exclusive_display && window->fullscreen_exclusive) {
            if (!Cocoa_SetWindowFullscreenSpace(window, SDL_FALSE)) {
                goto error;
            }
        } else if (fullscreen && window->last_fullscreen_exclusive_display && !window->fullscreen_exclusive) {
            display = SDL_GetVideoDisplayForWindow(window);
            SDL_SetDisplayModeForDisplay(display, NULL);
            if (_this->SetWindowFullscreen) {
                _this->SetWindowFullscreen(_this, window, display, SDL_FALSE);
            }
        }

        if (Cocoa_SetWindowFullscreenSpace(window, fullscreen)) {
            if (Cocoa_IsWindowInFullscreenSpace(window) != fullscreen) {
                goto error;
            }
            goto done;
        }
    }
#elif __WINRT__ && (NTDDI_VERSION < NTDDI_WIN10)
    /* HACK: WinRT 8.x apps can't choose whether or not they are fullscreen
       or not.  The user can choose this, via OS-provided UI, but this can't
       be set programmatically.

       Just look at what SDL's WinRT video backend code detected with regards
       to fullscreen (being active, or not), and figure out a return/error code
       from that.
    */
    if (fullscreen == !(WINRT_DetectWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
        /* Uh oh, either:
            1. fullscreen was requested, and we're already windowed
            2. windowed-mode was requested, and we're already fullscreen

            WinRT 8.x can't resolve either programmatically, so we're
            giving up.
        */
        goto error;
    } else {
        /* Whatever was requested, fullscreen or windowed mode, is already
            in-place.
        */
        goto done;
    }
#endif

    /* Restore the video mode on other displays if needed */
    if (window->last_fullscreen_exclusive_display) {
        int i;

        for (i = 0; i < _this->num_displays; ++i) {
            SDL_VideoDisplay *other = &_this->displays[i];
            if (display != other && other->id == window->last_fullscreen_exclusive_display) {
                SDL_SetDisplayModeForDisplay(other, NULL);
            }
        }
    }

    if (fullscreen) {
        int mode_w = 0, mode_h = 0;
        SDL_bool resized = SDL_FALSE;

        /* Hide any other fullscreen windows */
        if (display->fullscreen_window &&
            display->fullscreen_window != window) {
            SDL_MinimizeWindow(display->fullscreen_window);
        }

        if (SDL_SetDisplayModeForDisplay(display, mode) < 0) {
            goto error;
        }
        if (_this->SetWindowFullscreen) {
            _this->SetWindowFullscreen(_this, window, display, SDL_TRUE);
        }
        display->fullscreen_window = window;

#if defined(__ANDROID__)
        /* Android may not resize the window to exactly what our fullscreen mode is,
         * especially on windowed Android environments like the Chromebook or Samsung DeX.
         * Given this, we shouldn't use the mode size. Android's SetWindowFullscreen
         * will generate the window event for us with the proper final size.
         */
#elif defined(__WIN32__)
        /* This is also unnecessary on Win32 (WIN_SetWindowFullscreen calls SetWindowPos,
         * WM_WINDOWPOSCHANGED will send SDL_EVENT_WINDOW_RESIZED).
         */
#else
        if (mode) {
            mode_w = mode->screen_w;
            mode_h = mode->screen_h;
        } else {
            mode_w = display->desktop_mode.screen_w;
            mode_h = display->desktop_mode.screen_h;
        }
        if (window->w != mode_w || window->h != mode_h) {
            resized = SDL_TRUE;
        }
#endif
        if (resized) {
            SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, mode_w, mode_h);
        } else {
            SDL_OnWindowResized(window);
        }

        /* Restore the cursor position */
        SDL_RestoreMousePosition(window);

    } else {
        if (display->fullscreen_window == window) {
            /* Restore the desktop mode */
            SDL_SetDisplayModeForDisplay(display, NULL);
        }

        if (_this->SetWindowFullscreen) {
            _this->SetWindowFullscreen(_this, window, display, SDL_FALSE);
        }
        if (display->fullscreen_window == window) {
            display->fullscreen_window = NULL;
        }

        SDL_OnWindowResized(window);

        /* Restore the cursor position */
        SDL_RestoreMousePosition(window);
    }

done:
    window->last_fullscreen_exclusive_display = display && window->fullscreen_exclusive ? display->id : 0;
    return 0;

error:
    if (fullscreen) {
        /* Something went wrong and the window is no longer fullscreen. */
        window->flags &= ~SDL_WINDOW_FULLSCREEN;
        SDL_UpdateFullscreenMode(window, SDL_FALSE);
    }
    return -1;
}

int SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (mode) {
        if (!SDL_GetFullscreenModeMatch(mode)) {
            return SDL_SetError("Invalid fullscreen display mode");
        }

        /* Save the mode so we can look up the closest match later */
        SDL_memcpy(&window->requested_fullscreen_mode, mode, sizeof(window->requested_fullscreen_mode));
    } else {
        SDL_zero(window->requested_fullscreen_mode);
    }

    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        SDL_memcpy(&window->current_fullscreen_mode, &window->requested_fullscreen_mode, sizeof(window->current_fullscreen_mode));
        if (SDL_WINDOW_FULLSCREEN_VISIBLE(window)) {
            SDL_UpdateFullscreenMode(window, SDL_TRUE);
        }
    }

    return 0;
}

const SDL_DisplayMode *SDL_GetWindowFullscreenMode(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, NULL);

    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        return SDL_GetFullscreenModeMatch(&window->current_fullscreen_mode);
    } else {
        return SDL_GetFullscreenModeMatch(&window->requested_fullscreen_mode);
    }
}

void *SDL_GetWindowICCProfile(SDL_Window *window, size_t *size)
{
    if (!_this->GetWindowICCProfile) {
        SDL_Unsupported();
        return NULL;
    }
    return _this->GetWindowICCProfile(_this, window, size);
}

Uint32 SDL_GetWindowPixelFormat(SDL_Window *window)
{
    SDL_DisplayID displayID;
    const SDL_DisplayMode *mode;

    CHECK_WINDOW_MAGIC(window, SDL_PIXELFORMAT_UNKNOWN);

    displayID = SDL_GetDisplayForWindow(window);
    mode = SDL_GetCurrentDisplayMode(displayID);
    if (mode) {
        return mode->format;
    } else {
        return SDL_PIXELFORMAT_UNKNOWN;
    }
}

#define CREATE_FLAGS \
    (SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_POPUP_MENU | SDL_WINDOW_UTILITY | SDL_WINDOW_TOOLTIP | SDL_WINDOW_VULKAN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_METAL)

static SDL_INLINE SDL_bool IsAcceptingDragAndDrop(void)
{
    if (SDL_EventEnabled(SDL_EVENT_DROP_FILE) || SDL_EventEnabled(SDL_EVENT_DROP_TEXT)) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

/* prepare a newly-created window */
static SDL_INLINE void PrepareDragAndDropSupport(SDL_Window *window)
{
    if (_this->AcceptDragAndDrop) {
        _this->AcceptDragAndDrop(window, IsAcceptingDragAndDrop());
    }
}

/* toggle d'n'd for all existing windows. */
void SDL_ToggleDragAndDropSupport(void)
{
    if (_this && _this->AcceptDragAndDrop) {
        const SDL_bool enable = IsAcceptingDragAndDrop();
        SDL_Window *window;
        for (window = _this->windows; window; window = window->next) {
            _this->AcceptDragAndDrop(window, enable);
        }
    }
}

static void SDL_FinishWindowCreation(SDL_Window *window, Uint32 flags)
{
    PrepareDragAndDropSupport(window);

    if (flags & SDL_WINDOW_MAXIMIZED) {
        SDL_MaximizeWindow(window);
    }
    if (flags & SDL_WINDOW_MINIMIZED) {
        SDL_MinimizeWindow(window);
    }
    if (flags & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(window, flags);
    }
    if (flags & SDL_WINDOW_MOUSE_GRABBED) {
        /* We must specifically call SDL_SetWindowGrab() and not
           SDL_SetWindowMouseGrab() here because older applications may use
           this flag plus SDL_HINT_GRAB_KEYBOARD to indicate that they want
           the keyboard grabbed too and SDL_SetWindowMouseGrab() won't do that.
        */
        SDL_SetWindowGrab(window, SDL_TRUE);
    }
    if (flags & SDL_WINDOW_KEYBOARD_GRABBED) {
        SDL_SetWindowKeyboardGrab(window, SDL_TRUE);
    }
    if (!(flags & SDL_WINDOW_HIDDEN)) {
        SDL_ShowWindow(window);
    }
}

static int SDL_ContextNotSupported(const char *name)
{
    return SDL_SetError("%s support is either not configured in SDL "
                        "or not available in current SDL video driver "
                        "(%s) or platform",
                        name,
                        _this->name);
}

static int SDL_DllNotSupported(const char *name)
{
    return SDL_SetError("No dynamic %s support in current SDL video driver (%s)", name, _this->name);
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    SDL_Window *window;
    Uint32 type_flags, graphics_flags;

    if (_this == NULL) {
        /* Initialize the video system if needed */
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            return NULL;
        }

        /* Make clang-tidy happy */
        if (_this == NULL) {
            return NULL;
        }
    }

    /* Make sure the display list is up to date for window placement */
    if (_this->RefreshDisplays) {
        _this->RefreshDisplays(_this);
    }

    /* ensure no more than one of these flags is set */
    type_flags = flags & (SDL_WINDOW_UTILITY | SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU);
    if (type_flags & (type_flags - 1)) {
        SDL_SetError("Conflicting window flags specified");
        return NULL;
    }

    /* Some platforms can't create zero-sized windows */
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }

    /* Some platforms blow up if the windows are too large. Raise it later? */
    if ((w > 16384) || (h > 16384)) {
        SDL_SetError("Window is too large.");
        return NULL;
    }

    /* ensure no more than one of these flags is set */
    graphics_flags = flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_METAL | SDL_WINDOW_VULKAN);
    if (graphics_flags & (graphics_flags - 1)) {
        SDL_SetError("Conflicting window flags specified");
        return NULL;
    }

    /* Some platforms have certain graphics backends enabled by default */
    if (!graphics_flags && !SDL_IsVideoContextExternal()) {
        flags |= SDL_DefaultGraphicsBackends(_this);
    }

    if (flags & SDL_WINDOW_OPENGL) {
        if (!_this->GL_CreateContext) {
            SDL_ContextNotSupported("OpenGL");
            return NULL;
        }
        if (SDL_GL_LoadLibrary(NULL) < 0) {
            return NULL;
        }
    }

    if (flags & SDL_WINDOW_VULKAN) {
        if (!_this->Vulkan_CreateSurface) {
            SDL_ContextNotSupported("Vulkan");
            return NULL;
        }
        if (SDL_Vulkan_LoadLibrary(NULL) < 0) {
            return NULL;
        }
    }

    if (flags & SDL_WINDOW_METAL) {
        if (!_this->Metal_CreateView) {
            SDL_ContextNotSupported("Metal");
            return NULL;
        }
    }

    if (!SDL_GetHintBoolean("SDL_VIDEO_HIGHDPI_DISABLED", SDL_FALSE)) {
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    }

    window = (SDL_Window *)SDL_calloc(1, sizeof(*window));
    if (window == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }
    window->magic = &_this->window_magic;
    window->id = _this->next_object_id++;
    window->windowed.x = window->x = x;
    window->windowed.y = window->y = y;
    window->windowed.w = window->w = w;
    window->windowed.h = window->h = h;
    if (SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISUNDEFINED(y) ||
        SDL_WINDOWPOS_ISCENTERED(x) || SDL_WINDOWPOS_ISCENTERED(y)) {
        SDL_VideoDisplay *display = SDL_GetVideoDisplayForWindow(window);
        SDL_Rect bounds;

        SDL_GetDisplayBounds(display->id, &bounds);
        if (SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISCENTERED(x)) {
            window->windowed.x = window->x = bounds.x + (bounds.w - w) / 2;
        }
        if (SDL_WINDOWPOS_ISUNDEFINED(y) || SDL_WINDOWPOS_ISCENTERED(y)) {
            window->windowed.y = window->y = bounds.y + (bounds.h - h) / 2;
        }
    }

    if (flags & SDL_WINDOW_FULLSCREEN) {
        SDL_VideoDisplay *display = SDL_GetVideoDisplayForWindow(window);
        SDL_Rect bounds;

        SDL_GetDisplayBounds(display->id, &bounds);
        window->x = bounds.x;
        window->y = bounds.y;
        window->w = bounds.w;
        window->h = bounds.h;
    }

    window->flags = ((flags & CREATE_FLAGS) | SDL_WINDOW_HIDDEN);
    window->opacity = 1.0f;
    window->next = _this->windows;
    window->is_destroying = SDL_FALSE;
    window->last_displayID = SDL_GetDisplayForWindow(window);

    if (_this->windows) {
        _this->windows->prev = window;
    }
    _this->windows = window;

    if (_this->CreateSDLWindow && _this->CreateSDLWindow(_this, window) < 0) {
        SDL_DestroyWindow(window);
        return NULL;
    }

    /* Clear minimized if not on windows, only windows handles it at create rather than FinishWindowCreation,
     * but it's important or window focus will get broken on windows!
     */
#if !defined(__WIN32__) && !defined(__GDK__)
    if (window->flags & SDL_WINDOW_MINIMIZED) {
        window->flags &= ~SDL_WINDOW_MINIMIZED;
    }
#endif

#if __WINRT__ && (NTDDI_VERSION < NTDDI_WIN10)
    /* HACK: WinRT 8.x apps can't choose whether or not they are fullscreen
       or not.  The user can choose this, via OS-provided UI, but this can't
       be set programmatically.

       Just look at what SDL's WinRT video backend code detected with regards
       to fullscreen (being active, or not), and figure out a return/error code
       from that.
    */
    flags = window->flags;
#endif

    if (title) {
        SDL_SetWindowTitle(window, title);
    }
    SDL_FinishWindowCreation(window, flags);

    /* If the window was created fullscreen, make sure the mode code matches */
    SDL_UpdateFullscreenMode(window, SDL_WINDOW_FULLSCREEN_VISIBLE(window));

    return window;
}

SDL_Window *SDL_CreateWindowFrom(const void *data)
{
    SDL_Window *window;
    Uint32 flags = SDL_WINDOW_FOREIGN;

    if (_this == NULL) {
        SDL_UninitializedVideo();
        return NULL;
    }
    if (!_this->CreateSDLWindowFrom) {
        SDL_Unsupported();
        return NULL;
    }

    if (SDL_GetHintBoolean(SDL_HINT_VIDEO_FOREIGN_WINDOW_OPENGL, SDL_FALSE)) {
        if (!_this->GL_CreateContext) {
            SDL_ContextNotSupported("OpenGL");
            return NULL;
        }
        if (SDL_GL_LoadLibrary(NULL) < 0) {
            return NULL;
        }
        flags |= SDL_WINDOW_OPENGL;
    }

    if (SDL_GetHintBoolean(SDL_HINT_VIDEO_FOREIGN_WINDOW_VULKAN, SDL_FALSE)) {
        if (!_this->Vulkan_CreateSurface) {
            SDL_ContextNotSupported("Vulkan");
            return NULL;
        }
        if (flags & SDL_WINDOW_OPENGL) {
            SDL_SetError("Vulkan and OpenGL not supported on same window");
            return NULL;
        }
        if (SDL_Vulkan_LoadLibrary(NULL) < 0) {
            return NULL;
        }
        flags |= SDL_WINDOW_VULKAN;
    }

    window = (SDL_Window *)SDL_calloc(1, sizeof(*window));
    if (window == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }
    window->magic = &_this->window_magic;
    window->id = _this->next_object_id++;
    window->flags = flags;
    window->is_destroying = SDL_FALSE;
    window->opacity = 1.0f;
    window->next = _this->windows;
    if (_this->windows) {
        _this->windows->prev = window;
    }
    _this->windows = window;

    if (_this->CreateSDLWindowFrom(_this, window, data) < 0) {
        SDL_DestroyWindow(window);
        return NULL;
    }

    window->last_displayID = SDL_GetDisplayForWindow(window);
    PrepareDragAndDropSupport(window);

    return window;
}

int SDL_RecreateWindow(SDL_Window *window, Uint32 flags)
{
    SDL_bool loaded_opengl = SDL_FALSE;
    SDL_bool need_gl_unload = SDL_FALSE;
    SDL_bool need_gl_load = SDL_FALSE;
    SDL_bool loaded_vulkan = SDL_FALSE;
    SDL_bool need_vulkan_unload = SDL_FALSE;
    SDL_bool need_vulkan_load = SDL_FALSE;
    Uint32 graphics_flags;

    /* ensure no more than one of these flags is set */
    graphics_flags = flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_METAL | SDL_WINDOW_VULKAN);
    if (graphics_flags & (graphics_flags - 1)) {
        return SDL_SetError("Conflicting window flags specified");
    }

    if ((flags & SDL_WINDOW_OPENGL) && !_this->GL_CreateContext) {
        return SDL_ContextNotSupported("OpenGL");
    }
    if ((flags & SDL_WINDOW_VULKAN) && !_this->Vulkan_CreateSurface) {
        return SDL_ContextNotSupported("Vulkan");
    }
    if ((flags & SDL_WINDOW_METAL) && !_this->Metal_CreateView) {
        return SDL_ContextNotSupported("Metal");
    }

    if (window->flags & SDL_WINDOW_FOREIGN) {
        /* Can't destroy and re-create foreign windows, hrm */
        flags |= SDL_WINDOW_FOREIGN;
    } else {
        flags &= ~SDL_WINDOW_FOREIGN;
    }

    /* Restore video mode, etc. */
    if (!(window->flags & SDL_WINDOW_FOREIGN)) {
        SDL_HideWindow(window);
    }

    /* Tear down the old native window */
    if (window->surface) {
        window->surface->flags &= ~SDL_DONTFREE;
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
        window->surface_valid = SDL_FALSE;
    }

    if (_this->checked_texture_framebuffer) { /* never checked? No framebuffer to destroy. Don't risk calling the wrong implementation. */
        if (_this->DestroyWindowFramebuffer) {
            _this->DestroyWindowFramebuffer(_this, window);
        }
        _this->checked_texture_framebuffer = SDL_FALSE;
    }

    if ((window->flags & SDL_WINDOW_OPENGL) != (flags & SDL_WINDOW_OPENGL)) {
        if (flags & SDL_WINDOW_OPENGL) {
            need_gl_load = SDL_TRUE;
        } else {
            need_gl_unload = SDL_TRUE;
        }
    } else if (window->flags & SDL_WINDOW_OPENGL) {
        need_gl_unload = SDL_TRUE;
        need_gl_load = SDL_TRUE;
    }

    if ((window->flags & SDL_WINDOW_VULKAN) != (flags & SDL_WINDOW_VULKAN)) {
        if (flags & SDL_WINDOW_VULKAN) {
            need_vulkan_load = SDL_TRUE;
        } else {
            need_vulkan_unload = SDL_TRUE;
        }
    } else if (window->flags & SDL_WINDOW_VULKAN) {
        need_vulkan_unload = SDL_TRUE;
        need_vulkan_load = SDL_TRUE;
    }

    if (need_gl_unload) {
        SDL_GL_UnloadLibrary();
    }

    if (need_vulkan_unload) {
        SDL_Vulkan_UnloadLibrary();
    }

    if (_this->DestroyWindow && !(flags & SDL_WINDOW_FOREIGN)) {
        _this->DestroyWindow(_this, window);
    }

    if (need_gl_load) {
        if (SDL_GL_LoadLibrary(NULL) < 0) {
            return -1;
        }
        loaded_opengl = SDL_TRUE;
    }

    if (need_vulkan_load) {
        if (SDL_Vulkan_LoadLibrary(NULL) < 0) {
            return -1;
        }
        loaded_vulkan = SDL_TRUE;
    }

    window->flags = ((flags & CREATE_FLAGS) | SDL_WINDOW_HIDDEN);
    window->is_destroying = SDL_FALSE;

    if (_this->CreateSDLWindow && !(flags & SDL_WINDOW_FOREIGN)) {
        if (_this->CreateSDLWindow(_this, window) < 0) {
            if (loaded_opengl) {
                SDL_GL_UnloadLibrary();
                window->flags &= ~SDL_WINDOW_OPENGL;
            }
            if (loaded_vulkan) {
                SDL_Vulkan_UnloadLibrary();
                window->flags &= ~SDL_WINDOW_VULKAN;
            }
            return -1;
        }
    }

    if (flags & SDL_WINDOW_FOREIGN) {
        window->flags |= SDL_WINDOW_FOREIGN;
    }

    if (_this->SetWindowTitle && window->title) {
        _this->SetWindowTitle(_this, window);
    }

    if (_this->SetWindowIcon && window->icon) {
        _this->SetWindowIcon(_this, window, window->icon);
    }

    if (window->hit_test) {
        _this->SetWindowHitTest(window, SDL_TRUE);
    }

    SDL_FinishWindowCreation(window, flags);

    return 0;
}

SDL_bool SDL_HasWindows(void)
{
    return _this && _this->windows != NULL;
}

SDL_WindowID SDL_GetWindowID(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, 0);

    return window->id;
}

SDL_Window *SDL_GetWindowFromID(SDL_WindowID id)
{
    SDL_Window *window;

    if (_this == NULL) {
        return NULL;
    }
    for (window = _this->windows; window; window = window->next) {
        if (window->id == id) {
            return window;
        }
    }
    return NULL;
}

Uint32 SDL_GetWindowFlags(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, 0);

    return window->flags;
}

int SDL_SetWindowTitle(SDL_Window *window, const char *title)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (title == window->title) {
        return 0;
    }
    SDL_free(window->title);

    window->title = SDL_strdup(title ? title : "");

    if (_this->SetWindowTitle) {
        _this->SetWindowTitle(_this, window);
    }
    return 0;
}

const char *SDL_GetWindowTitle(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, "");

    return window->title ? window->title : "";
}

int SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (icon == NULL) {
        return 0;
    }

    SDL_DestroySurface(window->icon);

    /* Convert the icon into ARGB8888 */
    window->icon = SDL_ConvertSurfaceFormat(icon, SDL_PIXELFORMAT_ARGB8888);
    if (!window->icon) {
        return -1;
    }

    if (_this->SetWindowIcon) {
        _this->SetWindowIcon(_this, window, window->icon);
    }
    return 0;
}

void *SDL_SetWindowData(SDL_Window *window, const char *name, void *userdata)
{
    SDL_WindowUserData *prev, *data;

    CHECK_WINDOW_MAGIC(window, NULL);

    /* Input validation */
    if (name == NULL || name[0] == '\0') {
        SDL_InvalidParamError("name");
        return NULL;
    }

    /* See if the named data already exists */
    prev = NULL;
    for (data = window->data; data; prev = data, data = data->next) {
        if (data->name && SDL_strcmp(data->name, name) == 0) {
            void *last_value = data->data;

            if (userdata) {
                /* Set the new value */
                data->data = userdata;
            } else {
                /* Delete this value */
                if (prev) {
                    prev->next = data->next;
                } else {
                    window->data = data->next;
                }
                SDL_free(data->name);
                SDL_free(data);
            }
            return last_value;
        }
    }

    /* Add new data to the window */
    if (userdata) {
        data = (SDL_WindowUserData *)SDL_malloc(sizeof(*data));
        data->name = SDL_strdup(name);
        data->data = userdata;
        data->next = window->data;
        window->data = data;
    }
    return NULL;
}

void *SDL_GetWindowData(SDL_Window *window, const char *name)
{
    SDL_WindowUserData *data;

    CHECK_WINDOW_MAGIC(window, NULL);

    /* Input validation */
    if (name == NULL || name[0] == '\0') {
        SDL_InvalidParamError("name");
        return NULL;
    }

    for (data = window->data; data; data = data->next) {
        if (data->name && SDL_strcmp(data->name, name) == 0) {
            return data->data;
        }
    }
    return NULL;
}

int SDL_SetWindowPosition(SDL_Window *window, int x, int y)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (SDL_WINDOWPOS_ISCENTERED(x) || SDL_WINDOWPOS_ISCENTERED(y)) {
        SDL_DisplayID displayID = 0;
        SDL_Rect bounds;

        if (!displayID) {
            displayID = SDL_GetDisplayForWindowCoordinate(x);
        }
        if (!displayID) {
            displayID = SDL_GetDisplayForWindowCoordinate(y);
        }

        SDL_zero(bounds);
        SDL_GetDisplayBounds(displayID, &bounds);
        if (SDL_WINDOWPOS_ISCENTERED(x)) {
            x = bounds.x + (bounds.w - window->windowed.w) / 2;
        }
        if (SDL_WINDOWPOS_ISCENTERED(y)) {
            y = bounds.y + (bounds.h - window->windowed.h) / 2;
        }
    }

    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        if (!SDL_WINDOWPOS_ISUNDEFINED(x)) {
            window->windowed.x = x;
        }
        if (!SDL_WINDOWPOS_ISUNDEFINED(y)) {
            window->windowed.y = y;
        }
    } else {
        if (!SDL_WINDOWPOS_ISUNDEFINED(x)) {
            window->x = x;
        }
        if (!SDL_WINDOWPOS_ISUNDEFINED(y)) {
            window->y = y;
        }

        if (_this->SetWindowPosition) {
            _this->SetWindowPosition(_this, window);
        }
    }
    return 0;
}

int SDL_GetWindowPosition(SDL_Window *window, int *x, int *y)
{
    CHECK_WINDOW_MAGIC(window, -1);

    /* Fullscreen windows are always at their display's origin */
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        SDL_DisplayID displayID;

        if (x) {
            *x = 0;
        }
        if (y) {
            *y = 0;
        }

        /* Find the window's monitor and update to the
           monitor offset. */
        displayID = SDL_GetDisplayForWindow(window);
        if (displayID != 0) {
            SDL_Rect bounds;

            SDL_zero(bounds);

            SDL_GetDisplayBounds(displayID, &bounds);
            if (x) {
                *x = bounds.x;
            }
            if (y) {
                *y = bounds.y;
            }
        }
    } else {
        if (x) {
            *x = window->x;
        }
        if (y) {
            *y = window->y;
        }
    }
    return 0;
}

int SDL_SetWindowBordered(SDL_Window *window, SDL_bool bordered)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (!(window->flags & SDL_WINDOW_FULLSCREEN)) {
        const int want = (bordered != SDL_FALSE); /* normalize the flag. */
        const int have = !(window->flags & SDL_WINDOW_BORDERLESS);
        if ((want != have) && (_this->SetWindowBordered)) {
            if (want) {
                window->flags &= ~SDL_WINDOW_BORDERLESS;
            } else {
                window->flags |= SDL_WINDOW_BORDERLESS;
            }
            _this->SetWindowBordered(_this, window, (SDL_bool)want);
        }
    }
    return 0;
}

int SDL_SetWindowResizable(SDL_Window *window, SDL_bool resizable)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (!(window->flags & SDL_WINDOW_FULLSCREEN)) {
        const int want = (resizable != SDL_FALSE); /* normalize the flag. */
        const int have = ((window->flags & SDL_WINDOW_RESIZABLE) != 0);
        if ((want != have) && (_this->SetWindowResizable)) {
            if (want) {
                window->flags |= SDL_WINDOW_RESIZABLE;
            } else {
                window->flags &= ~SDL_WINDOW_RESIZABLE;
            }
            _this->SetWindowResizable(_this, window, (SDL_bool)want);
        }
    }
    return 0;
}

int SDL_SetWindowAlwaysOnTop(SDL_Window *window, SDL_bool on_top)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (!(window->flags & SDL_WINDOW_FULLSCREEN)) {
        const int want = (on_top != SDL_FALSE); /* normalize the flag. */
        const int have = ((window->flags & SDL_WINDOW_ALWAYS_ON_TOP) != 0);
        if ((want != have) && (_this->SetWindowAlwaysOnTop)) {
            if (want) {
                window->flags |= SDL_WINDOW_ALWAYS_ON_TOP;
            } else {
                window->flags &= ~SDL_WINDOW_ALWAYS_ON_TOP;
            }
            _this->SetWindowAlwaysOnTop(_this, window, (SDL_bool)want);
        }
    }
    return 0;
}

int SDL_SetWindowSize(SDL_Window *window, int w, int h)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (w <= 0) {
        return SDL_InvalidParamError("w");
    }
    if (h <= 0) {
        return SDL_InvalidParamError("h");
    }

    /* Make sure we don't exceed any window size limits */
    if (window->min_w && w < window->min_w) {
        w = window->min_w;
    }
    if (window->max_w && w > window->max_w) {
        w = window->max_w;
    }
    if (window->min_h && h < window->min_h) {
        h = window->min_h;
    }
    if (window->max_h && h > window->max_h) {
        h = window->max_h;
    }

    window->windowed.w = w;
    window->windowed.h = h;

    if (!SDL_WINDOW_FULLSCREEN_VISIBLE(window)) {
        if (_this->SetWindowSize) {
            _this->SetWindowSize(_this, window);
        }
    }
    return 0;
}

int SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (w) {
        *w = window->w;
    }
    if (h) {
        *h = window->h;
    }
    return 0;
}

int SDL_GetWindowBordersSize(SDL_Window *window, int *top, int *left, int *bottom, int *right)
{
    int dummy = 0;

    if (top == NULL) {
        top = &dummy;
    }
    if (left == NULL) {
        left = &dummy;
    }
    if (right == NULL) {
        right = &dummy;
    }
    if (bottom == NULL) {
        bottom = &dummy;
    }

    /* Always initialize, so applications don't have to care */
    *top = *left = *bottom = *right = 0;

    CHECK_WINDOW_MAGIC(window, -1);

    if (!_this->GetWindowBordersSize) {
        return SDL_Unsupported();
    }

    return _this->GetWindowBordersSize(_this, window, top, left, bottom, right);
}

int SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h)
{
    int filter;

    CHECK_WINDOW_MAGIC(window, -1);

    if (w == NULL) {
        w = &filter;
    }

    if (h == NULL) {
        h = &filter;
    }

    if (_this->GetWindowSizeInPixels) {
        _this->GetWindowSizeInPixels(_this, window, w, h);
    } else {
        SDL_DisplayID displayID = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *mode;

        SDL_GetWindowSize(window, w, h);

        if (SDL_GetWindowFullscreenMode(window)) {
            mode = SDL_GetCurrentDisplayMode(displayID);
        } else {
            mode = SDL_GetDesktopDisplayMode(displayID);
        }
        if (mode) {
            if (*w == mode->screen_w) {
                *w = mode->pixel_w;
            } else {
                *w = (int)SDL_ceilf(*w * mode->display_scale);
            }
            if (*h == mode->screen_h) {
                *h = mode->pixel_h;
            } else {
                *h = (int)SDL_ceilf(*h * mode->display_scale);
            }
        }
    }
    return 0;
}

int SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (min_w <= 0) {
        return SDL_InvalidParamError("min_w");
    }
    if (min_h <= 0) {
        return SDL_InvalidParamError("min_h");
    }

    if ((window->max_w && min_w > window->max_w) ||
        (window->max_h && min_h > window->max_h)) {
        return SDL_SetError("SDL_SetWindowMinimumSize(): Tried to set minimum size larger than maximum size");
    }

    window->min_w = min_w;
    window->min_h = min_h;

    if (!(window->flags & SDL_WINDOW_FULLSCREEN)) {
        if (_this->SetWindowMinimumSize) {
            _this->SetWindowMinimumSize(_this, window);
        }
        /* Ensure that window is not smaller than minimal size */
        return SDL_SetWindowSize(window, SDL_max(window->w, window->min_w), SDL_max(window->h, window->min_h));
    }
    return 0;
}

int SDL_GetWindowMinimumSize(SDL_Window *window, int *min_w, int *min_h)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (min_w) {
        *min_w = window->min_w;
    }
    if (min_h) {
        *min_h = window->min_h;
    }
    return 0;
}

int SDL_SetWindowMaximumSize(SDL_Window *window, int max_w, int max_h)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (max_w <= 0) {
        return SDL_InvalidParamError("max_w");
    }
    if (max_h <= 0) {
        return SDL_InvalidParamError("max_h");
    }

    if (max_w < window->min_w || max_h < window->min_h) {
        return SDL_SetError("SDL_SetWindowMaximumSize(): Tried to set maximum size smaller than minimum size");
    }

    window->max_w = max_w;
    window->max_h = max_h;

    if (!(window->flags & SDL_WINDOW_FULLSCREEN)) {
        if (_this->SetWindowMaximumSize) {
            _this->SetWindowMaximumSize(_this, window);
        }
        /* Ensure that window is not larger than maximal size */
        return SDL_SetWindowSize(window, SDL_min(window->w, window->max_w), SDL_min(window->h, window->max_h));
    }
    return 0;
}

int SDL_GetWindowMaximumSize(SDL_Window *window, int *max_w, int *max_h)
{
    CHECK_WINDOW_MAGIC(window, -1);
    if (max_w) {
        *max_w = window->max_w;
    }
    if (max_h) {
        *max_h = window->max_h;
    }
    return 0;
}

int SDL_ShowWindow(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        return 0;
    }

    if (_this->ShowWindow) {
        _this->ShowWindow(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_SHOWN, 0, 0);
    return 0;
}

int SDL_HideWindow(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (window->flags & SDL_WINDOW_HIDDEN) {
        return 0;
    }

    window->is_hiding = SDL_TRUE;
    SDL_UpdateFullscreenMode(window, SDL_FALSE);

    if (_this->HideWindow) {
        _this->HideWindow(_this, window);
    }
    window->is_hiding = SDL_FALSE;
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_HIDDEN, 0, 0);
    return 0;
}

int SDL_RaiseWindow(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (window->flags & SDL_WINDOW_HIDDEN) {
        return 0;
    }
    if (_this->RaiseWindow) {
        _this->RaiseWindow(_this, window);
    }
    return 0;
}

int SDL_MaximizeWindow(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        return 0;
    }

    /* !!! FIXME: should this check if the window is resizable? */

    if (_this->MaximizeWindow) {
        _this->MaximizeWindow(_this, window);
    }
    return 0;
}

static SDL_bool SDL_CanMinimizeWindow(SDL_Window *window)
{
    if (!_this->MinimizeWindow) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

int SDL_MinimizeWindow(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (window->flags & SDL_WINDOW_MINIMIZED) {
        return 0;
    }

    if (!SDL_CanMinimizeWindow(window)) {
        return 0;
    }

    if (!DisableUnsetFullscreenOnMinimize(_this)) {
        SDL_UpdateFullscreenMode(window, SDL_FALSE);
    }

    if (_this->MinimizeWindow) {
        _this->MinimizeWindow(_this, window);
    }
    return 0;
}

int SDL_RestoreWindow(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!(window->flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
        return 0;
    }

    if (_this->RestoreWindow) {
        _this->RestoreWindow(_this, window);
    }
    return 0;
}

int SDL_SetWindowFullscreen(SDL_Window *window, SDL_bool fullscreen)
{
    int ret = 0;
    Uint32 flags = fullscreen ? SDL_WINDOW_FULLSCREEN : 0;

    CHECK_WINDOW_MAGIC(window, -1);

    if (flags == (window->flags & SDL_WINDOW_FULLSCREEN)) {
        return 0;
    }

    /* Clear the previous flags and OR in the new ones */
    window->flags = (window->flags & ~SDL_WINDOW_FULLSCREEN) | flags;

    if (fullscreen) {
        /* Set the current fullscreen mode to the desired mode */
        SDL_memcpy(&window->current_fullscreen_mode, &window->requested_fullscreen_mode, sizeof(window->current_fullscreen_mode));
    }

    ret = SDL_UpdateFullscreenMode(window, SDL_WINDOW_FULLSCREEN_VISIBLE(window));

    if (!fullscreen || ret != 0) {
        /* Clear the current fullscreen mode. */
        SDL_zero(window->current_fullscreen_mode);
    }

    return ret;
}

static SDL_Surface *SDL_CreateWindowFramebuffer(SDL_Window *window)
{
    Uint32 format = 0;
    void *pixels = NULL;
    int pitch = 0;
    SDL_bool created_framebuffer = SDL_FALSE;
    int w, h;

    SDL_GetWindowSizeInPixels(window, &w, &h);

    /* This will switch the video backend from using a software surface to
       using a GPU texture through the 2D render API, if we think this would
       be more efficient. This only checks once, on demand. */
    if (!_this->checked_texture_framebuffer) {
        SDL_bool attempt_texture_framebuffer = SDL_TRUE;

        /* See if the user or application wants to specifically disable the framebuffer */
        const char *hint = SDL_GetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
        if (hint) {
            if ((*hint == '0') || (SDL_strcasecmp(hint, "false") == 0) || (SDL_strcasecmp(hint, "software") == 0)) {
                attempt_texture_framebuffer = SDL_FALSE;
            }
        }

        if (_this->is_dummy) { /* dummy driver never has GPU support, of course. */
            attempt_texture_framebuffer = SDL_FALSE;
        }

#if defined(__LINUX__)
        /* On WSL, direct X11 is faster than using OpenGL for window framebuffers, so try to detect WSL and avoid texture framebuffer. */
        else if ((_this->CreateWindowFramebuffer != NULL) && (SDL_strcmp(_this->name, "x11") == 0)) {
            struct stat sb;
            if ((stat("/proc/sys/fs/binfmt_misc/WSLInterop", &sb) == 0) || (stat("/run/WSL", &sb) == 0)) { /* if either of these exist, we're on WSL. */
                attempt_texture_framebuffer = SDL_FALSE;
            }
        }
#endif
#if defined(__WIN32__) || defined(__WINGDK__) /* GDI BitBlt() is way faster than Direct3D dynamic textures right now. (!!! FIXME: is this still true?) */
        else if ((_this->CreateWindowFramebuffer != NULL) && (SDL_strcmp(_this->name, "windows") == 0)) {
            attempt_texture_framebuffer = SDL_FALSE;
        }
#endif
#if defined(__EMSCRIPTEN__)
        else {
            attempt_texture_framebuffer = SDL_FALSE;
        }
#endif

        if (attempt_texture_framebuffer) {
            if (SDL_CreateWindowTexture(_this, window, &format, &pixels, &pitch) == -1) {
                /* !!! FIXME: if this failed halfway (made renderer, failed to make texture, etc),
                   !!! FIXME:  we probably need to clean this up so it doesn't interfere with
                   !!! FIXME:  a software fallback at the system level (can we blit to an
                   !!! FIXME:  OpenGL window? etc). */
            } else {
                /* future attempts will just try to use a texture framebuffer. */
                /* !!! FIXME:  maybe we shouldn't override these but check if we used a texture
                   !!! FIXME:  framebuffer at the right places; is it feasible we could have an
                   !!! FIXME:  accelerated OpenGL window and a second ends up in software? */
                _this->CreateWindowFramebuffer = SDL_CreateWindowTexture;
                _this->UpdateWindowFramebuffer = SDL_UpdateWindowTexture;
                _this->DestroyWindowFramebuffer = SDL_DestroyWindowTexture;
                created_framebuffer = SDL_TRUE;
            }
        }

        _this->checked_texture_framebuffer = SDL_TRUE; /* don't check this again. */
    }

    if (!created_framebuffer) {
        if (!_this->CreateWindowFramebuffer || !_this->UpdateWindowFramebuffer) {
            return NULL;
        }

        if (_this->CreateWindowFramebuffer(_this, window, &format, &pixels, &pitch) < 0) {
            return NULL;
        }
    }

    if (window->surface) {
        return window->surface;
    }

    return SDL_CreateSurfaceFrom(pixels, w, h, pitch, format);
}

SDL_Surface *SDL_GetWindowSurface(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, NULL);

    if (!window->surface_valid) {
        if (window->surface) {
            window->surface->flags &= ~SDL_DONTFREE;
            SDL_DestroySurface(window->surface);
            window->surface = NULL;
        }
        window->surface = SDL_CreateWindowFramebuffer(window);
        if (window->surface) {
            window->surface_valid = SDL_TRUE;
            window->surface->flags |= SDL_DONTFREE;
        }
    }
    return window->surface;
}

int SDL_UpdateWindowSurface(SDL_Window *window)
{
    SDL_Rect full_rect;

    CHECK_WINDOW_MAGIC(window, -1);

    full_rect.x = 0;
    full_rect.y = 0;
    SDL_GetWindowSizeInPixels(window, &full_rect.w, &full_rect.h);

    return SDL_UpdateWindowSurfaceRects(window, &full_rect, 1);
}

int SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects,
                                 int numrects)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!window->surface_valid) {
        return SDL_SetError("Window surface is invalid, please call SDL_GetWindowSurface() to get a new surface");
    }

    SDL_assert(_this->checked_texture_framebuffer); /* we should have done this before we had a valid surface. */

    return _this->UpdateWindowFramebuffer(_this, window, rects, numrects);
}

int SDL_SetWindowOpacity(SDL_Window *window, float opacity)
{
    int retval;
    CHECK_WINDOW_MAGIC(window, -1);

    if (!_this->SetWindowOpacity) {
        return SDL_Unsupported();
    }

    if (opacity < 0.0f) {
        opacity = 0.0f;
    } else if (opacity > 1.0f) {
        opacity = 1.0f;
    }

    retval = _this->SetWindowOpacity(_this, window, opacity);
    if (retval == 0) {
        window->opacity = opacity;
    }

    return retval;
}

int SDL_GetWindowOpacity(SDL_Window *window, float *out_opacity)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (out_opacity) {
        *out_opacity = window->opacity;
    }

    return 0;
}

int SDL_SetWindowModalFor(SDL_Window *modal_window, SDL_Window *parent_window)
{
    CHECK_WINDOW_MAGIC(modal_window, -1);
    CHECK_WINDOW_MAGIC(parent_window, -1);

    if (!_this->SetWindowModalFor) {
        return SDL_Unsupported();
    }

    return _this->SetWindowModalFor(_this, modal_window, parent_window);
}

int SDL_SetWindowInputFocus(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!_this->SetWindowInputFocus) {
        return SDL_Unsupported();
    }

    return _this->SetWindowInputFocus(_this, window);
}

void SDL_UpdateWindowGrab(SDL_Window *window)
{
    SDL_bool keyboard_grabbed, mouse_grabbed;

    if (window->flags & SDL_WINDOW_INPUT_FOCUS) {
        if (SDL_GetMouse()->relative_mode || (window->flags & SDL_WINDOW_MOUSE_GRABBED)) {
            mouse_grabbed = SDL_TRUE;
        } else {
            mouse_grabbed = SDL_FALSE;
        }

        if (window->flags & SDL_WINDOW_KEYBOARD_GRABBED) {
            keyboard_grabbed = SDL_TRUE;
        } else {
            keyboard_grabbed = SDL_FALSE;
        }
    } else {
        mouse_grabbed = SDL_FALSE;
        keyboard_grabbed = SDL_FALSE;
    }

    if (mouse_grabbed || keyboard_grabbed) {
        if (_this->grabbed_window && (_this->grabbed_window != window)) {
            /* stealing a grab from another window! */
            _this->grabbed_window->flags &= ~(SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED);
            if (_this->SetWindowMouseGrab) {
                _this->SetWindowMouseGrab(_this, _this->grabbed_window, SDL_FALSE);
            }
            if (_this->SetWindowKeyboardGrab) {
                _this->SetWindowKeyboardGrab(_this, _this->grabbed_window, SDL_FALSE);
            }
        }
        _this->grabbed_window = window;
    } else if (_this->grabbed_window == window) {
        _this->grabbed_window = NULL; /* ungrabbing input. */
    }

    if (_this->SetWindowMouseGrab) {
        _this->SetWindowMouseGrab(_this, window, mouse_grabbed);
    }
    if (_this->SetWindowKeyboardGrab) {
        _this->SetWindowKeyboardGrab(_this, window, keyboard_grabbed);
    }
}

int SDL_SetWindowGrab(SDL_Window *window, SDL_bool grabbed)
{
    CHECK_WINDOW_MAGIC(window, -1);

    SDL_SetWindowMouseGrab(window, grabbed);

    if (SDL_GetHintBoolean(SDL_HINT_GRAB_KEYBOARD, SDL_FALSE)) {
        SDL_SetWindowKeyboardGrab(window, grabbed);
    }
    return 0;
}

int SDL_SetWindowKeyboardGrab(SDL_Window *window, SDL_bool grabbed)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!!grabbed == !!(window->flags & SDL_WINDOW_KEYBOARD_GRABBED)) {
        return 0;
    }
    if (grabbed) {
        window->flags |= SDL_WINDOW_KEYBOARD_GRABBED;
    } else {
        window->flags &= ~SDL_WINDOW_KEYBOARD_GRABBED;
    }
    SDL_UpdateWindowGrab(window);
    return 0;
}

int SDL_SetWindowMouseGrab(SDL_Window *window, SDL_bool grabbed)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!!grabbed == !!(window->flags & SDL_WINDOW_MOUSE_GRABBED)) {
        return 0;
    }
    if (grabbed) {
        window->flags |= SDL_WINDOW_MOUSE_GRABBED;
    } else {
        window->flags &= ~SDL_WINDOW_MOUSE_GRABBED;
    }
    SDL_UpdateWindowGrab(window);
    return 0;
}

SDL_bool SDL_GetWindowGrab(SDL_Window *window)
{
    return SDL_GetWindowKeyboardGrab(window) || SDL_GetWindowMouseGrab(window);
}

SDL_bool SDL_GetWindowKeyboardGrab(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, SDL_FALSE);
    return window == _this->grabbed_window && (_this->grabbed_window->flags & SDL_WINDOW_KEYBOARD_GRABBED);
}

SDL_bool SDL_GetWindowMouseGrab(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, SDL_FALSE);
    return window == _this->grabbed_window && (_this->grabbed_window->flags & SDL_WINDOW_MOUSE_GRABBED);
}

SDL_Window *SDL_GetGrabbedWindow(void)
{
    if (_this->grabbed_window &&
        (_this->grabbed_window->flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED)) != 0) {
        return _this->grabbed_window;
    } else {
        return NULL;
    }
}

int SDL_SetWindowMouseRect(SDL_Window *window, const SDL_Rect *rect)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (rect) {
        SDL_memcpy(&window->mouse_rect, rect, sizeof(*rect));
    } else {
        SDL_zero(window->mouse_rect);
    }

    if (_this->SetWindowMouseRect) {
        _this->SetWindowMouseRect(_this, window);
    }
    return 0;
}

const SDL_Rect *SDL_GetWindowMouseRect(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, NULL);

    if (SDL_RectEmpty(&window->mouse_rect)) {
        return NULL;
    } else {
        return &window->mouse_rect;
    }
}

int SDL_FlashWindow(SDL_Window *window, SDL_FlashOperation operation)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (_this->FlashWindow) {
        return _this->FlashWindow(_this, window, operation);
    }

    return SDL_Unsupported();
}

void SDL_OnWindowShown(SDL_Window *window)
{
    SDL_OnWindowRestored(window);
}

void SDL_OnWindowHidden(SDL_Window *window)
{
    SDL_UpdateFullscreenMode(window, SDL_FALSE);
}

void SDL_OnWindowDisplayChanged(SDL_Window *window)
{
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        SDL_DisplayID displayID = SDL_GetDisplayForWindowPosition(window);
        const SDL_DisplayMode *new_mode = NULL;

        if (window->current_fullscreen_mode.pixel_w != 0 || window->current_fullscreen_mode.pixel_h != 0) {
            new_mode = SDL_GetClosestFullscreenDisplayMode(displayID, window->current_fullscreen_mode.pixel_w, window->current_fullscreen_mode.pixel_h, window->current_fullscreen_mode.refresh_rate);
        }

        if (new_mode) {
            SDL_memcpy(&window->current_fullscreen_mode, new_mode, sizeof(window->current_fullscreen_mode));
        } else {
            SDL_zero(window->current_fullscreen_mode);
        }

        if (SDL_WINDOW_FULLSCREEN_VISIBLE(window)) {
            SDL_UpdateFullscreenMode(window, SDL_TRUE);
        }
    }

    SDL_CheckWindowPixelSizeChanged(window);
}

void SDL_OnWindowMoved(SDL_Window *window)
{
    SDL_CheckWindowDisplayChanged(window);
}

void SDL_OnWindowResized(SDL_Window *window)
{
    SDL_CheckWindowDisplayChanged(window);
    SDL_CheckWindowPixelSizeChanged(window);
}

void SDL_CheckWindowPixelSizeChanged(SDL_Window *window)
{
    int pixel_w = 0, pixel_h = 0;

    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, pixel_w, pixel_h);
}

void SDL_OnWindowPixelSizeChanged(SDL_Window *window)
{
    window->surface_valid = SDL_FALSE;
}

void SDL_OnWindowMinimized(SDL_Window *window)
{
    if (!DisableUnsetFullscreenOnMinimize(_this)) {
        SDL_UpdateFullscreenMode(window, SDL_FALSE);
    }
}

void SDL_OnWindowMaximized(SDL_Window *window)
{
}

void SDL_OnWindowRestored(SDL_Window *window)
{
    /*
     * FIXME: Is this fine to just remove this, or should it be preserved just
     * for the fullscreen case? In principle it seems like just hiding/showing
     * windows shouldn't affect the stacking order; maybe the right fix is to
     * re-decouple OnWindowShown and OnWindowRestored.
     */
    /*SDL_RaiseWindow(window);*/

    if (SDL_WINDOW_FULLSCREEN_VISIBLE(window)) {
        SDL_UpdateFullscreenMode(window, SDL_TRUE);
    }
}

void SDL_OnWindowEnter(SDL_Window *window)
{
    if (_this->OnWindowEnter) {
        _this->OnWindowEnter(_this, window);
    }
}

void SDL_OnWindowLeave(SDL_Window *window)
{
}

void SDL_OnWindowFocusGained(SDL_Window *window)
{
    SDL_Mouse *mouse = SDL_GetMouse();

    if (mouse && mouse->relative_mode) {
        SDL_SetMouseFocus(window);
        if (mouse->relative_mode_warp) {
            SDL_PerformWarpMouseInWindow(window, (float)window->w / 2.0f, (float)window->h / 2.0f, SDL_TRUE);
        }
    }

    SDL_UpdateWindowGrab(window);
}

static SDL_bool SDL_ShouldMinimizeOnFocusLoss(SDL_Window *window)
{
    const char *hint;

    if (!(window->flags & SDL_WINDOW_FULLSCREEN) || window->is_destroying) {
        return SDL_FALSE;
    }

#if defined(__MACOS__) && defined(SDL_VIDEO_DRIVER_COCOA)
    if (SDL_strcmp(_this->name, "cocoa") == 0) { /* don't do this for X11, etc */
        if (Cocoa_IsWindowInFullscreenSpace(window)) {
            return SDL_FALSE;
        }
    }
#endif

#ifdef __ANDROID__
    {
        extern SDL_bool Android_JNI_ShouldMinimizeOnFocusLoss(void);
        if (!Android_JNI_ShouldMinimizeOnFocusLoss()) {
            return SDL_FALSE;
        }
    }
#endif

    /* Real fullscreen windows should minimize on focus loss so the desktop video mode is restored */
    hint = SDL_GetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS);
    if (hint == NULL || !*hint || SDL_strcasecmp(hint, "auto") == 0) {
        if (window->fullscreen_exclusive && !ModeSwitchingEmulated(_this)) {
            return SDL_TRUE;
        } else {
            return SDL_FALSE;
        }
    }
    return SDL_GetHintBoolean(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, SDL_FALSE);
}

void SDL_OnWindowFocusLost(SDL_Window *window)
{
    SDL_UpdateWindowGrab(window);

    if (SDL_ShouldMinimizeOnFocusLoss(window)) {
        SDL_MinimizeWindow(window);
    }
}

/* !!! FIXME: is this different than SDL_GetKeyboardFocus()?
   !!! FIXME:  Also, SDL_GetKeyboardFocus() is O(1), this isn't. */
SDL_Window *SDL_GetFocusWindow(void)
{
    SDL_Window *window;

    if (_this == NULL) {
        return NULL;
    }
    for (window = _this->windows; window; window = window->next) {
        if (window->flags & SDL_WINDOW_INPUT_FOCUS) {
            return window;
        }
    }
    return NULL;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    SDL_VideoDisplay *display;

    CHECK_WINDOW_MAGIC(window,);

    window->is_destroying = SDL_TRUE;

    /* Restore video mode, etc. */
    if (!(window->flags & SDL_WINDOW_FOREIGN)) {
        SDL_HideWindow(window);
    }

    /* Make sure this window no longer has focus */
    if (SDL_GetKeyboardFocus() == window) {
        SDL_SetKeyboardFocus(NULL);
    }
    if (SDL_GetMouseFocus() == window) {
        SDL_SetMouseFocus(NULL);
    }

    if (window->surface) {
        window->surface->flags &= ~SDL_DONTFREE;
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
        window->surface_valid = SDL_FALSE;
    }

    if (_this->checked_texture_framebuffer) { /* never checked? No framebuffer to destroy. Don't risk calling the wrong implementation. */
        if (_this->DestroyWindowFramebuffer) {
            _this->DestroyWindowFramebuffer(_this, window);
        }
    }

    /* make no context current if this is the current context window. */
    if (window->flags & SDL_WINDOW_OPENGL) {
        if (_this->current_glwin == window) {
            SDL_GL_MakeCurrent(window, NULL);
        }
    }
    if (window->flags & SDL_WINDOW_OPENGL) {
        SDL_GL_UnloadLibrary();
    }
    if (window->flags & SDL_WINDOW_VULKAN) {
        SDL_Vulkan_UnloadLibrary();
    }

    if (_this->DestroyWindow) {
        _this->DestroyWindow(_this, window);
    }

    display = SDL_GetVideoDisplayForWindow(window);
    if (display->fullscreen_window == window) {
        display->fullscreen_window = NULL;
    }

    /* Now invalidate magic */
    window->magic = NULL;

    /* Free memory associated with the window */
    SDL_free(window->title);
    SDL_DestroySurface(window->icon);
    while (window->data) {
        SDL_WindowUserData *data = window->data;

        window->data = data->next;
        SDL_free(data->name);
        SDL_free(data);
    }

    /* Unlink the window from the list */
    if (window->next) {
        window->next->prev = window->prev;
    }
    if (window->prev) {
        window->prev->next = window->next;
    } else {
        _this->windows = window->next;
    }

    SDL_free(window);
}

SDL_bool SDL_ScreenSaverEnabled()
{
    if (_this == NULL) {
        return SDL_TRUE;
    }
    return _this->suspend_screensaver ? SDL_FALSE : SDL_TRUE;
}

int SDL_EnableScreenSaver()
{
    if (_this == NULL) {
        return 0;
    }
    if (!_this->suspend_screensaver) {
        return 0;
    }
    _this->suspend_screensaver = SDL_FALSE;
    if (_this->SuspendScreenSaver) {
        return _this->SuspendScreenSaver(_this);
    }

    return SDL_Unsupported();
}

int SDL_DisableScreenSaver()
{
    if (_this == NULL) {
        return 0;
    }
    if (_this->suspend_screensaver) {
        return 0;
    }
    _this->suspend_screensaver = SDL_TRUE;
    if (_this->SuspendScreenSaver) {
        return _this->SuspendScreenSaver(_this);
    }

    return SDL_Unsupported();
}

void SDL_VideoQuit(void)
{
    int i;

    if (_this == NULL) {
        return;
    }

    /* Halt event processing before doing anything else */
    SDL_QuitTouch();
    SDL_QuitMouse();
    SDL_QuitKeyboard();
    SDL_QuitSubSystem(SDL_INIT_EVENTS);

    SDL_EnableScreenSaver();

    /* Clean up the system video */
    while (_this->windows) {
        SDL_DestroyWindow(_this->windows);
    }
    _this->VideoQuit(_this);

    for (i = _this->num_displays; i--; ) {
        SDL_VideoDisplay *display = &_this->displays[i];
        SDL_DelVideoDisplay(display->id, SDL_FALSE);
    }
    if (_this->displays) {
        SDL_assert(_this->num_displays == 0);
        SDL_free(_this->displays);
        _this->displays = NULL;
        _this->num_displays = 0;
    }
    SDL_free(_this->clipboard_text);
    _this->clipboard_text = NULL;
    _this->free(_this);
    _this = NULL;
}

int SDL_GL_LoadLibrary(const char *path)
{
    int retval;

    if (_this == NULL) {
        return SDL_UninitializedVideo();
    }
    if (_this->gl_config.driver_loaded) {
        if (path && SDL_strcmp(path, _this->gl_config.driver_path) != 0) {
            return SDL_SetError("OpenGL library already loaded");
        }
        retval = 0;
    } else {
        if (!_this->GL_LoadLibrary) {
            return SDL_DllNotSupported("OpenGL");
        }
        retval = _this->GL_LoadLibrary(_this, path);
    }
    if (retval == 0) {
        ++_this->gl_config.driver_loaded;
    } else {
        if (_this->GL_UnloadLibrary) {
            _this->GL_UnloadLibrary(_this);
        }
    }
    return retval;
}

SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc)
{
    void *func;

    if (_this == NULL) {
        SDL_UninitializedVideo();
        return NULL;
    }
    func = NULL;
    if (_this->GL_GetProcAddress) {
        if (_this->gl_config.driver_loaded) {
            func = _this->GL_GetProcAddress(_this, proc);
        } else {
            SDL_SetError("No GL driver has been loaded");
        }
    } else {
        SDL_SetError("No dynamic GL support in current SDL video driver (%s)", _this->name);
    }
    return func;
}

SDL_FunctionPointer SDL_EGL_GetProcAddress(const char *proc)
{
#if SDL_VIDEO_OPENGL_EGL
    void *func;

    if (!_this) {
        SDL_UninitializedVideo();
        return NULL;
    }
    func = NULL;

    if (_this->egl_data) {
        func = SDL_EGL_GetProcAddressInternal(_this, proc);
    } else {
        SDL_SetError("No EGL library has been loaded");
    }

    return func;
#else
    SDL_SetError("SDL was not built with EGL support");
    return NULL;
#endif
}

void SDL_GL_UnloadLibrary(void)
{
    if (_this == NULL) {
        SDL_UninitializedVideo();
        return;
    }
    if (_this->gl_config.driver_loaded > 0) {
        if (--_this->gl_config.driver_loaded > 0) {
            return;
        }
        if (_this->GL_UnloadLibrary) {
            _this->GL_UnloadLibrary(_this);
        }
    }
}

#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES || SDL_VIDEO_OPENGL_ES2
typedef GLenum (APIENTRY* PFNGLGETERRORPROC) (void);
typedef void (APIENTRY* PFNGLGETINTEGERVPROC) (GLenum pname, GLint *params);
typedef const GLubyte *(APIENTRY* PFNGLGETSTRINGPROC) (GLenum name);
#if !SDL_VIDEO_OPENGL
typedef const GLubyte *(APIENTRY* PFNGLGETSTRINGIPROC) (GLenum name, GLuint index);
#endif

static SDL_INLINE SDL_bool isAtLeastGL3(const char *verstr)
{
    return verstr && (SDL_atoi(verstr) >= 3);
}
#endif /* SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES || SDL_VIDEO_OPENGL_ES2 */

SDL_bool SDL_GL_ExtensionSupported(const char *extension)
{
#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES || SDL_VIDEO_OPENGL_ES2
    PFNGLGETSTRINGPROC glGetStringFunc;
    const char *extensions;
    const char *start;
    const char *where, *terminator;

    /* Extension names should not have spaces. */
    where = SDL_strchr(extension, ' ');
    if (where || *extension == '\0') {
        return SDL_FALSE;
    }
    /* See if there's an environment variable override */
    start = SDL_getenv(extension);
    if (start && *start == '0') {
        return SDL_FALSE;
    }

    /* Lookup the available extensions */

    glGetStringFunc = (PFNGLGETSTRINGPROC)SDL_GL_GetProcAddress("glGetString");
    if (glGetStringFunc == NULL) {
        return SDL_FALSE;
    }

    if (isAtLeastGL3((const char *)glGetStringFunc(GL_VERSION))) {
        PFNGLGETSTRINGIPROC glGetStringiFunc;
        PFNGLGETINTEGERVPROC glGetIntegervFunc;
        GLint num_exts = 0;
        GLint i;

        glGetStringiFunc = (PFNGLGETSTRINGIPROC)SDL_GL_GetProcAddress("glGetStringi");
        glGetIntegervFunc = (PFNGLGETINTEGERVPROC)SDL_GL_GetProcAddress("glGetIntegerv");
        if ((glGetStringiFunc == NULL) || (glGetIntegervFunc == NULL)) {
            return SDL_FALSE;
        }

#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
        glGetIntegervFunc(GL_NUM_EXTENSIONS, &num_exts);
        for (i = 0; i < num_exts; i++) {
            const char *thisext = (const char *)glGetStringiFunc(GL_EXTENSIONS, i);
            if (SDL_strcmp(thisext, extension) == 0) {
                return SDL_TRUE;
            }
        }

        return SDL_FALSE;
    }

    /* Try the old way with glGetString(GL_EXTENSIONS) ... */

    extensions = (const char *)glGetStringFunc(GL_EXTENSIONS);
    if (extensions == NULL) {
        return SDL_FALSE;
    }
    /*
     * It takes a bit of care to be fool-proof about parsing the OpenGL
     * extensions string. Don't be fooled by sub-strings, etc.
     */

    start = extensions;

    for (;;) {
        where = SDL_strstr(start, extension);
        if (where == NULL) {
            break;
        }

        terminator = where + SDL_strlen(extension);
        if (where == extensions || *(where - 1) == ' ') {
            if (*terminator == ' ' || *terminator == '\0') {
                return SDL_TRUE;
            }
        }

        start = terminator;
    }
    return SDL_FALSE;
#else
    return SDL_FALSE;
#endif
}

/* Deduce supported ES profile versions from the supported
   ARB_ES*_compatibility extensions. There is no direct query.

   This is normally only called when the OpenGL driver supports
   {GLX,WGL}_EXT_create_context_es2_profile.
 */
void SDL_GL_DeduceMaxSupportedESProfile(int *major, int *minor)
{
/* THIS REQUIRES AN EXISTING GL CONTEXT THAT HAS BEEN MADE CURRENT. */
/*  Please refer to https://bugzilla.libsdl.org/show_bug.cgi?id=3725 for discussion. */
#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES || SDL_VIDEO_OPENGL_ES2
    /* XXX This is fragile; it will break in the event of release of
     * new versions of OpenGL ES.
     */
    if (SDL_GL_ExtensionSupported("GL_ARB_ES3_2_compatibility")) {
        *major = 3;
        *minor = 2;
    } else if (SDL_GL_ExtensionSupported("GL_ARB_ES3_1_compatibility")) {
        *major = 3;
        *minor = 1;
    } else if (SDL_GL_ExtensionSupported("GL_ARB_ES3_compatibility")) {
        *major = 3;
        *minor = 0;
    } else {
        *major = 2;
        *minor = 0;
    }
#endif
}

void SDL_EGL_SetEGLAttributeCallbacks(SDL_EGLAttribArrayCallback platformAttribCallback,
                                      SDL_EGLIntArrayCallback surfaceAttribCallback,
                                      SDL_EGLIntArrayCallback contextAttribCallback)
{
    if (!_this) {
        return;
    }
    _this->egl_platformattrib_callback = platformAttribCallback;
    _this->egl_surfaceattrib_callback = surfaceAttribCallback;
    _this->egl_contextattrib_callback = contextAttribCallback;
}

void SDL_GL_ResetAttributes()
{
    if (_this == NULL) {
        return;
    }

    _this->egl_platformattrib_callback = NULL;
    _this->egl_surfaceattrib_callback = NULL;
    _this->egl_contextattrib_callback = NULL;

    _this->gl_config.red_size = 3;
    _this->gl_config.green_size = 3;
    _this->gl_config.blue_size = 2;
    _this->gl_config.alpha_size = 0;
    _this->gl_config.buffer_size = 0;
    _this->gl_config.depth_size = 16;
    _this->gl_config.stencil_size = 0;
    _this->gl_config.double_buffer = 1;
    _this->gl_config.accum_red_size = 0;
    _this->gl_config.accum_green_size = 0;
    _this->gl_config.accum_blue_size = 0;
    _this->gl_config.accum_alpha_size = 0;
    _this->gl_config.stereo = 0;
    _this->gl_config.multisamplebuffers = 0;
    _this->gl_config.multisamplesamples = 0;
    _this->gl_config.floatbuffers = 0;
    _this->gl_config.retained_backing = 1;
    _this->gl_config.accelerated = -1; /* accelerated or not, both are fine */

#if SDL_VIDEO_OPENGL
    _this->gl_config.major_version = 2;
    _this->gl_config.minor_version = 1;
    _this->gl_config.profile_mask = 0;
#elif SDL_VIDEO_OPENGL_ES2
    _this->gl_config.major_version = 2;
    _this->gl_config.minor_version = 0;
    _this->gl_config.profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
#elif SDL_VIDEO_OPENGL_ES
    _this->gl_config.major_version = 1;
    _this->gl_config.minor_version = 1;
    _this->gl_config.profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
#endif

    if (_this->GL_DefaultProfileConfig) {
        _this->GL_DefaultProfileConfig(_this, &_this->gl_config.profile_mask,
                                       &_this->gl_config.major_version,
                                       &_this->gl_config.minor_version);
    }

    _this->gl_config.flags = 0;
    _this->gl_config.framebuffer_srgb_capable = 0;
    _this->gl_config.no_error = 0;
    _this->gl_config.release_behavior = SDL_GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH;
    _this->gl_config.reset_notification = SDL_GL_CONTEXT_RESET_NO_NOTIFICATION;

    _this->gl_config.share_with_current_context = 0;

    _this->gl_config.egl_platform = 0;
}

int SDL_GL_SetAttribute(SDL_GLattr attr, int value)
{
#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES || SDL_VIDEO_OPENGL_ES2
    int retval;

    if (_this == NULL) {
        return SDL_UninitializedVideo();
    }
    retval = 0;
    switch (attr) {
    case SDL_GL_RED_SIZE:
        _this->gl_config.red_size = value;
        break;
    case SDL_GL_GREEN_SIZE:
        _this->gl_config.green_size = value;
        break;
    case SDL_GL_BLUE_SIZE:
        _this->gl_config.blue_size = value;
        break;
    case SDL_GL_ALPHA_SIZE:
        _this->gl_config.alpha_size = value;
        break;
    case SDL_GL_DOUBLEBUFFER:
        _this->gl_config.double_buffer = value;
        break;
    case SDL_GL_BUFFER_SIZE:
        _this->gl_config.buffer_size = value;
        break;
    case SDL_GL_DEPTH_SIZE:
        _this->gl_config.depth_size = value;
        break;
    case SDL_GL_STENCIL_SIZE:
        _this->gl_config.stencil_size = value;
        break;
    case SDL_GL_ACCUM_RED_SIZE:
        _this->gl_config.accum_red_size = value;
        break;
    case SDL_GL_ACCUM_GREEN_SIZE:
        _this->gl_config.accum_green_size = value;
        break;
    case SDL_GL_ACCUM_BLUE_SIZE:
        _this->gl_config.accum_blue_size = value;
        break;
    case SDL_GL_ACCUM_ALPHA_SIZE:
        _this->gl_config.accum_alpha_size = value;
        break;
    case SDL_GL_STEREO:
        _this->gl_config.stereo = value;
        break;
    case SDL_GL_MULTISAMPLEBUFFERS:
        _this->gl_config.multisamplebuffers = value;
        break;
    case SDL_GL_MULTISAMPLESAMPLES:
        _this->gl_config.multisamplesamples = value;
        break;
    case SDL_GL_FLOATBUFFERS:
        _this->gl_config.floatbuffers = value;
        break;
    case SDL_GL_ACCELERATED_VISUAL:
        _this->gl_config.accelerated = value;
        break;
    case SDL_GL_RETAINED_BACKING:
        _this->gl_config.retained_backing = value;
        break;
    case SDL_GL_CONTEXT_MAJOR_VERSION:
        _this->gl_config.major_version = value;
        break;
    case SDL_GL_CONTEXT_MINOR_VERSION:
        _this->gl_config.minor_version = value;
        break;
    case SDL_GL_CONTEXT_FLAGS:
        if (value & ~(SDL_GL_CONTEXT_DEBUG_FLAG |
                      SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG |
                      SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG |
                      SDL_GL_CONTEXT_RESET_ISOLATION_FLAG)) {
            retval = SDL_SetError("Unknown OpenGL context flag %d", value);
            break;
        }
        _this->gl_config.flags = value;
        break;
    case SDL_GL_CONTEXT_PROFILE_MASK:
        if (value != 0 &&
            value != SDL_GL_CONTEXT_PROFILE_CORE &&
            value != SDL_GL_CONTEXT_PROFILE_COMPATIBILITY &&
            value != SDL_GL_CONTEXT_PROFILE_ES) {
            retval = SDL_SetError("Unknown OpenGL context profile %d", value);
            break;
        }
        _this->gl_config.profile_mask = value;
        break;
    case SDL_GL_SHARE_WITH_CURRENT_CONTEXT:
        _this->gl_config.share_with_current_context = value;
        break;
    case SDL_GL_FRAMEBUFFER_SRGB_CAPABLE:
        _this->gl_config.framebuffer_srgb_capable = value;
        break;
    case SDL_GL_CONTEXT_RELEASE_BEHAVIOR:
        _this->gl_config.release_behavior = value;
        break;
    case SDL_GL_CONTEXT_RESET_NOTIFICATION:
        _this->gl_config.reset_notification = value;
        break;
    case SDL_GL_CONTEXT_NO_ERROR:
        _this->gl_config.no_error = value;
        break;
    case SDL_GL_EGL_PLATFORM:
        _this->gl_config.egl_platform = value;
        break;
    default:
        retval = SDL_SetError("Unknown OpenGL attribute");
        break;
    }
    return retval;
#else
    return SDL_Unsupported();
#endif /* SDL_VIDEO_OPENGL */
}

int SDL_GL_GetAttribute(SDL_GLattr attr, int *value)
{
#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES || SDL_VIDEO_OPENGL_ES2
    PFNGLGETERRORPROC glGetErrorFunc;
    GLenum attrib = 0;
    GLenum error = 0;

    /*
     * Some queries in Core Profile desktop OpenGL 3+ contexts require
     * glGetFramebufferAttachmentParameteriv instead of glGetIntegerv. Note that
     * the enums we use for the former function don't exist in OpenGL ES 2, and
     * the function itself doesn't exist prior to OpenGL 3 and OpenGL ES 2.
     */
#if SDL_VIDEO_OPENGL
    PFNGLGETSTRINGPROC glGetStringFunc;
    PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC glGetFramebufferAttachmentParameterivFunc;
    GLenum attachment = GL_BACK_LEFT;
    GLenum attachmentattrib = 0;
#endif

    if (value == NULL) {
        return SDL_InvalidParamError("value");
    }

    /* Clear value in any case */
    *value = 0;

    if (_this == NULL) {
        return SDL_UninitializedVideo();
    }

    switch (attr) {
    case SDL_GL_RED_SIZE:
#if SDL_VIDEO_OPENGL
        attachmentattrib = GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE;
#endif
        attrib = GL_RED_BITS;
        break;
    case SDL_GL_BLUE_SIZE:
#if SDL_VIDEO_OPENGL
        attachmentattrib = GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE;
#endif
        attrib = GL_BLUE_BITS;
        break;
    case SDL_GL_GREEN_SIZE:
#if SDL_VIDEO_OPENGL
        attachmentattrib = GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE;
#endif
        attrib = GL_GREEN_BITS;
        break;
    case SDL_GL_ALPHA_SIZE:
#if SDL_VIDEO_OPENGL
        attachmentattrib = GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE;
#endif
        attrib = GL_ALPHA_BITS;
        break;
    case SDL_GL_DOUBLEBUFFER:
#if SDL_VIDEO_OPENGL
        attrib = GL_DOUBLEBUFFER;
        break;
#else
        /* OpenGL ES 1.0 and above specifications have EGL_SINGLE_BUFFER      */
        /* parameter which switches double buffer to single buffer. OpenGL ES */
        /* SDL driver must set proper value after initialization              */
        *value = _this->gl_config.double_buffer;
        return 0;
#endif
    case SDL_GL_DEPTH_SIZE:
#if SDL_VIDEO_OPENGL
        attachment = GL_DEPTH;
        attachmentattrib = GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE;
#endif
        attrib = GL_DEPTH_BITS;
        break;
    case SDL_GL_STENCIL_SIZE:
#if SDL_VIDEO_OPENGL
        attachment = GL_STENCIL;
        attachmentattrib = GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE;
#endif
        attrib = GL_STENCIL_BITS;
        break;
#if SDL_VIDEO_OPENGL
    case SDL_GL_ACCUM_RED_SIZE:
        attrib = GL_ACCUM_RED_BITS;
        break;
    case SDL_GL_ACCUM_GREEN_SIZE:
        attrib = GL_ACCUM_GREEN_BITS;
        break;
    case SDL_GL_ACCUM_BLUE_SIZE:
        attrib = GL_ACCUM_BLUE_BITS;
        break;
    case SDL_GL_ACCUM_ALPHA_SIZE:
        attrib = GL_ACCUM_ALPHA_BITS;
        break;
    case SDL_GL_STEREO:
        attrib = GL_STEREO;
        break;
#else
    case SDL_GL_ACCUM_RED_SIZE:
    case SDL_GL_ACCUM_GREEN_SIZE:
    case SDL_GL_ACCUM_BLUE_SIZE:
    case SDL_GL_ACCUM_ALPHA_SIZE:
    case SDL_GL_STEREO:
        /* none of these are supported in OpenGL ES */
        *value = 0;
        return 0;
#endif
    case SDL_GL_MULTISAMPLEBUFFERS:
        attrib = GL_SAMPLE_BUFFERS;
        break;
    case SDL_GL_MULTISAMPLESAMPLES:
        attrib = GL_SAMPLES;
        break;
    case SDL_GL_CONTEXT_RELEASE_BEHAVIOR:
#if SDL_VIDEO_OPENGL
        attrib = GL_CONTEXT_RELEASE_BEHAVIOR;
#else
        attrib = GL_CONTEXT_RELEASE_BEHAVIOR_KHR;
#endif
        break;
    case SDL_GL_BUFFER_SIZE:
    {
        int rsize = 0, gsize = 0, bsize = 0, asize = 0;

        /* There doesn't seem to be a single flag in OpenGL for this! */
        if (SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &rsize) < 0) {
            return -1;
        }
        if (SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &gsize) < 0) {
            return -1;
        }
        if (SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &bsize) < 0) {
            return -1;
        }
        if (SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &asize) < 0) {
            return -1;
        }

        *value = rsize + gsize + bsize + asize;
        return 0;
    }
    case SDL_GL_ACCELERATED_VISUAL:
    {
        /* FIXME: How do we get this information? */
        *value = (_this->gl_config.accelerated != 0);
        return 0;
    }
    case SDL_GL_RETAINED_BACKING:
    {
        *value = _this->gl_config.retained_backing;
        return 0;
    }
    case SDL_GL_CONTEXT_MAJOR_VERSION:
    {
        *value = _this->gl_config.major_version;
        return 0;
    }
    case SDL_GL_CONTEXT_MINOR_VERSION:
    {
        *value = _this->gl_config.minor_version;
        return 0;
    }
    case SDL_GL_CONTEXT_FLAGS:
    {
        *value = _this->gl_config.flags;
        return 0;
    }
    case SDL_GL_CONTEXT_PROFILE_MASK:
    {
        *value = _this->gl_config.profile_mask;
        return 0;
    }
    case SDL_GL_SHARE_WITH_CURRENT_CONTEXT:
    {
        *value = _this->gl_config.share_with_current_context;
        return 0;
    }
    case SDL_GL_FRAMEBUFFER_SRGB_CAPABLE:
    {
        *value = _this->gl_config.framebuffer_srgb_capable;
        return 0;
    }
    case SDL_GL_CONTEXT_NO_ERROR:
    {
        *value = _this->gl_config.no_error;
        return 0;
    }
    case SDL_GL_EGL_PLATFORM:
    {
        *value = _this->gl_config.egl_platform;
        return 0;
    }
    default:
        return SDL_SetError("Unknown OpenGL attribute");
    }

#if SDL_VIDEO_OPENGL
    glGetStringFunc = (PFNGLGETSTRINGPROC)SDL_GL_GetProcAddress("glGetString");
    if (glGetStringFunc == NULL) {
        return -1;
    }

    if (attachmentattrib && isAtLeastGL3((const char *)glGetStringFunc(GL_VERSION))) {
        /* glGetFramebufferAttachmentParameteriv needs to operate on the window framebuffer for this, so bind FBO 0 if necessary. */
        GLint current_fbo = 0;
        PFNGLGETINTEGERVPROC glGetIntegervFunc = (PFNGLGETINTEGERVPROC) SDL_GL_GetProcAddress("glGetIntegerv");
        PFNGLBINDFRAMEBUFFERPROC glBindFramebufferFunc = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
        if (glGetIntegervFunc && glBindFramebufferFunc) {
            glGetIntegervFunc(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
        }

        glGetFramebufferAttachmentParameterivFunc = (PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC)SDL_GL_GetProcAddress("glGetFramebufferAttachmentParameteriv");
        if (glGetFramebufferAttachmentParameterivFunc) {
            if (glBindFramebufferFunc && (current_fbo != 0)) {
                glBindFramebufferFunc(GL_DRAW_FRAMEBUFFER, 0);
            }
            glGetFramebufferAttachmentParameterivFunc(GL_FRAMEBUFFER, attachment, attachmentattrib, (GLint *)value);
            if (glBindFramebufferFunc && (current_fbo != 0)) {
                glBindFramebufferFunc(GL_DRAW_FRAMEBUFFER, current_fbo);
            }
        } else {
            return -1;
        }
    } else
#endif
    {
        PFNGLGETINTEGERVPROC glGetIntegervFunc = (PFNGLGETINTEGERVPROC)SDL_GL_GetProcAddress("glGetIntegerv");
        if (glGetIntegervFunc) {
            glGetIntegervFunc(attrib, (GLint *)value);
        } else {
            return -1;
        }
    }

    glGetErrorFunc = (PFNGLGETERRORPROC)SDL_GL_GetProcAddress("glGetError");
    if (glGetErrorFunc == NULL) {
        return -1;
    }

    error = glGetErrorFunc();
    if (error != GL_NO_ERROR) {
        if (error == GL_INVALID_ENUM) {
            return SDL_SetError("OpenGL error: GL_INVALID_ENUM");
        } else if (error == GL_INVALID_VALUE) {
            return SDL_SetError("OpenGL error: GL_INVALID_VALUE");
        }
        return SDL_SetError("OpenGL error: %08X", error);
    }
    return 0;
#else
    return SDL_Unsupported();
#endif /* SDL_VIDEO_OPENGL */
}

#define NOT_AN_OPENGL_WINDOW "The specified window isn't an OpenGL window"

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    SDL_GLContext ctx = NULL;
    CHECK_WINDOW_MAGIC(window, NULL);

    if (!(window->flags & SDL_WINDOW_OPENGL)) {
        SDL_SetError(NOT_AN_OPENGL_WINDOW);
        return NULL;
    }

    ctx = _this->GL_CreateContext(_this, window);

    /* Creating a context is assumed to make it current in the SDL driver. */
    if (ctx) {
        _this->current_glwin = window;
        _this->current_glctx = ctx;
        SDL_TLSSet(_this->current_glwin_tls, window, NULL);
        SDL_TLSSet(_this->current_glctx_tls, ctx, NULL);
    }
    return ctx;
}

int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    int retval;

    if (_this == NULL) {
        return SDL_UninitializedVideo();
    }

    if (window == SDL_GL_GetCurrentWindow() &&
        context == SDL_GL_GetCurrentContext()) {
        /* We're already current. */
        return 0;
    }

    if (!context) {
        window = NULL;
    } else if (window) {
        CHECK_WINDOW_MAGIC(window, -1);

        if (!(window->flags & SDL_WINDOW_OPENGL)) {
            return SDL_SetError(NOT_AN_OPENGL_WINDOW);
        }
    } else if (!_this->gl_allow_no_surface) {
        return SDL_SetError("Use of OpenGL without a window is not supported on this platform");
    }

    retval = _this->GL_MakeCurrent(_this, window, context);
    if (retval == 0) {
        _this->current_glwin = window;
        _this->current_glctx = context;
        SDL_TLSSet(_this->current_glwin_tls, window, NULL);
        SDL_TLSSet(_this->current_glctx_tls, context, NULL);
    }
    return retval;
}

SDL_Window *SDL_GL_GetCurrentWindow(void)
{
    if (_this == NULL) {
        SDL_UninitializedVideo();
        return NULL;
    }
    return (SDL_Window *)SDL_TLSGet(_this->current_glwin_tls);
}

SDL_GLContext SDL_GL_GetCurrentContext(void)
{
    if (_this == NULL) {
        SDL_UninitializedVideo();
        return NULL;
    }
    return (SDL_GLContext)SDL_TLSGet(_this->current_glctx_tls);
}

SDL_EGLDisplay SDL_EGL_GetCurrentEGLDisplay(void)
{
#if SDL_VIDEO_OPENGL_EGL
    if (!_this) {
        SDL_UninitializedVideo();
        return EGL_NO_DISPLAY;
    }
    if (!_this->egl_data) {
        SDL_SetError("There is no current EGL display");
        return EGL_NO_DISPLAY;
    }
    return _this->egl_data->egl_display;
#else
    SDL_SetError("SDL was not built with EGL support");
    return NULL;
#endif
}

SDL_EGLConfig SDL_EGL_GetCurrentEGLConfig(void)
{
#if SDL_VIDEO_OPENGL_EGL
    if (!_this) {
        SDL_UninitializedVideo();
        return NULL;
    }
    if (!_this->egl_data) {
        SDL_SetError("There is no current EGL display");
        return NULL;
    }
    return _this->egl_data->egl_config;
#else
    SDL_SetError("SDL was not built with EGL support");
    return NULL;
#endif
}

SDL_EGLConfig SDL_EGL_GetWindowEGLSurface(SDL_Window *window)
{
#if SDL_VIDEO_OPENGL_EGL
    if (!_this) {
        SDL_UninitializedVideo();
        return NULL;
    }
    if (!_this->egl_data) {
        SDL_SetError("There is no current EGL display");
        return NULL;
    }
    if (_this->GL_GetEGLSurface) {
        return _this->GL_GetEGLSurface(_this, window);
    }
    return NULL;
#else
    SDL_SetError("SDL was not built with EGL support");
    return NULL;
#endif
}

int SDL_GL_SetSwapInterval(int interval)
{
    if (_this == NULL) {
        return SDL_UninitializedVideo();
    } else if (SDL_GL_GetCurrentContext() == NULL) {
        return SDL_SetError("No OpenGL context has been made current");
    } else if (_this->GL_SetSwapInterval) {
        return _this->GL_SetSwapInterval(_this, interval);
    } else {
        return SDL_SetError("Setting the swap interval is not supported");
    }
}

int SDL_GL_GetSwapInterval(int *interval)
{
    if (interval == NULL) {
       return SDL_InvalidParamError("interval");
    }

    *interval = 0;

    if (_this == NULL) {
        return SDL_SetError("no video driver");;
    } else if (SDL_GL_GetCurrentContext() == NULL) {
        return SDL_SetError("no current context");;
    } else if (_this->GL_GetSwapInterval) {
        return _this->GL_GetSwapInterval(_this, interval);
    } else {
        return SDL_SetError("not implemented");;
    }
}

int SDL_GL_SwapWindow(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!(window->flags & SDL_WINDOW_OPENGL)) {
        return SDL_SetError(NOT_AN_OPENGL_WINDOW);
    }

    if (SDL_GL_GetCurrentWindow() != window) {
        return SDL_SetError("The specified window has not been made current");
    }

    return _this->GL_SwapWindow(_this, window);
}

int SDL_GL_DeleteContext(SDL_GLContext context)
{
    if (_this == NULL) {
        return SDL_UninitializedVideo();                                       \
    }
    if (!context) {
        return SDL_InvalidParamError("context");
    }

    if (SDL_GL_GetCurrentContext() == context) {
        SDL_GL_MakeCurrent(NULL, NULL);
    }

    return _this->GL_DeleteContext(_this, context);
}

#if 0 /* FIXME */
/*
 * Utility function used by SDL_WM_SetIcon(); flags & 1 for color key, flags
 * & 2 for alpha channel.
 */
static void CreateMaskFromColorKeyOrAlpha(SDL_Surface *icon, Uint8 *mask, int flags)
{
    int x, y;
    Uint32 colorkey;
#define SET_MASKBIT(icon, x, y, mask) \
    mask[(y * ((icon->w + 7) / 8)) + (x / 8)] &= ~(0x01 << (7 - (x % 8)))

    colorkey = icon->format->colorkey;
    switch (icon->format->BytesPerPixel) {
    case 1:
        {
            Uint8 *pixels;
            for (y = 0; y < icon->h; ++y) {
                pixels = (Uint8 *) icon->pixels + y * icon->pitch;
                for (x = 0; x < icon->w; ++x) {
                    if (*pixels++ == colorkey) {
                        SET_MASKBIT(icon, x, y, mask);
                    }
                }
            }
        }
        break;

    case 2:
        {
            Uint16 *pixels;
            for (y = 0; y < icon->h; ++y) {
                pixels = (Uint16 *) icon->pixels + y * icon->pitch / 2;
                for (x = 0; x < icon->w; ++x) {
                    if ((flags & 1) && *pixels == colorkey) {
                        SET_MASKBIT(icon, x, y, mask);
                    } else if ((flags & 2)
                               && (*pixels & icon->format->Amask) == 0) {
                        SET_MASKBIT(icon, x, y, mask);
                    }
                    pixels++;
                }
            }
        }
        break;

    case 4:
        {
            Uint32 *pixels;
            for (y = 0; y < icon->h; ++y) {
                pixels = (Uint32 *) icon->pixels + y * icon->pitch / 4;
                for (x = 0; x < icon->w; ++x) {
                    if ((flags & 1) && *pixels == colorkey) {
                        SET_MASKBIT(icon, x, y, mask);
                    } else if ((flags & 2)
                               && (*pixels & icon->format->Amask) == 0) {
                        SET_MASKBIT(icon, x, y, mask);
                    }
                    pixels++;
                }
            }
        }
        break;
    }
}

/*
 * Sets the window manager icon for the display window.
 */
void SDL_WM_SetIcon(SDL_Surface *icon, Uint8 *mask)
{
    if (icon && _this->SetIcon) {
        /* Generate a mask if necessary, and create the icon! */
        if (mask == NULL) {
            int mask_len = icon->h * (icon->w + 7) / 8;
            int flags = 0;
            mask = (Uint8 *) SDL_malloc(mask_len);
            if (mask == NULL) {
                return;
            }
            SDL_memset(mask, ~0, mask_len);
            if (icon->flags & SDL_SRCCOLORKEY)
                flags |= 1;
            if (icon->flags & SDL_SRCALPHA)
                flags |= 2;
            if (flags) {
                CreateMaskFromColorKeyOrAlpha(icon, mask, flags);
            }
            _this->SetIcon(_this, icon, mask);
            SDL_free(mask);
        } else {
            _this->SetIcon(_this, icon, mask);
        }
    }
}
#endif

int SDL_GetWindowWMInfo(SDL_Window *window, struct SDL_SysWMinfo *info, Uint32 version)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (info == NULL) {
        return SDL_InvalidParamError("info");
    }

    /* Set the version in the structure to the minimum of our version and the application expected version */
    version = SDL_min(version, SDL_SYSWM_CURRENT_VERSION);

    if (version == 1) {
        SDL_memset(info, 0, SDL_SYSWM_INFO_SIZE_V1);
    } else {
        return SDL_SetError("Unknown info version");
    }

    info->subsystem = SDL_SYSWM_UNKNOWN;
    info->version = version;

    if (_this->GetWindowWMInfo) {
        return (_this->GetWindowWMInfo(_this, window, info));
    } else {
        return 0;
    }
}

void SDL_StartTextInput(void)
{
    SDL_Window *window;

    /* First, enable text events */
    SDL_SetEventEnabled(SDL_EVENT_TEXT_INPUT, SDL_TRUE);
    SDL_SetEventEnabled(SDL_EVENT_TEXT_EDITING, SDL_TRUE);

    /* Then show the on-screen keyboard, if any */
    window = SDL_GetFocusWindow();
    if (window && _this && _this->ShowScreenKeyboard) {
        _this->ShowScreenKeyboard(_this, window);
    }

    /* Finally start the text input system */
    if (_this && _this->StartTextInput) {
        _this->StartTextInput(_this);
    }
}

void SDL_ClearComposition(void)
{
    if (_this && _this->ClearComposition) {
        _this->ClearComposition(_this);
    }
}

SDL_bool SDL_TextInputShown(void)
{
    if (_this && _this->IsTextInputShown) {
        return _this->IsTextInputShown(_this);
    }

    return SDL_FALSE;
}

SDL_bool SDL_TextInputActive(void)
{
    return SDL_EventEnabled(SDL_EVENT_TEXT_INPUT);
}

void SDL_StopTextInput(void)
{
    SDL_Window *window;

    /* Stop the text input system */
    if (_this && _this->StopTextInput) {
        _this->StopTextInput(_this);
    }

    /* Hide the on-screen keyboard, if any */
    window = SDL_GetFocusWindow();
    if (window && _this && _this->HideScreenKeyboard) {
        _this->HideScreenKeyboard(_this, window);
    }

    /* Finally disable text events */
    SDL_SetEventEnabled(SDL_EVENT_TEXT_INPUT, SDL_FALSE);
    SDL_SetEventEnabled(SDL_EVENT_TEXT_EDITING, SDL_FALSE);
}

int SDL_SetTextInputRect(const SDL_Rect *rect)
{
    if (rect == NULL) {
        return SDL_InvalidParamError("rect");
    }

    if (_this && _this->SetTextInputRect) {
        return _this->SetTextInputRect(_this, rect);
    }
    return SDL_Unsupported();
}

SDL_bool SDL_HasScreenKeyboardSupport(void)
{
    if (_this && _this->HasScreenKeyboardSupport) {
        return _this->HasScreenKeyboardSupport(_this);
    }
    return SDL_FALSE;
}

SDL_bool SDL_ScreenKeyboardShown(SDL_Window *window)
{
    if (window && _this && _this->IsScreenKeyboardShown) {
        return _this->IsScreenKeyboardShown(_this, window);
    }
    return SDL_FALSE;
}

int SDL_GetMessageBoxCount(void)
{
    return SDL_AtomicGet(&SDL_messagebox_count);
}

#if SDL_VIDEO_DRIVER_ANDROID
#include "android/SDL_androidmessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_WINDOWS
#include "windows/SDL_windowsmessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_WINRT
#include "winrt/SDL_winrtmessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_COCOA
#include "cocoa/SDL_cocoamessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_UIKIT
#include "uikit/SDL_uikitmessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_WAYLAND
#include "wayland/SDL_waylandmessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_X11
#include "x11/SDL_x11messagebox.h"
#endif
#if SDL_VIDEO_DRIVER_HAIKU
#include "haiku/SDL_bmessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_RISCOS
#include "riscos/SDL_riscosmessagebox.h"
#endif
#if SDL_VIDEO_DRIVER_VITA
#include "vita/SDL_vitamessagebox.h"
#endif

#if SDL_VIDEO_DRIVER_WINDOWS || SDL_VIDEO_DRIVER_WINRT || SDL_VIDEO_DRIVER_COCOA || SDL_VIDEO_DRIVER_UIKIT || SDL_VIDEO_DRIVER_X11 || SDL_VIDEO_DRIVER_WAYLAND || SDL_VIDEO_DRIVER_HAIKU || SDL_VIDEO_DRIVER_RISCOS
static SDL_bool SDL_IsMessageboxValidForDriver(const SDL_MessageBoxData *messageboxdata, SDL_SYSWM_TYPE drivertype)
{
    SDL_SysWMinfo info;
    SDL_Window *window = messageboxdata->window;

    if (window == NULL || SDL_GetWindowWMInfo(window, &info, SDL_SYSWM_CURRENT_VERSION) < 0) {
        return SDL_TRUE;
    } else {
        return info.subsystem == drivertype;
    }
}
#endif

int SDL_ShowMessageBox(const SDL_MessageBoxData *messageboxdata, int *buttonid)
{
    int dummybutton;
    int retval = -1;
    SDL_bool relative_mode;
    SDL_bool show_cursor_prev;
    SDL_Window *current_window;
    SDL_MessageBoxData mbdata;

    if (messageboxdata == NULL) {
        return SDL_InvalidParamError("messageboxdata");
    } else if (messageboxdata->numbuttons < 0) {
        return SDL_SetError("Invalid number of buttons");
    }

    (void)SDL_AtomicIncRef(&SDL_messagebox_count);

    current_window = SDL_GetKeyboardFocus();
    relative_mode = SDL_GetRelativeMouseMode();
    SDL_UpdateMouseCapture(SDL_FALSE);
    SDL_SetRelativeMouseMode(SDL_FALSE);
    show_cursor_prev = SDL_CursorVisible();
    SDL_ShowCursor();
    SDL_ResetKeyboard();

    if (buttonid == NULL) {
        buttonid = &dummybutton;
    }

    SDL_memcpy(&mbdata, messageboxdata, sizeof(*messageboxdata));
    if (!mbdata.title) {
        mbdata.title = "";
    }
    if (!mbdata.message) {
        mbdata.message = "";
    }
    messageboxdata = &mbdata;

    SDL_ClearError();

    if (_this && _this->ShowMessageBox) {
        retval = _this->ShowMessageBox(_this, messageboxdata, buttonid);
    }

    /* It's completely fine to call this function before video is initialized */
#if SDL_VIDEO_DRIVER_ANDROID
    if (retval == -1 &&
        Android_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_WINDOWS && !defined(__XBOXONE__) && !defined(__XBOXSERIES__)
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_WINDOWS) &&
        WIN_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_WINRT
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_WINRT) &&
        WINRT_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_COCOA
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_COCOA) &&
        Cocoa_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_UIKIT
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_UIKIT) &&
        UIKit_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_WAYLAND
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_WAYLAND) &&
        Wayland_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_X11
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_X11) &&
        X11_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_HAIKU
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_HAIKU) &&
        HAIKU_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_RISCOS
    if (retval == -1 &&
        SDL_IsMessageboxValidForDriver(messageboxdata, SDL_SYSWM_RISCOS) &&
        RISCOS_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
#if SDL_VIDEO_DRIVER_VITA
    if (retval == -1 &&
        VITA_ShowMessageBox(messageboxdata, buttonid) == 0) {
        retval = 0;
    }
#endif
    if (retval == -1) {
        const char *error = SDL_GetError();

        if (!*error) {
            SDL_SetError("No message system available");
        }
    }

    (void)SDL_AtomicDecRef(&SDL_messagebox_count);

    if (current_window) {
        SDL_RaiseWindow(current_window);
    }

    if (!show_cursor_prev) {
        SDL_HideCursor();
    }
    SDL_SetRelativeMouseMode(relative_mode);
    SDL_UpdateMouseCapture(SDL_FALSE);

    return retval;
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
#ifdef __EMSCRIPTEN__
    /* !!! FIXME: propose a browser API for this, get this #ifdef out of here? */
    /* Web browsers don't (currently) have an API for a custom message box
       that can block, but for the most common case (SDL_ShowSimpleMessageBox),
       we can use the standard Javascript alert() function. */
    if (title == NULL) {
        title = "";
    }
    if (message == NULL) {
        message = "";
    }
    EM_ASM({
        alert(UTF8ToString($0) + "\n\n" + UTF8ToString($1));
    },
            title, message);
    return 0;
#else
    SDL_MessageBoxData data;
    SDL_MessageBoxButtonData button;

    SDL_zero(data);
    data.flags = flags;
    data.title = title;
    data.message = message;
    data.numbuttons = 1;
    data.buttons = &button;
    data.window = window;

    SDL_zero(button);
    button.flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    button.flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    button.text = "OK";

    return SDL_ShowMessageBox(&data, NULL);
#endif
}

SDL_bool SDL_ShouldAllowTopmost(void)
{
    return SDL_GetHintBoolean(SDL_HINT_ALLOW_TOPMOST, SDL_TRUE);
}

int SDL_SetWindowHitTest(SDL_Window *window, SDL_HitTest callback, void *callback_data)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!_this->SetWindowHitTest) {
        return SDL_Unsupported();
    } else if (_this->SetWindowHitTest(window, callback != NULL) == -1) {
        return -1;
    }

    window->hit_test = callback;
    window->hit_test_data = callback_data;

    return 0;
}

/*
 * Functions used by iOS application delegates
 */
void SDL_OnApplicationWillTerminate(void)
{
    SDL_SendAppEvent(SDL_EVENT_TERMINATING);
}

void SDL_OnApplicationDidReceiveMemoryWarning(void)
{
    SDL_SendAppEvent(SDL_EVENT_LOW_MEMORY);
}

void SDL_OnApplicationWillResignActive(void)
{
    if (_this) {
        SDL_Window *window;
        for (window = _this->windows; window != NULL; window = window->next) {
            SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
            SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_MINIMIZED, 0, 0);
        }
    }
    SDL_SendAppEvent(SDL_EVENT_WILL_ENTER_BACKGROUND);
}

void SDL_OnApplicationDidEnterBackground(void)
{
    SDL_SendAppEvent(SDL_EVENT_DID_ENTER_BACKGROUND);
}

void SDL_OnApplicationWillEnterForeground(void)
{
    SDL_SendAppEvent(SDL_EVENT_WILL_ENTER_FOREGROUND);
}

void SDL_OnApplicationDidBecomeActive(void)
{
    SDL_SendAppEvent(SDL_EVENT_DID_ENTER_FOREGROUND);

    if (_this) {
        SDL_Window *window;
        for (window = _this->windows; window != NULL; window = window->next) {
            SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
            SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
        }
    }
}

#define NOT_A_VULKAN_WINDOW "The specified window isn't a Vulkan window"

int SDL_Vulkan_LoadLibrary(const char *path)
{
    int retval;
    if (_this == NULL) {
        SDL_UninitializedVideo();
        return -1;
    }
    if (_this->vulkan_config.loader_loaded) {
        if (path && SDL_strcmp(path, _this->vulkan_config.loader_path) != 0) {
            return SDL_SetError("Vulkan loader library already loaded");
        }
        retval = 0;
    } else {
        if (!_this->Vulkan_LoadLibrary) {
            return SDL_DllNotSupported("Vulkan");
        }
        retval = _this->Vulkan_LoadLibrary(_this, path);
    }
    if (retval == 0) {
        _this->vulkan_config.loader_loaded++;
    }
    return retval;
}

SDL_FunctionPointer SDL_Vulkan_GetVkGetInstanceProcAddr(void)
{
    if (_this == NULL) {
        SDL_UninitializedVideo();
        return NULL;
    }
    if (!_this->vulkan_config.loader_loaded) {
        SDL_SetError("No Vulkan loader has been loaded");
        return NULL;
    }
    return (SDL_FunctionPointer)_this->vulkan_config.vkGetInstanceProcAddr;
}

void SDL_Vulkan_UnloadLibrary(void)
{
    if (_this == NULL) {
        SDL_UninitializedVideo();
        return;
    }
    if (_this->vulkan_config.loader_loaded > 0) {
        if (--_this->vulkan_config.loader_loaded > 0) {
            return;
        }
        if (_this->Vulkan_UnloadLibrary) {
            _this->Vulkan_UnloadLibrary(_this);
        }
    }
}

SDL_bool SDL_Vulkan_GetInstanceExtensions(unsigned *count, const char **names)
{
    if (count == NULL) {
        SDL_InvalidParamError("count");
        return SDL_FALSE;
    }

    return _this->Vulkan_GetInstanceExtensions(_this, count, names);
}

SDL_bool SDL_Vulkan_CreateSurface(SDL_Window *window,
                                  VkInstance instance,
                                  VkSurfaceKHR *surface)
{
    CHECK_WINDOW_MAGIC(window, SDL_FALSE);

    if (!(window->flags & SDL_WINDOW_VULKAN)) {
        SDL_SetError(NOT_A_VULKAN_WINDOW);
        return SDL_FALSE;
    }

    if (!instance) {
        SDL_InvalidParamError("instance");
        return SDL_FALSE;
    }

    if (surface == NULL) {
        SDL_InvalidParamError("surface");
        return SDL_FALSE;
    }

    return _this->Vulkan_CreateSurface(_this, window, instance, surface);
}

SDL_MetalView SDL_Metal_CreateView(SDL_Window *window)
{
    CHECK_WINDOW_MAGIC(window, NULL);

    if (!(window->flags & SDL_WINDOW_METAL)) {
        /* No problem, we can convert to Metal */
        if (window->flags & SDL_WINDOW_OPENGL) {
            window->flags &= ~SDL_WINDOW_OPENGL;
            SDL_GL_UnloadLibrary();
        }
        if (window->flags & SDL_WINDOW_VULKAN) {
            window->flags &= ~SDL_WINDOW_VULKAN;
            SDL_Vulkan_UnloadLibrary();
        }
        window->flags |= SDL_WINDOW_METAL;
    }

    return _this->Metal_CreateView(_this, window);
}

void SDL_Metal_DestroyView(SDL_MetalView view)
{
    if (_this && view && _this->Metal_DestroyView) {
        _this->Metal_DestroyView(_this, view);
    }
}

void *SDL_Metal_GetLayer(SDL_MetalView view)
{
    if (_this && _this->Metal_GetLayer) {
        if (view) {
            return _this->Metal_GetLayer(_this, view);
        } else {
            SDL_InvalidParamError("view");
            return NULL;
        }
    } else {
        SDL_SetError("Metal is not supported.");
        return NULL;
    }
}
