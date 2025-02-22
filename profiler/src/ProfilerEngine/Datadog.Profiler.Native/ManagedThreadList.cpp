// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2022 Datadog, Inc.

#include "ManagedThreadList.h"
#include "Log.h"
#include "OpSysTools.h"


const std::uint32_t ManagedThreadList::MinBufferSize = 50;


ManagedThreadList::ManagedThreadList(ICorProfilerInfo4* pCorProfilerInfo) :
    _pCorProfilerInfo{pCorProfilerInfo}
{
    _threads.reserve(MinBufferSize);
    _lookupByClrThreadId.reserve(MinBufferSize);
    _lookupByProfilerThreadInfoId.reserve(MinBufferSize);

    // in case of tests, this could be null
    if (_pCorProfilerInfo != nullptr)
    {
        _pCorProfilerInfo->AddRef();
    }
}

ManagedThreadList::~ManagedThreadList()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    for (auto* pInfo: _threads)
    {
        pInfo->Release();
    }

    ICorProfilerInfo4* pCorProfilerInfo = _pCorProfilerInfo;
    if (pCorProfilerInfo != nullptr)
    {
        pCorProfilerInfo->Release();
        _pCorProfilerInfo = nullptr;
    }
}

const char* ManagedThreadList::GetName()
{
    return _serviceName;
}

bool ManagedThreadList::Start()
{
    // nothing special to start
    return true;
}

bool ManagedThreadList::Stop()
{
    // nothing special to stop
    return true;
}

bool ManagedThreadList::GetOrCreateThread(ThreadID clrThreadId)
{
    return GetOrCreate(clrThreadId) != nullptr;
}

ManagedThreadInfo* ManagedThreadList::GetOrCreate(ThreadID clrThreadId)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    ManagedThreadInfo* pInfo = FindByClrId(clrThreadId);
    if (pInfo == nullptr)
    {
        pInfo = new ManagedThreadInfo(clrThreadId);
        pInfo->AddRef();
        _threads.push_back(pInfo);

        _lookupByClrThreadId[clrThreadId] = pInfo;
        _lookupByProfilerThreadInfoId[pInfo->GetProfilerThreadInfoId()] = pInfo;
    }

    return pInfo;
}

void ManagedThreadList::UpdateIterators(uint32_t removalPos)
{
    // Interators are positions (in the threads vector) pointing to the next thread to return via LoopNext.
    // So, when a thread is removed from the vector at a position BEFORE an iterator position,
    // this iterator needs to be moved left by 1 to keep on pointing to the same thread.
    // There is no need to update iterators pointing to threads before or at the same spot
    // as the removal position because they will point to the same thread
    //
    // In the following example, the thread at position 1 will be removed and an iterator
    // is pointing to ^ the thread in the third position (i.e. at pos = 2).
    //      x
    //  T0  T1  T2  T3
    //          ^ = 2
    // -->          |
    //  T0  T2  T3  v
    //      ^ = 1 =(2 - 1)
    //
    // After the removal, this iterator should now point to the thread at position 1 instead of 2
    //
    // If the new pos is beyond the vector (i.e. the last element was removed),
    // then reset the iterator to the beginning of the vector:
    //          x
    //  T0  T1  T2  (_threads.size() == 2 when this function is called)
    //          ^ = 2
    // -->
    //  T0  T1
    //  ^ = 0  (reset)
    //
    for (auto i = _iterators.begin(); i != _iterators.end(); ++i)
    {
        uint32_t pos = *i;
        if (removalPos < pos)
        {
            pos = pos - 1;
        }

        // reset iterator if needed
        if (pos >= _threads.size())  // the thread has already been removed from the vector
        {
            pos = 0;
        }

        *i = pos;
    }
}

