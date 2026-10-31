#include "Window.h"

#include "Win95/stdGdi.h"
#include "Platform/std3D.h"
#include "Main/Main.h"
#include "Main/jkMain.h"
#include "Main/jkGame.h"
#include "Gui/jkGUI.h"
#include "Gui/jkGUIRend.h"
#include "Win95/stdDisplay.h"
#include "World/jkPlayer.h"
#include "Platform/stdControl.h"
#include "stdPlatform.h"
#include "Devices/sithConsole.h"
#include "Platform/wuRegistry.h"
#include "Main/jkQuakeConsole.h"
#include "Main/jkTouchControls.h"

#include "Dw/dwMain.h"

#include "jk.h"

#ifdef ARCH_WASM
#include <emscripten.h>
#endif

#ifdef PLATFORM_WSRELAY
#include "Platform/Networking/WSRelay/stdComm_WSRelay.h"
#endif

#ifdef SDL2_RENDER

#include <fcntl.h> 
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif //!_WIN32

#if !defined(WIN64_MINGW) && !defined(_WIN32)
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#else
#include <conio.h>
#endif
//#include <stropts.h>

#include "SDL2_helper.h"

#ifdef TARGET_IOS
#include "Platform/iOS/iosAngle.h"
#define SDL_GL_SwapWindow(pWindow)          iosAngle_SwapBuffers()
#define SDL_GL_SetSwapInterval(interval)    iosAngle_SetSwapInterval(interval)
#define SDL_GL_DestroyContext(pContext)     iosAngle_DestroyContext()
#define SDL_GL_MakeCurrent(pWindow, pCtx)   ((void)0)
#endif

#include <string.h>

//#include <GL/glew.h>
#ifndef MACOS
//#include <GL/gl.h>
#endif
#include "Win95/Video.h"

#if defined(MACOS)
#include <stdbool.h>
#import <Carbon/Carbon.h>
#endif

extern int Window_xPos, Window_yPos;
#endif // SDL2_RENDER

int Window_xSize = WINDOW_DEFAULT_WIDTH;
int Window_ySize = WINDOW_DEFAULT_HEIGHT;
int Window_screenXSize = WINDOW_DEFAULT_WIDTH;
int Window_screenYSize = WINDOW_DEFAULT_HEIGHT;
int Window_isHiDpi = 0;
int Window_isFullscreen = 0;
int Window_needsRecreate = 0;
int Window_bShouldPopSteamKeyboard = 0;

void Window_SetHiDpi(int val)
{
    if (Window_isHiDpi != val)
    {
        Window_isHiDpi = val;

        Window_needsRecreate = 1;
    }

    wuRegistry_SaveBool("Window_isHiDpi", Window_isHiDpi);
}

void Window_SetFullscreen(int val)
{
    if (Window_isFullscreen != val)
    {
        // Reset window when exiting fullscreen
        // TODO: Add settings for these sizes maybe?
        if (Window_isFullscreen && !val) {
            Window_xSize = WINDOW_DEFAULT_WIDTH;
            Window_ySize = WINDOW_DEFAULT_HEIGHT;
            Window_screenXSize = WINDOW_DEFAULT_WIDTH;
            Window_screenYSize = WINDOW_DEFAULT_HEIGHT;
#ifdef SDL2_RENDER
            Window_xPos = SDL_WINDOWPOS_CENTERED;
            Window_yPos = SDL_WINDOWPOS_CENTERED;
#endif
        }

        Window_isFullscreen = val;
        Window_needsRecreate = 1;
    }

    wuRegistry_SaveBool("Window_isFullscreen", Window_isFullscreen);
    
}

//static wm_handler Window_ext_handlers[16] = {0};

int Window_AddMsgHandler(WindowHandler_t a1)
{
    int i = 0;

    // Added: no duplicates
    for (i = 0; i < 16; i++)
    {
        if (Window_ext_handlers[i].exists && Window_ext_handlers[i].handler == a1)
            return 1;
    }

    for (i = 0; i < 16; i++)
    {
        if ( !Window_ext_handlers[i].exists )
            break;
    }
    
    // Added: no OOB
    if (i >= 16) return 1;

    Window_ext_handlers[i].handler = a1;
    Window_ext_handlers[i].exists = 1;
    ++g_handler_count;
    return 1;
}

int Window_RemoveMsgHandler(WindowHandler_t a1)
{
    int i = 0;

    // Added: the original would still decrement on missing handlers
    for (i = 0; i < 16; i++)
    {
        if ( Window_ext_handlers[i].handler == a1 )
        {
            Window_ext_handlers[i].handler = 0;
            Window_ext_handlers[i].exists = 0;
            g_handler_count -= 1; // doing g_handler_count-- changes behavior???
            return 1;
        }
    }

    return 1;
}

int Window_AddDialogHwnd(HWND a1)
{
    int v1; // eax

    v1 = g_thing_two_some_dialog_count;
    if ( (unsigned int)g_thing_two_some_dialog_count >= 0x10 )
        return 0;
    Window_aDialogHwnds[g_thing_two_some_dialog_count] = a1;
    g_thing_two_some_dialog_count = v1 + 1;
    return 1;
}

#if !defined(SDL2_RENDER) && defined(WIN32)
#define dword_855E98 (*(int*)0x855E98)
#define dword_855DE4 (*(int*)0x855DE4)
#else
static int dword_855E98 = 0;
static int dword_855DE4 = 0;
#endif // SDL2_RENDER

int Window_msg_main_handler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    int handler_count; // ebx
    struct wm_handler *ext_handler; // esi
    DWORD dwProcessId; // [esp+10h] [ebp-8h] BYREF
    LRESULT v10; // [esp+14h] [ebp-4h] BYREF

    switch ( Msg )
    {
        case WM_CREATE:
            g_app_active = 0;
            g_window_active = 0;
            break;
        case WM_DESTROY:
            g_window_not_destroyed = 0;
            Main_Shutdown();
            break;
        case WM_ACTIVATE:
            if ( (uint16_t)wParam == 2 || (uint16_t)wParam == 1 )// WA_ACTIVE or WA_CLICKACTIVE
            {
                g_window_active = 1;
                if ( dword_855E98 )
                {
                    dword_855E98 = 0;
                    if ( Window_setCooperativeLevel )
                        Window_setCooperativeLevel(0);
                }
#ifdef WIN32_BLOBS
                jk_SetFocus(g_hWnd);
#endif
            }
            else
            {
                if ( dword_855DE4 == 1 && g_window_not_destroyed && g_app_active && !dword_855E98 )
                {
                    dwProcessId = 0;
                    lParam = 1;
                    if ( lParam )
                    {
#ifdef WIN32_BLOBS
                        jk_GetWindowThreadProcessId((HWND)lParam, (LPDWORD)&lParam);
                        jk_GetWindowThreadProcessId(hWnd, &dwProcessId);
#endif
                    }
                    if ( dwProcessId == lParam )
                    {
                        dword_855E98 = 1;
                        if ( Window_drawAndFlip )
                            Window_drawAndFlip(0);
                    }
                }
                g_window_active = 0;
            }
            break;
        case WM_ACTIVATEAPP:
            g_app_active = wParam != 0;
            break;
        default:
            break;
    }

    if ( !g_app_active || (g_app_suspended = 1, !g_window_active) )
        g_app_suspended = 0;
    handler_count = 0;

    if ( g_handler_count <= 0 )
        return Window_DefaultHandler(hWnd, Msg, wParam, lParam, NULL);

    for ( ext_handler = Window_ext_handlers; !ext_handler->exists || !ext_handler->handler(hWnd, Msg, wParam, lParam, &v10); ++ext_handler )
    {
        if ( ++handler_count >= g_handler_count )
            return Window_DefaultHandler(hWnd, Msg, wParam, lParam, NULL);
    }
    return v10;
}

#if !defined(SDL2_RENDER) && defined(WIN32)

int Window_Main(HINSTANCE hInstance, int a2, char *lpCmdLine, int nShowCmd, LPCSTR lpWindowName)
{
    int result;
    WNDCLASSEXA wndClass;
    MSG msg;

    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = hInstance;
    g_nShowCmd = nShowCmd;

    wndClass.cbSize = 48;
    wndClass.hInstance = hInstance;
    wndClass.lpszClassName = "wKernel";
    wndClass.lpszMenuName = 0;
    wndClass.lpfnWndProc = Window_msg_main_handler;
    wndClass.style = 3;
    wndClass.hIcon = jk_LoadIconA(hInstance, "APPICON");
    if ( !wndClass.hIcon )
        wndClass.hIcon = jk_LoadIconA(0, (void*)32512);
    wndClass.hIconSm = jk_LoadIconA(hInstance, "APPICON");
    if ( !wndClass.hIconSm )
        wndClass.hIconSm = jk_LoadIconA(0, (void*)32512);
    wndClass.hCursor = jk_LoadCursorA(0, (void*)0x7F00);
    wndClass.cbClsExtra = 0;
    wndClass.cbWndExtra = 0;
    wndClass.hbrBackground = jk_GetStockObject(4);

    if (jk_RegisterClassExA(&wndClass))
    {
        if ( jk_FindWindowA("wKernel", lpWindowName) )
            jk_exit(-1);

        uint32_t hres = jk_GetSystemMetrics(1);
        uint32_t vres = jk_GetSystemMetrics(0);
        g_hWnd = jk_CreateWindowExA(0x40000u, "wKernel", lpWindowName, 0x90000000, 0, 0, vres, hres, 0, 0, hInstance, 0);

        if (g_hWnd)
        {
            g_hInstance = hInstance;
            jk_ShowWindow(g_hWnd, 1);
            jk_UpdateWindow(g_hWnd);
        }
    }

    stdGdi_SetHwnd(g_hWnd);
    stdGdi_SetHInstance(g_hInstance);
    jk_InitCommonControls();

    g_855E8C = 2 * jk_GetSystemMetrics(32);
    uint32_t metrics_32 = jk_GetSystemMetrics(32);
    g_855E90 = jk_GetSystemMetrics(15) + 2 * metrics_32;
    result = Main_Startup(lpCmdLine);

    if (!result) return result;

    
    g_window_not_destroyed = 1;

    while (1)
    {
        if (jk_PeekMessageA(&msg, 0, 0, 0, 0))
        {
            if (!jk_GetMessageA(&msg, 0, 0, 0))
            {
                result = msg.wParam;
                g_should_exit = 1;
                break;
            }

            uint32_t some_cnt = 0;
            if (g_thing_two_some_dialog_count > 0)
            {
#if 0
                v16 = &thing_three;
                do
                {
                    //TODO if ( jk_IsDialogMessageA(*v16, &msg) )
                    //  break;
                    ++some_cnt;
                    ++v16;
                }
                while ( some_cnt < g_thing_two_some_dialog_count );
#endif
            }

            if (some_cnt == g_thing_two_some_dialog_count)
            {
                jk_TranslateMessage(&msg);
                jk_DispatchMessageA(&msg);
            }

            if (!jk_PeekMessageA(&msg, 0, 0, 0, 0))
            {
                result = 0;
                if ( g_should_exit )
                    return result;
            }
        }

        //if (user32->stopping) break;

        jkMain_GuiAdvance();
    }

    return result;
}

