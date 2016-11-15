//==============================================================================
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
/// \author AMD Developer Tools Team
/// \file
/// \brief  This file is the main agent file for the HSA API Trace module
//==============================================================================

#include <hsa_api_trace.h>

#include <cstdlib>
#include <iostream>

#include <AMDTOSWrappers/Include/osTimeInterval.h>

#include "Logger.h"
#include "FileUtils.h"
#include "GlobalSettings.h"
#include "StackTracer.h"

#include "HSAAPITableVersions.h"
#include "HSATraceInterceptionTable1_0.h"
#include "HSATraceInterceptionTable1_2.h"
#include "HSAAgentUtils.h"

#include "HSASignalPool.h"
#include "HSAAqlPacketTimeCollector.h"

#include "HSAFdnAPIInfoManager.h"
#include "AutoGenerated/HSATraceInterception.h"
#include "AutoGenerated/HSACoreAPITraceClasses.h"


void TimerThread(void* param)
{
    SP_UNREFERENCED_PARAMETER(param);

    unsigned int interval = HSAAPIInfoManager::Instance()->GetInterval();

    if (interval == 0)
    {
        interval = 1; // safety net in case interval is zero (it shouldn't be...)
    }

    const unsigned int sleepInterval = interval < 10 ? interval : 10; // sleep at most 10 ms at a time
    const unsigned int sleepsBeforeFlush = sleepInterval == 0 ? 1 : interval / sleepInterval;

    unsigned int iterationNum = 1;

    while (HSAAPIInfoManager::Instance()->IsRunning())
    {
        OSUtils::Instance()->SleepMillisecond(sleepInterval);

        if (iterationNum == sleepsBeforeFlush)
        {
            iterationNum = 1;
            HSAAPIInfoManager::Instance()->TrySwapBuffer();
            HSAAPIInfoManager::Instance()->FlushTraceData();
        }
        else
        {
            iterationNum++;
        }
    }
}

extern "C" DLL_PUBLIC void amdtCodeXLStopProfiling()
{
    HSAAPIInfoManager::Instance()->StopTracing();
}

extern "C" DLL_PUBLIC void amdtCodeXLResumeProfiling()
{
    HSAAPIInfoManager::Instance()->ResumeTracing();
}

static HSASignalCollectorThread* g_pSignalCollector = nullptr;

extern "C" bool DLL_PUBLIC OnLoad(void* pTable, uint64_t runtimeVersion, uint64_t failedToolCount, const char* const* pFailedToolNames)
{
#ifdef _DEBUG
    FileUtils::CheckForDebuggerAttach();
#endif

    std::string strLogFile = FileUtils::GetDefaultOutputPath() + "hsatraceagent.log";
    LogFileInitialize(strLogFile.c_str());

    if (!CheckRuntimeToolsLibLoaded(runtimeVersion, failedToolCount, pFailedToolNames))
    {
        std::cout << "CodeXL GPU Profiler could not be enabled. Version mismatch between HSA runtime and " << HSA_RUNTIME_TOOLS_LIB << std::endl;
        return false;
    }

    std::cout << "CodeXL GPU Profiler " << GPUPROFILER_BACKEND_VERSION_STRING << " is enabled\n";

    Parameters params;
    FileUtils::GetParametersFromFile(params);

    if (params.m_bStartDisabled)
    {
        HSAAPIInfoManager::Instance()->StopTracing();
    }
    else
    {
        HSAAPIInfoManager::Instance()->EnableProfileDelayStart(params.m_bDelayStartEnabled, params.m_delayInMilliseconds);
        HSAAPIInfoManager::Instance()->EnableProfileDuration(params.m_bProfilerDurationEnabled, params.m_durationInMilliseconds);
        if (params.m_bDelayStartEnabled)
        {
            HSAAPIInfoManager::Instance()->CreateTimer(PROFILEDELAYTIMER, params.m_delayInMilliseconds);
            HSAAPIInfoManager::Instance()->SetTimerFinishHandler(PROFILEDELAYTIMER, HSATraceAgentTimerEndResponse);
            HSAAPIInfoManager::Instance()->StopTracing();
            HSAAPIInfoManager::Instance()->startTimer(PROFILEDELAYTIMER);
        }
        else if (params.m_bProfilerDurationEnabled)
        {
            HSAAPIInfoManager::Instance()->CreateTimer(PROFILEDURATIONTIMER, params.m_durationInMilliseconds);
            HSAAPIInfoManager::Instance()->SetTimerFinishHandler(PROFILEDURATIONTIMER, HSATraceAgentTimerEndResponse);
            HSAAPIInfoManager::Instance()->startTimer(PROFILEDURATIONTIMER);
        }
    }

    GlobalSettings::GetInstance()->m_params = params;
    HSAAPIInfoManager::Instance()->SetOutputFile(params.m_strOutputFile);

    if (!params.m_strAPIFilterFile.empty())
    {
        HSAAPIInfoManager::Instance()->LoadAPIFilterFile(params.m_strAPIFilterFile);
    }

    StackTracer::Instance()->InitSymPath();

    if (params.m_bTimeOutBasedOutput)
    {
        HSAAPIInfoManager::Instance()->SetInterval(params.m_uiTimeOutInterval);

        if (!HSAAPIInfoManager::Instance()->StartTimer(TimerThread))
        {
            std::cout << "Failed to initialize HSATraceAgent.\n";
        }
    }

    if (ROCM_1_1_X_AND_EARLIER_ROOT_RUNTIME_VERSION == runtimeVersion)
    {
        // ROCm versions 1.1.1 and earlier
        InitHSAAPIInterceptTrace1_0(reinterpret_cast<ApiTable1_0*>(pTable));
    }
    else
    {
        HsaApiTable* pHsaTable = reinterpret_cast<HsaApiTable*>(pTable);

        if (IsROCm12(pHsaTable))
        {
            // ROCm 1.2 backwards compatibility
            HsaApiTable1_2* pHsaTable1_2 = reinterpret_cast<HsaApiTable1_2*>(pTable);
            InitHSAAPIInterceptTrace1_2(pHsaTable1_2);
        }
        else
        {
            InitHSAAPIInterceptTrace(pHsaTable);
        }
    }

    // Add a fabricated entry for hsa_init when OnLoad is called.
    // OnLoad is called when the first hsa_init is called.
    // The timestamps are made up, but at least there is an entry.
    // The retVal is always HSA_STATUS_SUCCESS because if the RT were to
    // return anything else, this OnLoad function wouldn't get called.
    ULONGLONG ullStart = OSUtils::Instance()->GetTimeNanos();
    hsa_status_t retVal = HSA_STATUS_SUCCESS;

    HSA_APITrace_hsa_init* pAPIInfo = new(std::nothrow) HSA_APITrace_hsa_init();
    SpAssertRet(pAPIInfo != NULL) false;

    ULONGLONG ullEnd = OSUtils::Instance()->GetTimeNanos();

    pAPIInfo->Create(
        ullStart,
        ullEnd,
        retVal);

    RECORD_STACK_TRACE_FOR_API(pAPIInfo);
    HSAAPIInfoManager::Instance()->AddAPIInfoEntry(pAPIInfo);

    if (params.m_bAqlPacketTracing)
    {
        g_pSignalCollector = new HSASignalCollectorThread();
        g_pSignalCollector->execute();
    }

    return true;
}

