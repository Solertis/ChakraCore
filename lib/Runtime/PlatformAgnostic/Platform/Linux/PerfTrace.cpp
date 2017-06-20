//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "Common.h"
#include "Runtime.h"
#include "ChakraPlatform.h"

#include <signal.h>
#include <errno.h>
#include <unistd.h>

using namespace Js;



struct sigaction previousSigusr2;

//
//  Handle the SIGUSR2 signal by setting a flag
//  indicating that we should call WritePerfMap later on
//
void handle_sigusr2(int signum)
{
    PlatformAgnostic::PerfTrace::mapsRequested = true;
}

namespace PlatformAgnostic
{

volatile bool PerfTrace::mapsRequested = false;
  
//
// Registers a signal handler for SIGUSR2 
//
void PerfTrace::Register()
{
    struct sigaction newAction;
    newAction.sa_flags = SA_RESTART;
    newAction.sa_handler = handle_sigusr2;
    sigemptyset(&newAction.sa_mask);

    if (-1 == sigaction(SIGUSR2, &newAction, &previousSigusr2))
    {
        AssertMsg(errno, "PerfTrace::Register: sigaction() call failed\n");
    }
}

void  PerfTrace::WritePerfMap()
{
#if ENABLE_NATIVE_CODEGEN
    // Lock threadContext list during etw rundown
    AutoCriticalSection autoThreadContextCs(ThreadContext::GetCriticalSection());

    ThreadContext * threadContext = ThreadContext::GetThreadContextList();

    FILE * perfMapFile;

    {
        const size_t PERFMAP_FILENAME_MAX_LENGTH = 30;
        char perfMapFilename[PERFMAP_FILENAME_MAX_LENGTH];
        pid_t processId = getpid();
        snprintf(perfMapFilename, PERFMAP_FILENAME_MAX_LENGTH, "/tmp/perf-%d.map", processId);

        perfMapFile = fopen(perfMapFilename, "w");
        if (perfMapFile == NULL) {
            return;
        }
    }

    while(threadContext != nullptr)
    {
        // Take etw rundown lock on this thread context
        AutoCriticalSection autoEtwRundownCs(threadContext->GetFunctionBodyLock());

        ScriptContext* scriptContext = threadContext->GetScriptContextList();
        while(scriptContext != NULL)
        {
            if(scriptContext->IsClosed())
            {
                scriptContext = scriptContext->next;
                continue;
            }

            scriptContext->MapFunction([=] (FunctionBody* body)
            {
#if DYNAMIC_INTERPRETER_THUNK
                if(body->HasInterpreterThunkGenerated())
                {
                    const char16* functionName = body->GetExternalDisplayName();
                    fwprintf(perfMapFile, _u("%llX %llX %s(Interpreted)\n"),
                        body->GetDynamicInterpreterEntryPoint(),
                        body->GetDynamicInterpreterThunkSize(),
                        functionName);
                }
#endif

#if ENABLE_NATIVE_CODEGEN
                body->MapEntryPoints([&](int index, FunctionEntryPointInfo * entryPoint)
                {
                    if(entryPoint->IsCodeGenDone())
                    {
                        const ExecutionMode jitMode = entryPoint->GetJitMode();
                        if (jitMode == ExecutionMode::SimpleJit)
                        {
                            fwprintf(perfMapFile, _u("%llX %llX %s(SimpleJIT)\n"),
                                entryPoint->GetNativeAddress(),
                                entryPoint->GetCodeSize(),
                                body->GetExternalDisplayName());
                        }
                        else
                        {
                            fwprintf(perfMapFile, _u("%llX %llX %s(FullJIT)\n"),
                                entryPoint->GetNativeAddress(),
                                entryPoint->GetCodeSize(),
                                body->GetExternalDisplayName());
                        }
                    }
                });

                body->MapLoopHeadersWithLock([&](uint loopNumber, LoopHeader* header)
                {
                    header->MapEntryPoints([&](int index, LoopEntryPointInfo * entryPoint)
                    {
                        if(entryPoint->IsCodeGenDone())
                        {
                            const uint16 loopNumber = ((uint16)body->GetLoopNumberWithLock(header));
                            fwprintf(perfMapFile, _u("%llX %llX %s(Loop%u)\n"),
                                entryPoint->GetNativeAddress(),
                                entryPoint->GetCodeSize(),
                                body->GetExternalDisplayName(),
                                loopNumber+1);
                        }
                    });
                });
#endif
            });

            scriptContext = scriptContext->next;
        }

        threadContext = threadContext->Next();
    }

    fflush(perfMapFile);
    fclose(perfMapFile);
#endif
    PerfTrace::mapsRequested = false;
}

}

