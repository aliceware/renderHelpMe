/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <Psapi.h>
#include <tchar.h>
#include <tlhelp32.h>
#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#include <string>

static rdcarray<EnvironmentModification> &GetEnvModifications()
{
  static rdcarray<EnvironmentModification> envCallbacks;
  return envCallbacks;
}

struct InsensitiveComparison
{
  bool operator()(const rdcstr &a, const rdcstr &b) const { return strlower(a) < strlower(b); }
};

typedef std::map<rdcstr, rdcstr, InsensitiveComparison> EnvMap;

static EnvMap EnvStringToEnvMap(const wchar_t *envstring)
{
  EnvMap ret;

  const wchar_t *e = envstring;

  while(*e)
  {
    const wchar_t *equals = wcschr(e, L'=');

    rdcstr name = StringFormat::Wide2UTF8(rdcwstr(e, equals - e));
    rdcstr value = StringFormat::Wide2UTF8(equals + 1);

    ret[name] = value;

    // jump to \0 and past it
    e += wcslen(e) + 1;
  }

  return ret;
}

void Process::RegisterEnvironmentModification(const EnvironmentModification &modif)
{
  GetEnvModifications().push_back(modif);
}

static void ApplyEnvModifications(EnvMap &envValues,
                                  const rdcarray<EnvironmentModification> &modifications,
                                  bool setToSystem)
{
  for(size_t i = 0; i < modifications.size(); i++)
  {
    const EnvironmentModification &m = modifications[i];

    rdcstr value;

    auto it = envValues.find(m.name);
    if(it != envValues.end())
      value = it->second;

    switch(m.mod)
    {
      case EnvMod::Set: value = m.value.c_str(); break;
      case EnvMod::Append:
      {
        if(!value.empty())
        {
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            value += ";";
          else if(m.sep == EnvSep::Colon)
            value += ":";
        }
        value += m.value.c_str();
        break;
      }
      case EnvMod::Prepend:
      {
        if(!value.empty())
        {
          rdcstr prep = m.value;
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            prep += ";";
          else if(m.sep == EnvSep::Colon)
            prep += ":";
          value = prep + value;
        }
        else
        {
          value = m.value.c_str();
        }
        break;
      }
    }

    envValues[m.name] = value;

    if(setToSystem)
      SetEnvironmentVariableW(StringFormat::UTF82Wide(m.name).c_str(),
                              StringFormat::UTF82Wide(value).c_str());
  }
}

// on windows we apply environment changes here, after process initialisation
// but before any real work (in RenderDoc::Initialise) so that we support
// injecting the dll into processes we didn't launch (ie didn't control the
// starting environment for), or even the application loading the dll itself
// without any interaction with our replay app.
void Process::ApplyEnvironmentModification()
{
  // turn environment string to a UTF-8 map
  LPWCH envStrings = GetEnvironmentStringsW();
  EnvMap envValues = EnvStringToEnvMap(envStrings);
  FreeEnvironmentStringsW(envStrings);
  rdcarray<EnvironmentModification> &modifications = GetEnvModifications();

  ApplyEnvModifications(envValues, modifications, true);

  // these have been applied to the current process
  modifications.clear();
}

