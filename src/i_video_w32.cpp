// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DOOM Win32 GDI video backend (replaces the X11 i_video.c of linuxdoom-1.10)
//
// English: keep the same I_* contract as the original: the engine draws into
// screens[0] (320x200, 8-bit palettized) and calls I_FinishUpdate each frame.
// This backend presents it in a 640x400 window, nearest-neighbour x2, with
// palette via SetDIBColorTable, keyboard via WM_KEY*, mouse via Raw Input.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include "w32compat.h"   // windows.h with DOOM-safe defines

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

// Treat the window as fixed 640x400 (320x200 pixels, x2).
#define W32_SCALE 2
#define W32_W (SCREENWIDTH * W32_SCALE)
#define W32_H (SCREENHEIGHT * W32_SCALE)

static HWND  w32_window = 0;
static HDC   w32_memDC = 0;
static HBITMAP w32_dib = 0;            // 320x200 8bpp DIB section
static byte* w32_dibbits = 0;          // its memory
static HBITMAP w32_dib2x = 0;          // 640x400 staging, 8bpp
static byte* w32_dib2xbits = 0;
static BITMAPINFO w32_bmi;             // 320x200 header
static BITMAPINFO w32_bmi2x;
static bool w32_dirtyPalette = true;
static RGBQUAD w32_pal[256];
static byte  w32_paletteRaw[256*3];    // last palette handed to us by the game
static bool  w32_active = false;

// Mouse accumulation for the current tic.
static int  w32_mouseDX = 0;
static int  w32_mouseDY = 0;
static int  w32_mouseButtons = 0;
static bool w32_cursorGrabbed = false;

// ---------------------------------------------------------------------------
// Cursor grab / ungrab (modern mouse look style)
// ---------------------------------------------------------------------------

static void w32_grabMouse(void)
{
    if (w32_cursorGrabbed || !w32_window) return;
    RECT rc;
    GetClientRect(w32_window, &rc);
    ClientToScreen(w32_window, (POINT*)&rc.left);
    ClientToScreen(w32_window, (POINT*)&rc.right);
    ClipCursor(&rc);
    SetCapture(w32_window);
    ShowCursor(FALSE);
    w32_cursorGrabbed = true;
}

static void w32_ungrabMouse(void)
{
    if (!w32_cursorGrabbed) return;
    ClipCursor(NULL);
    ReleaseCapture();
    ShowCursor(TRUE);
    w32_cursorGrabbed = false;
}

