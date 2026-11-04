// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DOOM SDL2 video backend (Linux counterpart of i_video_w32.cpp)
//
// English: keep the same I_* contract as the original: the engine draws into
// screens[0] (320x200, 8-bit palettized) and calls I_FinishUpdate each frame.
// This backend presents it in a desktop-fullscreen window with nearest
// neighbour scaling, keyboard via SDL key events, mouse via relative mode.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "d_event.h"
#include "doomdef.h"

// The engine framebuffer. Owned by v_video.c.
extern byte* screens[5];

static SDL_Window*   sdl_window = 0;
static SDL_Renderer* sdl_renderer = 0;
static SDL_Texture*  sdl_texture = 0;      // 320x200 ARGB8888, streaming
static unsigned int* sdl_pixels = 0;       // converted frame staging
static SDL_Color     sdl_palette[256];
static byte          sdl_paletteRaw[256*3]; // last palette handed to us
static bool          sdl_dirtyPalette = true;
static bool          sdl_active = false;
static bool          sdl_cursorGrabbed = false;

// Mouse accumulation for the current tic.
static int  sdl_mouseDX = 0;
static int  sdl_mouseDY = 0;
static int  sdl_mouseButtons = 0;

// ---------------------------------------------------------------------------
// Cursor grab / ungrab (modern mouse look style)
// ---------------------------------------------------------------------------

static void sdl_grabMouse(void)
{
    if (sdl_cursorGrabbed || !sdl_window) return;
    SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_ShowCursor(SDL_DISABLE);
    sdl_cursorGrabbed = true;
}

static void sdl_ungrabMouse(void)
{
    if (!sdl_cursorGrabbed) return;
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);
    sdl_cursorGrabbed = false;
}

// ---------------------------------------------------------------------------
// SDL -> DOOM key translation (DOOM codes match the original xlatekey():
// ASCII for printable keys, 0x80+scancode for specials).
// ---------------------------------------------------------------------------