rdcstr Process::GetEnvVariable(const rdcstr &name)
{
  DWORD len = GetEnvironmentVariableA(name.c_str(), NULL, 0);
  if(len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    return rdcstr();

  rdcstr ret;
  ret.resize(len + 1);

  GetEnvironmentVariableA(name.c_str(), ret.data(), len);
  ret.trim();
  return ret;
}

uint64_t Process::GetMemoryUsage()
{
  HANDLE proc = GetCurrentProcess();

  if(proc == NULL)
  {
    RDCERR("Couldn't open process: %d", GetLastError());
    return 0;
  }

  PROCESS_MEMORY_COUNTERS memInfo = {};

  uint64_t ret = 0;

  if(GetProcessMemoryInfo(proc, &memInfo, sizeof(memInfo)))
  {
    ret = memInfo.WorkingSetSize;
  }
  else
  {
    RDCERR("Couldn't get process memory info: %d", GetLastError());
  }

  return ret;
}

// helpers for various shims and dlls etc, not part of the public API
extern "C" __declspec(dllexport) void __cdecl INTERNAL_GetTargetControlIdent(uint32_t *ident)
{
  if(ident)
    *ident = RenderDoc::Inst().GetTargetControlIdent();
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureOptions(CaptureOptions *opts)
{
  if(opts)
    RenderDoc::Inst().SetCaptureOptions(*opts);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureFile(const char *capfile)
{
  if(capfile)
    RenderDoc::Inst().SetCaptureFileTemplate(capfile);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetDebugLogFile(const char *logfile)
{
  RENDERDOC_SetDebugLogFile(logfile ? logfile : rdcstr());
}

static EnvironmentModification tempEnvMod;

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModName(const char *name)
{
  if(name)
    tempEnvMod.name = name;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModValue(const char *value)
{
  if(value)
    tempEnvMod.value = value;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvSep(EnvSep *sep)
{
  if(sep)
    tempEnvMod.sep = *sep;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvMod(EnvMod *mod)
{
  if(mod)
  {
    tempEnvMod.mod = *mod;
    Process::RegisterEnvironmentModification(tempEnvMod);
  }
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_ApplyEnvMods(void *ignored)
{
  Process::ApplyEnvironmentModification();
}

uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName);

static void AppendByte(rdcarray<uint8_t> &code, uint8_t value)
{
  code.push_back(value);
}

#if defined(_WIN64)
static void AppendUInt64(rdcarray<uint8_t> &code, uint64_t value)
{
  for(int i = 0; i < 8; i++)
    code.push_back(uint8_t(value >> (i * 8)));
}
#else
static void AppendUInt32(rdcarray<uint8_t> &code, uint32_t value)
{
  for(int i = 0; i < 4; i++)
    code.push_back(uint8_t(value >> (i * 8)));
}
#endif

static bool InjectDLLWithThreadContext(DWORD pid, HANDLE hProcess, rdcwstr libName,
                                       rdcstr *failureReason)
{
  RDCDEBUG("SetThreadContext injection path for %u: %ls", pid, libName.c_str());
  auto fail = [&](const char *stage) {
    DWORD error = GetLastError();
    if(failureReason)
      *failureReason = StringFormat::Fmt("%s (error %u)", stage, error);
    RDCERR("SetThreadContext injection failed at %s for process %u: %u", stage, pid, error);
    return false;
  };

  HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  HMODULE kernelbase = GetModuleHandleA("kernelbase.dll");
  HMODULE loadLibraryModule = kernelbase ? kernelbase : kernel32;
  if(kernel32 == NULL || loadLibraryModule == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    return fail("resolving LoadLibraryExW");
  }

  // Use LoadLibraryExW with the DLL-directory and default-directory search flags so the injected
  // DLL first resolves private dependencies from its own directory, then still finds system
  // runtime DLLs. This avoids accidentally binding to an older same-named runtime shipped beside
  // the game while retaining normal API-set resolution.
  uintptr_t loadLibraryLocal = (uintptr_t)GetProcAddress(loadLibraryModule, "LoadLibraryExW");
  uintptr_t getLastErrorLocal = (uintptr_t)GetProcAddress(kernel32, "GetLastError");
  if(loadLibraryLocal == 0)
  {
    RDCERR("Couldn't resolve LoadLibraryExW in target process %u", pid);
    return fail("resolving LoadLibraryExW");
  }

  const char *loadLibraryModuleName = kernelbase ? "kernelbase.dll" : "kernel32.dll";
  uintptr_t kernel32Remote = FindRemoteDLL(pid, loadLibraryModuleName);
  uintptr_t kernel32BaseRemote = FindRemoteDLL(pid, "kernel32.dll");
  uintptr_t loadLibraryRemote = loadLibraryLocal;
  if(kernel32Remote != 0)
    loadLibraryRemote = kernel32Remote + loadLibraryLocal - (uintptr_t)loadLibraryModule;
  uintptr_t getLastErrorRemote = getLastErrorLocal;
  if(kernel32BaseRemote != 0 && getLastErrorLocal != 0)
    getLastErrorRemote = kernel32BaseRemote + getLastErrorLocal - (uintptr_t)kernel32;
  RDCDEBUG("SetThreadContext injection: kernel32 local=%p remote=%p LoadLibraryExW=%p", kernel32,
           (void *)kernel32Remote, (void *)loadLibraryRemote);

  THREADENTRY32 threadEntry;
  RDCEraseEl(threadEntry);
  threadEntry.dwSize = sizeof(threadEntry);

  HANDLE threadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if(threadSnapshot == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't enumerate threads for process %u: %u", pid, GetLastError());
    return fail("enumerating threads");
  }

  struct ThreadCandidate
  {
    DWORD id;
    HANDLE handle;
    uint64_t cpuTime;
  };
  rdcarray<ThreadCandidate> candidates;
  if(Thread32First(threadSnapshot, &threadEntry))
  {
    do
    {
      if(threadEntry.th32OwnerProcessID == pid)
      {
        HANDLE candidate = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                          THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                      FALSE, threadEntry.th32ThreadID);
        if(candidate != NULL)
        {
          FILETIME creation, exit, kernel, user;
          if(GetThreadTimes(candidate, &creation, &exit, &kernel, &user))
          {
            ULARGE_INTEGER kernelTime, userTime;
            kernelTime.LowPart = kernel.dwLowDateTime;
            kernelTime.HighPart = kernel.dwHighDateTime;
            userTime.LowPart = user.dwLowDateTime;
            userTime.HighPart = user.dwHighDateTime;
            candidates.push_back({threadEntry.th32ThreadID,
                                  candidate,
                                  kernelTime.QuadPart + userTime.QuadPart});
            continue;
          }
          CloseHandle(candidate);
        }
      }
    } while(Thread32Next(threadSnapshot, &threadEntry));
  }
  CloseHandle(threadSnapshot);

  Sleep(20);

  HANDLE thread = NULL;
  uint64_t bestCpuDelta = 0;
  size_t bestCandidate = ~size_t(0);
  for(size_t i = 0; i < candidates.size(); i++)
  {
    FILETIME creation, exit, kernel, user;
    uint64_t after = candidates[i].cpuTime;
    if(GetThreadTimes(candidates[i].handle, &creation, &exit, &kernel, &user))
    {
      ULARGE_INTEGER kernelTime, userTime;
      kernelTime.LowPart = kernel.dwLowDateTime;
      kernelTime.HighPart = kernel.dwHighDateTime;
      userTime.LowPart = user.dwLowDateTime;
      userTime.HighPart = user.dwHighDateTime;
      after = kernelTime.QuadPart + userTime.QuadPart;
    }

    uint64_t delta = after >= candidates[i].cpuTime ? after - candidates[i].cpuTime : 0;
    if(bestCandidate == ~size_t(0) || delta > bestCpuDelta)
    {
      bestCandidate = i;
      bestCpuDelta = delta;
    }
  }

  if(bestCandidate != ~size_t(0))
  {
    thread = candidates[bestCandidate].handle;
    threadEntry.th32ThreadID = candidates[bestCandidate].id;
  }

  for(size_t i = 0; i < candidates.size(); i++)
  {
    if(i != bestCandidate)
      CloseHandle(candidates[i].handle);
  }

  if(thread == NULL)
  {
    RDCERR("Couldn't open a thread in process %u for context injection: %u", pid,
           GetLastError());
    return fail("opening a target thread");
  }

  DWORD originalSuspendCount = SuspendThread(thread);
  if(originalSuspendCount == DWORD(-1))
  {
    RDCERR("Couldn't suspend thread %u for context injection: %u", threadEntry.th32ThreadID,
           GetLastError());
    CloseHandle(thread);
    return fail("suspending target thread");
  }

  // SuspendThread returns the count before our suspend.  A target can already be suspended by
  // a debugger, launcher, or another tool, so drain the count while running the trampoline and
  // restore the exact original count afterwards.
  DWORD currentSuspendCount = originalSuspendCount + 1;
  auto adjustSuspendCount = [&](DWORD targetCount) {
    for(uint32_t attempt = 0; currentSuspendCount != targetCount && attempt < 64; attempt++)
    {
      if(currentSuspendCount < targetCount)
      {
        DWORD previous = SuspendThread(thread);
        if(previous == DWORD(-1))
          return false;
        currentSuspendCount = previous + 1;
      }
      else
      {
        DWORD previous = ResumeThread(thread);
        if(previous == DWORD(-1))
          return false;
        currentSuspendCount = previous > 0 ? previous - 1 : 0;
      }
    }

    return currentSuspendCount == targetCount;
  };

  CONTEXT savedContext;
  RDCEraseEl(savedContext);
#if !defined(_WIN64)
  const bool useWow64Context = false;
  WOW64_CONTEXT savedWow64Context;
  RDCEraseEl(savedWow64Context);
  savedContext.ContextFlags = CONTEXT_ALL;
  savedWow64Context.ContextFlags = WOW64_CONTEXT_ALL;
  BOOL gotContext = useWow64Context ? Wow64GetThreadContext(thread, &savedWow64Context)
                                    : GetThreadContext(thread, &savedContext);
#else
  savedContext.ContextFlags = CONTEXT_ALL;
  BOOL gotContext = GetThreadContext(thread, &savedContext);
#endif
  if(!gotContext)
  {
    RDCERR("Couldn't read thread context for process %u: %u", pid, GetLastError());
    adjustSuspendCount(originalSuspendCount);
    CloseHandle(thread);
    return fail("reading target thread context");
  }

  // LaunchAndInjectIntoProcess creates the target suspended.  In that case the initial thread
  // can still be inside the process loader's startup path.  Calling LoadLibraryExW by hijacking
  // that context re-enters the loader too early and can make the DLL report
  // ERROR_DLL_INIT_FAILED.  Let the initial thread reach user code, then suspend it again and
  // capture a fresh context for the SetThreadContext trampoline.
  if(originalSuspendCount > 0)
  {
    FILETIME creation, exit, kernel, user;
    uint64_t initialCpuTime = 0;
    if(GetThreadTimes(thread, &creation, &exit, &kernel, &user))
    {
      ULARGE_INTEGER kernelTime, userTime;
      kernelTime.LowPart = kernel.dwLowDateTime;
      kernelTime.HighPart = kernel.dwHighDateTime;
      userTime.LowPart = user.dwLowDateTime;
      userTime.HighPart = user.dwHighDateTime;
      initialCpuTime = kernelTime.QuadPart + userTime.QuadPart;
    }

    if(!adjustSuspendCount(0))
    {
      CloseHandle(thread);
      return fail("resuming target during loader startup");
    }

    // Wait until the initial thread has executed, with a short settling period for the loader
    // and CRT startup.  The process is still very early here, so this does not allow normal
    // rendering to get meaningfully ahead of the capture setup.
    for(uint32_t retry = 0; retry < 50; retry++)
    {
      Sleep(10);

      FILETIME currentCreation, currentExit, currentKernel, currentUser;
      if(GetThreadTimes(thread, &currentCreation, &currentExit, &currentKernel, &currentUser))
      {
        ULARGE_INTEGER currentKernelTime, currentUserTime;
        currentKernelTime.LowPart = currentKernel.dwLowDateTime;
        currentKernelTime.HighPart = currentKernel.dwHighDateTime;
        currentUserTime.LowPart = currentUser.dwLowDateTime;
        currentUserTime.HighPart = currentUser.dwHighDateTime;
        uint64_t currentCpuTime = currentKernelTime.QuadPart + currentUserTime.QuadPart;
        if(currentCpuTime > initialCpuTime && retry >= 10)
          break;
      }
    }

    // The initial thread may now be blocked in a startup wait.  A suspended thread in a kernel
    // wait will not execute a newly assigned RIP until that wait completes, so refresh the thread
    // list and prefer a thread that is actively consuming CPU after startup.
    rdcarray<ThreadCandidate> freshCandidates;
    HANDLE freshSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if(freshSnapshot != INVALID_HANDLE_VALUE)
    {
      THREADENTRY32 freshEntry;
      RDCEraseEl(freshEntry);
      freshEntry.dwSize = sizeof(freshEntry);
      if(Thread32First(freshSnapshot, &freshEntry))
      {
        do
        {
          if(freshEntry.th32OwnerProcessID == pid)
          {
            HANDLE candidate = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                              THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                          FALSE, freshEntry.th32ThreadID);
            if(candidate != NULL)
            {
              FILETIME candidateCreation, candidateExit, candidateKernel, candidateUser;
              if(GetThreadTimes(candidate, &candidateCreation, &candidateExit, &candidateKernel,
                                &candidateUser))
              {
                ULARGE_INTEGER candidateKernelTime, candidateUserTime;
                candidateKernelTime.LowPart = candidateKernel.dwLowDateTime;
                candidateKernelTime.HighPart = candidateKernel.dwHighDateTime;
                candidateUserTime.LowPart = candidateUser.dwLowDateTime;
                candidateUserTime.HighPart = candidateUser.dwHighDateTime;
                freshCandidates.push_back({freshEntry.th32ThreadID,
                                           candidate,
                                           candidateKernelTime.QuadPart + candidateUserTime.QuadPart});
                continue;
              }
              CloseHandle(candidate);
            }
          }
        } while(Thread32Next(freshSnapshot, &freshEntry));
      }
      CloseHandle(freshSnapshot);
    }

    Sleep(20);

    size_t freshBest = ~size_t(0);
    uint64_t freshBestDelta = 0;
    for(size_t i = 0; i < freshCandidates.size(); i++)
    {
      FILETIME candidateCreation, candidateExit, candidateKernel, candidateUser;
      uint64_t after = freshCandidates[i].cpuTime;
      if(GetThreadTimes(freshCandidates[i].handle, &candidateCreation, &candidateExit,
                        &candidateKernel, &candidateUser))
      {
        ULARGE_INTEGER candidateKernelTime, candidateUserTime;
        candidateKernelTime.LowPart = candidateKernel.dwLowDateTime;
        candidateKernelTime.HighPart = candidateKernel.dwHighDateTime;
        candidateUserTime.LowPart = candidateUser.dwLowDateTime;
        candidateUserTime.HighPart = candidateUser.dwHighDateTime;
        after = candidateKernelTime.QuadPart + candidateUserTime.QuadPart;
      }

      uint64_t delta = after >= freshCandidates[i].cpuTime ? after - freshCandidates[i].cpuTime : 0;
      if(freshBest == ~size_t(0) || delta > freshBestDelta)
      {
        freshBest = i;
        freshBestDelta = delta;
      }
    }

    if(freshBest != ~size_t(0))
    {
      HANDLE freshThread = freshCandidates[freshBest].handle;
      DWORD freshThreadId = freshCandidates[freshBest].id;
      for(size_t i = 0; i < freshCandidates.size(); i++)
      {
        if(i != freshBest)
          CloseHandle(freshCandidates[i].handle);
      }

      CloseHandle(thread);
      thread = freshThread;
      threadEntry.th32ThreadID = freshThreadId;
      originalSuspendCount = 0;
    }
    else
    {
      for(ThreadCandidate &candidate : freshCandidates)
        CloseHandle(candidate.handle);
    }

    DWORD suspendBeforeRefresh = SuspendThread(thread);
    if(suspendBeforeRefresh == DWORD(-1))
    {
      CloseHandle(thread);
      return fail("suspending target after loader startup");
    }
    currentSuspendCount = suspendBeforeRefresh + 1;

    RDCEraseEl(savedContext);
#if !defined(_WIN64)
    RDCEraseEl(savedWow64Context);
    savedContext.ContextFlags = CONTEXT_ALL;
    savedWow64Context.ContextFlags = WOW64_CONTEXT_ALL;
    gotContext = useWow64Context ? Wow64GetThreadContext(thread, &savedWow64Context)
                                : GetThreadContext(thread, &savedContext);
#else
    savedContext.ContextFlags = CONTEXT_ALL;
    gotContext = GetThreadContext(thread, &savedContext);
#endif
    if(!gotContext)
    {
      adjustSuspendCount(originalSuspendCount);
      CloseHandle(thread);
      return fail("reading target context after loader startup");
    }
  }

#if defined(_WIN64)
  uintptr_t savedStackPointer = (uintptr_t)savedContext.Rsp;
#elif defined(_WIN32)
  uintptr_t savedStackPointer =
      (uintptr_t)(useWow64Context ? savedWow64Context.Esp : savedContext.Esp);
#else
  uintptr_t savedStackPointer = (uintptr_t)savedContext.Esp;
#endif
  MEMORY_BASIC_INFORMATION stackInfo;
  RDCEraseEl(stackInfo);
  void *stackBackupAddress = NULL;
  SIZE_T stackBackupSize = 0;
  rdcarray<uint8_t> stackBackup;
  if(VirtualQueryEx(hProcess, (void *)savedStackPointer, &stackInfo, sizeof(stackInfo)) ==
         sizeof(stackInfo) &&
     stackInfo.State == MEM_COMMIT && stackInfo.RegionSize > 0)
  {
    stackBackupAddress = stackInfo.BaseAddress;
    stackBackupSize = stackInfo.RegionSize;
    stackBackup.resize(stackBackupSize);

    SIZE_T stackBytesRead = 0;
    if(!ReadProcessMemory(hProcess, stackBackupAddress, stackBackup.data(), stackBackupSize,
                          &stackBytesRead) ||
       stackBytesRead != stackBackupSize)
    {
      stackBackupAddress = NULL;
      stackBackupSize = 0;
      stackBackup.clear();
    }
  }

  if(stackBackupAddress == NULL)
  {
    ResumeThread(thread);
    CloseHandle(thread);
    return fail("backing up target thread stack");
  }

  const SIZE_T pathSize = (wcslen(libName.c_str()) + 1) * sizeof(wchar_t);
  void *remotePath = VirtualAllocEx(hProcess, NULL, pathSize, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
  void *remoteFlag = VirtualAllocEx(hProcess, NULL, sizeof(uint32_t), MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
  void *remoteError = VirtualAllocEx(hProcess, NULL, sizeof(uint32_t), MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);

  rdcarray<uint8_t> code;
  code.reserve(256);

#if defined(_WIN64)
  // Save all GPRs and flags, call LoadLibraryExW using the existing thread stack, then set a
  // completion flag and spin. The host restores the complete original CONTEXT before resuming.
  AppendByte(code, 0x50); // push rax
  AppendByte(code, 0x48);
  AppendByte(code, 0xb8); // mov rax, remoteFlag
  size_t entryFlagImmediate = code.size();
  AppendUInt64(code, 0);
  AppendByte(code, 0xc6);
  AppendByte(code, 0x00);
  AppendByte(code, 0x01); // mov byte ptr [rax], 1
  AppendByte(code, 0x9c); // pushfq
  AppendByte(code, 0x53); // push rbx
  AppendByte(code, 0x51); // push rcx
  AppendByte(code, 0x52); // push rdx
  AppendByte(code, 0x55); // push rbp
  AppendByte(code, 0x56); // push rsi
  AppendByte(code, 0x57); // push rdi
  for(uint8_t reg = 0x50; reg <= 0x57; reg++)
  {
    AppendByte(code, 0x41);
    AppendByte(code, reg); // push r8..r15
  }
  AppendByte(code, 0x48);
  AppendByte(code, 0x83);
  AppendByte(code, 0xec);
  AppendByte(code, 0x28); // shadow space and alignment
  AppendByte(code, 0x48);
  AppendByte(code, 0xb9); // mov rcx, remotePath
  size_t pathImmediate = code.size();
  AppendUInt64(code, 0);
  AppendByte(code, 0x48);
  AppendByte(code, 0xb8); // mov rax, LoadLibraryExW
  AppendUInt64(code, loadLibraryRemote);
  AppendByte(code, 0xff);
  AppendByte(code, 0xd0); // call rax
  AppendByte(code, 0x48);
  AppendByte(code, 0xb8); // mov rax, remoteFlag
  size_t callFlagImmediate = code.size();
  AppendUInt64(code, 0);
  AppendByte(code, 0xc6);
  AppendByte(code, 0x00);
  AppendByte(code, 0x02); // mov byte ptr [rax], 2
  AppendByte(code, 0x48);
  AppendByte(code, 0x83);
  AppendByte(code, 0xc4);
  AppendByte(code, 0x28);
  for(uint8_t reg = 0x5f; reg >= 0x58; reg--)
  {
    AppendByte(code, 0x41);
    AppendByte(code, reg); // pop r15..r8
  }
  AppendByte(code, 0x5f); // pop rdi
  AppendByte(code, 0x5e); // pop rsi
  AppendByte(code, 0x5d); // pop rbp
  AppendByte(code, 0x5a); // pop rdx
  AppendByte(code, 0x59); // pop rcx
  AppendByte(code, 0x5b); // pop rbx
  AppendByte(code, 0x9d); // popfq
  AppendByte(code, 0x58); // pop rax
  AppendByte(code, 0x48);
  AppendByte(code, 0xb8); // mov rax, remoteFlag
  size_t flagImmediate = code.size();
  AppendUInt64(code, 0);
  AppendByte(code, 0xc6);
  AppendByte(code, 0x00);
  AppendByte(code, 0x01); // mov byte ptr [rax], 1
#else
  // The x86 Windows ABI uses stdcall for LoadLibraryExW, so the callee removes its three
  // arguments.
  // The host restores the complete original CONTEXT after the trampoline reports its result.
  size_t entryFlagImmediate = code.size();
  AppendByte(code, 0xba); // mov edx, remoteFlag
  AppendUInt32(code, 0);
  AppendByte(code, 0xc6);
  AppendByte(code, 0x02);
  AppendByte(code, 0x03); // mov byte ptr [edx], 3 (trampoline entered)
  size_t getLastErrorImmediate = 0;
  size_t failureErrorImmediate = 0;
  AppendByte(code, 0x83);
  AppendByte(code, 0xec);
  AppendByte(code, 0x10); // reserve stack space below the original ESP
  AppendByte(code, 0x68); // push LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
  AppendByte(code, 0x00);
  AppendByte(code, 0x11);
  AppendByte(code, 0x00);
  AppendByte(code, 0x00);
  AppendByte(code, 0x6a); // push hFile = NULL
  AppendByte(code, 0x00);
  AppendByte(code, 0x68); // push remotePath
  size_t pathImmediate = code.size();
  AppendUInt32(code, 0);
  AppendByte(code, 0xb8); // mov eax, LoadLibraryExW
  size_t loadLibraryImmediate = code.size();
  AppendUInt32(code, 0);
  AppendByte(code, 0xff);
  AppendByte(code, 0xd0); // call eax
  AppendByte(code, 0x83);
  AppendByte(code, 0xc4);
  AppendByte(code, 0x10); // release the reserved stack space
  AppendByte(code, 0x85);
  AppendByte(code, 0xc0); // test eax, eax
  AppendByte(code, 0x74);
  AppendByte(code, 0x0a); // jump over the success marker when LoadLibraryExW failed
  AppendByte(code, 0xba); // mov edx, remoteFlag
  size_t successFlagImmediate = code.size();
  AppendUInt32(code, 0);
  AppendByte(code, 0xc6);
  AppendByte(code, 0x02);
  AppendByte(code, 0x01); // mov byte ptr [edx], 1
  AppendByte(code, 0xeb);
  AppendByte(code, 0x16); // skip the failure error capture and marker
  AppendByte(code, 0xb8); // mov eax, GetLastError
  getLastErrorImmediate = code.size();
  AppendUInt32(code, 0);
  AppendByte(code, 0xff);
  AppendByte(code, 0xd0); // call eax
  AppendByte(code, 0xba); // mov edx, remoteError
  failureErrorImmediate = code.size();
  AppendUInt32(code, 0);
  AppendByte(code, 0x89);
  AppendByte(code, 0x02); // mov [edx], eax
  AppendByte(code, 0xba); // mov edx, remoteFlag
  size_t failureFlagImmediate = code.size();
  AppendUInt32(code, 0);
  AppendByte(code, 0xc6);
  AppendByte(code, 0x02);
  AppendByte(code, 0x02); // mov byte ptr [edx], 2
#endif

  AppendByte(code, 0xeb);
  AppendByte(code, 0xfe); // spin until the host restores the original context

#if defined(_WIN64)
  // Run a tiny call stub on the target thread's existing stack. The host restores the complete
  // original CONTEXT after the stub reports the LoadLibraryExW result.
  bool directLoadLibrary = true;
  size_t directPathImmediate = 0;
  size_t directEntryFlagImmediate = 0;
  size_t directFlagImmediate = 0;
  size_t directSuccessFlagImmediate = 0;
  size_t directGetLastErrorImmediate = 0;
  size_t directErrorImmediate = 0;
  if(directLoadLibrary)
  {
    code.clear();
    AppendByte(code, 0x48);
    AppendByte(code, 0x83);
    AppendByte(code, 0xe4);
    AppendByte(code, 0xf0); // and rsp, -16 (keep the ABI stack alignment)
    AppendByte(code, 0x48);
    AppendByte(code, 0xb8); // mov rax, remoteFlag
    directEntryFlagImmediate = code.size();
    AppendUInt64(code, 0);
    AppendByte(code, 0xc6);
    AppendByte(code, 0x00);
    AppendByte(code, 0x03); // mov byte ptr [rax], 3 (trampoline entered)
    AppendByte(code, 0x48);
    AppendByte(code, 0x83);
    AppendByte(code, 0xec);
    AppendByte(code, 0x20); // reserve the x64 shadow space on the target thread's stack
    AppendByte(code, 0x48);
    AppendByte(code, 0xb9); // mov rcx, remotePath
    directPathImmediate = code.size();
    AppendUInt64(code, 0);
    AppendByte(code, 0xba); // mov edx, NULL hFile
    AppendByte(code, 0x00);
    AppendByte(code, 0x00);
    AppendByte(code, 0x00);
    AppendByte(code, 0x00);
    AppendByte(code, 0x41);
    AppendByte(code, 0xb8); // mov r8d, DLL directory + default search flags
    AppendByte(code, 0x00);
    AppendByte(code, 0x11);
    AppendByte(code, 0x00);
    AppendByte(code, 0x00);
    AppendByte(code, 0x48);
    AppendByte(code, 0xb8); // mov rax, LoadLibraryExW
    AppendUInt64(code, loadLibraryRemote);
    AppendByte(code, 0xff);
    AppendByte(code, 0xd0); // call rax
    AppendByte(code, 0x48);
    AppendByte(code, 0x85);
    AppendByte(code, 0xc0); // test rax, rax (LoadLibraryExW result)
    AppendByte(code, 0x74);
    AppendByte(code, 0x0f); // jump over the success marker when LoadLibraryExW failed
    AppendByte(code, 0x48);
    AppendByte(code, 0xb8); // mov rax, remoteFlag
    directFlagImmediate = code.size();
    AppendUInt64(code, 0);
    AppendByte(code, 0xc6);
    AppendByte(code, 0x00);
    AppendByte(code, 0x01); // mov byte ptr [rax], 1 (LoadLibraryExW succeeded)
    AppendByte(code, 0xeb);
    AppendByte(code, 0x25); // skip the failure marker and remote error capture
    AppendByte(code, 0x48);
    AppendByte(code, 0xb8); // mov rax, GetLastError
    directGetLastErrorImmediate = code.size();
    AppendUInt64(code, 0);
    AppendByte(code, 0xff);
    AppendByte(code, 0xd0); // call rax
    AppendByte(code, 0x48);
    AppendByte(code, 0xb9); // mov rcx, remoteError
    directErrorImmediate = code.size();
    AppendUInt64(code, 0);
    AppendByte(code, 0x89);
    AppendByte(code, 0x01); // mov [rcx], eax
    AppendByte(code, 0x48);
    AppendByte(code, 0xb8); // mov rax, remoteFlag
    directSuccessFlagImmediate = code.size();
    AppendUInt64(code, 0);
    AppendByte(code, 0xc6);
    AppendByte(code, 0x00);
    AppendByte(code, 0x02); // mov byte ptr [rax], 2 (LoadLibraryExW failed)
    AppendByte(code, 0xeb);
    AppendByte(code, 0xfe); // spin until the host restores the original context

  }
#endif

  void *remoteCode = VirtualAllocEx(hProcess, NULL, code.size(), MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
  bool success = remotePath != NULL && remoteFlag != NULL && remoteError != NULL && remoteCode != NULL;
  const char *failureStage = NULL;
  if(!success)
    failureStage = "allocating remote injection memory";

  SIZE_T written = 0;
  if(success)
  {
    success = WriteProcessMemory(hProcess, remotePath, libName.c_str(), pathSize, &written) &&
              written == pathSize;
    if(!success)
      failureStage = "writing DLL path";
  }

  if(success)
  {
#if defined(_WIN64)
    uint64_t flagAddress = (uint64_t)(uintptr_t)remoteFlag;
    if(directLoadLibrary)
    {
      uint64_t pathAddress = (uint64_t)(uintptr_t)remotePath;
      uint64_t errorFunctionAddress = (uint64_t)getLastErrorRemote;
      uint64_t errorAddress = (uint64_t)(uintptr_t)remoteError;
      memcpy(&code[directPathImmediate], &pathAddress, sizeof(pathAddress));
      memcpy(&code[directEntryFlagImmediate], &flagAddress, sizeof(flagAddress));
      memcpy(&code[directFlagImmediate], &flagAddress, sizeof(flagAddress));
      memcpy(&code[directGetLastErrorImmediate], &errorFunctionAddress,
             sizeof(errorFunctionAddress));
      memcpy(&code[directErrorImmediate], &errorAddress, sizeof(errorAddress));
      memcpy(&code[directSuccessFlagImmediate], &flagAddress, sizeof(flagAddress));
    }
    else
    {
      uint64_t pathAddress = (uint64_t)(uintptr_t)remotePath;
      memcpy(&code[pathImmediate], &pathAddress, sizeof(pathAddress));
      memcpy(&code[flagImmediate], &flagAddress, sizeof(flagAddress));
      memcpy(&code[entryFlagImmediate], &flagAddress, sizeof(flagAddress));
      memcpy(&code[callFlagImmediate], &flagAddress, sizeof(flagAddress));
    }
#else
    uint32_t pathAddress = (uint32_t)(uintptr_t)remotePath;
    uint32_t flagAddress = (uint32_t)(uintptr_t)remoteFlag;
    uint32_t errorAddress = (uint32_t)(uintptr_t)remoteError;
    uint32_t loadLibraryAddress = (uint32_t)loadLibraryRemote;
    uint32_t getLastErrorAddress = (uint32_t)getLastErrorRemote;
    memcpy(&code[entryFlagImmediate], &flagAddress, sizeof(flagAddress));
    memcpy(&code[pathImmediate], &pathAddress, sizeof(pathAddress));
    memcpy(&code[loadLibraryImmediate], &loadLibraryAddress, sizeof(loadLibraryAddress));
    memcpy(&code[successFlagImmediate], &flagAddress, sizeof(flagAddress));
    memcpy(&code[failureFlagImmediate], &flagAddress, sizeof(flagAddress));
    memcpy(&code[getLastErrorImmediate], &getLastErrorAddress, sizeof(getLastErrorAddress));
    memcpy(&code[failureErrorImmediate], &errorAddress, sizeof(errorAddress));
#endif
    uint32_t zero = 0;
    success = WriteProcessMemory(hProcess, remoteFlag, &zero, sizeof(zero), &written) &&
              written == sizeof(uint32_t) &&
              WriteProcessMemory(hProcess, remoteError, &zero, sizeof(zero), &written) &&
              written == sizeof(uint32_t) &&
              WriteProcessMemory(hProcess, remoteCode, code.data(), code.size(), &written) &&
              written == code.size() && FlushInstructionCache(hProcess, remoteCode, code.size());
    if(!success)
      failureStage = "writing trampoline";
  }

  CONTEXT injectedContext = savedContext;
#if defined(_WIN64)
  injectedContext.Rip = (DWORD64)(uintptr_t)remoteCode;
  injectedContext.Rsp = savedContext.Rsp;
#else
  WOW64_CONTEXT injectedWow64Context = savedWow64Context;
  if(useWow64Context)
  {
    injectedWow64Context.Eip = (DWORD)(uintptr_t)remoteCode;
    injectedWow64Context.Esp = savedWow64Context.Esp;
  }
  else
  {
    injectedContext.Eip = (DWORD)(uintptr_t)remoteCode;
    injectedContext.Esp = savedContext.Esp;
  }
#endif

  if(success)
  {
#if defined(_WIN64)
    success = SetThreadContext(thread, &injectedContext) != FALSE;
#else
    success = useWow64Context ? Wow64SetThreadContext(thread, &injectedWow64Context)
                              : SetThreadContext(thread, &injectedContext);
#endif
    if(!success)
      failureStage = "setting target thread context";
    else
    {
      CONTEXT verifyContext;
      RDCEraseEl(verifyContext);
#if !defined(_WIN64)
      WOW64_CONTEXT verifyWow64Context;
      RDCEraseEl(verifyWow64Context);
      verifyContext.ContextFlags = CONTEXT_CONTROL;
      verifyWow64Context.ContextFlags = WOW64_CONTEXT_CONTROL;
      BOOL verified = useWow64Context ? Wow64GetThreadContext(thread, &verifyWow64Context)
                                      : GetThreadContext(thread, &verifyContext);
#else
      verifyContext.ContextFlags = CONTEXT_CONTROL;
      BOOL verified = GetThreadContext(thread, &verifyContext);
#endif
      if(!verified)
      {
        success = false;
        failureStage = "verifying target thread context";
      }
#if defined(_WIN64)
      else if(verifyContext.Rip != injectedContext.Rip)
#elif defined(_WIN32)
      else if(useWow64Context ? (verifyWow64Context.Eip != injectedWow64Context.Eip)
                              : (verifyContext.Eip != injectedContext.Eip))
#else
      else if(verifyContext.Eip != injectedContext.Eip)
#endif
      {
        success = false;
        failureStage = "target thread context was not updated";
      }
    }
  }

  if(success)
  {
    if(!adjustSuspendCount(0))
    {
      success = false;
      failureStage = "resuming target thread";
    }
  }

  bool completed = false;
  uint32_t observedFlag = 0;
  uint32_t observedError = 0;
  DWORD observedProcessExit = STILL_ACTIVE;
  for(uint32_t retry = 0; success && retry < 3000; retry++)
  {
    uint32_t flag = 0;
    SIZE_T read = 0;
    if(ReadProcessMemory(hProcess, remoteFlag, &flag, sizeof(flag), &read) && read == sizeof(flag) &&
       (flag == 1 || flag == 2))
    {
      observedFlag = flag;
      if(observedFlag == 2)
      {
        SIZE_T errorRead = 0;
        ReadProcessMemory(hProcess, remoteError, &observedError, sizeof(observedError), &errorRead);
      }
      completed = true;
      break;
    }

    if(ReadProcessMemory(hProcess, remoteFlag, &flag, sizeof(flag), &read) && flag == 3)
      observedFlag = 3;

    if(GetExitCodeProcess(hProcess, &observedProcessExit) &&
       observedProcessExit != STILL_ACTIVE)
    {
      failureStage = "target process exited before LoadLibraryExW trampoline completed";
      break;
    }
    Sleep(10);
  }
  bool restored = true;
  if(currentSuspendCount == 0)
  {
    restored = adjustSuspendCount(1);
  }

  if(restored)
  {
    SIZE_T stackBytesWritten = 0;
    restored = WriteProcessMemory(hProcess, stackBackupAddress, stackBackup.data(),
                                  stackBackupSize, &stackBytesWritten) &&
               stackBytesWritten == stackBackupSize;
  }

  if(restored)
  {
#if defined(_WIN64)
    restored = SetThreadContext(thread, &savedContext) != FALSE;
#else
    restored = useWow64Context ? Wow64SetThreadContext(thread, &savedWow64Context)
                              : SetThreadContext(thread, &savedContext);
#endif
    if(restored)
      restored = adjustSuspendCount(originalSuspendCount);
  }

  if(success && !completed)
  {
    SIZE_T read = 0;
    ReadProcessMemory(hProcess, remoteFlag, &observedFlag, sizeof(observedFlag), &read);
    if(observedFlag == 3)
      failureStage = "LoadLibraryExW trampoline entered but did not return";
    else if(observedProcessExit != STILL_ACTIVE)
      failureStage = "target process exited before LoadLibraryExW trampoline completed";
    else
      failureStage = "waiting for LoadLibraryExW trampoline";
  }
  else if(completed && observedFlag == 2)
  {
    failureStage = "LoadLibraryExW returned NULL";
  }
  if(!restored)
    failureStage = "restoring target thread context";
  success = success && completed && restored;
  if(success && observedFlag == 2)
  {
    success = false;
    failureStage = "LoadLibraryExW returned NULL";
  }
  if(success && observedFlag == 1)
  {
    rdcstr loadedName = get_basename(StringFormat::Wide2UTF8(libName));
    if(FindRemoteDLL(pid, loadedName) == 0)
    {
      success = false;
      failureStage = "LoadLibraryExW reported success but the module was not found";
    }
  }

  if(!success && failureReason)
  {
    DWORD error = observedFlag == 2 ? observedError : GetLastError();
    *failureReason = StringFormat::Fmt("%s for '%s' (status %u, error %u)",
                                       failureStage ? failureStage : "unknown",
                                       StringFormat::Wide2UTF8(libName).c_str(), observedFlag, error);
  }

  if(remoteCode)
    VirtualFreeEx(hProcess, remoteCode, 0, MEM_RELEASE);
  if(remoteFlag)
    VirtualFreeEx(hProcess, remoteFlag, 0, MEM_RELEASE);
  if(remoteError)
    VirtualFreeEx(hProcess, remoteError, 0, MEM_RELEASE);
  if(remotePath)
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
  CloseHandle(thread);
  return success && completed;
}

uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  rdcwstr wlibName = StringFormat::UTF82Wide(strlower(libName));

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      RDCWARN("CreateToolhelp32Snapshot(%u) -> 0x%08x", pid, err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't create toolhelp dump of modules in process %u", pid);
    return 0;
  }

  MODULEENTRY32 me32;
  RDCEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    RDCERR("Couldn't get first module in process %u: 0x%08x", pid, err);
    CloseHandle(hModuleSnap);
    return 0;
  }

  uintptr_t ret = 0;

  int numModules = 0;

  do
  {
    wchar_t modnameLower[MAX_MODULE_NAME32 + 1];
    RDCEraseEl(modnameLower);
    wcsncpy_s(modnameLower, me32.szModule, MAX_MODULE_NAME32);

    wchar_t *wc = &modnameLower[0];
    while(*wc)
    {
      *wc = towlower(*wc);
      wc++;
    }

    numModules++;

    if(wcsstr(modnameLower, wlibName.c_str()) == modnameLower)
    {
      ret = (uintptr_t)me32.modBaseAddr;
    }
  } while(ret == 0 && Module32Next(hModuleSnap, &me32));

  if(ret == 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);

    DWORD exitCode = 0;

    if(h)
      GetExitCodeProcess(h, &exitCode);

    if(h == NULL || exitCode != STILL_ACTIVE)
    {
      RDCERR(
          "Error injecting into remote process with PID %u which is no longer available.\n"
          "Possibly the process has crashed during early startup, or is missing DLLs to run?",
          pid);
    }
    else
    {
      RDCERR("Couldn't find module '%s' among %d modules", libName.c_str(), numModules);
    }

    if(h)
      CloseHandle(h);
  }

  CloseHandle(hModuleSnap);

  return ret;
}

void InjectFunctionCall(HANDLE hProcess, uintptr_t renderdoc_remote, const char *funcName,
                        void *data, const size_t dataLen)
{
  if(dataLen == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll");

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  // we've found SetCaptureOptions in our local instance of the module, now calculate the offset and
  // so get the function
  // in the remote module (which might be loaded at a different base address
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

  void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  SIZE_T numWritten;
  WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  HANDLE hThread =
      CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)func_remote, remoteMem, 0, NULL);
  WaitForSingleObject(hThread, INFINITE);

  ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  CloseHandle(hThread);
  VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
}

static PROCESS_INFORMATION RunProcess(const rdcstr &app, const rdcstr &workingDir,
                                      const rdcstr &cmdLine,
                                      const rdcarray<EnvironmentModification> &env, bool internal,
                                      HANDLE *phChildStdOutput_Rd, HANDLE *phChildStdError_Rd)
{
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  SECURITY_ATTRIBUTES pSec;
  SECURITY_ATTRIBUTES tSec;

  RDCEraseEl(pi);
  RDCEraseEl(si);
  RDCEraseEl(pSec);
  RDCEraseEl(tSec);

  si.cb = sizeof(si);

  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  rdcwstr workdir = L"";

  if(!workingDir.empty())
    workdir = StringFormat::UTF82Wide(workingDir);
  else
    workdir = StringFormat::UTF82Wide(get_dirname(app));

  wchar_t *paramsAlloc = NULL;

  rdcwstr wapp = StringFormat::UTF82Wide(app);

  // CreateProcessW can modify the params, need space.
  size_t len = wapp.length() + 10;

  rdcwstr wcmd = L"";

  if(!cmdLine.empty())
  {
    wcmd = StringFormat::UTF82Wide(cmdLine);
    len += wcmd.length();
  }

  paramsAlloc = new wchar_t[len];

  RDCEraseMem(paramsAlloc, len * sizeof(wchar_t));

  wcscpy_s(paramsAlloc, len, L"\"");
  wcscat_s(paramsAlloc, len, wapp.c_str());
  wcscat_s(paramsAlloc, len, L"\"");

  if(!cmdLine.empty())
  {
    wcscat_s(paramsAlloc, len, L" ");
    wcscat_s(paramsAlloc, len, wcmd.c_str());
  }

  bool inheritHandles = false;

  HANDLE hChildStdOutput_Wr = 0, hChildStdError_Wr = 0;
  if(phChildStdOutput_Rd)
  {
    RDCASSERT(phChildStdError_Rd);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if(!CreatePipe(phChildStdOutput_Rd, &hChildStdOutput_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdOutput_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    if(!CreatePipe(phChildStdError_Rd, &hChildStdError_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdError_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hChildStdOutput_Wr;
    si.hStdError = hChildStdError_Wr;

    // Need to inherit handles in CreateProcess for ReadFile to read stdout
    inheritHandles = true;
  }

  // if it's a utility launch, hide the command prompt window from showing
  if(phChildStdOutput_Rd || internal)
    si.dwFlags |= STARTF_USESHOWWINDOW;

  if(!internal)
    RDCLOG("Running process %s", app.c_str());

  // turn environment string to a UTF-8 map
  std::wstring envString;

  if(!env.empty())
  {
    LPWCH envStrings = GetEnvironmentStringsW();
    EnvMap envValues = EnvStringToEnvMap(envStrings);
    FreeEnvironmentStringsW(envStrings);

    ApplyEnvModifications(envValues, env, false);

    for(auto it = envValues.begin(); it != envValues.end(); ++it)
    {
      envString += StringFormat::UTF82Wide(it->first).c_str();
      envString += L"=";
      envString += StringFormat::UTF82Wide(it->second).c_str();
      envString.push_back(0);
    }
  }

  BOOL retValue = CreateProcessW(
      NULL, paramsAlloc, &pSec, &tSec, inheritHandles, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      envString.empty() ? NULL : (void *)envString.data(), workdir.c_str(), &si, &pi);

  DWORD err = GetLastError();

  if(phChildStdOutput_Rd)
  {
    CloseHandle(hChildStdOutput_Wr);
    CloseHandle(hChildStdError_Wr);
  }

  SAFE_DELETE_ARRAY(paramsAlloc);

  if(!retValue)
  {
    if(!internal)
      RDCWARN("Process %s could not be loaded (error %d).", app.c_str(), err);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    RDCEraseEl(pi);
    // Preserve the CreateProcessW failure for the caller.  The cleanup above can overwrite the
    // thread-local last-error value, which previously reduced the UI message to a generic failure.
    SetLastError(err);
  }

  return pi;
}

rdcpair<RDResult, uint32_t> Process::InjectIntoProcess(uint32_t pid,
                                                       const rdcarray<EnvironmentModification> &env,
                                                       const rdcstr &capturefile,
                                                       const CaptureOptions &opts, bool waitForExit)
{
  rdcwstr wcapturefile = StringFormat::UTF82Wide(capturefile);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                  FALSE, pid);

  if(opts.delayForDebugger > 0)
  {
    RDCDEBUG("Waiting for debugger attach to %lu", pid);
    uint32_t timeout = 0;

    BOOL debuggerAttached = FALSE;

    while(!debuggerAttached)
    {
      CheckRemoteDebuggerPresent(hProcess, &debuggerAttached);

      Sleep(10);
      timeout += 10;

      if(timeout > opts.delayForDebugger * 1000)
        break;
    }

    if(debuggerAttached)
      RDCDEBUG("Debugger attach detected after %.2f s", float(timeout) / 1000.0f);
    else
      RDCDEBUG("Timed out waiting for debugger, gave up after %u s", opts.delayForDebugger);
  }

  RDCLOG("Injecting renderdoc into process %lu", pid);

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), &renderdocPath[0],
                                      MAX_PATH - 1);

  wchar_t renderdocPathLower[MAX_PATH] = {0};
  memcpy(renderdocPathLower, renderdocPath, MAX_PATH * sizeof(wchar_t));
  for(size_t i = 0; i < MAX_PATH && renderdocPathLower[i]; i++)
  {
    // lowercase
    if(renderdocPathLower[i] >= 'A' && renderdocPathLower[i] <= 'Z')
      renderdocPathLower[i] = 'a' + char(renderdocPathLower[i] - 'A');

    // normalise paths
    if(renderdocPathLower[i] == '/')
      renderdocPathLower[i] = '\\';
  }

  BOOL isWow64 = FALSE;
  BOOL success = IsWow64Process(hProcess, &isWow64);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of process, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  bool capalt = false;

#if DISABLED(RDOC_X64)
  BOOL selfWow64 = FALSE;

  HANDLE hSelfProcess = GetCurrentProcess();

  // check to see if we're a WoW64 process
  success = IsWow64Process(hSelfProcess, &selfWow64);

  CloseHandle(hSelfProcess);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of self, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  // we know we're 32-bit, so if the target process is not wow64
  // and we are, it's 64-bit. If we're both not wow64 then we're
  // running on 32-bit windows, and if we're both wow64 then we're
  // both 32-bit on 64-bit windows.
  //
  // We don't support capturing 64-bit programs from a 32-bit install
  // because it's pointless - a 64-bit install will work for all in
  // that case. But we do want to handle the case of:
  // 64-bit renderdoc -> 32-bit program (via 32-bit helpmecmd)
  //    -> 64-bit program (going back to 64-bit renderdoccmd).
  // so we try to see if we're an x86 invoked renderdoccmd in an
  // otherwise 64-bit install, and 'promote' back to 64-bit.
  if(selfWow64 && !isWow64)
  {
    wchar_t *slash = wcsrchr(renderdocPath, L'\\');

    if(slash && slash > renderdocPath + 4)
    {
      slash -= 4;

      if(slash && !wcsncmp(slash, L"\\x86", 4))
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    if(!capalt)
    {
      const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
      if(!devLocation)
        devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if we couldn't promote, then bail out.
    if(!capalt)
    {
      RDCDEBUG("Running from %ls", renderdocPathLower);

      CloseHandle(hProcess);
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                       "Can't capture 64-bit program with 32-bit build of HelpMe. Please run a "
                       "64-bit build of HelpMe");
      return {result, 0};
    }
  }
#else
  // farm off to alternate bitness helpmecmd.exe

  // if the target process is 'wow64' that means it's 32-bit.
  capalt = (isWow64 == TRUE);
#endif

  if(capalt)
  {
#if ENABLED(RDOC_X64)
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\x64\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\Win32\\Development\\helpmecmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\x64\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\Win32\\Release\\helpmecmd.exe");
      }
    }

    if(!devLocation)
    {
      // look in a subfolder for x86.

      // remove the filename from the path
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\x86\\helpmecmd.exe");
    }
#else
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\x64\\Development\\helpmecmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\x64\\Release\\helpmecmd.exe");
      }
    }

    if(!devLocation)
    {
      // look upwards on 32-bit to find the parent renderdoccmd.
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      // remove the filename
      if(slash)
        *slash = 0;

      // remove the \\x86
      slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\helpmecmd.exe");
    }
#endif

    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES pSec;
    SECURITY_ATTRIBUTES tSec;

    RDCEraseEl(pi);
    RDCEraseEl(si);
    RDCEraseEl(pSec);
    RDCEraseEl(tSec);

    // hide the console window
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    pSec.nLength = sizeof(pSec);
    tSec.nLength = sizeof(tSec);

    // serialise to string with two chars per byte
    rdcstr optstr = opts.EncodeAsString();

    wchar_t *paramsAlloc = new wchar_t[2048];

    rdcstr debugLogfile = RDCGETLOGFILE();
    rdcwstr wdebugLogfile = StringFormat::UTF82Wide(debugLogfile);

    _snwprintf_s(
        paramsAlloc, 2047, 2047,
        L"\"%ls\" capaltbit --pid=%u --capfile=\"%ls\" --debuglog=\"%ls\" --capopts=\"%hs\"",
        renderdocPath, pid, wcapturefile.c_str(), wdebugLogfile.c_str(), optstr.c_str());

    RDCDEBUG("params %ls", paramsAlloc);

    paramsAlloc[2047] = 0;

    wchar_t *commandLine = paramsAlloc;

    std::wstring cmdWithEnv;

    if(!env.empty())
    {
      cmdWithEnv = paramsAlloc;

      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;

        if(name == "")
          break;

        cmdWithEnv += L" +env-";
        switch(e.mod)
        {
          case EnvMod::Set: cmdWithEnv += L"replace"; break;
          case EnvMod::Append: cmdWithEnv += L"append"; break;
          case EnvMod::Prepend: cmdWithEnv += L"prepend"; break;
        }

        if(e.mod != EnvMod::Set)
        {
          switch(e.sep)
          {
            case EnvSep::Platform: cmdWithEnv += L"-platform"; break;
            case EnvSep::SemiColon: cmdWithEnv += L"-semicolon"; break;
            case EnvSep::Colon: cmdWithEnv += L"-colon"; break;
            case EnvSep::NoSep: break;
          }
        }

        cmdWithEnv += L" ";

        // escape the parameters
        for(size_t it = 0; it < name.size(); it++)
        {
          if(name[it] == '"')
          {
            name.insert(it, '\\');
            it++;
          }
        }

        for(size_t it = 0; it < value.size(); it++)
        {
          if(value[it] == '"')
          {
            value.insert(it, '\\');
            it++;
          }
        }

        if(name.back() == '\\')
          name += "\\";

        if(value.back() == '\\')
          value += "\\";

        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(name).c_str()) + L"\" ";
        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(value).c_str()) + L"\" ";
      }

      commandLine = (wchar_t *)cmdWithEnv.c_str();
    }

    BOOL retValue = CreateProcessW(NULL, commandLine, &pSec, &tSec, false,
                                   CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, NULL, &si, &pi);

    SAFE_DELETE_ARRAY(paramsAlloc);

    if(!retValue)
    {
      RDResult result;
#if RENDERDOC_OFFICIAL_BUILD
      SET_ERROR_RESULT(result, ResultCode::InternalError,
                       "Can't run 32-bit helpmecmd to capture 32-bit program.");
#else
      SET_ERROR_RESULT(
          result, ResultCode::InternalError,
          "Can't run 32-bit helpmecmd to capture 32-bit program."
          "If this is a locally built HelpMe you must build both 32-bit and 64-bit versions.");
#endif
      CloseHandle(hProcess);
      return {result, 0};
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if(waitForExit)
      WaitForSingleObject(hProcess, INFINITE);

    CloseHandle(hProcess);

    if(exitCode == 0)
    {
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::UnknownError,
                       "Encountered error while launching target 32-bit program.");
      return {result, 0};
    }

    if(exitCode < RenderDoc_FirstTargetControlPort)
    {
      ResultCode code = (ResultCode)exitCode;

      RDResult result;
      SET_ERROR_RESULT(result, code, "32-bit helpmecmd returned '%s'", ToStr(code).c_str());
      return {code, 0};
    }

    return {ResultCode::Succeeded, (uint32_t)exitCode};
  }

  rdcstr injectionFailure;
  if(!InjectDLLWithThreadContext(pid, hProcess, renderdocPath, &injectionFailure))
  {
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed,
                     "Failed to inject %s.dll into process using SetThreadContext: %s",
                     STRINGIZE(RDOC_BASE_NAME), injectionFailure.c_str());
    CloseHandle(hProcess);
    return {result, 0};
  }

  const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);

  uintptr_t loc = 0;
  for(uint32_t retry = 0; retry < 20 && loc == 0; retry++)
  {
    loc = FindRemoteDLL(pid, STRINGIZE(RDOC_BASE_NAME) ".dll");
    if(loc == 0)
      Sleep(50);
  }

  rdcpair<RDResult, uint32_t> result = {ResultCode::Succeeded, 0};

  if(loc == 0)
  {
    SET_ERROR_RESULT(
        result.first, ResultCode::InjectionFailed,
        "Failed to inject %s.dll into process. Check that the process did not crash or exit "
        "early in initialisation, e.g. if the working directory is incorrectly set.",
        rdoc_dll);
  }
  else
  {
    // safe to cast away the const as we know these functions don't modify the parameters

    if(!capturefile.empty())
      InjectFunctionCall(hProcess, loc, "INTERNAL_SetCaptureFile", (void *)capturefile.c_str(),
                         capturefile.size() + 1);

    rdcstr debugLogfile = RDCGETLOGFILE();

    InjectFunctionCall(hProcess, loc, "INTERNAL_SetDebugLogFile", (void *)debugLogfile.c_str(),
                       debugLogfile.size() + 1);

    InjectFunctionCall(hProcess, loc, "INTERNAL_SetCaptureOptions", (CaptureOptions *)&opts,
                       sizeof(CaptureOptions));

    InjectFunctionCall(hProcess, loc, "INTERNAL_GetTargetControlIdent", &result.second,
                       sizeof(result.second));

    if(!env.empty())
    {
      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;
        EnvMod mod = e.mod;
        EnvSep sep = e.sep;

        if(name == "")
          break;

        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModName", (void *)name.c_str(),
                           name.size() + 1);
        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModValue", (void *)value.c_str(),
                           value.size() + 1);
        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvSep", &sep, sizeof(sep));
        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvMod", &mod, sizeof(mod));
      }

      // parameter is unused
      void *dummy = NULL;
      InjectFunctionCall(hProcess, loc, "INTERNAL_ApplyEnvMods", &dummy, sizeof(dummy));
    }
  }

  if(waitForExit)
    WaitForSingleObject(hProcess, INFINITE);

  CloseHandle(hProcess);

  return result;
}

