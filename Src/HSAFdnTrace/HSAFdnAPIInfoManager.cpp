//==============================================================================
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
/// \author AMD Developer Tools Team
/// \file
/// \brief  This class manages all the traces API objects
//==============================================================================

#include <string>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include "HSAFdnAPIInfoManager.h"
#include "HSARTModuleLoader.h"
#include "FinalizerInfoManager.h"
#include "HSAKernelDemangler.h"
#include "HSATraceStringUtils.h"
#include "HSASignalPool.h"
#include "AutoGenerated/HSATraceInterception.h"
#include "AutoGenerated/HSATraceStringOutput.h"
#include "../Common/FileUtils.h"
#include "../Common/GlobalSettings.h"
#include "../HSAFdnCommon/HSAFunctionDefsUtils.h"
#include "../HSAFdnCommon/HSAAgentIterateReplacer.h"
#include <AMDTBaseTools/Include/gtAssert.h>
#include "HSAFdnMaxApiTime.h"
#include <ProfilerOutputFileDefs.h>
#include "HSAAgentUtils.h"
#include "ROCProfilerModule.h"

using namespace std;

std::mutex HSAAPIInfoManager::ms_asyncTimeStampsMtx;

AsyncCopyInfoList HSAAPIInfoManager::ms_asyncCopyInfoList;

HSAAPIInfoManager::HSAAPIInfoManager(void) : m_tracedApiCount(0), m_queueCreationCount(0)
{
    m_strTraceModuleName = "hsa";

    // add APIs that we should always intercept...
    m_mustInterceptAPIs.insert(HSA_API_Type_hsa_queue_create);               // needed so we can create a profiled queue for kernel timestamps
    m_mustInterceptAPIs.insert(HSA_API_Type_hsa_executable_get_symbol);      // needed to extract kernel name
    m_mustInterceptAPIs.insert(HSA_API_Type_hsa_executable_symbol_get_info); // needed to extract kernel name
    m_pDelayTimer = nullptr;
    m_pDurationTimer = nullptr;
    m_bNoHSATransferTime = false;
}

HSAAPIInfoManager::~HSAAPIInfoManager(void)
{
    if (nullptr != m_pDelayTimer)
    {
        m_pDelayTimer->stopTimer();
        SAFE_DELETE(m_pDelayTimer);
    }

    if (nullptr != m_pDurationTimer)
    {
        m_pDurationTimer->stopTimer();
        SAFE_DELETE(m_pDurationTimer);
    }
}

bool HSAAPIInfoManager::WriteAsyncCopyTimestamp(std::ostream& sout, const AsyncCopyInfo* pAsyncCopyInfo)
{
    if (nullptr != pAsyncCopyInfo)
    {
        sout << std::left << std::setw(21) << pAsyncCopyInfo->m_threadId;
        sout << std::left << std::setw(21) << pAsyncCopyInfo->m_signal.handle;
        sout << std::left << std::setw(21) << pAsyncCopyInfo->m_start;
        sout << std::left << std::setw(21) << pAsyncCopyInfo->m_end;
        sout << std::left << std::setw(21) << pAsyncCopyInfo->m_asyncCopyIdentifier;

        return true;
    }

    return false;
}