int Window_DefaultHandler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, void* unused)
{
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

#endif

#ifdef SDL2_RENDER

SDL_Window* displayWindow = NULL;
SDL_Event event;
SDL_GLContext glWindowContext;

int Window_lastXRel = 0;
int Window_lastYRel = 0;
static float Window_xRelLeftOver = 0.0f; // Added
static float Window_yRelLeftOver = 0.0f; // Added
int Window_lastSampleTime = 0;
int Window_lastSampleMs = 0;
int Window_bMouseLeft = 0;
int Window_bMouseRight = 0;
int Window_resized = 0;
int Window_mouseX = 0;
int Window_mouseY = 0;
int Window_mouseWheelX = 0;
int Window_mouseWheelY = 0;
int Window_lastMouseX = 0;
int Window_lastMouseY = 0;
int Window_xPos = SDL_WINDOWPOS_CENTERED;
int Window_yPos = SDL_WINDOWPOS_CENTERED;
int last_jkGame_isDDraw = 0;
#ifdef QUAKE_CONSOLE
int last_jkQuakeConsole_bOpen = 0;
#endif
int Window_menu_mouseX = 0;
int Window_menu_mouseY = 0;

extern int jkGuiBuildMulti_bRendering;

#ifdef ARCH_WASM
// Added: browsers draw their own "has control of your pointer" notice each
// time the game locks the mouse, and while it's locked they keep Esc for
// themselves: Esc frees the mouse and never reaches the game. SDL isn't told
// when that happens, so the game checks the browser every frame.
EM_JS(int, Window_wasmIsPointerLocked, (), {
  return document.pointerLockElement === Module.canvas ? 1 : 0;
});

// SDL waits for the next click or key before asking for the mouse lock. While
// the page is fullscreen, Firefox allows the lock straight away, and so does
// Chrome when the page let go of it itself (for a menu), so ask right away
// there, at most twice a second. Elsewhere the browser refuses and SDL's
// request still follows.
EM_JS(void, Window_wasmLockPointerNow, (), {
  var canvas = Module.canvas;
  if (!document.fullscreenElement || !document.hasFocus() || !canvas.requestPointerLock) return;
  var now = performance.now();
  if (Module.jkLastPointerLockTry && now - Module.jkLastPointerLockTry < 500) return;
  Module.jkLastPointerLockTry = now;
  try {
    var request = canvas.requestPointerLock();
    if (request && request.catch) request.catch(function() {});
  } catch (e) {}
});

// Added: how a frame ends in the browser. SDL's swap ended it with emscripten_sleep(0), a
// setTimeout(0), which browsers hold for at least 4 ms once timers nest and which takes no
// account of the screen or the graphics card: a fast machine drew frames nobody saw, and a
// slow card let the game queue frames until the browser stopped everything for seconds to
// catch up.
// With Vsync on (the default), a frame waits for the next screen refresh, and the next
// frame is drawn in it. With Vsync off, it yields through a MessageChannel, which has no
// clamp, while the card has finished the frame before this one and for at most two frames
// in a refresh; otherwise it waits for the refresh. A hidden tab gets no refreshes, so there
// it waits on a timer as before, and a host's game keeps running, but also until the card
// has drawn the frame: the browser doesn't slow timers in a tab playing sound, and the game
// got seconds ahead of the card there, which came out as a freeze when the tab was shown.
// Module.jkFrames keeps when the last in-game frames ended and when the game got the
// thread back, for the page's slow-frame note and ?bench.
EM_ASYNC_JS(void, Window_wasmEndFrame, (int bVsync, int bInGame), {
  var pace = Module.jkPace;
  if (!pace) {
    pace = Module.jkPace = { sinceRefresh: 0, refreshAsked: false, resume: null, channel: new MessageChannel(), fences: [] };
    pace.channel.port1.onmessage = function() { var resume = pace.resume; pace.resume = null; if (resume) resume(); };
  }
  var frames = Module.jkFrames;
  if (!frames) frames = Module.jkFrames = { size: 2048, count: 0, broken: true, ends: new Float64Array(2048),
                                            resumes: new Float64Array(2048), joined: new Uint8Array(2048) };
  var ended = performance.now();
  var wasHidden = document.hidden;

  /* A fence after each frame tells when the card has drawn it, the last two kept. Its state
     only changes between tasks. */
  var gl = Module.ctx;
  function drawn(fence) {
    try { return gl.isContextLost() || gl.getSyncParameter(fence, gl.SYNC_STATUS) === gl.SIGNALED; } catch (e) { return true; }
  }
  try {
    if (wasHidden || !bVsync) {
      pace.fences.push(gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0));
      gl.flush();
    }
    while (pace.fences.length > (wasHidden || !bVsync ? 2 : 0)) gl.deleteSync(pace.fences.shift());
  } catch (e) {
    pace.fences = [];
  }
  var last = pace.fences[pace.fences.length - 1], before = pace.fences.length > 1 ? pace.fences[0] : null;

  if (wasHidden) {
    var started = performance.now();
    do {
      await new Promise(function(resolve) { setTimeout(resolve, 0); });
    } while (last && document.hidden && performance.now() - started < 1000 && !drawn(last));
  } else if (bVsync || pace.sinceRefresh >= 2 || (before && !drawn(before))) {
    /* A tab hidden while it waits gets no refresh, so that ends the wait too. */
    await new Promise(function(resolve) {
      function done() { document.removeEventListener('visibilitychange', done); resolve(); }
      document.addEventListener('visibilitychange', done);
      requestAnimationFrame(done);
    });
    pace.sinceRefresh = 0;
  } else {
    if (!pace.refreshAsked) {
      pace.refreshAsked = true;
      requestAnimationFrame(function() { pace.refreshAsked = false; pace.sinceRefresh = 0; });
    }
    pace.sinceRefresh++;
    await new Promise(function(resolve) { pace.resume = resolve; pace.channel.port2.postMessage(0); });
  }

  /* joined: the frame began when the game got the thread back from the one before, so the
     time between the two is a frame time. Not after a menu, or a frame outside a level or in a
     hidden tab. */
  if (bInGame && !wasHidden && !document.hidden) {
    var i = frames.count++ % frames.size;
    frames.ends[i] = ended;
    frames.resumes[i] = performance.now();
    frames.joined[i] = frames.broken ? 0 : 1;
    frames.broken = false;
  } else {
    frames.broken = true;
  }
});

// Added: a menu drew a frame, so the next frame in a level doesn't count the time in menus.
EM_JS(void, Window_wasmMenuFrame, (), {
  if (Module.jkFrames) Module.jkFrames.broken = true;
});

// Added: for the page's slow-frame note and ?bench (index.html): the FPS limit (0 for none),
// then the Display settings that cost the most to draw.
EMSCRIPTEN_KEEPALIVE int Window_WebDisplaySetting(int which)
{
    switch (which)
    {
    case 0: return jkPlayer_fpslimit;
    case 1: return (int)(jkPlayer_ssaaMultiple * 100 + 0.5); // SSAA Multiplier, in hundredths
    case 2: return jkPlayer_enableSSAO;
    case 3: return jkPlayer_enableBloom;
    case 4: return jkPlayer_enableVsync;
    }
    return 0;
}

static int Window_bWasmHadPointerLock = 0;
static int Window_bWasmSentEsc = 0;

// Added: the browser can report one big movement just after it takes the mouse
// lock (the first click back in the game after a menu or another window), as
// far as the cursor's trip to wherever it locks, which turns the player round.
// Mouse movement from the first moments of a lock is left out.
#define WINDOW_WASM_LOCK_SETTLE_MS (100)
static int Window_bWasmMoveLocked = 0;
static Uint64 Window_wasmLockedAtMs = 0;

static int Window_WasmLockJustTaken()
{
    int bLocked = Window_wasmIsPointerLocked();

    if (bLocked && !Window_bWasmMoveLocked)
        Window_wasmLockedAtMs = SDL_GetTicks();
    Window_bWasmMoveLocked = bLocked;
    return bLocked && SDL_GetTicks() - Window_wasmLockedAtMs < WINDOW_WASM_LOCK_SETTLE_MS;
}