// ---------------------------------------------------------------------------
// VK -> DOOM key translation (DOOM codes match the original xlatekey():
// ASCII for printable keys, 0x80+scancode for specials).
// ---------------------------------------------------------------------------
static int w32_translateKey(WPARAM vk)
{
    // F-keys and specials: 0x80 + set-1 scancode.
    switch (vk)
    {
      case VK_F1:  return KEY_F1;
      case VK_F2:  return KEY_F2;
      case VK_F3:  return KEY_F3;
      case VK_F4:  return KEY_F4;
      case VK_F5:  return KEY_F5;
      case VK_F6:  return KEY_F6;
      case VK_F7:  return KEY_F7;
      case VK_F8:  return KEY_F8;
      case VK_F9:  return KEY_F9;
      case VK_F10: return KEY_F10;
      case VK_F11: return KEY_F11;
      case VK_F12: return KEY_F12;
      case VK_PAUSE: return KEY_PAUSE;
      case VK_BACK: return KEY_BACKSPACE;
      case VK_TAB: return KEY_TAB;
      case VK_ESCAPE: return KEY_ESCAPE;
      case VK_RETURN: return KEY_ENTER;
      case VK_SPACE: return ' ';
      case VK_LEFT: return KEY_LEFTARROW;
      case VK_RIGHT: return KEY_RIGHTARROW;
      case VK_UP: return KEY_UPARROW;
      case VK_DOWN: return KEY_DOWNARROW;
      // The original maps Delete onto BackSpace; other nav keys were
      // simply ignored (keysym > NUMKEYS).
      case VK_DELETE: return KEY_BACKSPACE;
      case VK_SHIFT:
      case VK_LSHIFT:
      case VK_RSHIFT: return KEY_RSHIFT;
      case VK_CONTROL:
      case VK_LCONTROL:
      case VK_RCONTROL: return KEY_RCTRL;
      case VK_MENU:
      case VK_LMENU:
      case VK_RMENU: return KEY_RALT;
      case VK_OEM_1:      return ';';
      case VK_SUBTRACT: return '-';
      case VK_MULTIPLY: return '*';
      case VK_DIVIDE: return '/';
      case VK_NUMPAD0: return '0';
      case VK_NUMPAD1: return '1';
      case VK_NUMPAD2: return '2';
      case VK_NUMPAD3: return '3';
      case VK_NUMPAD4: return '4';
      case VK_NUMPAD5: return '5';
      case VK_NUMPAD6: return '6';
      case VK_NUMPAD7: return '7';
      case VK_NUMPAD8: return '8';
      case VK_NUMPAD9: return '9';
      case VK_DECIMAL: return '.';
      default: break;
    }

    switch (vk)
    {
      case VK_OEM_PERIOD: return '.';
      case VK_OEM_COMMA:  return ',';
      case VK_OEM_MINUS:  return '-';
      case VK_OEM_PLUS:   return '=';
      case VK_OEM_1:      return ';';
      case VK_OEM_2:      return '/';
      case VK_OEM_3:      return '`';
      case VK_OEM_4:      return '[';
      case VK_OEM_5:      return '\\';
      case VK_OEM_6:      return ']';
      case VK_OEM_7:      return '\'';
      case VK_OEM_CLEAR:  return 0;      // skip numpad-clear
      default: break;
    }

    if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= 'A' && vk <= 'Z') return (int)vk + 32;   // lowercase like X11 keysyms
    return 0;
}

// ---------------------------------------------------------------------------
// Raw input for mouse (relative movement, no legacy messages for mouse)
// ---------------------------------------------------------------------------