void HSAAPIInfoManager::FlushNonAPITimestampData(const osProcessId& pid)
{
    {
        std::lock_guard<std::mutex> lock(ms_asyncTimeStampsMtx);

        if (ms_asyncCopyInfoList.size() > 0)
        {
            string tmpAsyncCopyTimestampFile = GetTempFileName(pid, 0, TMP_ASYNC_COPY_TIME_STAMP_EXT);
            ofstream foutCopyTS(tmpAsyncCopyTimestampFile.c_str(), fstream::out | fstream::app);

            for (auto asyncCopyInfo : ms_asyncCopyInfoList)
            {
                WriteAsyncCopyTimestamp(foutCopyTS, asyncCopyInfo);
                foutCopyTS << std::endl;
            }

            foutCopyTS.close();

            for (auto it = ms_asyncCopyInfoList.begin(); it != ms_asyncCopyInfoList.end(); ++it)
            {
                delete (*it);
            }

            ms_asyncCopyInfoList.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_packetTraceMtx);

        string tmpAqlTraceFile = GetTempFileName(pid, 0, TMP_KERNEL_TIME_STAMP_EXT);
        ofstream foutAqlTrace(tmpAqlTraceFile.c_str(), fstream::out | fstream::app);

        PacketList notReadyPackets;

        for (auto it = m_packetList.begin(); it != m_packetList.end(); ++it)
        {
            if ((*it)->m_isReady)
            {
                (*it)->WritePacketEntry(foutAqlTrace);
                foutAqlTrace << std::endl;
                delete(*it);
            }
            else
            {
                notReadyPackets.push_back(*it);
            }
        }

        foutAqlTrace.close();

        // assign not-ready packets for the next time we flush
        m_packetList = notReadyPackets;
    }
}

void HSAAPIInfoManager::AddAPIToFilter(const std::string& strAPIName)
{
    HSA_API_Type type = HSAFunctionDefsUtils::Instance()->ToHSAAPIType(strAPIName);

    if (type != HSA_API_Type_UNKNOWN)
    {
        m_filterAPIs.insert(type);
    }
    else
    {
        Log(logWARNING, "Unknown API name = %s\n", strAPIName.c_str());
    }
}

bool HSAAPIInfoManager::IsInFilterList(HSA_API_Type type) const
{
    return m_filterAPIs.find(type) != m_filterAPIs.end();
}

bool HSAAPIInfoManager::ShouldIntercept(HSA_API_Type type) const
{
    return !IsInFilterList(type) || m_mustInterceptAPIs.find(type) != m_mustInterceptAPIs.end();
}

bool HSAAPIInfoManager::IsCapReached() const
{
    return m_tracedApiCount >= GlobalSettings::GetInstance()->m_params.m_uiMaxNumOfAPICalls;
}

void HSAAPIInfoManager::AddQueue(const hsa_queue_t* pQueue)
{
    if (nullptr != pQueue)
    {
        std::lock_guard<std::mutex> lock(m_queueMapMtx);

        if (m_queueIdMap.end() != m_queueIdMap.find(pQueue))
        {
            Log(logWARNING, "Queue added to map more than once\n");
            m_queueIdMap[pQueue] = m_queueCreationCount;
        }
        else
        {
            m_queueIdMap.insert(QueueIdMapPair(pQueue, m_queueCreationCount));
        }

        m_queueCreationCount++;
    }
}

bool HSAAPIInfoManager::GetQueueId(const hsa_queue_t* pQueue, uint64_t& queueId)
{
    bool retVal = false;
    std::lock_guard<std::mutex> lock(m_queueMapMtx);

    QueueIdMap::const_iterator it = m_queueIdMap.find(pQueue);

    if (m_queueIdMap.end() != it)
    {
        retVal = true;
        queueId = it->second;
    }

    return retVal;
}