uint32_t Process::LaunchProcess(const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
                                bool internal, ProcessResult *result)
{
  HANDLE hChildStdOutput_Rd = NULL, hChildStdError_Rd = NULL;

  rdcstr appPath = app;
  size_t len = appPath.length();
  rdcstr ext;
  if(len > 4)
    ext = strlower(appPath.substr(len - 4));
  if(ext != ".exe")
    appPath += ".exe";

  PROCESS_INFORMATION pi =
      RunProcess(appPath, workingDir, cmdLine, {}, internal, result ? &hChildStdOutput_Rd : NULL,
                 result ? &hChildStdError_Rd : NULL);

  if(pi.dwProcessId == 0)
  {
    if(!internal)
      RDCWARN("Couldn't launch process '%s'", appPath.c_str());

    if(hChildStdError_Rd != NULL)
      CloseHandle(hChildStdError_Rd);
    if(hChildStdOutput_Rd != NULL)
      CloseHandle(hChildStdOutput_Rd);

    return 0;
  }

  if(!internal)
    RDCLOG("Launched process '%s' with '%s'", appPath.c_str(), cmdLine.c_str());

  ResumeThread(pi.hThread);

  if(result)
  {
    result->strStdout = "";
    result->strStderror = "";

    char chBuf[4096];
    DWORD dwOutputRead, dwErrorRead;
    BOOL success = FALSE;
    rdcstr s;
    for(;;)
    {
      success = ReadFile(hChildStdOutput_Rd, chBuf, sizeof(chBuf), &dwOutputRead, NULL);
      s = rdcstr(chBuf, dwOutputRead);
      result->strStdout += s;

      if(!success && !dwOutputRead)
        break;
    }

    for(;;)
    {
      success = ReadFile(hChildStdError_Rd, chBuf, sizeof(chBuf), &dwErrorRead, NULL);
      s = rdcstr(chBuf, dwErrorRead);
      result->strStderror += s;

      if(!success && !dwErrorRead)
        break;
    }

    CloseHandle(hChildStdOutput_Rd);
    CloseHandle(hChildStdError_Rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, (LPDWORD)&result->retCode);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return pi.dwProcessId;
}

uint32_t Process::LaunchScript(const rdcstr &script, const rdcstr &workingDir,
                               const rdcstr &argList, bool internal, ProcessResult *result)
{
  // Change parameters to invoke command interpreter
  rdcstr args = "/C " + script + " " + argList;

  return LaunchProcess("cmd.exe", workingDir, args, internal, result);
}

rdcpair<RDResult, uint32_t> Process::LaunchAndInjectIntoProcess(
    const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
    const rdcarray<EnvironmentModification> &env, const rdcstr &capturefile,
    const CaptureOptions &opts, bool waitForExit)
{
  void *func =
      GetProcAddress(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), "INTERNAL_SetCaptureFile");

  if(func == NULL)
  {
    const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InternalError,
                     "Can't find required export function in %s.dll - corrupted/missing file?",
                     rdoc_dll);
    return {result, 0};
  }

  if(get_basename(app) == "explorer.exe" || get_basename(app) == "dllhost.exe")
  {
    RDResult result;
    SET_ERROR_RESULT(
        result, ResultCode::InjectionFailed,
        "For safety reasons HelpMe does not support capturing executables with a "
        "reserved system filename such as '%s'. Please rename your executable to capture.",
        get_basename(app).c_str());
    return {result, 0};
  }

  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine, env, false, NULL, NULL);

  if(pi.dwProcessId == 0)
  {
    RDResult result;
    DWORD err = GetLastError();
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed,
                     "Failed to launch process '%s' (Win32 error %lu).", app.c_str(), err);
    return {result, 0};
  }

  rdcpair<RDResult, uint32_t> ret = InjectIntoProcess(pi.dwProcessId, {}, capturefile, opts, false);

  CloseHandle(pi.hProcess);
  ResumeThread(pi.hThread);
  ResumeThread(pi.hThread);

  // SetThreadContext can reach the target while its first loader initialisation is still in
  // progress.  In that window LoadLibraryExW may enter our DLL and return ERROR_DLL_INIT_FAILED
  // even though the same DLL loads normally once the process has reached user code.  The target
  // has already been resumed above, so give it a short settling period and retry the injection
  // against a normal running thread before reporting the failure to the caller.
  if(ret.second == 0 && ret.first == ResultCode::InjectionFailed)
  {
    DWORD exitCode = STILL_ACTIVE;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pi.dwProcessId);
    bool running = process != NULL &&
                   (GetExitCodeProcess(process, &exitCode) != FALSE) &&
                   exitCode == STILL_ACTIVE;
    if(process)
      CloseHandle(process);

    if(running)
    {
      Sleep(500);
      rdcpair<RDResult, uint32_t> retry =
          InjectIntoProcess(pi.dwProcessId, {}, capturefile, opts, false);
      // Keep the retry result even when it fails, since it describes the now-running target and
      // is more useful than the initial loader-window error.
      ret = retry;
    }
  }

  if(ret.second == 0 || ret.first != ResultCode::Succeeded)
  {
    CloseHandle(pi.hThread);
    return ret;
  }

  if(waitForExit)
    WaitForSingleObject(pi.hThread, INFINITE);

  CloseHandle(pi.hThread);

  return ret;
}