// Added: Chromium (Chrome, Edge, Opera, Brave) on Windows and Linux keeps the
// hidden cursor away from the page's edges by moving it back to the middle, unless
// the lock has raw mouse input (index.html asks for it, and Windows has it). That
// move can come through as one jump of about a third of the page against the way
// the mouse is going, which snaps the view round 90 degrees or more (seen in Opera
// on Linux). A hand can't turn that far back between two readings, so a jump of
// a fifth of the page or more against the way the mouse went just before is
// held back. A jump the same way as the mouse is kept, as it can be a real flick.
// Only between readings at most 40 ms apart: when the game runs slower, one reading
// holds a whole frame of movement, and a real quick turn the other way can be
// that big.
// The reading after a held jump settles it. After the cursor's trip back the mouse
// goes on the way it was going, and the jump is left out. A real quick turn the
// other way goes on the way of the jump, and the jump is kept, one reading late.
// It used to be left out straight away, and so were the next readings of a real
// turn for 40 ms: with Windows' pointer acceleration (raw mouse input unticked)
// a quick flick back passes a fifth of the page in one reading, and lost most of
// itself.
EM_JS(int, Window_wasmMouseMayJump, (), {
  var raw = Module.jkRawMouse;
  return raw && raw.warps && !raw.active && document.pointerLockElement === Module.canvas ? 1 : 0;
});

#define WINDOW_WASM_JUMP_FRACTION (0.2)
#define WINDOW_WASM_JUMP_RECENT_MS (40)
#define WINDOW_WASM_JUMP_SETTLE_MS (100)
static int Window_wasmLastXRel = 0;
static int Window_wasmLastYRel = 0;
static Uint64 Window_wasmLastRelMs = 0;
static int Window_wasmHeldXRel = 0;
static int Window_wasmHeldYRel = 0;
static int Window_wasmHeldAxes = 0; // 1: X jumped, 2: Y jumped, 0: nothing held
static Uint64 Window_wasmHeldMs = 0;

static int Window_WasmIsBackJump(int rel, int lastRel, int size)
{
    int limit = (int)(size * WINDOW_WASM_JUMP_FRACTION);

    if (size <= 0 || lastRel <= -limit || lastRel >= limit)
        return 0;
    return (rel >= limit && lastRel < 0) || (rel <= -limit && lastRel > 0);
}

static int Window_WasmGoesOn(int rel, int heldRel)
{
    return (heldRel > 0 && rel > 0) || (heldRel < 0 && rel < 0);
}

// Takes the cursor's jumps out of a reading, and puts back a held one that this
// reading shows was real.
static void Window_WasmFilterMouseJumps(int* pXRel, int* pYRel, Uint64 timestampNs)
{
    Uint64 ms = timestampNs / 1000000;
    int xrel = *pXRel;
    int yrel = *pYRel;
    int axes = 0;

    if (Window_wasmHeldAxes)
    {
        if (ms - Window_wasmHeldMs < WINDOW_WASM_JUMP_SETTLE_MS
            && (!(Window_wasmHeldAxes & 1) || Window_WasmGoesOn(xrel, Window_wasmHeldXRel))
            && (!(Window_wasmHeldAxes & 2) || Window_WasmGoesOn(yrel, Window_wasmHeldYRel)))
        {
            stdPlatform_Printf("Mouse: kept a quick turn of %d,%d\n", Window_wasmHeldXRel, Window_wasmHeldYRel);
            *pXRel += Window_wasmHeldXRel;
            *pYRel += Window_wasmHeldYRel;
            Window_wasmLastXRel = Window_wasmHeldXRel;
            Window_wasmLastYRel = Window_wasmHeldYRel;
            Window_wasmLastRelMs = Window_wasmHeldMs;
        }
        else
        {
            stdPlatform_Printf("Mouse: left out a jump of %d,%d\n", Window_wasmHeldXRel, Window_wasmHeldYRel);
        }
        Window_wasmHeldAxes = 0;
    }

    if (Window_wasmLastRelMs && ms - Window_wasmLastRelMs < WINDOW_WASM_JUMP_RECENT_MS && Window_wasmMouseMayJump())
    {
        axes = (Window_WasmIsBackJump(xrel, Window_wasmLastXRel, Window_screenXSize) ? 1 : 0)
             | (Window_WasmIsBackJump(yrel, Window_wasmLastYRel, Window_screenYSize) ? 2 : 0);
    }
    if (axes)
    {
        Window_wasmHeldXRel = xrel;
        Window_wasmHeldYRel = yrel;
        Window_wasmHeldAxes = axes;
        Window_wasmHeldMs = ms;
        *pXRel -= xrel;
        *pYRel -= yrel;
        return;
    }
    Window_wasmLastXRel = xrel;
    Window_wasmLastYRel = yrel;
    Window_wasmLastRelMs = ms;
}

// Every frame the game wants the mouse locked.
static void Window_WasmKeepPointerLock()
{
    int bLocked = Window_wasmIsPointerLocked();

    if (Window_bWasmHadPointerLock && !bLocked) {
        // The browser let go of the mouse by itself, on Esc or on switching
        // windows. Treat it as the Esc the game never got, so one press opens
        // the menu (or closes chat) as players expect, and the menu frees the
        // mouse instead of the view turning with the cursor.
#ifdef PLATFORM_WSRELAY
        // Added: but not under ?nettest's scripted player, which plays
        // without the mouse: the menu would only hide its match from the
        // test's screenshots.
        if (stdComm_WSRelay_bTestInput)
            stdPlatform_Printf("Browser released the mouse; the scripted player plays on\n");
        else
#endif
        {
            stdPlatform_Printf("Browser released the mouse, sending Esc\n");
            Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
            Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
        }
        Window_bWasmSentEsc = 1;
    }
    else if (bLocked) {
        Window_bWasmSentEsc = 0;
    }
    else if (!Window_bWasmSentEsc) {
        // Not locked yet: back from a menu, or just gone fullscreen
        Window_wasmLockPointerNow();
    }
    if (!bLocked) {
        Window_bWasmMoveLocked = 0; // Added
        Window_wasmLastRelMs = 0; // Added
        Window_wasmHeldAxes = 0; // Added
    }

    Window_bWasmHadPointerLock = bLocked;
}

// Added: Firefox can grant a lock after the game has already let go of the
// mouse (it ignores a release that comes before the lock is granted), and SDL,
// thinking it's free, never lets go again. The cursor stays hidden in the menu.
EM_JS(int, Window_wasmFreeStrayPointerLock, (), {
  if (document.pointerLockElement !== Module.canvas) return 0;
  document.exitPointerLock();
  return 1;
});

// Every frame the game frees the mouse itself.
static void Window_WasmPointerLockReleased()
{
    if (Window_wasmFreeStrayPointerLock())
        stdPlatform_Printf("Mouse: the browser locked it after the game let go, freeing it\n");

    Window_bWasmHadPointerLock = 0;
    Window_bWasmSentEsc = 0;
    Window_bWasmMoveLocked = 0; // Added
    Window_wasmLastRelMs = 0; // Added
    Window_wasmHeldAxes = 0; // Added
}
#endif

// Added: browser wheel amounts to whole notches. Chrome and Edge send 1.0 per
// notch, but Firefox and some Windows scroll settings send less, so an amount
// of a quarter notch or more is at least one notch. Touchpads send many small
// amounts, which add up to a notch per 1.0.
static float Window_wheelRestX = 0.0f;
static float Window_wheelRestY = 0.0f;

static int Window_WheelNotches(float amount, float* pRest)
{
    int notches;

    // A turn the other way starts afresh
    if (amount * *pRest < 0.0f)
        *pRest = 0.0f;

    if (amount >= 0.25f || amount <= -0.25f)
    {
        notches = (int)amount;
        if (!notches)
            notches = amount > 0.0f ? 1 : -1;
        *pRest = 0.0f;
        return notches;
    }

    *pRest += amount;
    notches = (int)*pRest;
    *pRest -= notches;
    return notches;
}

// map raw SDL window coords into the letterboxed 640x480 canvas.
// The same 4:3 pillarbox math the GL present side uses (std3D_DrawMenu)
static void Window_TranslateMouseToCanvas(int rawX, int rawY, int* pOutX, int* pOutY)
{
    if (!jkGame_isDDraw)
    {
        flex_t fX = (flex_t)rawX;
        flex_t fY = (flex_t)rawY;

        // Keep 4:3 aspect
        flex_t menu_x = ((flex_t)Window_screenXSize - ((flex_t)Window_screenYSize * (640.0 / 480.0))) / 2.0;
        flex_t menu_w = ((flex_t)Window_screenYSize * (640.0 / 480.0));

        *pOutX = (int)(((fX - menu_x) / (flex_t)menu_w) * 640.0);
        *pOutY = (int)((fY / (flex_t)Window_screenYSize) * 480.0);
    }
    else
    {
        *pOutX = rawX;
        *pOutY = rawY;
    }

    if (*pOutX < 0)
        *pOutX = 0;
}