bool AsyncSignalHandler(hsa_signal_value_t value, void* pArg)
{
    // pAsyncCopyInfo must be guarded with the mutex in the callback
    std::lock_guard<std::mutex> lock(HSAAPIInfoManager::ms_asyncTimeStampsMtx);

    AsyncCopyInfo* pAsyncCopyInfo = reinterpret_cast<AsyncCopyInfo*>(pArg);

    if (nullptr == pAsyncCopyInfo)
    {
        GPULogger::Log(GPULogger::logERROR, "AsyncSignalhandler called with a null user arg.\n");
    }
    else
    {
        if (0 > value)
        {
            // the signal passed hsa_amd_memory_async_copy will get a value less than zero to indicate that the copy operation failed
            // we will flag this condition by using 0 start and end times
            pAsyncCopyInfo->m_start = 0;
            pAsyncCopyInfo->m_end = 0;

            // recover the original signal since the signal handle is used to identify async copy calls even if it fails
            hsa_signal_t origSignal;

            if (HSAAPIInfoManager::Instance()->GetOriginalAsyncCopySignal(pAsyncCopyInfo->m_signal, origSignal))
            {
                pAsyncCopyInfo->m_signal = origSignal;
            }
        }
        else
        {
            hsa_amd_profiling_async_copy_time_t asyncCopyTime;
            hsa_status_t status = g_pRealAmdExtFunctions->hsa_amd_profiling_get_async_copy_time_fn(pAsyncCopyInfo->m_signal, &asyncCopyTime);

            if (HSA_STATUS_SUCCESS != status)
            {
                GPULogger::Log(GPULogger::logERROR, "Error returned from hsa_amd_profiling_get_async_copy_time\n");
            }
            else
            {
                pAsyncCopyInfo->m_start = asyncCopyTime.start;
                pAsyncCopyInfo->m_end = asyncCopyTime.end;

                HSAAPIInfoManager::Instance()->LockSignalMap();
                hsa_signal_t origSignal;

                if (HSAAPIInfoManager::Instance()->GetOriginalAsyncCopySignal(pAsyncCopyInfo->m_signal, origSignal))
                {
                    g_pRealCoreFunctions->hsa_signal_store_relaxed_fn(origSignal, value);
                    HSAAPIInfoManager::Instance()->RemoveAsyncCopySignal(pAsyncCopyInfo->m_signal);
                    HSASignalPool::Instance()->ReleaseSignal(pAsyncCopyInfo->m_signal);
                    pAsyncCopyInfo->m_signal = origSignal;
                }
                else
                {
                    GPULogger::Log(GPULogger::logERROR, "Unable to find original async copy signal\n");
                }

                HSAAPIInfoManager::Instance()->UnlockSignalMap();

                // the filling of ms_asyncCopyInfoList must be placed inside this callback to avoid zero valued timestamps before this callback is invoked
                HSAAPIInfoManager::ms_asyncCopyInfoList.push_back(pAsyncCopyInfo);
            }
        }
    }

    return false; // no longer monitor this signal (it will be re-added if necessary)
}

void HSAAPIInfoManager::LockSignalMap()
{
    m_signalMapMtx.lock();
}

void HSAAPIInfoManager::UnlockSignalMap()
{
    m_signalMapMtx.unlock();
}

void HSAAPIInfoManager::AddAsyncCopyCompletionSignal(const hsa_signal_t& completionSignal, unsigned long long asyncCopyIdentifier)
{
    hsa_signal_value_t signalValue = g_pRealCoreFunctions->hsa_signal_load_scacquire_fn(completionSignal);

    AsyncCopyInfo* pAsyncCopyInfo = new(std::nothrow) AsyncCopyInfo(osGetUniqueCurrentThreadId(), completionSignal);

    if (nullptr == pAsyncCopyInfo)
    {
        GPULogger::Log(GPULogger::logERROR, "Unable to allocate memory for ASyncCopyInfo\n");
    }
    else
    {
        std::lock_guard<std::mutex> lock(ms_asyncTimeStampsMtx);

        pAsyncCopyInfo->m_asyncCopyIdentifier = asyncCopyIdentifier;

        hsa_status_t status = g_pRealAmdExtFunctions->hsa_amd_signal_async_handler_fn(completionSignal, HSA_SIGNAL_CONDITION_LT, signalValue, AsyncSignalHandler, pAsyncCopyInfo);

        if (HSA_STATUS_SUCCESS != status)
        {
            GPULogger::Log(GPULogger::logERROR, "Error returned from hsa_amd_signal_async_handler\n");
        }
    }
}

void HSAAPIInfoManager::AddReplacementAsyncCopySignal(const hsa_signal_t& originalSignal, const hsa_signal_t& replacementSignal)
{
    std::lock_guard<std::mutex> lock(m_signalMapMtx);
    m_signalMap[replacementSignal.handle] = originalSignal;
}