bool Process::CanGlobalHook()
{
  // all we need is admin rights and it's the caller's responsibility to ensure that.
  return true;
}

// to simplify the below code, rather than splitting by 32-bit/64-bit we split by native and Wow32.
// This means that for 32-bit code (whether it's on 32-bit OS or not) we just have native, and the
// Wow32 stuff is empty/unused. For 64-bit we use both. Thus the native registry key is always the
// same path regardless of the bitness we're running as and we don't have to move things around or
// have conditionals all over

struct GlobalHookData
{
  struct
  {
    HANDLE pipe = NULL;
    DWORD appinitEnabled = 0;
    rdcwstr appinitDLLs;
  } dataNative, dataWow32;

  int32_t finished = 0;
  Threading::ThreadHandle pipeThread = 0;
};

// utility function to close the registry keys, print an error, and quit
static RDResult HandleRegError(HKEY keyNative, HKEY keyWow32, LSTATUS ret, const char *msg)
{
  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  RDCLOG("Error with AppInit registry keys - %s (%d)", msg, ret);

  RETURN_ERROR_RESULT(ResultCode::InjectionFailed,
                      "Error updating registry to enable global hook.\n"
                      "Check that HelpMe is correctly running as administrator.");
}

#define REG_CHECK(msg)                                    \
  if(ret != ERROR_SUCCESS)                                \
  {                                                       \
    return HandleRegError(keyNative, keyWow32, ret, msg); \
  }