void Window_HandleMouseMove(SDL_MouseMotionEvent *event)
{
    int x = (int)event->x;
    int y = (int)event->y;

    Window_lastMouseX = Window_mouseX;
    Window_lastMouseY = Window_mouseY;

    Window_TranslateMouseToCanvas(x, y, &Window_mouseX, &Window_mouseY);

    if (jkQuakeConsole_bOpen) return; // Hijack all input to console

    uint32_t pos = ((Window_mouseX) & 0xFFFF) | (((Window_mouseY) << 16) & 0xFFFF0000);
    
    // event->timestamp is nanoseconds as of SDL3 (was milliseconds); convert to
    // milliseconds to keep Window_lastSampleTime's units (set from SDL_GetTicks()).
    Window_lastSampleMs = (int)(event->timestamp / 1000000) - Window_lastSampleTime;
    //Window_lastSampleTime = event->timestamp;

    // Added: SDL3 gives movement in fractions of a pixel. In the browser it scales
    // the page's movement to the canvas, which comes to a fraction whenever the
    // canvas's size in the page isn't a whole number of pixels (display scaling,
    // browser zoom), and browsers can give fractions themselves. Each movement was
    // cut to whole pixels and the rest lost, so a slow, small movement turned the
    // view less than it should, or not at all. The rest is kept for the next one.
    Window_xRelLeftOver += event->xrel;
    Window_yRelLeftOver += event->yrel;
    int xrel = (int)Window_xRelLeftOver;
    int yrel = (int)Window_yRelLeftOver;
    Window_xRelLeftOver -= xrel;
    Window_yRelLeftOver -= yrel;

#ifdef ARCH_WASM
    if (Window_WasmLockJustTaken()) // Added
        xrel = yrel = 0;
    else
        Window_WasmFilterMouseJumps(&xrel, &yrel, event->timestamp); // Added
#endif
    Window_lastXRel += xrel;
    Window_lastYRel += yrel;

    Window_msg_main_handler(g_hWnd, WM_MOUSEMOVE, 0, pos);
}

