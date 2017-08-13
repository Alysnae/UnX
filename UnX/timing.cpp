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
#include "timing.h"
#include "config.h"
#include "hook.h"
#include "log.h"

#include <Windows.h>

Sleep_pfn                   SK_Sleep                         = nullptr;

typedef LONG NTSTATUS;

typedef NTSTATUS (NTAPI *NtQueryTimerResolution_pfn)
(
  OUT PULONG              MinimumResolution,
  OUT PULONG              MaximumResolution,
  OUT PULONG              CurrentResolution
);

typedef NTSTATUS (NTAPI *NtSetTimerResolution_pfn)
(
  IN  ULONG               DesiredResolution,
  IN  BOOLEAN             SetResolution,
  OUT PULONG              CurrentResolution
);

HMODULE                    NtDll                  = 0;

NtQueryTimerResolution_pfn NtQueryTimerResolution = nullptr;
NtSetTimerResolution_pfn   NtSetTimerResolution   = nullptr;

typedef DWORD (WINAPI *SleepEx_pfn)(
  _In_ DWORD dwMilliseconds,
  _In_ BOOL  bAlertable
);

SleepEx_pfn SleepEx_Original = nullptr;

DWORD
SleepEx_Detour (DWORD dwMilliseconds, BOOL bAlertable)
{
  //if (dwMilliseconds > 5)
    //dwMilliseconds = 5;

  return SleepEx_Original (dwMilliseconds, bAlertable);
}



typedef int (__cdecl *VSYNCEmulation_pfn)(int);
VSYNCEmulation_pfn VSYNCEmulation_Original = nullptr;

extern LPVOID __UNX_base_img_addr;

int
__cdecl
VSYNCEmulation_Detour (int x)
{
  dll_log->Log (L"[   Time   ] VSYNC Emulation Thread Created By Game", x);

  return (VSYNCEmulation_Original (x));
}


void
WINAPI
UNX_SE_FixFramerateLimiter (DWORD dwMilliseconds)
{
  if (dwMilliseconds == 0) {
    YieldProcessor ();
    return;
  }

  if (dwMilliseconds == 5) {
    //YieldProcessor ();
    return;
    ////dwMilliseconds = 0;
  }

  return SK_Sleep (dwMilliseconds);
}

#include "command.h"

void
unx::TimingFix::Init (void)
{
  if (NtDll == 0) {
    NtDll = LoadLibrary (L"ntdll.dll");

    NtQueryTimerResolution =
      (NtQueryTimerResolution_pfn)
        GetProcAddress (NtDll, "NtQueryTimerResolution");

    NtSetTimerResolution =
      (NtSetTimerResolution_pfn)
        GetProcAddress (NtDll, "NtSetTimerResolution");

    if (NtQueryTimerResolution != nullptr &&
        NtSetTimerResolution   != nullptr) {
      ULONG min, max, cur;
      NtQueryTimerResolution (&min, &max, &cur);
      dll_log->Log ( L"[  Timing  ] Kernel resolution.: %f ms",
                       (float)(cur * 100)/1000000.0f );
      NtSetTimerResolution   (max, TRUE,  &cur);
      dll_log->Log ( L"[  Timing  ] New resolution....: %f ms",
                       (float)(cur * 100)/1000000.0f );

    }
  }

  HMODULE hModKernel32 = LoadLibrary (L"kernel32.dll");

#if 0
   UNX_CreateFuncHook ( L"VSYNCEmulation",
       (LPVOID)((intptr_t)__UNX_base_img_addr + 0x27A010),
                       VSYNCEmulation_Detour,
            (LPVOID *)&VSYNCEmulation_Original );
  UNX_EnableHook ((LPVOID)((intptr_t)__UNX_base_img_addr + 0x27A010));
#endif

#if 0
  if (config.stutter.reduce)
  {
    //SK_GetCommandProcessor ()->ProcessCommandFormatted ("MaxDeltaTime 0");

    UNX_CreateDLLHook2 ( config.system.injector.c_str (),
                         "Sleep_Detour",
                         UNX_SE_FixFramerateLimiter,
              (LPVOID *)&SK_Sleep );

    UNX_CreateDLLHook2 ( L"kernel32.dll",
                         "SleepEx",
                         SleepEx_Detour,
              (LPVOID *)&SleepEx_Original );

    UNX_ApplyQueuedHooks ();
  }
#endif
}

void
unx::TimingFix::Shutdown (void)
{
  FreeLibrary (NtDll);
}