bool ManagedThreadList::UnregisterThread(ThreadID clrThreadId, ManagedThreadInfo** ppThreadInfo)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    uint32_t pos = 0;
    for (auto i = _threads.begin(); i != _threads.end(); ++i)
    {
        auto pInfo = *i;
        if (pInfo->GetClrThreadId() == clrThreadId)
        {
            // NOTE: the caller needs to release the returned ManagedThreadInfo*
            *ppThreadInfo = pInfo;

            // remove it from the storage and index
            _threads.erase(i);
            _lookupByClrThreadId.erase(pInfo->GetClrThreadId());
            _lookupByProfilerThreadInfoId.erase(pInfo->GetProfilerThreadInfoId());

            // iterators might need to be updated
            UpdateIterators(pos);

            return true;
        }
        pos++;
    }

    Log::Error("ManagedThreadList: thread ", std::dec, clrThreadId, "cannot be unregister because not in the list");
    *ppThreadInfo = nullptr;
    return false;
}

bool ManagedThreadList::SetThreadOsInfo(ThreadID clrThreadId, DWORD osThreadId, HANDLE osThreadHandle)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    ManagedThreadInfo* pInfo = GetOrCreate(clrThreadId);
    if (pInfo == nullptr)
    {
        Log::Error("ManagedThreadList: thread 0x", std::hex, clrThreadId, " cannot be associated to OS ID(0x", std::hex, osThreadId, std::dec, ") because not in the list");
        return false;
    }

    pInfo->SetOsInfo(osThreadId, osThreadHandle);

    Log::Debug("ManagedThreadList::SetThreadOsInfo(clrThreadId: 0x", std::hex, clrThreadId,
               ", osThreadId: ", std::dec, osThreadId,
               ", osThreadHandle: 0x", std::hex, osThreadHandle, ")",
               " completed for ProfilerThreadInfoId=", std::dec, pInfo->GetProfilerThreadInfoId(), ".");

    return true;
}

bool ManagedThreadList::SetThreadName(ThreadID clrThreadId, const shared::WSTRING& threadName)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    ManagedThreadInfo* pInfo = GetOrCreate(clrThreadId);
    if (pInfo == nullptr)
    {
        Log::Error("ManagedThreadList: impossible to set thread 0x", std::hex, clrThreadId, " name to  \"", (threadName.empty() ? WStr("null") : threadName), "\") because not in the list");
        return false;
    }

    pInfo->SetThreadName(threadName);

    Log::Debug("ManagedThreadList::SetThreadName(clrThreadId: 0x", std::hex, clrThreadId,
               ", pThreadName: \"", threadName, "\")",
               " completed for ProfilerThreadInfoId=", pInfo->GetProfilerThreadInfoId(), ".");

    return true;
}

uint32_t ManagedThreadList::Count(void)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return static_cast<uint32_t>(_threads.size());
}

uint32_t ManagedThreadList::CreateIterator()
{
    uint32_t iterator = static_cast<uint32_t>(_iterators.size());
    _iterators.push_back(0);
    return iterator;
}

ManagedThreadInfo* ManagedThreadList::LoopNext(uint32_t iterator)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    auto activeThreadCount = _threads.size();
    if (activeThreadCount == 0)
    {
        return nullptr;
    }

    if (iterator >= _iterators.size())
    {
        return nullptr;
    }

    uint32_t pos = _iterators[iterator];
    ManagedThreadInfo* pInfo = _threads[pos];
    pInfo->AddRef(); // Caller must release

    // move the iterator to the next thread
    if (pos < activeThreadCount - 1)
    {
        pos++;
    }
    else // go back to the first thread if the end is reached
    {
        pos = 0;
    }
    _iterators[iterator] = pos;

    return pInfo;
}

ManagedThreadInfo* ManagedThreadList::FindByProfilerId(uint32_t profilerThreadInfoId)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (_threads.size() == 0)
    {
        return nullptr;
    }

    auto elem = _lookupByProfilerThreadInfoId.find(profilerThreadInfoId);
    if (elem == _lookupByProfilerThreadInfoId.end())
    {
        return nullptr;
    }

    ManagedThreadInfo* pInfo = elem->second;
    pInfo->AddRef(); // caller must release
    return pInfo;
}