int jkCutscene_wasPaused = 0;
int jkGame_wasDDraw = 0;
int Window_bNeedsKeyboardFixed = 0;
void Window_HandleWindowEvent(SDL_Event* event)
{
    // SDL3 flattened window sub-events into top-level SDL_EVENT_WINDOW_* types
    // (no more nested event->window.event); this dispatches on event->type,
    // called for the whole SDL_EVENT_WINDOW_FIRST..LAST range.
    switch (event->type)
    {
        case SDL_EVENT_WINDOW_SHOWN:
#ifdef MACOS
            {
                static int bMacosOnlyOncePerProcessLifetimeTriggerTheStupidDylibLoad = 0;
                if (!bMacosOnlyOncePerProcessLifetimeTriggerTheStupidDylibLoad)
                {
                    CGEventRef ref = CGEventCreateKeyboardEvent(NULL, 0x72 /* help */, 1);
                    CGEventSetFlags( ref, kCGEventFlagMaskNumericPad );
                    CGEventSetFlags( ref, kCGEventFlagMaskSecondaryFn );
                    CGEventPost(kCGHIDEventTap, ref);
                    CFRelease(ref);
                    bMacosOnlyOncePerProcessLifetimeTriggerTheStupidDylibLoad = 1;
                }
            }
#endif
            //printf("Window %d shown", event->window.windowID);
            break;
        case SDL_EVENT_WINDOW_HIDDEN:
            //printf("Window %d hidden", event->window.windowID);
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            //printf("Window %d exposed", event->window.windowID);
            break;
        case SDL_EVENT_WINDOW_MOVED:
            /*printf("Window %d moved to %d,%d",
                    event->window.windowID, event->window.data1,
                    event->window.data2);*/
            Window_xPos = event->window.data1;
            Window_yPos = event->window.data2;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (Window_xSize != event->window.data1 || Window_ySize != event->window.data2)
                Window_resized = 1;

            //Window_xSize = event->window.data1;
            //Window_ySize = event->window.data2;
            SDL_GetWindowSizeInPixels(displayWindow, &Window_xSize, &Window_ySize);
            SDL_GetWindowSize(displayWindow, &Window_screenXSize, &Window_screenYSize);

            if (Window_xSize < 640) Window_xSize = 640;
            if (Window_ySize < 480) Window_ySize = 480;
            //printf("%u %u\n", Window_xSize, Window_ySize);
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            stdPlatform_Printf("Window %d minimized", event->window.windowID);

            // HACK: Cutscene audio gets messed up when multitasking on Android :/
#ifdef TARGET_ANDROID
            stdPlatform_Printf("SDL_EVENT_WINDOW_MINIMIZED");
            jkCutscene_wasPaused = jkCutscene_55AA54 && jkCutscene_isRendering && std3D_IsReady();
            jkGame_wasDDraw = jkGame_isDDraw;
            if (std3D_IsReady() && jkCutscene_isRendering && !jkCutscene_wasPaused) {
                stdPlatform_Printf("Pause cutscene...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
            }
            else if (std3D_IsReady() && jkGame_isDDraw) {
                stdPlatform_Printf("Pause game...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
            }
#endif
            break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
            stdPlatform_Printf("Window %d maximized", event->window.windowID);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            stdPlatform_Printf("Window %d restored", event->window.windowID);

            // HACK: Cutscene audio gets messed up when multitasking on Android :/
#ifdef TARGET_ANDROID
            stdPlatform_Printf("SDL_EVENT_WINDOW_RESTORED");
            if (std3D_IsReady() && jkCutscene_isRendering && !jkCutscene_wasPaused) {
                stdPlatform_Printf("Play cutscene...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
            }
            else if (std3D_IsReady() && !jkGame_isDDraw && jkGame_wasDDraw) {
                stdPlatform_Printf("Resume game...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
            }
#endif
            break;
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            stdPlatform_Printf("Mouse entered window %d\n", event->window.windowID);
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            stdPlatform_Printf("Mouse left window %d\n", event->window.windowID);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            stdPlatform_Printf("Window %d gained keyboard focus\n", event->window.windowID);
            Window_bNeedsKeyboardFixed = 0;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            stdPlatform_Printf("Window %d lost keyboard focus\n", event->window.windowID);
            if (stdControl_IsSystemKeyboardShowing() && Window_bNeedsKeyboardFixed) {
                stdPlatform_Printf("Fixing keyboard...\n");

                SDL_Window* gimmeKeyboard = SDL_CreateWindow("Gimme Keyboard", 20, 20, SDL_WINDOW_KEYBOARD_GRABBED | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
                SDL_RaiseWindow(gimmeKeyboard);
                SDL_DestroyWindow(gimmeKeyboard);
                SDL_RaiseWindow(displayWindow);

                SDL_MinimizeWindow(displayWindow);
                SDL_RestoreWindow(displayWindow);
                SDL_RaiseWindow(displayWindow);
            }
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            //printf("Window %d closed", event->window.windowID);
            break;
        case SDL_EVENT_WINDOW_HIT_TEST:
            //printf("Window %d has a special hit test", event->window.windowID);
            break;
    }
}

#if defined(WIN64_MINGW) || defined(_WIN32)
CHAR my_getch() {
    DWORD mode, cc;
    DWORD num;
    INPUT_RECORD irInBuf[1];
    HANDLE h = GetStdHandle( STD_INPUT_HANDLE );

    if (h == NULL) {
        return 0; // console not found
    }

    GetConsoleMode( h, &mode );
    SetConsoleMode( h, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) );
    TCHAR c = 0;
    GetNumberOfConsoleInputEvents(h, &num);
    if (num)
    {
        if (!ReadConsoleInput(
            h,      // input buffer handle 
            irInBuf,     // buffer to read into 
            1,         // size of read buffer 
            &num))
        {

        }
        else
        {
            if (irInBuf[0].EventType == KEY_EVENT && irInBuf[0].Event.KeyEvent.bKeyDown) {
                c = irInBuf[0].Event.KeyEvent.uChar.AsciiChar;
            }
        }
    }
    SetConsoleMode( h, mode );
    return c;
}

int my_kbhit() {
    DWORD num;
    DWORD mode, cc;
    HANDLE h = GetStdHandle( STD_INPUT_HANDLE );
    if (h == NULL) {
        return 0; // console not found
    }

    GetConsoleMode( h, &mode );
    SetConsoleMode( h, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) );
    
    GetNumberOfConsoleInputEvents(h, &num);
    SetConsoleMode( h, mode );

    return num;
}
#else
int my_kbhit() {
    static const int STDIN = 0;
    static int initialized = 0;

    if (! initialized) {
        // Use termios to turn off line buffering
        struct termios term;
        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        term.c_lflag &= ~ECHO;
        tcsetattr(STDIN, TCSANOW, &term);
        setbuf(stdin, NULL);
        initialized = 1;
    }

    int bytesWaiting;
    ioctl(STDIN, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}
#endif

static char Window_headlessBuffer[256];

void Window_UpdateHeadless()
{
    char buffer[32];
    size_t bytes_read = 0;

    if (my_kbhit() > 0) {
#if defined(WIN64_MINGW) || (_WIN32)
        buffer[0] = my_getch();
        buffer[1] = 0;
        bytes_read = 1;
#else
        int fd = STDIN_FILENO;
        bytes_read = read(fd, buffer, sizeof(buffer)-1);
        buffer[bytes_read] = 0;
#endif

        for (int i = 0; i < bytes_read; i++)
        {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                printf("\r> %s\n", Window_headlessBuffer);
                sithConsole_ExeCommand(Window_headlessBuffer);
                memset(Window_headlessBuffer, 0, sizeof(Window_headlessBuffer));
                continue;
            }
            else if (buffer[i] == 0x7F && strlen(Window_headlessBuffer)) {
                Window_headlessBuffer[strlen(Window_headlessBuffer)-1] = 0;
                printf("\r> %s ", Window_headlessBuffer);
                continue;
            }
            else if (buffer[i] < ' ' || buffer[i] > '~')
            {
                continue;
            }

            char tmp[2] = {buffer[i], 0};
            strncat(Window_headlessBuffer, tmp, 255);
        }
    }
    
    printf("\r> %s", Window_headlessBuffer);
    //printf("> %x %x %s\n", buffer[0], my_kbhit(), Window_headlessBuffer);
    fflush(stdout);

    if (Window_resized)
    {
        jkMain_FixRes();
        if (!jkGui_SetModeMenu(0))
        {
            stdDisplay_SetMode(0, 0, 0);
            //jkMain_FixRes();
        }

        jkGui_SetModeGame();

        // -droidworks keeps its own fixed 640x480 canvas, re-push the palette + dirty the whole screen
        if (Main_bDroidWorks)
        {
            dwMain_NotifyWindowResized();
            Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
        }

        Window_resized = 0;
    }

    int sampleTime_roundtrip = SDL_GetTicks() - Window_lastSampleTime;
    //printf("%u\n", sampleTime_roundtrip);
    Window_lastSampleTime = SDL_GetTicks();

    static int sampleTime_delay = 0;
    int menu_framelimit_amt_ms = 6;

    if (!jkGame_isDDraw)
    {

        if (!jkGuiBuildMulti_bRendering) {
            std3D_StartScene();
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            std3D_EndScene();
            //SDL_GL_SwapWindow(displayWindow);
        }
        else {
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            //SDL_GL_SwapWindow(displayWindow);
            //menu_framelimit_amt_ms = 64;
        }
    }
    else
    {
        // Save mouse position for menu
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }
    }

    // Keep entire loop at 6ms (150FPS)
    if (sampleTime_roundtrip < menu_framelimit_amt_ms) {
        sampleTime_delay++;
    }
    else {
        sampleTime_delay--;
    }
    if (sampleTime_delay <= 0) {
        sampleTime_delay = 1;
    }
    if (sampleTime_delay >= menu_framelimit_amt_ms) {
        sampleTime_delay = menu_framelimit_amt_ms;
    }
    SDL_Delay(sampleTime_delay);

    last_jkGame_isDDraw = jkGame_isDDraw;
    last_jkQuakeConsole_bOpen = jkQuakeConsole_bOpen;
}

// reentrancy guard for Window_SdlUpdate
static int Window_bInSdlUpdate = 0;

void Window_SdlUpdate()
{
    if (Main_bHeadless)
    {
        Window_UpdateHeadless();
        return;
    }

    // reentrancy guard. This function both drains the SDL event queue AND presents
    // (std3D_DrawMenu + SDL_GL_SwapWindow) at its tail. DroidWorks' WM_MOUSEMOVE handler calls
    // dwCursor_Redraw() -> stdDisplay_DDrawGdiSurfaceFlip() -> Window_SdlUpdate(), so a motion
    // event dispatched from this function's drain re-enters it recursively
    if (Window_bInSdlUpdate)
        return;
    Window_bInSdlUpdate = 1;

    uint16_t left, right;
    uint32_t pos, msgl, msgr;
    int hasLeft, hasRight;
    SDL_Event event;
    SDL_MouseButtonEvent* mevent;

    // HACK: Escape key for controllers
    extern int stdControl_bControllerEscapeKey;
    extern int stdControl_bControllerEscapeKey_last;

    while (SDL_PollEvent(&event))
    {
        int bIsOdin = 0;
        int bIsGamepad = 0;

        // Added: route touches to the on-screen pad first; it returns 1 when it
        // has claimed the finger.
        if (jkTouchControls_HandleSdlEvent(&event)) {
            continue;
        }

        // Added: any real hardware input retires the on-screen pad. Mouse
        // events synthesized from touches carry SDL_TOUCH_MOUSEID and must not
        // count, or the pad would hide itself the instant it was used.
        switch (event.type)
        {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                jkTouchControls_NotifyPhysicalInput();
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.which != SDL_TOUCH_MOUSEID) {
                    jkTouchControls_NotifyPhysicalInput();
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.which != SDL_TOUCH_MOUSEID) {
                    jkTouchControls_NotifyPhysicalInput();
                }
                break;
            default:
                break;
        }

        if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || event.type == SDL_EVENT_JOYSTICK_BUTTON_UP) {
            const char* name = SDL_GetJoystickNameForID(event.jbutton.which);
            bIsOdin = name && strcmp(name, "Odin Controller") == 0;
            bIsGamepad = SDL_IsGamepad(event.jbutton.which);
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            bIsGamepad = 1;
        }

        switch (event.type)
        {
            case SDL_EVENT_JOYSTICK_ADDED: {
                stdPlatform_Printf("[ev] JOYSTICK_ADDED %d\n", (int)event.jdevice.which); // Added: field-debug
                stdControl_bReadJoysticks = 1;
                stdControl_InitSdlJoysticks();
                break;
            }
            case SDL_EVENT_JOYSTICK_REMOVED: {
                stdPlatform_Printf("[ev] JOYSTICK_REMOVED %d\n", (int)event.jdevice.which); // Added: field-debug
                stdControl_InitSdlJoysticks();
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
                stdPlatform_Printf("[ev] GAMEPAD_ADDED %d\n", (int)event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                stdPlatform_Printf("[ev] GAMEPAD_REMOVED %d\n", (int)event.gdevice.which);
                stdControl_InitSdlJoysticks();
                break;
            case SDL_EVENT_GAMEPAD_REMAPPED:
                stdPlatform_Printf("[ev] GAMEPAD_REMAPPED %d\n", (int)event.gdevice.which);
                break;

            case SDL_EVENT_TEXT_INPUT:
                for (int i = 0; i < _strlen(event.text.text); i++)
                {
                    Window_msg_main_handler(g_hWnd, WM_CHAR, event.text.text[i], 0);
                }
                break;
            case SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST:
                Window_HandleWindowEvent(&event);
                break;
            case SDL_EVENT_KEY_DOWN:
                //stdPlatform_Printf("scancode %d\n", event.key.scancode);
                //handleKey(&event.key.keysym, WM_KEYDOWN, 0x1);
                if (event.key.key == SDLK_ESCAPE)
                {
#ifdef ARCH_WASM
                    // Added: holding Esc is how players leave fullscreen on the
                    // web (index.html), so a held Esc mustn't keep opening and
                    // closing the menu.
                    if (!event.key.repeat)
#endif
                    {
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, event.key.repeat & 0xFFFF);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, event.key.repeat & 0xFFFF);
                    }
                }
                else if (event.key.key == SDLK_PAGEUP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_PRIOR, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_PAGEDOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_NEXT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_LEFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_LEFT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_RIGHT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_RIGHT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_UP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_UP, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_DOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_DOWN, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_BACKSPACE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_BACK, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_BACK, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_DELETE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_DELETE, event.key.repeat & 0xFFFF);
                    //Window_msg_main_handler(g_hWnd, WM_CHAR, VK_DELETE, 0);
                }
                else if (event.key.key == SDLK_INSERT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_INSERT, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_INSERT, 0);
                }
                else if (event.key.key == SDLK_RETURN)
                {
                    // HACK apparently Windows buffers these events in some way, but to replicate the behavior in jkGUI we just spam KEYFIRST
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_RETURN, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_RETURN, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_LSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_LSHIFT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_RSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_RSHIFT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_TAB)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_TAB, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_TAB, event.key.repeat & 0xFFFF);
                }
                else if (event.key.key == SDLK_END)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_END, event.key.repeat & 0xFFFF);
                    //Window_msg_main_handler(g_hWnd, WM_CHAR, 0x23, 0);
                }
                else if (event.key.key == SDLK_HOME)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_HOME, event.key.repeat & 0xFFFF);
                    //Window_msg_main_handler(g_hWnd, WM_CHAR, 0x24, 0);
                }
                // Added: the scancode too, so the console key is the one under
                // Esc on every keyboard layout (² on French ones, § on Swedish
                // ones), the same key Scores is bound to (DIK_GRAVE).
                else if (event.key.key == SDLK_GRAVE || event.key.scancode == SDL_SCANCODE_GRAVE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_OEM_3, event.key.repeat & 0xFFFF);
                }
                else if (event.key.scancode == SDL_SCANCODE_AC_BACK) {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, event.key.repeat & 0xFFFF);
                }

                //if (!event.key.repeat)
                //    stdControl_SetSDLKeydown(event.key.scancode, 1, event.key.timestamp);
                break;
            case SDL_EVENT_KEY_UP:
                if (event.key.key == SDLK_ESCAPE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_ESCAPE, 0);
                }
                else if (event.key.key == SDLK_PAGEUP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_PRIOR, 0);
                }
                else if (event.key.key == SDLK_PAGEDOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_NEXT, 0);
                }
                else if (event.key.key == SDLK_LEFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_LEFT, 0);
                }
                else if (event.key.key == SDLK_RIGHT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_RIGHT, 0);
                }
                else if (event.key.key == SDLK_UP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_UP, 0);
                }
                else if (event.key.key == SDLK_DOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_DOWN, 0);
                }
                else if (event.key.key == SDLK_BACKSPACE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_BACK, 0);
                }
                else if (event.key.key == SDLK_DELETE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_DELETE, 0);
                }
                else if (event.key.key == SDLK_INSERT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_INSERT, 0);
                }
                else if (event.key.key == SDLK_RETURN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_RETURN, 0); // 0xB?
                }
                else if (event.key.key == SDLK_LSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_LSHIFT, 0);
                }
                else if (event.key.key == SDLK_RSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_RSHIFT, 0);
                }
                else if (event.key.key == SDLK_TAB)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_TAB, 0);
                }
                else if (event.key.key == SDLK_END)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_END, 0);
                }
                else if (event.key.key == SDLK_HOME)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_HOME, 0);
                }
                else if (event.key.key == SDLK_GRAVE || event.key.scancode == SDL_SCANCODE_GRAVE) // Added: the scancode too, as above
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_OEM_3, 0);
                }
                else if (event.key.scancode == SDL_SCANCODE_AC_BACK) {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_ESCAPE, 0);
                }
                //handleKey(&event.key.keysym, WM_KEYUP, 0xc0000001);

                if (jkQuakeConsole_bOpen) break; // Hijack all input to console

                stdControl_SetSDLKeydown(event.key.scancode, 0, (uint32_t)(event.key.timestamp / 1000000));
                break;
            case SDL_EVENT_MOUSE_MOTION:
                // Added: while the on-screen pad owns the screen, drop the mouse
                // events SDL synthesizes from touches -- otherwise a thumbstick
                // drag also warps the cursor and holds fire (KEY_MOUSE_B1 is
                // bound to INPUT_FUNC_FIRE1 by default).
                //
                // This deliberately filters the events rather than switching
                // SDL_HINT_TOUCH_MOUSE_EVENTS off: SDL tracks finger_touching /
                // track_fingerid *inside* the same `if (touch_mouse_events)`
                // block that emits the events, so flipping the hint mid-gesture
                // strands that state -- the synthetic button-up never fires
                // (stuck firing) and finger_touching stays true forever, after
                // which no tap ever produces a click again (dead menus, and
                // cutscenes that cannot be skipped).
                if (jkTouchControls_IsShown() && event.motion.which == SDL_TOUCH_MOUSEID) {
                    break;
                }
                Window_HandleMouseMove(&event.motion);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                // Added: as above -- but a button-UP is always let through, so
                // that a press which began before the pad came up can never stay
                // latched down.
                if (jkTouchControls_IsShown() && event.button.which == SDL_TOUCH_MOUSEID
                    && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    break;
                }

                mevent = (SDL_MouseButtonEvent*)&event;
                left = 0;
                right = 0;
                hasLeft = 0;
                hasRight = 0;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                {
                    left = (mevent->button == SDL_BUTTON_LEFT ? 1 : 0);
                    right = (mevent->button == SDL_BUTTON_RIGHT ? 2 : 0);
                    
                    if (left)
                        hasLeft = 1;
                    if (right)
                        hasRight = 1;
                }
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
                {
                    left = (mevent->button == SDL_BUTTON_LEFT ? 0 : 1);
                    right = (mevent->button == SDL_BUTTON_RIGHT ? 0 : 2);
                    
                    if (!left)
                        hasLeft = 1;
                    if (!right)
                        hasRight = 1;
                }
                
                if (hasLeft)
                    Window_bMouseLeft = left;
                if (hasRight)
                    Window_bMouseRight = right;

                // route button coords through the same letterbox transform as motion
                Window_TranslateMouseToCanvas((int)mevent->x, (int)mevent->y, &Window_mouseX, &Window_mouseY);

                pos = ((Window_mouseX) & 0xFFFF) | (((Window_mouseY) << 16) & 0xFFFF0000);
                msgl = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? WM_LBUTTONDOWN : WM_LBUTTONUP);
                msgr = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? WM_RBUTTONDOWN : WM_RBUTTONUP);

                // Middle mouse button activates the highlighted menu item.
                if (mevent->button == SDL_BUTTON_MIDDLE &&
                    jkGuiRend_IsMenuActive())
                {
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                        !jkQuakeConsole_bOpen)
                    {
                        Window_msg_main_handler(g_hWnd, WM_MBUTTONDOWN, 0, 0);
                    }

                    break;
                }

                if (jkQuakeConsole_bOpen) break; // Hijack all input to console
                
                if (hasLeft)
                    Window_msg_main_handler(g_hWnd, msgl, left | right, pos);
                if (hasRight)
                    Window_msg_main_handler(g_hWnd, msgr, left | right, pos);

                //stdControl_UpdateKeyState(KEY_MOUSE_B1, Window_bMouseLeft, mevent->timestamp);
                //stdControl_UpdateKeyState(KEY_MOUSE_B2, Window_bMouseRight, mevent->timestamp);

                break;
            case SDL_EVENT_MOUSE_WHEEL:
            {
                // Added: whole notches (Window_WheelNotches), added up until
                // the game reads them rather than only the last amount kept
                int wheelY = Window_WheelNotches(event.wheel.y, &Window_wheelRestY);
                int wheelX = Window_WheelNotches(event.wheel.x, &Window_wheelRestX);

                // Preserve console scrolling.
                if (jkQuakeConsole_bOpen)
                {
                    Window_mouseWheelY += wheelY;
                    Window_mouseWheelX += wheelX;
                    break;
                }

                // Send wheel movement to the active game menu.
                if (jkGuiRend_IsMenuActive())
                {
                    int steps = wheelY > 0 ? wheelY : -wheelY;
                    int key = wheelY > 0 ? VK_UP : VK_DOWN;

                    for (int i = 0; i < steps; i++)
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, key, 0);

                    // Prevent menu scrolling from reaching gameplay.
                    Window_mouseWheelY = 0;
                    Window_mouseWheelX = 0;
                    break;
                }

                // Added: nothing reads the wheel while the game isn't taking
                // mouse input (loading, cutscenes, mouse turned off), so notches
                // mustn't pile up and fire all at once later.
                if (!stdControl_bControlsActive || !stdControl_bReadMouse)
                    break;

                Window_mouseWheelY += wheelY;
                Window_mouseWheelX += wheelX;
                break;
            }

            // HACK: Escape key for controllers
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                if (!bIsGamepad) {
                    //stdPlatform_Printf("button %d, %d\n", event.jbutton.button, event.jbutton.state);
                }
                if (bIsOdin && !bIsGamepad && (event.jbutton.button == 6 || event.jbutton.button == 4)) {
                    stdControl_bControllerEscapeKey = event.jbutton.down;
                }
                else if (!bIsGamepad && jkCutscene_isRendering && event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && event.jbutton.button == 3) { // y
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                }
                else if (!bIsGamepad && jkCutscene_isRendering  && event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN&& event.jbutton.button == 2) { // x
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                }
                else if (!bIsGamepad && jkCutscene_isRendering && event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && event.jbutton.button == 1) { // b
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                }
                else if (!bIsGamepad && jkCutscene_isRendering && event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && event.jbutton.button == 0) { // a
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                }
                break;

            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                if (event.jaxis.which == 0) {
                    //stdPlatform_Printf("axis %d, %d\n", event.jaxis.axis, event.jaxis.value);
                }
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (bIsGamepad) {
                    //stdPlatform_Printf("gpad button %d, %d\n", event.gbutton.button, event.gbutton.state);
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START || event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                        stdControl_bControllerEscapeKey = event.gbutton.down;
                    }
                    else if (jkCutscene_isRendering && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) { // y
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                    }
                    else if (jkCutscene_isRendering  && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) { // x
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                    }
                    else if (jkCutscene_isRendering && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) { // b
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                    }
                    else if (jkCutscene_isRendering && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) { // a
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                //stdPlatform_Printf("Controller %d Axis %d moved to %d\n",
                //       event.caxis.which, event.caxis.axis, event.caxis.value);
                break;

            case SDL_EVENT_QUIT:
                stdPlatform_Printf("Quit!\n");

                // Added
                if (jkPlayer_bHasLoadedSettingsOnce) {
                    jkPlayer_WriteConf(jkPlayer_playerShortName);
                }
                
                exit(-1);
                break;
            default:
                break;
        }
    }

    // HACK: Escape key for controllers
    if (stdControl_bControllerEscapeKey && !stdControl_bControllerEscapeKey_last) {
        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
    }
    stdControl_bControllerEscapeKey_last = stdControl_bControllerEscapeKey;
    
    if (Window_resized)
    {
        jkMain_FixRes();
        if (!jkGui_SetModeMenu(0))
        {
            stdDisplay_SetMode(0, 0, 0);
            //jkMain_FixRes();
        }

        // -droidworks keeps its own fixed 640x480 canvas, re-push the palette + dirty the whole screen
        if (Main_bDroidWorks)
        {
            dwMain_NotifyWindowResized();
            Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
        }

        Window_resized = 0;
    }
    
    static int sampleTime_delay = 0;
    int sampleTime_roundtrip = SDL_GetTicks() - Window_lastSampleTime;
    //printf("%u\n", sampleTime_roundtrip);
    Window_lastSampleTime = SDL_GetTicks();

    static int jkPlayer_enableVsync_last = 0;
    int menu_framelimit_amt_ms = 16;

    if (jkPlayer_enableVsync_last != jkPlayer_enableVsync)
    {
        SDL_GL_SetSwapInterval(jkPlayer_enableVsync);
    }

    if (!jkGame_isDDraw)
    {
        // Restore menu mouse position
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            SDL_WarpMouseInWindow(displayWindow, Window_menu_mouseX, Window_menu_mouseY);
        }

        SDL_SetWindowRelativeMouseMode(displayWindow, false);