extern "C" void DLL_PUBLIC OnUnload()
{
    // Add a fabricated entry for hsa_shut_down when OnUnload is called.
    // OnUnload is called when the last hsa_shut_down is called (i.e. refcount goes to zero).
    // The timestamps are made up, but at least there is an entry.
    // The retVal is always HSA_STATUS_SUCCESS because if the RT were to
    // return anything else, this OnUnload function wouldn't get called.
    ULONGLONG ullStart = OSUtils::Instance()->GetTimeNanos();
    hsa_status_t retVal = HSA_STATUS_SUCCESS;

    HSA_APITrace_hsa_shut_down* pAPIInfo = new(std::nothrow) HSA_APITrace_hsa_shut_down();
    SpAssertRet(pAPIInfo != NULL);

    ULONGLONG ullEnd = OSUtils::Instance()->GetTimeNanos();

    pAPIInfo->Create(
        ullStart,
        ullEnd,
        retVal);

    RECORD_STACK_TRACE_FOR_API(pAPIInfo);
    HSAAPIInfoManager::Instance()->AddAPIInfoEntry(pAPIInfo);

    if (nullptr != g_pSignalCollector)
    {
        HSATimeCollectorGlobals::Instance()->m_doQuit = true;

        auto& forceSignalCollection = HSATimeCollectorGlobals::Instance()->m_forceSignalCollection;

        g_pRealCoreFunctions->hsa_signal_store_screlease_fn(forceSignalCollection, 1);

#if defined (_LINUX) || defined (LINUX)
        // notify the signal collector thread to collect all remaining signals
        if (!HSATimeCollectorGlobals::Instance()->m_dispatchesInFlight.unlockCondition())
        {
             Log(logERROR, "unable to unlock condition\n");
        }

        HSATimeCollectorGlobals::Instance()->m_dispatchesInFlight.signalSingleThread();
#endif

        g_pSignalCollector->waitForThreadEnd(osTimeInterval(static_cast<gtUInt64>(10 * 1e9))); // wait ten seconds for thread to end
        g_pSignalCollector->terminate();
        g_pSignalCollector = nullptr;

        retVal = g_pRealCoreFunctions->hsa_signal_destroy_fn(forceSignalCollection);
    }

    HSASignalQueue::Instance()->Clear();
    HSASignalPool::Instance()->Clear();

    if (HSA_STATUS_SUCCESS != retVal)
    {
        Log(logERROR, "Unable to destroy signal\n");
    }

    if (HSAAPIInfoManager::Instance()->IsTimeOutMode())
    {
        HSAAPIInfoManager::Instance()->StopTimer();
        HSAAPIInfoManager::Instance()->TrySwapBuffer();
        HSAAPIInfoManager::Instance()->FlushTraceData();
        HSAAPIInfoManager::Instance()->TrySwapBuffer();
        HSAAPIInfoManager::Instance()->FlushTraceData();
        HSAAPIInfoManager::Instance()->ResumeTimer();
    }

    DoneHSAAPIInterceptTrace();
}