static void w32_registerMouse()
{
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;                    // mouse
    rid.dwFlags = RIDEV_NOLEGACY;
    rid.hwndTarget = w32_window;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

static void w32_handleRawMouse(LPARAM lParam)
{
    UINT dwSize = sizeof(RAWINPUT);
    static byte rawIn[64];
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, rawIn, &dwSize,
                        sizeof(RAWINPUTHEADER)) == (UINT)-1)
        return;

    RAWINPUT* raw = (RAWINPUT*)rawIn;
    if (raw->header.dwType != RIM_TYPEMOUSE)
        return;

    if (raw->data.mouse.usFlags == MOUSE_MOVE_RELATIVE
        || !(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
    {
        w32_mouseDX += raw->data.mouse.lLastX;
        w32_mouseDY += raw->data.mouse.lLastY;
    }

    USHORT f = raw->data.mouse.usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   w32_mouseButtons |= 1;
    if (f & RI_MOUSE_LEFT_BUTTON_UP)     w32_mouseButtons &= ~1;
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) w32_mouseButtons |= 2;
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP)   w32_mouseButtons &= ~2;
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  w32_mouseButtons |= 4;
    if (f & RI_MOUSE_RIGHT_BUTTON_UP)    w32_mouseButtons &= ~4;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK w32_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int keyval = 0;
    event_t ev;

    switch (msg)
    {
      case WM_CLOSE:
        I_Quit();
        return 0;

      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        // don't spam DOOM with autorepeats
        if (lp & (1 << 30)) break;
        keyval = w32_translateKey(wp);
        // Esc: release the mouse so the user can leave the window freely
        // (menu/pause); clicking back in the window re-grabs it.
        if (keyval == KEY_ESCAPE) w32_ungrabMouse();
        if (keyval)
        {
            ev.type = ev_keydown;
            ev.data1 = keyval;
            D_PostEvent(&ev);
        }
        return 0;

      case WM_KEYUP:
      case WM_SYSKEYUP:
        keyval = w32_translateKey(wp);
        if (keyval)
        {
            ev.type = ev_keyup;
            ev.data1 = keyval;
            D_PostEvent(&ev);
        }
        return 0;

      case WM_INPUT:
        w32_handleRawMouse(lp);
        return 0;

      case WM_ACTIVATE:
        w32_active = (LOWORD(wp) != WA_INACTIVE);
        if (w32_active && w32_window)
            w32_grabMouse();
        else
            w32_ungrabMouse();
        return 0;

      case WM_SETCURSOR:
        if (w32_active)
        {
            SetCursor(NULL);
            return TRUE;
        }
        break;

      case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (w32_memDC && w32_dib2x)
                BitBlt(hdc, 0, 0, W32_W, W32_H, w32_memDC, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

void I_SetPalette (byte* palette)
{
    if (!palette) return;
    memcpy(w32_paletteRaw, palette, 256*3);
    for (int i = 0; i < 256; ++i)
    {
        // GDI palettized DIBs accept the full 0..255 range directly.
        w32_pal[i].rgbRed   = palette[i*3+0];
        w32_pal[i].rgbGreen = palette[i*3+1];
        w32_pal[i].rgbBlue  = palette[i*3+2];
        w32_pal[i].rgbReserved = 0;
    }
    w32_dirtyPalette = true;
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------

void I_FinishUpdate (void)
{
    if (!w32_window || !screens[0]) return;

    if (w32_dirtyPalette)
    {
        SetDIBColorTable(w32_memDC, 0, 256, w32_pal);
        w32_dirtyPalette = false;
    }

    // engine framebuffer -> 320x200 DIB bits
    memcpy(w32_dibbits, screens[0], SCREENWIDTH * SCREENHEIGHT);

    // nearest-neighbour x2 into the 640x400 staging DIB
    for (int y = 0; y < SCREENHEIGHT; ++y)
    {
        const byte* src = w32_dibbits + y * SCREENWIDTH;
        byte* dst1 = w32_dib2xbits + (y*2) * W32_W;
        byte* dst2 = w32_dib2xbits + (y*2+1) * W32_W;
        for (int x = 0; x < SCREENWIDTH; ++x)
        {
            byte p = src[x];
            dst1[x*2] = dst1[x*2+1] = p;
            dst2[x*2] = dst2[x*2+1] = p;
        }
    }

    RECT rc;
    if (GetClientRect(w32_window, &rc) && rc.right > 0 && rc.bottom > 0)
    {
        // Fullscreen: scale the 640x400 staging image over the whole client
        // area, preserving its 8:5 aspect (black letterbox bars otherwise).
        int cw = rc.right, ch = rc.bottom;
        int dw = cw, dh = dw * W32_H / W32_W;
        if (dh > ch) { dh = ch; dw = dh * W32_W / W32_H; }
        int dx = (cw - dw) / 2, dy = (ch - dh) / 2;

        HDC hdc = GetDC(w32_window);
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchBlt(hdc, dx, dy, dw, dh, w32_memDC, 0, 0, W32_W, W32_H, SRCCOPY);
        ReleaseDC(w32_window, hdc);
    }
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

static BITMAPINFOHEADER w32_prepareBmi(BITMAPINFO* bmi, int w, int h)
{
    memset(bmi, 0, sizeof(BITMAPINFO));
    bmi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biWidth       = w;
    bmi->bmiHeader.biHeight      = -h;     // top-down
    bmi->bmiHeader.biPlanes      = 1;
    bmi->bmiHeader.biBitCount    = 8;
    bmi->bmiHeader.biCompression = BI_RGB;
    return bmi->bmiHeader;
}

void I_InitGraphics (void)
{
    static bool done = false;
    if (done) return;
    done = true;

    for (int i = 0; i < 256; ++i)
    {
        w32_pal[i].rgbRed = w32_pal[i].rgbGreen = w32_pal[i].rgbBlue = 0;
        w32_pal[i].rgbReserved = 0;
    }

    // window class
    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = w32_wndproc;
    wc.hInstance = GetModuleHandle(0);
    wc.hIcon = LoadIcon(0, IDI_APPLICATION);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DoomCppWindow";
    RegisterClass(&wc);

    int scrw = GetSystemMetrics(SM_CXSCREEN);
    int scrh = GetSystemMetrics(SM_CYSCREEN);
    // Fullscreen: borderless popup window covering the whole screen.
    w32_window = CreateWindowEx(0, "DoomCppWindow", "DOOM (C++ port)",
                              WS_POPUP,
                              0, 0,
                              scrw, scrh, 0, 0, wc.hInstance, 0);

    w32_memDC = CreateCompatibleDC(0);

    // 320x200 DIB section (engine framebuffer staging)
    w32_prepareBmi(&w32_bmi, SCREENWIDTH, SCREENHEIGHT);
    w32_dib = CreateDIBSection(0, &w32_bmi, DIB_RGB_COLORS,
                               (void**)&w32_dibbits, 0, 0);
    SelectObject(w32_memDC, w32_dib);

    // 640x400 staging DIB drawn at 1:1 into the window
    w32_prepareBmi(&w32_bmi2x, W32_W, W32_H);
    w32_dib2x = CreateDIBSection(w32_memDC, &w32_bmi2x, DIB_RGB_COLORS,
                                 (void**)&w32_dib2xbits, 0, 0);
    SelectObject(w32_memDC, w32_dib2x);

    SetDIBColorTable(w32_memDC, 0, 256, w32_pal);

    // Something screens[0] can safely show before the first palette.
    memset(w32_dibbits, 0, SCREENWIDTH * SCREENHEIGHT);
    memset(w32_dib2xbits, 0, W32_W * W32_H);

    w32_registerMouse();
    ShowWindow(w32_window, SW_SHOWNORMAL);
    UpdateWindow(w32_window);
}

void I_ShutdownGraphics (void)
{
    if (w32_window)
    {
        ClipCursor(NULL);
        DestroyWindow(w32_window);
        w32_window = 0;
    }
    if (w32_memDC) { DeleteDC(w32_memDC); w32_memDC = 0; }
    if (w32_dib)   { DeleteObject(w32_dib); w32_dib = 0; }
    if (w32_dib2x) { DeleteObject(w32_dib2x); w32_dib2x = 0; }
}

// Start-of-tic work: pump messages, post accumulated mouse events.
void I_StartTic (void)
{
    MSG msg;
    while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Re-grab on click when we somehow lost the grab but still have focus
    // (e.g. after Esc-release, click back into the game window).
    if (w32_active && !w32_cursorGrabbed && (w32_mouseButtons & 1))
    {
        POINT p;
        RECT rc;
        GetCursorPos(&p);
        GetClientRect(w32_window, &rc);
        ClientToScreen(w32_window, (POINT*)&rc.left);
        ClientToScreen(w32_window, (POINT*)&rc.right);
        if (PtInRect(&rc, p))
        {
            w32_grabMouse();
            w32_mouseButtons &= ~1;   // that click must not fire the weapon
        }
    }

    // Post one mouse event per tic, ALWAYS — even when nothing changed.
    // If we skip zero-delta tics, a button release (all state back to 0)
    // never reaches the engine and mousebuttons[] keeps the stale "held"
    // value: the attack flag sticks forever. Posting unconditionally
    // delivers both press and release states reliably.
    {
        event_t ev;
        ev.type = ev_mouse;
        ev.data1 = w32_mouseButtons;
        // Match the original X11 front end: X deltas x4, Y inverted
        // (Windows Y grows downward, DOOM expects up = positive).
        ev.data2 = w32_mouseDX * 4;
        ev.data3 = (-w32_mouseDY) * 4;
        D_PostEvent(&ev);
        w32_mouseDX = 0;
        w32_mouseDY = 0;
    }
}