bool ManagedThreadList::TryGetThreadInfo(const uint32_t profilerThreadInfoId,
                                         ThreadID* pClrThreadId,
                                         DWORD* pOsThreadId,
                                         HANDLE* pOsThreadHandle,
                                         WCHAR* pThreadNameBuff,
                                         const uint32_t threadNameBuffLen,
                                         uint32_t* pActualThreadNameLen)
{
    ManagedThreadInfo* pThreadInfo = FindByProfilerId(profilerThreadInfoId);

    if (pThreadInfo == nullptr)
    {
        return false;
    }

    if (pClrThreadId != nullptr)
    {
        *pClrThreadId = pThreadInfo->GetClrThreadId();
    }

    if (pOsThreadId != nullptr)
    {
        *pOsThreadId = pThreadInfo->GetOsThreadId();
    }

    if (pOsThreadHandle != nullptr)
    {
        *pOsThreadHandle = pThreadInfo->GetOsThreadHandle();
    }

    const shared::WSTRING& tName = pThreadInfo->GetThreadName();

    if (pThreadNameBuff != nullptr && threadNameBuffLen > 0)
    {
        std::uint32_t copyCharCount = (std::min)(static_cast<std::uint32_t>(tName.size()), threadNameBuffLen - 1);

        // If a managed thread name was set, we will use it.
        // If a managed thread name was not set, we will attempt to use a potentially set native thread name (or debugger thread description).
        // Note that we do not get callbacks for the update of the latter, so we have to query it every time.
        // However, the results of this TryGetThreadInfo(..) method are cached on the managed side.
        // As a result, this API should not be called more than once per export cycle per thread, and we do not expect much overhead from this query.
        // (But native thread name updates may propagate with a delay).
        if (copyCharCount > 0)
        {
            tName.copy(pThreadNameBuff, copyCharCount, 0);
            pThreadNameBuff[copyCharCount] = static_cast<WCHAR>(0);
        }
        else
        {
            if (false == OpSysTools::GetNativeThreadName(pThreadInfo->GetOsThreadHandle(), pThreadNameBuff, threadNameBuffLen))
            {
                pThreadNameBuff[0] = static_cast<WCHAR>(0);
            }
        }
    }

    if (pActualThreadNameLen != nullptr)
    {
        *pActualThreadNameLen = static_cast<std::uint32_t>(tName.size());
    }

    pThreadInfo->Release();
    return true;
}

HRESULT ManagedThreadList::TryGetCurrentThreadInfo(ManagedThreadInfo** ppThreadInfo)
{
    // in case of tests, no ICorProfilerInfo is provided
    if (_pCorProfilerInfo == nullptr)
    {
        return E_FAIL;
    }

    ThreadID clrThreadId;
    HRESULT hr = _pCorProfilerInfo->GetCurrentThreadID(&clrThreadId);
    if (FAILED(hr))
    {
        return hr;
    }

    if (clrThreadId == 0)
    {
        return E_FAIL;
    }

    *ppThreadInfo = FindByClrId(clrThreadId);
    if (*ppThreadInfo != nullptr)
    {
        return S_OK;
    }
    else
    {
        return S_FALSE;
    }
}

ManagedThreadInfo* ManagedThreadList::FindByClrId(ThreadID clrThreadId)
{
    // !!! This helper method must be called under the update lock (_mutex) from modifying functions !!!

    if (_threads.empty())
    {
        return nullptr;
    }

    std::unordered_map<ThreadID, ManagedThreadInfo*>::const_iterator elem = _lookupByClrThreadId.find(clrThreadId);
    if (elem == _lookupByClrThreadId.end())
    {
        return nullptr;
    }
    else
    {
        return elem->second;
    }
}