static int sdl_translateKey(SDL_Scancode sc)
{
    // F-keys and specials: 0x80 + set-1 scancode.
    switch (sc)
    {
      case SDL_SCANCODE_F1:  return KEY_F1;
      case SDL_SCANCODE_F2:  return KEY_F2;
      case SDL_SCANCODE_F3:  return KEY_F3;
      case SDL_SCANCODE_F4:  return KEY_F4;
      case SDL_SCANCODE_F5:  return KEY_F5;
      case SDL_SCANCODE_F6:  return KEY_F6;
      case SDL_SCANCODE_F7:  return KEY_F7;
      case SDL_SCANCODE_F8:  return KEY_F8;
      case SDL_SCANCODE_F9:  return KEY_F9;
      case SDL_SCANCODE_F10: return KEY_F10;
      case SDL_SCANCODE_F11: return KEY_F11;
      case SDL_SCANCODE_F12: return KEY_F12;
      case SDL_SCANCODE_PAUSE: return KEY_PAUSE;
      case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
      case SDL_SCANCODE_TAB: return KEY_TAB;
      case SDL_SCANCODE_ESCAPE: return KEY_ESCAPE;
      case SDL_SCANCODE_RETURN:
      case SDL_SCANCODE_KP_ENTER: return KEY_ENTER;
      case SDL_SCANCODE_SPACE: return ' ';
      case SDL_SCANCODE_LEFT:
      case SDL_SCANCODE_KP_4: return KEY_LEFTARROW;
      case SDL_SCANCODE_RIGHT:
      case SDL_SCANCODE_KP_6: return KEY_RIGHTARROW;
      case SDL_SCANCODE_UP:
      case SDL_SCANCODE_KP_8: return KEY_UPARROW;
      case SDL_SCANCODE_DOWN:
      case SDL_SCANCODE_KP_2: return KEY_DOWNARROW;
      // The original maps Delete onto BackSpace; other nav keys were
      // simply ignored (keysym > NUMKEYS).
      case SDL_SCANCODE_DELETE: return KEY_BACKSPACE;
      case SDL_SCANCODE_LSHIFT:
      case SDL_SCANCODE_RSHIFT: return KEY_RSHIFT;
      case SDL_SCANCODE_LCTRL:
      case SDL_SCANCODE_RCTRL: return KEY_RCTRL;
      case SDL_SCANCODE_LALT:
      case SDL_SCANCODE_RALT: return KEY_RALT;
      case SDL_SCANCODE_KP_MINUS: return '-';
      case SDL_SCANCODE_KP_PLUS: return '=';
      case SDL_SCANCODE_KP_MULTIPLY: return '*';
      case SDL_SCANCODE_KP_DIVIDE: return '/';
      case SDL_SCANCODE_KP_0: return '0';
      case SDL_SCANCODE_KP_1: return '1';
      case SDL_SCANCODE_KP_3: return '3';
      case SDL_SCANCODE_KP_5: return '5';
      case SDL_SCANCODE_KP_7: return '7';
      case SDL_SCANCODE_KP_9: return '9';
      case SDL_SCANCODE_KP_PERIOD: return '.';
      default: break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

void I_SetPalette (byte* palette)
{
    if (!palette) return;
    memcpy(sdl_paletteRaw, palette, 256*3);
    for (int i = 0; i < 256; ++i)
    {
        sdl_palette[i].r = palette[i*3+0];
        sdl_palette[i].g = palette[i*3+1];
        sdl_palette[i].b = palette[i*3+2];
        sdl_palette[i].a = 255;
    }
    sdl_dirtyPalette = true;
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------

void I_FinishUpdate (void)
{
    if (!sdl_window || !screens[0]) return;

    if (sdl_dirtyPalette)
    {
        // Palette no longer needs to go to hardware directly: the conversion
        // to ARGB below reads these colours every frame.
        sdl_dirtyPalette = false;
    }

    // engine framebuffer (8-bit palettized) -> ARGB8888 staging buffer
    for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; ++i)
    {
        SDL_Color* c = &sdl_palette[screens[0][i]];
        sdl_pixels[i] = ( (unsigned int)c->a << 24 )
                      | ( (unsigned int)c->r << 16 )
                      | ( (unsigned int)c->g << 8  )
                      | ( (unsigned int)c->b       );
    }

    SDL_UpdateTexture(sdl_texture, 0, sdl_pixels,
                      SCREENWIDTH * sizeof(unsigned int));

    // Fullscreen: scale the 320x200 image over the whole window with the
    // logical size set in I_InitGraphics (aspect-preserving letterbox).
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, 0, 0);
    SDL_RenderPresent(sdl_renderer);
}

// the original X11 version posted "reading disk" fiddly bits; nothing for us
void I_UpdateNoBlit (void) {}

// Declared in i_system.h; the original was a stub too.
void I_StartFrame (void) {}

void I_ReadScreen (byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

void I_InitGraphics (void)
{
    static bool done = false;
    if (done) return;
    done = true;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "I_InitGraphics: SDL_Init(VIDEO) failed: %s\n",
                SDL_GetError());
        return;
    }

    for (int i = 0; i < 256; ++i)
    {
        sdl_palette[i].r = sdl_palette[i].g = sdl_palette[i].b = 0;
        sdl_palette[i].a = 255;
    }

    // Nearest-neighbour scaling so the 320x200 frame stays crisp/pixelated.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    // Desktop fullscreen borderless window; the renderer's logical size
    // keeps the 8:5 aspect and letterboxes the rest.
    sdl_window = SDL_CreateWindow("DOOM (C++ port)",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SCREENWIDTH * 2, SCREENHEIGHT * 2,
                                  SDL_WINDOW_SHOWN
                                  | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!sdl_window)
    {
        // Retry windowed if the demanding fullscreen failed.
        fprintf(stderr, "I_InitGraphics: fullscreen failed (%s), trying windowed\n",
                SDL_GetError());
        sdl_window = SDL_CreateWindow("DOOM (C++ port)",
                                      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      SCREENWIDTH * 2, SCREENHEIGHT * 2,
                                      SDL_WINDOW_SHOWN);
    }
    if (!sdl_window)
    {
        fprintf(stderr, "I_InitGraphics: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        return;
    }

    sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
                                      SDL_RENDERER_ACCELERATED);
    if (!sdl_renderer)
    {
        // Software fallback for headless/broken GPU setups.
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
                                          SDL_RENDERER_SOFTWARE);
    }
    if (!sdl_renderer)
    {
        fprintf(stderr, "I_InitGraphics: SDL_CreateRenderer failed: %s\n",
                SDL_GetError());
        return;
    }

    // Logical 8:5 aspect: everything is presented at 320x200 coordinates.
    SDL_RenderSetLogicalSize(sdl_renderer, SCREENWIDTH, SCREENHEIGHT);

    sdl_texture = SDL_CreateTexture(sdl_renderer,
                                    SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    SCREENWIDTH, SCREENHEIGHT);
    if (!sdl_texture)
    {
        fprintf(stderr, "I_InitGraphics: SDL_CreateTexture failed: %s\n",
                SDL_GetError());
        return;
    }

    sdl_pixels = (unsigned int*)malloc(
        SCREENWIDTH * SCREENHEIGHT * sizeof(unsigned int));
    memset(sdl_pixels, 0, SCREENWIDTH * SCREENHEIGHT * sizeof(unsigned int));

    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderPresent(sdl_renderer);

    sdl_grabMouse();
    SDL_RaiseWindow(sdl_window);
}

