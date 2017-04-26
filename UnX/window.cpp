/**
 * This file is part of UnX.
 *
 * UnX is free software : you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by The Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * UnX is distributed in the hope that it will be useful,
 *
 * But WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with UnX.
 *
 *   If not, see <http://www.gnu.org/licenses/>.
 *
**/
#include <Windows.h>
#include <windowsx.h> // GET_X_LPARAM

#include "window.h"
#include "input.h"
#include "config.h"
#include "log.h"
#include "hook.h"

#include "sound.h"
#include "cheat.h"

#include <dxgi.h>
#include <d3d11.h>

typedef HRESULT (WINAPI *DXGISwap_ResizeBuffers_pfn)(
   IDXGISwapChain *This,
   UINT            BufferCount,
   UINT            Width,
   UINT            Height,
   DXGI_FORMAT     NewFormat,
   UINT            SwapChainFlags
);
DXGISwap_ResizeBuffers_pfn DXGISwap_ResizeBuffers_Original = nullptr;

typedef HRESULT (WINAPI *DXGISwap_ResizeTarget_pfn)(
   IDXGISwapChain *This,
   DXGI_MODE_DESC *desc
);
DXGISwap_ResizeTarget_pfn DXGISwap_ResizeTarget_Original = nullptr;

typedef HWND (WINAPI *SK_GetGameWindow_pfn)(void);
extern SK_GetGameWindow_pfn SK_GetGameWindow;

IDXGISwapChain* pGameSwapChain = nullptr;

extern BOOL
APIENTRY
DllMain (HMODULE hModule,
         DWORD   ul_reason_for_call,
         LPVOID  /* lpReserved */);

enum unx_fullscreen_op_t {
  Fullscreen,
  Window,
  Restore
};

#include <atlbase.h>

bool
UNX_IsRenderThread (void)
{
  // Plugin not fully initialized yet ... we have no choice but
  //   to report this as false.
  if (unx::window.render_thread == 0)
    return false;

  if (GetCurrentThreadId () == unx::window.render_thread)
    return true;

  return false;
}

bool
UNX_IsWindowThread (void)
{
  // Plugin not fully initialized yet ... we have no choice but
  //   to report this as false.
  if (unx::window.hwnd == 0)
    return false;

  if ( GetCurrentThreadId       (                           ) ==
       GetWindowThreadProcessId ( unx::window.hwnd, nullptr ) )
    return true;

  return false;
}