#ifdef ARCH_WASM
        Window_WasmPointerLockReleased();
        Window_wasmMenuFrame();
#endif

        if (!jkGuiBuildMulti_bRendering) {
            std3D_StartScene();
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            std3D_EndScene();
            SDL_GL_SwapWindow(displayWindow);
        }
        else {
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            SDL_GL_SwapWindow(displayWindow);
            //menu_framelimit_amt_ms = 64;
        }

        if (Window_needsRecreate) {
            std3D_PurgeEntireTextureCache();
            Window_RecreateSDL2Window();
        }
        
        // Keep menu FPS at 60FPS, to avoid cranking the GPU unnecessarily.
        if (sampleTime_roundtrip < menu_framelimit_amt_ms) {
            sampleTime_delay++;
        }
        else {
            sampleTime_delay--;
        }
        if (sampleTime_delay <= 0) {
            sampleTime_delay = 1;
        }
        if (sampleTime_delay >= menu_framelimit_amt_ms) {
            sampleTime_delay = menu_framelimit_amt_ms;
        }
        SDL_Delay(sampleTime_delay);
    }
    else
    {
        // Save mouse position for menu
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }

#ifdef QUAKE_CONSOLE

        if (jkQuakeConsole_bOpen && jkQuakeConsole_bOpen != last_jkQuakeConsole_bOpen) {
            SDL_WarpMouseInWindow(displayWindow, Window_menu_mouseX, Window_menu_mouseY);
        }
        else if (!jkQuakeConsole_bOpen && jkQuakeConsole_bOpen != last_jkQuakeConsole_bOpen) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }

        if (jkQuakeConsole_bOpen)
        {
            SDL_SetWindowRelativeMouseMode(displayWindow, false);
        }