bool HSAAPIInfoManager::GetOriginalAsyncCopySignal(const hsa_signal_t& replacementSignal, hsa_signal_t& originalSignal)
{
    bool retVal = m_signalMap.count(replacementSignal.handle) > 0;

    if (retVal)
    {
        originalSignal = m_signalMap[replacementSignal.handle];
    }

    return retVal;
}

void HSAAPIInfoManager::RemoveAsyncCopySignal(const hsa_signal_t& replacementSignal)
{
    m_signalMap.erase(replacementSignal.handle);
}

void HSATraceAgentTimerEndResponse(ProfilerTimerType timerType)
{
    switch (timerType)
    {
        case PROFILEDELAYTIMER:
            HSAAPIInfoManager::Instance()->ResumeTracing();
            unsigned long profilerDuration;

            if (HSAAPIInfoManager::Instance()->IsProfilerDurationEnabled(profilerDuration))
            {
                HSAAPIInfoManager::Instance()->CreateTimer(PROFILEDURATIONTIMER, profilerDuration);
                HSAAPIInfoManager::Instance()->SetTimerFinishHandler(PROFILEDURATIONTIMER, HSATraceAgentTimerEndResponse);
                HSAAPIInfoManager::Instance()->startTimer(PROFILEDURATIONTIMER);
            }

            break;

        case PROFILEDURATIONTIMER:
            HSAAPIInfoManager::Instance()->StopTracing();
            break;

        default:
            break;
    }
}

void HSAAPIInfoManager::EnableProfileDelayStart(bool doEnable, unsigned long delayInMilliseconds)
{
    m_bDelayStartEnabled = doEnable;
    m_delayInMilliseconds = doEnable ? delayInMilliseconds : 0;
}

void HSAAPIInfoManager::EnableProfileDuration(bool doEnable, unsigned long durationInMilliseconds)
{
    m_bProfilerDurationEnabled = doEnable;
    m_durationInMilliseconds = doEnable ? durationInMilliseconds : 0;
}

bool HSAAPIInfoManager::IsProfilerDelayEnabled(unsigned long& delayInMilliseconds)
{
    delayInMilliseconds = m_delayInMilliseconds;
    return m_bDelayStartEnabled;
}

bool HSAAPIInfoManager::IsProfilerDurationEnabled(unsigned long& durationInSeconds)
{
    durationInSeconds = m_durationInMilliseconds;
    return m_bProfilerDurationEnabled;
}

void HSAAPIInfoManager::SetTimerFinishHandler(ProfilerTimerType timerType, TimerEndHandler timerEndHandler)
{
    switch (timerType)
    {
        case PROFILEDELAYTIMER:
            if (nullptr != m_pDelayTimer)
            {
                m_pDelayTimer->SetTimerFinishHandler(timerEndHandler);
            }

            break;

        case PROFILEDURATIONTIMER:
            if (nullptr != m_pDurationTimer)
            {
                m_pDurationTimer->SetTimerFinishHandler(timerEndHandler);
            }

            break;

        default:
            break;
    }
}

void HSAAPIInfoManager::CreateTimer(ProfilerTimerType timerType, unsigned long timeIntervalInMilliseconds)
{
    switch (timerType)
    {
        case PROFILEDELAYTIMER:
            if (m_pDelayTimer == nullptr && timeIntervalInMilliseconds > 0)
            {
                m_pDelayTimer = new(std::nothrow) ProfilerTimer(timeIntervalInMilliseconds);

                if (nullptr == m_pDelayTimer)
                {
                    GPULogger::Log(GPULogger::logERROR, "CreateTimer: unable to allocate memory for delay timer\n");
                }
                else
                {
                    m_pDelayTimer->SetTimerType(PROFILEDELAYTIMER);
                    m_bDelayStartEnabled = true;
                    m_delayInMilliseconds = timeIntervalInMilliseconds;
                }
            }

            break;

        case PROFILEDURATIONTIMER:
            if (m_pDurationTimer == nullptr && timeIntervalInMilliseconds > 0)
            {
                m_pDurationTimer = new(std::nothrow) ProfilerTimer(timeIntervalInMilliseconds);

                if (nullptr == m_pDurationTimer)
                {
                    GPULogger::Log(GPULogger::logERROR, "CreateTimer: unable to allocate memory for duration timer\n");
                }
                else
                {
                    m_pDurationTimer->SetTimerType(PROFILEDURATIONTIMER);
                    m_bProfilerDurationEnabled = true;
                    m_durationInMilliseconds = timeIntervalInMilliseconds;
                }
            }

            break;

        default:
            break;
    }
}