DWORD
WINAPI
UNX_ToggleFullscreenThread (LPVOID user)
{
  BOOL                  fullscreen = FALSE;
  CComPtr <IDXGIOutput> pOutput    = nullptr;

  if (SUCCEEDED (pGameSwapChain->GetFullscreenState (&fullscreen, &pOutput))) {
    if (fullscreen) {
      DXGI_SWAP_CHAIN_DESC swap_desc;
      pGameSwapChain->GetDesc (&swap_desc);

      DXGI_MODE_DESC mode = swap_desc.BufferDesc;
      swap_desc.BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
      swap_desc.BufferUsage       = DXGI_USAGE_DISCARD_ON_PRESENT;
      swap_desc.Flags             = 0x02;

      pGameSwapChain->ResizeTarget       (&mode);
      pGameSwapChain->SetFullscreenState (FALSE, nullptr);
    } else {
      DXGI_SWAP_CHAIN_DESC swap_desc;
      pGameSwapChain->GetDesc (&swap_desc);

      DXGI_MODE_DESC mode = swap_desc.BufferDesc;
      swap_desc.BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
      swap_desc.BufferUsage       = DXGI_USAGE_DISCARD_ON_PRESENT;
      swap_desc.Flags             = 0x02;

      pGameSwapChain->ResizeTarget       (&mode);
      pGameSwapChain->SetFullscreenState (TRUE, pOutput);

      pOutput.Release ();

      mode.RefreshRate.Denominator = 0;
      mode.RefreshRate.Numerator   = 0;

      pGameSwapChain->ResizeTarget  (&mode);

      //pGameSwapChain->ResizeBuffers (0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                     //DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    }
  }

  return 0;
}

void
UNX_ToggleFullscreen (void)
{
  // Mod is not yet fully initialized...
  if (! pGameSwapChain)
    return;

  if ( UNX_IsWindowThread () ||
       UNX_IsRenderThread () ) {
    // It is not safe to issue DXGI commands from the thread that drives
    //   the Windows message pump... so spawn a separate thread to do this.
    CreateThread (nullptr, 0, UNX_ToggleFullscreenThread, nullptr, 0, nullptr);
  } else {
    UNX_ToggleFullscreenThread (nullptr);
  }
}


void UNX_SetFullscreenState (unx_fullscreen_op_t op);

void
UNX_SetFullscreenState (unx_fullscreen_op_t op)
{
  static BOOL last_fullscreen = FALSE;

  // Mod is not yet fully initialized...
  if (pGameSwapChain == nullptr)
    return;

  BOOL fs;
  pGameSwapChain->GetFullscreenState (&fs, nullptr);

  if (op == Fullscreen) {
    if (fs != TRUE) {
      dll_log->Log (L"[Fullscreen] Transition: Window -> Full");

      last_fullscreen = fs;

      UNX_ToggleFullscreen ();
    }
  } else {
    if (op == Restore) {
      dll_log->Log (L"[Fullscreen] Operation: *Restore");
      UNX_SetFullscreenState (last_fullscreen ? Fullscreen : Window);
      last_fullscreen = fs;
    }

    if (op == Window) {
      if (fs == TRUE) {
        dll_log->Log (L"[Fullscreen] Transition: Full -> Window");

        last_fullscreen = fs;

        UNX_ToggleFullscreen ();
      }
    }
  }
}


unx::window_state_s unx::window;

LRESULT
CALLBACK
DetourWindowProc ( _In_  HWND   hWnd,
                   _In_  UINT   uMsg,
                   _In_  WPARAM wParam,
                   _In_  LPARAM lParam );


bool windowed = false;

typedef LRESULT (CALLBACK *DetourWindowProc_pfn)( _In_  HWND   hWnd,
                   _In_  UINT   uMsg,
                   _In_  WPARAM wParam,
                   _In_  LPARAM lParam
);

DetourWindowProc_pfn DetourWindowProc_Original = nullptr;

volatile ULONG schedule_load = FALSE;
volatile ULONG queue_death   = FALSE;

bool shutting_down = false;
bool last_active   = unx::window.active;

LRESULT
CALLBACK
DetourWindowProc ( _In_  HWND   hWnd,
                   _In_  UINT   uMsg,
                   _In_  WPARAM wParam,
                   _In_  LPARAM lParam )
{
  unx::window.hwnd = hWnd;

  extern LPVOID __UNX_base_img_addr;

  if (schedule_load) {
    schedule_load = false;

    typedef int (__stdcall *LoadSave_pfn)   (void);
    typedef int (__cdecl   *LoadSave2_pfn)  (int);
    typedef int (__stdcall *LoadSave3_pfn)  (void);
    typedef int (__cdecl   *LoadSaveXXX_pfn)(int);

    LoadSave_pfn LoadSave =
      (LoadSave_pfn)
        ((intptr_t)__UNX_base_img_addr + 0x248910);

    LoadSave2_pfn LoadSave2 =
      (LoadSave2_pfn)
        ((intptr_t)__UNX_base_img_addr + 0x248890);

    LoadSave3_pfn LoadSave3 =
      (LoadSave3_pfn)
        ((intptr_t)__UNX_base_img_addr + 0x230DE0);

    LoadSaveXXX_pfn LoadSaveXXX =
      (LoadSaveXXX_pfn)
        ((intptr_t)__UNX_base_img_addr + 0x421870);

    int xxx = 0;

    //*(int *)((intptr_t)__UNX_base_img_addr + 0xCE72D0) = 0;
      *(int *)((intptr_t)__UNX_base_img_addr + 0x8CB994) = 1;

    LoadSaveXXX (3);

//    LoadSave  ();
//    LoadSave2 (1);
//    LoadSave2 (2);
//    LoadSave3 ();
  }


  if (GetForegroundWindow () == hWnd)
    unx::window.active = true;
  else
    unx::window.active = false;


  //
  // Setup the Cheat Manager on the first message received
  //   while the render window is active
  //
  static volatile ULONG init_cheats = FALSE;
  if ( unx::window.active &&
     (! InterlockedCompareExchange (&init_cheats, TRUE, FALSE)) )
  {
    unx::CheatManager::Init ();
  }


  if (config.input.filter_ime) {
    if ( (uMsg >= WM_IME_SETCONTEXT        && uMsg <= WM_IME_KEYUP) ||
         (uMsg >= WM_DWMCOMPOSITIONCHANGED && uMsg <= WM_DWMSENDICONICLIVEPREVIEWBITMAP) ) {
      //dll_log->Log ( L"[IME Fix-Up]  Ignoring IME Message (%x)",
                     //uMsg );
      return DefWindowProc (hWnd, uMsg, wParam, lParam);
    }
  }

  // This state is persistent and we do not want Alt+F4 to remember
  //   a muted state.
  if (  uMsg == WM_DESTROY    || uMsg == WM_QUIT ||
      (config.input.fast_exit && uMsg == WM_CLOSE)  ) {
    shutting_down = true;

    UNX_SetGameMute (FALSE);

    // Don't change the active window when shutting down
    //if (SetForegroundWindow (SK_GetGameWindow ())) {

      // The game would deadlock and never shutdown if we tried to Alt+F4 in
      //   fullscreen mode... oh the quirks of D3D :(
      if (pGameSwapChain != nullptr && config.display.enable_fullscreen) {
        UNX_SetFullscreenState (Window);
        // Give some wait time before proceeding with shutdown so that
        //   the DLL properly finishes its shutdown procedure.
        Sleep (250);
      }
    //}

    // Don't trigger the code below that handles window deactivation
    //   in fullscreen mode
    unx::window.active = last_active;

    return DetourWindowProc_Original (hWnd, uMsg, wParam, lParam);
  }

  //
  // The window activation state is changing, among other things we can take
  //   this opportunity to setup a special framerate limit.
  //
  if ( (! shutting_down) && 
          ( unx::window.active != last_active ||
              ( uMsg == WM_ACTIVATEAPP        &&
                unx::window.active != last_active
              )
          )
     )
  {
    bool deactivate = ! (unx::window.active);

    if (uMsg == WM_ACTIVATEAPP) {
      deactivate = wParam == 0;
      unx::window.active = ! deactivate;
    }

    last_active = unx::window.active;

    dll_log->Log ( L"[Window Mgr] Activation: %s",
                     unx::window.active ? L"ACTIVE" :
                                          L"INACTIVE" );

    //
    // Allow Alt+Tab to work
    //
    if (pGameSwapChain != nullptr && config.display.enable_fullscreen) {
      if (! deactivate) {
        UNX_SetFullscreenState (Restore);
      } else {
        UNX_SetFullscreenState (Window);
      }
    }
  }

  if (config.input.trap_alt_tab) {
    if ( uMsg == WM_NCACTIVATE ) {
      return 0;
    }
  }

  if (uMsg == WM_TIMER) {
    if (wParam == unx::CHEAT_TIMER_FFX)
      unx::CheatTimer_FFX ();
    else if (wParam == unx::CHEAT_TIMER_FFX2)
      unx::CheatTimer_FFX2 ();
  }




  if (config.input.fix_bg_input) {
    // Block keyboard input to the game while the console is visible
    if (! (unx::window.active)/* || background_render*/) {
      if (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST)
        return DefWindowProc (hWnd, uMsg, wParam, lParam);

      // Block RAW Input
      if (uMsg == WM_INPUT)
        return DefWindowProc (hWnd, uMsg, wParam, lParam);
    }
  }


  // Block the menu key from messing with stuff*
  if ((uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP)) {
    // Alt + Enter (Fullscreen toggle)
    if (uMsg == WM_SYSKEYDOWN && wParam == VK_RETURN) {
      if (pGameSwapChain && config.display.enable_fullscreen) {
        UNX_ToggleFullscreen ();
        return DefWindowProc (hWnd, uMsg, wParam, lParam);
      }
    }

    if (wParam != VK_TAB || (! config.input.trap_alt_tab)) {
      // Actually, just block Alt+F4
      if (config.input.fast_exit && wParam == VK_F4)
        return DefWindowProc (hWnd, uMsg, wParam, lParam);
    }
  }

  return DetourWindowProc_Original (hWnd, uMsg, wParam, lParam);
}

DWORD
WINAPI
UNX_MWA_Thread (LPVOID pUser)
{
  return 0;

  DXGI_SWAP_CHAIN_DESC desc;

  if (config.display.enable_fullscreen) {
    BOOL fullscreen = FALSE;

    if (pGameSwapChain) {
      if ( SUCCEEDED ( pGameSwapChain->GetFullscreenState (
                            &fullscreen,
                              nullptr                     )
                     )
         )
      {
      }
    }
  }

  if (pGameSwapChain != nullptr) {
    if (SUCCEEDED (pGameSwapChain->GetDesc (&desc))) {
      //
      // Allow Alt+Enter
      //

      CComPtr <ID3D11Device> pDev;
      CComPtr <IDXGIDevice>  pDevDXGI;
      CComPtr <IDXGIAdapter> pAdapter;
      CComPtr <IDXGIFactory> pFactory;

      if ( SUCCEEDED (pGameSwapChain->GetDevice  (IID_PPV_ARGS (&pDev))     )&&
           SUCCEEDED (pDev->QueryInterface       (IID_PPV_ARGS (&pDevDXGI)) )&&
           SUCCEEDED (pDevDXGI->GetAdapter                     (&pAdapter)  )&&
           SUCCEEDED (      pAdapter->GetParent  (IID_PPV_ARGS (&pFactory)) ) )
      {
        dll_log->Log( L"[Fullscreen] Setting DXGI Window Association "
                      L"(HWND: Game=%X,SwapChain=%X)",
                        SK_GetGameWindow (), desc.OutputWindow );
        dll_log->Log( L"[Fullscreen]   >> flags: "
                      L"DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER" );

        pFactory->MakeWindowAssociation ( desc.OutputWindow,
                                            DXGI_MWA_NO_WINDOW_CHANGES |
                                            DXGI_MWA_NO_ALT_ENTER );
      }
    }
  }

  return 0;
}

HRESULT
WINAPI
DXGISwap_ResizeBuffers_Detour (
   IDXGISwapChain *This,
   UINT            BufferCount,
   UINT            Width,
   UINT            Height,
   DXGI_FORMAT     NewFormat,
   UINT            SwapChainFlags
)
{
  DXGI_SWAP_CHAIN_DESC desc;

  if (SUCCEEDED (This->GetDesc (&desc))) {
    if (desc.OutputWindow == SK_GetGameWindow ())
      pGameSwapChain = This;
  }

  // Allow Fullscreen
  if (config.display.enable_fullscreen)
    SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  else
    SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

  HRESULT hr =
    DXGISwap_ResizeBuffers_Original ( This,
                                        BufferCount,
                                          Width, Height,
                                            NewFormat,
                                              SwapChainFlags );

  //if ( UNX_IsWindowThread () || UNX_IsRenderThread () )
    //CreateThread (nullptr, 0, UNX_MWA_Thread, nullptr, 0, nullptr);
  //else
    UNX_MWA_Thread (nullptr);

  return hr;
}

typedef void (WINAPI *SK_BeginBufferSwap_pfn)(void);
SK_BeginBufferSwap_pfn SK_BeginBufferSwap_Original = nullptr;

typedef BOOL (WINAPI *SKX_DrawExternalOSD_pfn)(const char* szAppName, const char* szText);
SKX_DrawExternalOSD_pfn SKX_DrawExternalOSD = nullptr;

extern std::string UNX_SummarizeCheats (DWORD dwTime);

void
WINAPI
SK_BeginBufferSwap_Detour (void)
{
  if (unx::window.render_thread == 0)
    unx::window.render_thread = GetCurrentThreadId ();

  if (SK_BeginBufferSwap_Original != nullptr)
    SK_BeginBufferSwap_Original ();
  else {
    dll_log->Log (L" !!! Unexpected Lack of SK_BufferSwap_Override in dxgi.dll !!! ");
  }

  if (InterlockedCompareExchange (&queue_death, FALSE, TRUE)) {
    SK_GetCommandProcessor ()->ProcessCommandLine ("mem b D2A8E2 2");
  }

  if (SKX_DrawExternalOSD != nullptr) {
    static bool  first_frame      = true;
    static bool  draw_osd_toggle  = true;
    static DWORD first_frame_time = timeGetTime ();

    DWORD now = timeGetTime ();

    std::string osd_out = "";

    if (draw_osd_toggle) {
      if (now - first_frame_time < 5000) {
        osd_out +=
                   "  Press Ctrl + Shift + O         to toggle In-Game OSD\n"
                   "  Press Ctrl + Shift + Backspace to access In-Game Config Menu\n"
                   "    ( Press and HOLD L2 + R2 + Select on Gamepads )\n"
                   "      [Titlebar will colorcycle in gamepad mode]\n\n";
        //osd_out += "Press Ctrl + Shift + O to toggle OSD\n\n";
      } else {
        draw_osd_toggle = false;
      }
    }

    osd_out += UNX_SummarizeCheats (now);

    SKX_DrawExternalOSD ("UnX Status", osd_out.c_str ());
  }
}

void
UNX_InstallWindowHook (HWND hWnd)
{
  unx::window.hwnd = hWnd;

  UNX_CreateDLLHook ( config.system.injector.c_str (),
                      "SK_BeginBufferSwap",
                       SK_BeginBufferSwap_Detour,
            (LPVOID *)&SK_BeginBufferSwap_Original );

  UNX_CreateDLLHook ( config.system.injector.c_str (),
                      "SK_DetourWindowProc",
                       DetourWindowProc,
            (LPVOID *)&DetourWindowProc_Original );

  UNX_CreateDLLHook ( config.system.injector.c_str (),
                      "DXGISwap_ResizeBuffers_Override",
                       DXGISwap_ResizeBuffers_Detour,
            (LPVOID *)&DXGISwap_ResizeBuffers_Original );

  HMODULE hModInject  = GetModuleHandleW (config.system.injector.c_str ());

  SKX_DrawExternalOSD =
   (SKX_DrawExternalOSD_pfn)GetProcAddress (
     hModInject,
       "SKX_DrawExternalOSD"
   );
}





void
unx::WindowManager::Init (void)
{
//  CommandProcessor* comm_proc = CommandProcessor::getInstance ();
}

void
unx::WindowManager::Shutdown (void)
{
  unx::CheatManager::Shutdown ();
}


unx::WindowManager::
  CommandProcessor::CommandProcessor (void)
{
}

bool
  unx::WindowManager::
    CommandProcessor::OnVarChange (SK_IVariable* var, void* val)
{
  SK_ICommandProcessor* pCommandProc = SK_GetCommandProcessor ();

  bool known = false;

  if (! known) {
    dll_log->Log ( L"[Window Mgr] UNKNOWN Variable Changed (%p --> %p)",
                     var,
                       val );
  }

  return false;
}

unx::WindowManager::CommandProcessor*
   unx::WindowManager::CommandProcessor::pCommProc = nullptr;