// function to backup the previous settings for AppInit, then enable it and write our own paths.
RDResult BackupAndChangeRegistry(GlobalHookData &hookdata, const rdcstr &shimpathWow32,
                                 const rdcstr &shimpathNative)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;

  // AppInit_DLLs requires short paths, but short paths can be disabled globally or on a per-volume
  // level. If short paths are disabled we'll get the long path back, we *always* expect the path to
  // get shorter because the shim filename is bigger than 8.3.

  DWORD nativeShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), NULL,
                                            (DWORD)shimpathNative.length());
  if(nativeShortSize == (DWORD)shimpathNative.length() + 1)
  {
    RETURN_ERROR_RESULT(
        ResultCode::FileIOFailed,
        "HelpMe is installed on a volume or system that has short paths disabled.\n"
        "For the global hook, short paths must be enabled where HelpMe is installed.");
  }

  if(!shimpathWow32.empty())
  {
    DWORD wow32ShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), NULL,
                                             (DWORD)shimpathWow32.length());

    if(wow32ShortSize == (DWORD)shimpathWow32.length() + 1)
    {
      RETURN_ERROR_RESULT(
          ResultCode::FileIOFailed,
          "HelpMe is installed on a volume or system that has short paths disabled.\n"
          "For the global hook, short paths must be enabled where HelpMe is installed.");
    }
  }

  // open the native key
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

  // if we are doing Wow32, open that key as well
  if(!shimpathWow32.empty())
  {
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                          0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

    REG_CHECK("Could not open AppInit key");
  }

  const DWORD one = 1;

  // fetch the previous data for LoadAppInit_DLLs and AppInit_DLLs
  DWORD sz = 4;
  ret = RegGetValueA(keyNative, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                     (void *)&hookdata.dataNative.appinitEnabled, &sz);
  REG_CHECK("Could not fetch LoadAppInit_DLLs");

  sz = 0;
  ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
  if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
  {
    hookdata.dataNative.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
    ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                       hookdata.dataNative.appinitDLLs.data(), &sz);
  }
  REG_CHECK("Could not fetch AppInit_DLLs");

  // set DWORD:1 for LoadAppInit_DLLs and convert our path to a short path then set it
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  rdcwstr shortpath(shimpathNative.size());
  GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), shortpath.data(),
                    (DWORD)shortpath.length());

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                       DWORD(shortpath.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we're doing Wow32, repeat the process for those keys
  if(keyWow32)
  {
    sz = 4;
    ret = RegGetValueA(keyWow32, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                       (void *)&hookdata.dataWow32.appinitEnabled, &sz);
    REG_CHECK("Could not fetch LoadAppInit_DLLs");

    sz = 0;
    ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
    if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
    {
      hookdata.dataWow32.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
      ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                         hookdata.dataWow32.appinitDLLs.data(), &sz);
    }
    REG_CHECK("Could not fetch AppInit_DLLs");

    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    shortpath = rdcwstr(shimpathWow32.size());
    GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), shortpath.data(),
                      (DWORD)shortpath.length());

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                         DWORD(shortpath.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }

  std::wstring backup;

  // write a .reg file that contains the previous settings, so that if all else fails the user can
  // manually insert it back into the registry to restore everything.
  backup += L"Windows Registry Editor Version 5.00\n";
  backup += L"\n";
  backup += L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows]\n";
  backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
  backup += (hookdata.dataNative.appinitEnabled ? L"1\n" : L"0\n");
  backup += L"\"AppInit_DLLs\"=\"";
  // we append with the C string so we don't add trailing NULLs into the text.
  backup += hookdata.dataNative.appinitDLLs.c_str();
  backup += L"\"\n";
  if(keyWow32)
  {
    backup += L"\n";
    backup +=
        L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\"
        L"Windows NT\\CurrentVersion\\Windows]\n";
    backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
    backup += (hookdata.dataWow32.appinitEnabled ? L"1\n" : L"0\n");
    backup += L"\"AppInit_DLLs\"=\"";
    backup += hookdata.dataWow32.appinitDLLs.c_str();
    backup += L"\"\n";
  }

  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  keyNative = keyWow32 = NULL;

  // write it to disk but don't fail if we can't, just print it to the log and keep going.
  wchar_t reg_backup[MAX_PATH];
  GetTempPathW(MAX_PATH, reg_backup);
  wcscat_s(reg_backup, L"HelpMe_RestoreGlobalHook.reg");

  FILE *f = NULL;
  _wfopen_s(&f, reg_backup, L"w");
  if(f)
  {
    fputws(backup.c_str(), f);
    fclose(f);
  }
  else
  {
    RDCERR("Error opening registry backup file %ls", reg_backup);
    RDCERR("Backup registry data is:\n\n%ls\n\n", backup.c_str());
  }

  return RDResult();
}