#ifdef ARCH_WASM
        // Added: on its way to a menu the game lets go of the mouse itself
        // (stdControl_SetActivation), and this loop runs a few more frames
        // before the menu takes over. Asking for the mouse in those frames
        // isn't harmless in a browser: within a few seconds of a click it
        // gets the lock, after the game's own release, and the menu opens
        // with the mouse still locked.
        if (!jkQuakeConsole_bOpen && SDL_GetWindowFlags(displayWindow) & SDL_WINDOW_MOUSE_FOCUS && stdControl_bControlsActive) {
#else
        if (!jkQuakeConsole_bOpen && SDL_GetWindowFlags(displayWindow) & SDL_WINDOW_MOUSE_FOCUS) {
#endif
            SDL_SetWindowRelativeMouseMode(displayWindow, true);
            //SDL_WarpMouseInWindow(displayWindow, 100, 100);
#ifdef ARCH_WASM
            Window_WasmKeepPointerLock();
#endif
        }
        else
        {
            SDL_SetWindowRelativeMouseMode(displayWindow, false);
#ifdef ARCH_WASM
            Window_WasmPointerLockReleased();
#endif
        }
#endif
    }

    jkPlayer_enableVsync_last = jkPlayer_enableVsync;

    last_jkGame_isDDraw = jkGame_isDDraw;
#ifdef QUAKE_CONSOLE
    last_jkQuakeConsole_bOpen = jkQuakeConsole_bOpen;
#endif

    Window_bInSdlUpdate = 0; // release the reentrancy guard
}

void Window_SdlUpdateModal()
{
    int saved = Window_bInSdlUpdate;
    Window_bInSdlUpdate = 0;
    Window_SdlUpdate();
    Window_bInSdlUpdate = saved;
}

void Window_SdlVblank()
{
    if (Main_bHeadless) return;

    //static uint32_t roundtrip = 0;
    //uint32_t before = stdPlatform_GetTimeMsec();
#ifdef ARCH_WASM
    // The browser shows what was drawn once the page gives the thread back, so there's
    // nothing to swap; SDL's swap only yielded, with a timer (see Window_wasmEndFrame).
    if (!jkGuiBuildMulti_bRendering)
        Window_wasmEndFrame(jkPlayer_enableVsync, jkGame_isDDraw);
#else
    SDL_GL_SwapWindow(displayWindow);
#endif
    //uint32_t after = stdPlatform_GetTimeMsec();
    //printf("%u %u\n", after-before, before-roundtrip);

    //roundtrip = before;

    if (Window_needsRecreate)
        Window_RecreateSDL2Window();

#ifdef ARCH_WASM
    //emscripten_sleep(1);
#endif
}

#ifdef ARCH_WASM
// Fixed: this is embedded JavaScript, not C -- `canvas` refers to the
// <canvas id="canvas"> element the browser auto-exposes as a global (per the
// HTML "named access on the Window object" spec), not the C rdCanvas struct.
// An automated rd*-struct-member rename pass (d84fc7328) mistakenly renamed
// it to `pCanvas` along with the real C-side renames, breaking WASM at
// runtime with "pCanvas is not defined" (SDL_CreateWindow's size args).
EM_JS(int, canvas_get_width, (), {
  return canvas.width;
});

EM_JS(int, canvas_get_height, (), {
  return canvas.height;
});
#endif

void Window_RecreateSDL2Window()
{
#ifdef ARCH_WASM
    static int onlyOnce = 0;
    if (onlyOnce) {
        return;
    }
    onlyOnce = 1;
#endif

    if (Main_bHeadless) return;

    stdPlatform_Printf("Recreating SDL2 Window!\n");
    Window_needsRecreate = 0;

    if (displayWindow) {
        std3D_FreeResources();
        SDL_GL_DestroyContext(glWindowContext);
        SDL_DestroyWindow(displayWindow);
    }

    // HACK: side-step the json stuff
    if (Window_bShouldPopSteamKeyboard) {
        Window_isFullscreen = 1;
        Window_isHiDpi = 1;
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

    if (displayWindow) {
        flags = SDL_GetWindowFlags(displayWindow);
        //std3D_FreeResources();
        //SDL_GL_DestroyContext(glWindowContext);
        //SDL_DestroyWindow(displayWindow);

        flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    }

#ifdef WIN64_STANDALONE
    // SDL_HINT_WINDOWS_DPI_AWARENESS has no SDL3 equivalent (removed) -- SDL3
    // windows are DPI-aware by default.
#endif

    if (Window_isHiDpi)
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    else
        flags &= ~SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (Window_isFullscreen) {
        //flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    else {
        //flags &= ~SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

#if defined(ARCH_WASM)
    //flags &= ~SDL_WINDOW_RESIZABLE;
#endif

#ifdef TARGET_ANDROID
    flags = SDL_WINDOW_OPENGL;
#endif

#ifdef TARGET_IOS
    flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif

    // SDL3 SDL_CreateWindow() dropped the x/y position params; position is set
    // separately below via SDL_SetWindowPosition() on desktop.
#ifdef ARCH_WASM
    displayWindow = SDL_CreateWindow(Window_isHiDpi ? "OpenJKDF2 HiDPI" : "OpenJKDF2", canvas_get_width(), canvas_get_height(), flags);
#elif defined(TARGET_ANDROID)
    displayWindow = SDL_CreateWindow(Window_isHiDpi ? "OpenJKDF2 HiDPI" : "OpenJKDF2", Window_screenXSize, Window_screenYSize, flags);
#else
    displayWindow = SDL_CreateWindow(Window_isHiDpi ? "OpenJKDF2 HiDPI" : "OpenJKDF2", Window_screenXSize, Window_screenYSize, flags);
#endif
    if (!displayWindow) {
        char errtmp[256];
        snprintf(errtmp, 256, "!! Failed to create SDL2 window !!\n%s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", errtmp, NULL);
        exit (-1);
    }
    //SDL_SetRenderDrawBlendMode(displayRenderer, SDL_BLENDMODE_BLEND);

#if !defined(ARCH_WASM) && !defined(TARGET_ANDROID) && !defined(TARGET_IOS)
    SDL_SetWindowPosition(displayWindow, Window_xPos, Window_yPos);
#endif

    SDL_SetWindowFullscreen(displayWindow, Window_isFullscreen ? true : false);
    SDL_RaiseWindow(displayWindow);

#ifdef TARGET_IOS
    if (!iosAngle_CreateContext(displayWindow))
    {
        char errtmp[320];
        snprintf(errtmp, sizeof(errtmp), "!! Failed to initialize ANGLE !!\n%s", iosAngle_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", errtmp, NULL);
        exit(-1);
    }
#else
    glWindowContext = SDL_GL_CreateContext(displayWindow);
    
    // Retry with 3.30 instead
    if (glWindowContext == NULL)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        glWindowContext = SDL_GL_CreateContext(displayWindow);
    }

    // Retry with 3.20 and this thing instead
    if (glWindowContext == NULL)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        glWindowContext = SDL_GL_CreateContext(displayWindow);
    }
    
    if (glWindowContext == NULL)
    {
        char errtmp[256];
        snprintf(errtmp, 256, "!! Failed to initialize SDL OpenGL context !!\n%s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", errtmp, NULL);
        exit(-1);
    }
#endif // TARGET_IOS

    SDL_GL_MakeCurrent(displayWindow, glWindowContext);
    SDL_GL_SetSwapInterval(jkPlayer_enableVsync); // Disable vsync
#if !defined(TARGET_ANDROID) && !defined(TARGET_IOS)
#ifdef ARCH_WASM
    // Added: while SDL's text input is on, the browser still acts on most
    // keys (Alt+D, Firefox's Quick Find on / and ', ...) and can take the
    // keyboard from the game. Chat, the console and menu text boxes turn it on
    // while they're open (stdControl_ShowSystemKeyboard); DroidWorks' text
    // boxes need it on from the start.
    if (Main_bDroidWorks)
#endif
    SDL_StartTextInput(displayWindow);
#endif

    SDL_GetWindowSizeInPixels(displayWindow, &Window_xSize, &Window_ySize);
    SDL_GetWindowSize(displayWindow, &Window_screenXSize, &Window_screenYSize);

    Window_resized = 1;
}

void Window_Main_Loop()
{
    jkMain_GuiAdvance(); // TODO needed?
#ifdef TARGET_DREAMCAST
    // Loop-phase tracer (see std3D border tracer). BLUE (set in std3D_EndScene) still
    // showing here => hang inside the tick after the render; CYAN => hang in the
    // present; WHITE => hang at the top of the next tick's sim (before its render).
    std3D_BorderTrace(0, 255, 255);   // CYAN: tick returned, about to present
#endif
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
#ifdef TARGET_DREAMCAST
    std3D_BorderTrace(255, 255, 255); // WHITE: presented, looping to next tick
#endif

    //Window_SdlUpdate();
}

int Window_Main_Linux(int argc, char** argv)
{
    char cmdLine[1024];
    int result;

    // Init SDL
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_APP_NAME, "OpenJKDF2");

#ifdef ARCH_WASM
    // Added: while a mouse button is down SDL counts the mouse as captured and
    // stops keeping its pointer inside the window. With the mouse locked, that
    // pointer then wanders off with every turn made while firing, and the next
    // click finds it outside: SDL says the mouse left the window, and the game
    // lets go of the mouse lock and takes it again, a moment without mouse look.
    // A page can't capture the mouse outside its window anyway.
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
#endif

#if defined(TARGET_ANDROID)
    //SDL_SetHint(SDL_HINT_JOYSTICK_DEBUG, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    //SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1");
    // SDL_HINT_ACCELEROMETER_AS_JOYSTICK has no SDL3 equivalent (removed).
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint("SDL_MIXER_DEBUG_MUSIC_INTERFACES", "1");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "aaudio"); // This is fine for music tbh
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif

#if defined(TARGET_IOS)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    // Added: deliberately do NOT set SDL_HINT_AUDIO_CATEGORY here. Its "playback"
    // value makes SDL want Playback + DuckOthers alone (0x2), but iOS implicitly
    // adds MixWithOthers whenever DuckOthers is set, so a live session ALWAYS
    // reports 0x3 -- a value that hint can never match. SDL's UpdateAudioSession
    // would then take its mismatch branch every time and call [session
    // setActive:NO], permanently stopping OpenAL's RemoteIO unit (music, which is
    // SDL's own device, keeps playing while every sound effect goes silent).
    // Left unset, SDL derives MixWithOthers|DuckOthers == 0x3, which is exactly
    // what iosAudioSession_Initialize() installs, so SDL leaves the session alone.
#endif

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);

    
    if ((SDL_GetHintBoolean("SteamClientLaunch", 0) || SDL_GetHintBoolean("SteamOS", 0) || SDL_GetHintBoolean("SteamDeck", 0)) && SDL_GetHintBoolean("SteamGamepadUI", 0)) {
        Window_bShouldPopSteamKeyboard = 1;
        Window_isFullscreen = 1;
        Window_isHiDpi = 1;
    }

#if defined(RENDER_GL11)
    // Legacy fixed-function GL 1.1 backend: request a compatibility context.
    // Requesting 1.1 makes desktop drivers return their highest legacy
    // (compatibility) context, which is what old XP-era hardware exposes.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#elif defined(MACOS)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#else

#if defined(WIN64_STANDALONE)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);

    // apitrace
#if 0
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#endif
#elif defined(TARGET_ANDROID)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#elif defined(ARCH_WASM)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#endif

#endif

    Window_RecreateSDL2Window();
#if !defined(TARGET_ANDROID) && !defined(TARGET_IOS) && !defined(ARCH_WASM) && !defined(RENDER_GL11)
    glewInit();
#endif
    
    //SDL_RenderClear(displayRenderer);
    //SDL_RenderPresent(displayRenderer);
    
    
    strcpy(cmdLine, "");
    
    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = 0;//hInstance;
    g_nShowCmd = 0;//nShowCmd;
    
    for (int i = 1; i < argc; i++)
    {
        strcat(cmdLine, argv[i]);
        strcat(cmdLine, " ");
    }
    
    result = Main_Startup(cmdLine);

    int fullscreen = wuRegistry_GetBool("Window_isFullscreen", 0);
    int hidpi = wuRegistry_GetBool("Window_isHiDpi", 0);
    Window_SetFullscreen(fullscreen);
    Window_SetHiDpi(hidpi);
    Window_RecreateSDL2Window();

    if (!result) return result;

    if (Main_bHeadless)
    {
        if (displayWindow) {
            std3D_FreeResources();
            SDL_GL_DestroyContext(glWindowContext);
            SDL_DestroyWindow(displayWindow);
        }
    }

    g_window_not_destroyed = 1;
    
    Window_msg_main_handler(g_hWnd, 0x1, 0, 0); // WM_CREATE
    Window_msg_main_handler(g_hWnd, 0x6, 2, 0); // WM_ACTIVATE
    Window_msg_main_handler(g_hWnd, 0x1C, 1, 0); // WM_ACTIVATEAPP
    Window_msg_main_handler(g_hWnd, 0x18, 0, 0); // WM_SHOWWINDOW
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);


#ifdef ARCH_WASM
    //int fps = 0; // Use browser's requestAnimationFrame
    //emscripten_set_main_loop_arg(Window_Main_Loop, NULL, fps, 1);
    while (1)
    {
        Window_Main_Loop();
        if (g_should_exit) break;
    }
#else
    while (1)
    {
        Window_Main_Loop();
        if (g_should_exit) break;
    }
#endif

    // Added
    if (jkPlayer_bHasLoadedSettingsOnce) {
        jkPlayer_WriteConf(jkPlayer_playerShortName);
    }

#if defined(TARGET_IOS) || defined(TARGET_ANDROID)
    // Added: settings are already flushed above, so quit hard rather than
    // unwinding. Two things make the orderly path hang here: UIKit owns the run
    // loop, so returning out of main never terminates the process, and
    // Main_Shutdown() tears down ANGLE/OpenAL/SDL while the platform still holds
    // references to them. _Exit rather than exit deliberately -- exit() would
    // still run atexit handlers and the libraries' static destructors, which is
    // the teardown being avoided.
    _Exit(0);
#endif

    Main_Shutdown();
    return 1;
}

int Window_Main(HINSTANCE hInstance, int a2, char *lpCmdLine, int nShowCmd, LPCSTR lpWindowName)
{
    int result;

    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = hInstance;
    g_nShowCmd = nShowCmd;
#if 0
    if (jk_RegisterClassExA(&wndClass))
    {
        if ( jk_FindWindowA("wKernel", lpWindowName) )
            jk_exit(-1);

        uint32_t hres = jk_GetSystemMetrics(1);
        uint32_t vres = jk_GetSystemMetrics(0);
        g_hWnd = jk_CreateWindowExA(0x40000u, "wKernel", lpWindowName, 0x90000000, 0, 0, vres, hres, 0, 0, hInstance, 0);

        if (g_hWnd)
        {
            g_hInstance = hInstance;
            jk_ShowWindow(g_hWnd, 1);
            jk_UpdateWindow(g_hWnd);
        }
    }

    stdGdi_SetHwnd(g_hWnd);
    stdGdi_SetHInstance(g_hInstance);
    jk_InitCommonControls();

    g_855E8C = 2 * jk_GetSystemMetrics(32);
    uint32_t metrics_32 = jk_GetSystemMetrics(32);
    g_855E90 = jk_GetSystemMetrics(15) + 2 * metrics_32;
    result = Main_Startup(lpCmdLine);

    if (!result) return result;

    
    g_window_not_destroyed = 1;

    while (1)
    {
        if (jk_PeekMessageA(&msg, 0, 0, 0, 0))
        {
            if (!jk_GetMessageA(&msg, 0, 0, 0))
            {
                result = msg.wParam;
                g_should_exit = 1;
                break;
            }

            uint32_t some_cnt = 0;
            if (g_thing_two_some_dialog_count > 0)
            {
#if 0
                v16 = &thing_three;
                do
                {
                    //TODO if ( jk_IsDialogMessageA(*v16, &msg) )
                    //  break;
                    ++some_cnt;
                    ++v16;
                }
                while ( some_cnt < g_thing_two_some_dialog_count );
#endif
            }

            if (some_cnt == g_thing_two_some_dialog_count)
            {
                jk_TranslateMessage(&msg);
                jk_DispatchMessageA(&msg);
            }

            if (!jk_PeekMessageA(&msg, 0, 0, 0, 0))
            {
                result = 0;
                if ( g_should_exit )
                    return result;
            }
        }

        //if (user32->stopping) break;

        jkMain_GuiAdvance();
    }
#endif
    result = 1;
    return result;
}

int Window_ShowCursorUnwindowed(int a1)
{
    return stdControl_ShowMouseCursor(a1);
}

int Window_DefaultHandler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, void* unused)
{
    return 0;
}

int Window_MessageLoop()
{
    // Recursion guard
    int bWasInSdlUpdate = Window_bInSdlUpdate;
    Window_bInSdlUpdate = 0;

    // Added: controller menuing
    jkGuiRend_UpdateController();

    jkMain_GuiAdvance();
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
    
    //Window_SdlUpdate();

    Window_bInSdlUpdate = bWasInSdlUpdate;
    return 0;
}

#endif // SDL2_RENDER

void Window_SetDrawHandlers(WindowDrawHandler_t a1, WindowDrawHandler_t a2)
{
    Window_drawAndFlip = a1;
    Window_setCooperativeLevel = a2;
}

void Window_GetDrawHandlers(WindowDrawHandler_t *a1, WindowDrawHandler_t *a2)
{
    *a1 = Window_drawAndFlip;
    *a2 = Window_setCooperativeLevel;
}