void I_ShutdownGraphics (void)
{
    sdl_ungrabMouse();
    if (sdl_texture)
    {
        SDL_DestroyTexture(sdl_texture);
        sdl_texture = 0;
    }
    if (sdl_renderer)
    {
        SDL_DestroyRenderer(sdl_renderer);
        sdl_renderer = 0;
    }
    if (sdl_window)
    {
        SDL_DestroyWindow(sdl_window);
        sdl_window = 0;
    }
    if (sdl_pixels)
    {
        free(sdl_pixels);
        sdl_pixels = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// Start-of-tic work: pump messages, post keyboard and mouse events.
void I_StartTic (void)
{
    SDL_Event ev;
    event_t   de;               // doom event

    while (SDL_PollEvent(&ev))
    {
        switch (ev.type)
        {
          case SDL_QUIT:
            I_Quit();
            return;

          case SDL_KEYDOWN:
          case SDL_KEYUP:
            {
                // don't spam DOOM with autorepeats
                if (ev.type == SDL_KEYDOWN && ev.key.repeat)
                    break;
                int keyval;
                if (ev.key.keysym.sym >= SDLK_SPACE
                    && ev.key.keysym.sym <= SDLK_z)
                    keyval = (int)ev.key.keysym.sym;   // printable ASCII
                else
                    keyval = sdl_translateKey(ev.key.keysym.scancode);
                // Esc: release the mouse so the user can leave the window
                // freely (menu/pause); clicking back in re-grabs it.
                if (keyval == KEY_ESCAPE && ev.type == SDL_KEYDOWN)
                    sdl_ungrabMouse();
                if (keyval)
                {
                    de.type = (ev.type == SDL_KEYDOWN) ? ev_keydown : ev_keyup;
                    de.data1 = keyval;
                    de.data2 = de.data3 = 0;
                    D_PostEvent(&de);
                }
            }
            break;

          case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            {
                sdl_active = true;
                sdl_grabMouse();
            }
            else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            {
                sdl_active = false;
                sdl_ungrabMouse();
            }
            break;
        }
    }

    // Mouse state (relative motion + buttons), one event per tic, ALWAYS —
    // even when nothing changed, so button releases reach the engine.
    {
        int dx = 0, dy = 0;
        unsigned int buttons = SDL_GetRelativeMouseState(&dx, &dy);
        sdl_mouseDX += dx;
        sdl_mouseDY += dy;
        sdl_mouseButtons = 0;
        if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))   sdl_mouseButtons |= 1;
        if (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) sdl_mouseButtons |= 2;
        if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT))  sdl_mouseButtons |= 4;

        // Re-grab on click when we somehow lost the grab but still have
        // focus (e.g. after Esc-release, click back into the game window).
        if (sdl_active && !sdl_cursorGrabbed && (sdl_mouseButtons & 1))
        {
            int mx = 0, my = 0, wx = 0, wy = 0, ww = 0, wh = 0;
            SDL_GetGlobalMouseState(&mx, &my);
            SDL_GetWindowPosition(sdl_window, &wx, &wy);
            SDL_GetWindowSize(sdl_window, &ww, &wh);
            if (mx >= wx && mx < wx+ww && my >= wy && my < wy+wh)
            {
                sdl_grabMouse();
                // that click must not fire the weapon
                SDL_GetRelativeMouseState(0, 0);
                sdl_mouseButtons &= ~1;
            }
        }

        de.type = ev_mouse;
        de.data1 = sdl_mouseButtons;
        // Match the Win32 front end: X deltas x4, Y inverted
        // (SDL Y grows downward, DOOM expects up = positive).
        de.data2 = sdl_mouseDX * 4;
        de.data3 = (-sdl_mouseDY) * 4;
        D_PostEvent(&de);
        sdl_mouseDX = 0;
        sdl_mouseDY = 0;
    }
}