// switch error-handling to print-and-continue, as we can't really do anything about it at this
// point and we want to continue restoring in case only one thing failed.
#undef REG_CHECK
#define REG_CHECK(msg)                                                      \
  if(ret != ERROR_SUCCESS)                                                  \
  {                                                                         \
    HandleRegError(keyNative, keyWow32, ret, "Could not open AppInit key"); \
  }

void RestoreRegistry(const GlobalHookData &hookdata)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

#if ENABLED(RDOC_X64)
  ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0,
                        NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

  REG_CHECK("Could not open AppInit key");
#endif

  // set the native values back to where they were
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD,
                       (const BYTE *)&hookdata.dataNative.appinitEnabled,
                       sizeof(hookdata.dataNative.appinitEnabled));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ,
                       (const BYTE *)hookdata.dataNative.appinitDLLs.c_str(),
                       DWORD(hookdata.dataNative.appinitDLLs.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we opened it, restore the Wow32 values as well
  if(keyWow32)
  {
    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD,
                         (const BYTE *)&hookdata.dataWow32.appinitEnabled,
                         sizeof(hookdata.dataWow32.appinitEnabled));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ,
                         (const BYTE *)hookdata.dataWow32.appinitDLLs.c_str(),
                         DWORD(hookdata.dataWow32.appinitDLLs.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }
}