void HSAAPIInfoManager::startTimer(ProfilerTimerType timerType)
{
    switch (timerType)
    {
        case PROFILEDELAYTIMER:
            if (nullptr != m_pDelayTimer)
            {
                m_pDelayTimer->startTimer(true);
            }

            break;

        case PROFILEDURATIONTIMER:
            if (nullptr != m_pDurationTimer)
            {
                m_pDurationTimer->startTimer(true);
            }

            break;

        default:
            break;
    }
}

void HSAAPIInfoManager::AddAPIInfoEntry(APIBase* pApi)
{
    HSAAPIBase* hsaAPI = dynamic_cast<HSAAPIBase*>(pApi);
    bool isCapReached = IsCapReached();

    if (isCapReached || IsInFilterList(hsaAPI->m_type) || !IsTracing())
    {
        if (isCapReached)
        {
            HSAFdnMaxApiCallTime::Instance()->RecordMaxApiCallEndTime(pApi->m_ullEnd);
        }

        SAFE_DELETE(hsaAPI);
    }
    else
    {
        APIInfoManagerBase::AddTraceInfoEntry(hsaAPI);
        m_tracedApiCount++;
    }
}

void HSAAPIInfoManager::AddAqlPacketEntry(HSAAqlPacketBase* pPacket)
{
    bool isCapReached = IsCapReached();

    if (isCapReached || !IsTracing())
    {
        if (isCapReached && HSA_PACKET_TYPE_KERNEL_DISPATCH == pPacket->m_type)
        {
            HSAFdnMaxApiCallTime::Instance()->RecordMaxApiCallEndTime(reinterpret_cast<HSAAqlKernelDispatchPacket*>(pPacket)->GetEndTimestamp());
        }

        SAFE_DELETE(pPacket)
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_packetTraceMtx);

        // TODO Do we need to update m_ullStart and m_ullEnd?
        //      Doing so will ensure m_ullStart and m_ullEnd includes all AQL packet timestamps
        //      It may be possible that packet timestamps are outside of the range of API calls (corner case?)

        m_packetList.push_back(pPacket);
    }
}

void HSAAPIInfoManager::DisableHsaTransferTime()
{
    m_bNoHSATransferTime = true;
}

bool HSAAPIInfoManager::IsHsaTransferTimeDisabled()
{
    return m_bNoHSATransferTime;
}

void HSAAPIInfoManager::MarkRocProfilerDataAsReady()
{
    for (auto it = m_packetList.begin(); it != m_packetList.end(); ++it)
    {
        HSAAqlKernelDispatchPacket* pRocProfilerPacket = reinterpret_cast<HSAAqlKernelDispatchPacket*>(*it);

        if (pRocProfilerPacket->m_isRocProfilerPacket)
        {
            ContextEntry* pEntry = pRocProfilerPacket->m_pContextEntry;

            if (pEntry->m_data.record)
            {
                pRocProfilerPacket->SetTimestamps(pEntry->m_data.record->begin, pEntry->m_data.record->end);
            }

            ROCProfilerModule* pROCProfilerModule = HSARTModuleLoader<ROCProfilerModule>::Instance()->GetHSARTModule();

            if (nullptr != pROCProfilerModule && pROCProfilerModule->IsModuleLoaded())
            {
                hsa_status_t status = pROCProfilerModule->rocprofiler_close(pEntry->m_group.context);
                if (HSA_STATUS_SUCCESS != status)
                {
                    GPULogger::Log(GPULogger::logERROR, "Error returned from rocprofiler_close()\n");
                }
            }

            delete pEntry;
        }
    }
}