static GlobalHookData *globalHook = NULL;

// a thread we run in the background just to keep the pipes open and wait until we're ready to stop
// the global hook.
static void GlobalHookThread()
{
  Threading::SetCurrentThreadName("GlobalHookThread");

  // keep looping doing an atomic compare-exchange to check that finished is still 0
  while(Atomic::CmpExch32(&globalHook->finished, 0, 0) == 0)
  {
    // wake every quarter of a second to test again
    Threading::Sleep(250);
  }

  char exitData[32] = "exit";

  // write some data into the pipe and close it. The data is (currently) unimportant, just that it
  // causes the blocking read on the other end to succeed and close the program.
  DWORD dummy = 0;
  if(globalHook->dataNative.pipe)
  {
    WriteFile(globalHook->dataNative.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataNative.pipe);
  }

  if(globalHook->dataWow32.pipe)
  {
    WriteFile(globalHook->dataWow32.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataWow32.pipe);
  }
}

RDResult Process::StartGlobalHook(const rdcstr &pathmatch, const rdcstr &capturefile,
                                  const CaptureOptions &opts)
{
  if(pathmatch.empty())
  {
    RETURN_ERROR_RESULT(ResultCode::InvalidParameter,
                        "Invalid global hook parameter, empty path to match");
  }

  rdcstr renderdocPath;
  FileIO::GetLibraryFilename(renderdocPath);

  renderdocPath = get_dirname(renderdocPath);

  // the native helpmecmd.exe is always next to the dll. Wow32 will be somewhere else
  rdcstr cmdpathNative = renderdocPath + "\\helpmecmd.exe";
  rdcstr cmdpathWow32;

  rdcstr shimpathNative = renderdocPath;
  rdcstr shimpathWow32;

#if ENABLED(RDOC_X64)

  // native shim is just helpmesh64.dll
  shimpathNative = renderdocPath + "\\helpmesh64.dll";

  // if it looks like we're in the development environment, look for the alternate bitness in the
  // corresponding folder
  int devLocation = renderdocPath.find("\\x64\\Development");
  if(devLocation >= 0)
  {
    renderdocPath.erase(devLocation, ~0U);

    shimpathWow32 = renderdocPath + "\\Win32\\Development\\helpmesh32.dll";
    cmdpathWow32 = renderdocPath + "\\Win32\\Development\\helpmecmd.exe";
  }
  else
  {
    devLocation = renderdocPath.find("\\x64\\Release");

    if(devLocation >= 0)
    {
      renderdocPath.erase(devLocation, ~0U);

      shimpathWow32 = renderdocPath + "\\Win32\\Release\\helpmesh32.dll";
      cmdpathWow32 = renderdocPath + "\\Win32\\Release\\helpmecmd.exe";
    }
  }

  // if we're not in the dev environment, assume it's under a x86\ subfolder
  if(devLocation < 0)
  {
    shimpathWow32 = renderdocPath + "\\x86\\helpmesh32.dll";
    cmdpathWow32 = renderdocPath + "\\x86\\helpmecmd.exe";
  }

#else

  // nothing fancy to do here for 32-bit, just point the shim next to our dll.
  shimpathNative = renderdocPath + "\\helpmesh32.dll";

#endif

  GlobalHookData hookdata;

  // try to backup and change the registry settings to start loading our shim dlls. If that fails,
  // we bail out immediately
  RDResult regStatus = BackupAndChangeRegistry(hookdata, shimpathWow32, shimpathNative);
  if(regStatus != ResultCode::Succeeded)
    return regStatus;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  SECURITY_ATTRIBUTES pSec = {0};
  SECURITY_ATTRIBUTES tSec = {0};
  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  si.cb = sizeof(si);

  // serialise to string with two chars per byte
  rdcstr optstr = opts.EncodeAsString();
  rdcstr debugLogfile = RDCGETLOGFILE();

  rdcstr params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathNative.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  rdcwstr paramsAlloc = StringFormat::UTF82Wide(params);

  // we'll be setting stdin
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

  // hide the console window
  si.wShowWindow = SW_HIDE;

  // this is the end of the pipe that the child will inherit and use as stdin
  HANDLE childEnd = NULL;

  DWORD err;

  // create a pipe with the writing end for us, and the reading end as the child process's stdin
  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataNative.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 32-bit stdin pipe (err %u)",
                          err);
    }

    // we don't want the child process to inherit our end
    res = SetHandleInformation(hookdata.dataNative.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 32-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  // launch the process
  BOOL retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE,
                                 NULL, NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore, the child has it
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch helpmecmd from '%s' (err %u)",
                        cmdpathNative.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RDCEraseEl(pi);

// repeat the process for the Wow32 renderdoccmd
#if ENABLED(RDOC_X64)
  params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathWow32.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  paramsAlloc = StringFormat::UTF82Wide(params);

  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataWow32.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 64-bit stdin pipe (err %u)",
                          err);
    }

    res = SetHandleInformation(hookdata.dataWow32.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 64-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE, NULL,
                            NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    CloseHandle(hookdata.dataWow32.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch helpmecmd from '%s' (err %u)",
                        cmdpathWow32.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#endif

  // set static global pointer with our data, and launch the thread
  globalHook = new GlobalHookData;
  *globalHook = hookdata;

  globalHook->pipeThread = Threading::CreateThread(&GlobalHookThread);

  return RDResult();
}

bool Process::IsGlobalHookActive()
{
  return globalHook != NULL;
}
void Process::StopGlobalHook()
{
  if(!globalHook)
    return;

  // set the finished flag and join to the thread so it closes the pipes (and so the child
  // processes)
  Atomic::Inc32(&globalHook->finished);

  Threading::JoinThread(globalHook->pipeThread);
  Threading::CloseThread(globalHook->pipeThread);

  // restore the registry settings from before we started
  RestoreRegistry(*globalHook);

  delete globalHook;
  globalHook = NULL;
}

bool Process::IsModuleLoaded(const rdcstr &module)
{
  return GetModuleHandleA(module.c_str()) != NULL;
}

void *Process::LoadModule(const rdcstr &module)
{
  HMODULE mod = GetModuleHandleA(module.c_str());
  if(mod != NULL)
    return mod;

  return LoadLibraryA(module.c_str());
}

void *Process::GetFunctionAddress(void *module, const rdcstr &function)
{
  if(module == NULL)
    return NULL;

  return (void *)GetProcAddress((HMODULE)module, function.c_str());
}

uint32_t Process::GetCurrentPID()
{
  return (uint32_t)GetCurrentProcessId();
}

void Process::Shutdown()
{
  // nothing to do
}
