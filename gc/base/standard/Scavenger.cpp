/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 1991, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#if 0
#define OMR_SCAVENGER_DEBUG
#define OMR_SCAVENGER_TRACE
#define OMR_SCAVENGER_TRACE_BACKOUT
#define OMR_SCAVENGER_TRACE_COPY
#define OMR_SCAVENGER_TRACE_REMEMBERED_SET
#endif

#include <math.h>

#include "omrcfg.h"
#include "omrcomp.h"
#include "omrmodroncore.h"
#include "mmomrhook.h"
#include "mmomrhook_internal.h"
#include "modronapicore.hpp"
#include "modronbase.h"
#include "modronopt.h"
#include "ModronAssertions.h"
#include "omr.h"
#include "thread_api.h"

#if defined(OMR_GC_MODRON_SCAVENGER)

#include "AllocateDescription.hpp"
#include "AtomicOperations.hpp"
#include "CollectionStatisticsStandard.hpp"
#include "Collector.hpp"
#include "CollectorLanguageInterface.hpp"
#include "ConfigurationStandard.hpp"
#include "CycleState.hpp"
#include "Dispatcher.hpp"
#include "EnvironmentBase.hpp"
#include "EnvironmentStandard.hpp"
#include "ForwardedHeader.hpp"
#include "IndexableObjectScanner.hpp"
#include "Heap.hpp"
#include "HeapRegionDescriptorStandard.hpp"
#include "HeapRegionIterator.hpp"
#include "HeapRegionManager.hpp"
#include "HeapStats.hpp"
#include "MemoryPool.hpp"
#include "MemorySpace.hpp"
#include "MemorySubSpace.hpp"
#include "MemorySubSpaceRegionIterator.hpp"
#include "MemorySubSpaceRegionIteratorStandard.hpp"
#include "MemorySubSpaceSemiSpace.hpp"
#include "ObjectAllocationInterface.hpp"
#include "ObjectHeapIteratorAddressOrderedList.hpp"
#include "ObjectModel.hpp"
#include "ObjectScanner.hpp"
#include "OMRVMInterface.hpp"
#include "OMRVMThreadListIterator.hpp"
#include "ParallelScavengeTask.hpp"
#include "PhysicalSubArena.hpp"
#include "RSOverflow.hpp"
#include "Scavenger.hpp"
#include "ScavengerBackOutScanner.hpp"
#include "ScavengerRootScanner.hpp"
#include "ScavengerStats.hpp"
#include "SlotObject.hpp"
#include "SublistFragment.hpp"
#include "SublistIterator.hpp"
#include "SublistPool.hpp"
#include "SublistPuddle.hpp"
#include "SublistSlotIterator.hpp"

/* OMRTODO temporary workaround to allow both ut_j9mm.h and ut_omrmm.h to be included.
 *                 Dependency on ut_j9mm.h should be removed in the future.
 */
#undef UT_MODULE_LOADED
#undef UT_MODULE_UNLOADED
#include "ut_omrmm.h"

#define INITIAL_FREE_HISTORY_WEIGHT ((float)0.8)
#define TENURE_BYTES_HISTORY_WEIGHT ((float)0.8)

#define FLIP_TENURE_LARGE_SCAN 4
#define FLIP_TENURE_LARGE_SCAN_DEFERRED 5

/* VM Design 1774: Ideally we would pull these cache line values from the port library but this will suffice for
 * a quick implementation
 */
#if defined(AIXPPC) || defined(LINUXPPC)
#define CACHE_LINE_SIZE 128
#elif defined(J9ZOS390) || (defined(LINUX) && defined(S390))
#define CACHE_LINE_SIZE 256
#else
#define CACHE_LINE_SIZE 64
#endif

/* create macros to interpret the hot field descriptor */
#define HOTFIELD_SHOULD_ALIGN(descriptor) (0x1 == (0x1 & (descriptor)))
#define HOTFIELD_ALIGNMENT_BIAS(descriptor, heapObjectAlignment) (((descriptor) >> 1) * (heapObjectAlignment))

extern "C" {
	uintptr_t allocateMemoryForSublistFragment(void *vmThreadRawPtr, J9VMGC_SublistFragment *fragmentPrimitive);
}

uintptr_t
MM_Scavenger::getVMStateID()
{
	return J9VMSTATE_GC_COLLECTOR_SCAVENGER;
}

void
MM_Scavenger::hookGlobalCollectionStart(J9HookInterface** hook, uintptr_t eventNum, void* eventData, void* userData)
{
	MM_GlobalGCStartEvent *event = (MM_GlobalGCStartEvent *)eventData;
	MM_EnvironmentBase *env = MM_EnvironmentBase::getEnvironment(event->currentThread);

	((MM_Scavenger *)userData)->globalCollectionStart(env);
}

void
MM_Scavenger::hookGlobalCollectionComplete(J9HookInterface** hook, uintptr_t eventNum, void* eventData, void* userData)
{
	MM_GlobalGCEndEvent *event = (MM_GlobalGCEndEvent *)eventData;
	MM_EnvironmentBase *env = MM_EnvironmentBase::getEnvironment(event->currentThread);

	((MM_Scavenger *)userData)->globalCollectionComplete(env);
}

/**
 * Request to create sweepPoolState class for pool
 * @param  memoryPool memory pool to attach sweep state to
 * @return pointer to created class
 */
void *
MM_Scavenger::createSweepPoolState(MM_EnvironmentBase *env, MM_MemoryPool *memoryPool)
{
	Assert_MM_unreachable();
	return NULL;
}

/**
 * Request to destroy sweepPoolState class for pool
 * @param  sweepPoolState class to destroy
 */
void
MM_Scavenger::deleteSweepPoolState(MM_EnvironmentBase *env, void *sweepPoolState)
{
	Assert_MM_unreachable();
}

/**
 * Create a new instance of the receiver.
 * @return a new instance of the receiver or NULL on failure.
 */
MM_Scavenger *
MM_Scavenger::newInstance(MM_EnvironmentStandard *env, MM_CollectorLanguageInterface *cli, MM_HeapRegionManager *regionManager)
{
	MM_Scavenger *scavenger;

	scavenger = (MM_Scavenger *)env->getForge()->allocate(sizeof(MM_Scavenger), MM_AllocationCategory::FIXED, OMR_GET_CALLSITE());
	if (scavenger) {
		new(scavenger) MM_Scavenger(env, cli, regionManager);
		if (!scavenger->initialize(env)) {
			scavenger->kill(env);
			scavenger = NULL;
		}
	}
	return scavenger;
}

/**
 * Destroy and free all resources associated to the receiver.
 */
void
MM_Scavenger::kill(MM_EnvironmentBase *env)
{
	MM_EnvironmentStandard *envStandard = MM_EnvironmentStandard::getEnvironment(env);

	tearDown(envStandard);
	env->getForge()->free(this);
}

/**
 * Initialization
 */
bool
MM_Scavenger::initialize(MM_EnvironmentBase *env)
{
	J9HookInterface** mmOmrHooks = J9_HOOK_INTERFACE(_extensions->omrHookInterface);

	/* Register hook for global GC end. */
	(*mmOmrHooks)->J9HookRegister(mmOmrHooks, J9HOOK_MM_OMR_GLOBAL_GC_START, hookGlobalCollectionStart, (void *)this);
	(*mmOmrHooks)->J9HookRegister(mmOmrHooks, J9HOOK_MM_OMR_GLOBAL_GC_END, hookGlobalCollectionComplete, (void *)this);

	/* initialize the global scavenger gcCount */
	_extensions->scavengerStats._gcCount = 0;
	
	if (!_scavengeCacheFreeList.initialize(env, NULL)) {
		return false;
	}

	if (!_scavengeCacheScanList.initialize(env, &_cachedEntryCount)) {
		return false;
	}

	if (omrthread_monitor_init_with_name(&_scanCacheMonitor, 0, "MM_Scavenger::scanCacheMonitor")) {
		return false;
	}

	/* do not spin when acquiring monitor to notify blocking thread about new work */
	((J9ThreadAbstractMonitor *)_scanCacheMonitor)->flags &= ~J9THREAD_MONITOR_TRY_ENTER_SPIN;

	if (omrthread_monitor_init_with_name(&_freeCacheMonitor, 0, "MM_Scavenger::freeCacheMonitor")) {
		return false;
	}


	/* No thread can use more than _cachesPerThread cache entries at 1 time (flip, tenure, scan, large, possibly deferred)
	 * So long as (N * _cachesPerThread) cache entries exist,the head of the scan list
	 * will contain a valid entry. We set the appropriate number of caches per thread here */
	switch (_extensions->scavengerScanOrdering) {
	case MM_GCExtensionsBase::OMR_GC_SCAVENGER_SCANORDERING_BREADTH_FIRST:
		_cachesPerThread = FLIP_TENURE_LARGE_SCAN;
		break;
	case MM_GCExtensionsBase::OMR_GC_SCAVENGER_SCANORDERING_HIERARCHICAL:
		/* deferred cache is only needed for hierarchical scanning */
		_cachesPerThread = FLIP_TENURE_LARGE_SCAN_DEFERRED;
		break;
	default:
		Assert_MM_unreachable();
		break;
	}

	/**
	 *incrementNewSpaceSize = 
	 *  Xmnx <= 32MB		---> Xmnx
	 *  32MB < Xmnx < 4GB	---> MAX(Xmnx/16, 32MB)
	 *  Xmnx >= 4GB			---> 256MB
	 */
	uintptr_t incrementNewSpaceSize = OMR_MAX(_extensions->maxNewSpaceSize/16, 32*1024*1024);
	incrementNewSpaceSize = OMR_MIN(incrementNewSpaceSize, _extensions->maxNewSpaceSize);
	incrementNewSpaceSize = OMR_MIN(incrementNewSpaceSize, 256*1024*1024);

	uintptr_t incrementCacheCount = incrementNewSpaceSize / _extensions->scavengerScanCacheMinimumSize;
	uintptr_t totalActiveCacheCount = _extensions->heap->getActiveMemorySize(MEMORY_TYPE_NEW) / _extensions->scavengerScanCacheMinimumSize;
	if (0 == totalActiveCacheCount) {
		totalActiveCacheCount += 1;
	}


	if (!_scavengeCacheFreeList.resizeCacheEntries(env, totalActiveCacheCount, incrementCacheCount)) {
		return false;
	}

	_cacheLineAlignment = CACHE_LINE_SIZE;

	return true;
}

void
MM_Scavenger::tearDown(MM_EnvironmentBase *env)
{
	_scavengeCacheFreeList.tearDown(env);
	_scavengeCacheScanList.tearDown(env);

	if (NULL != _scanCacheMonitor) {
		omrthread_monitor_destroy(_scanCacheMonitor);
		_scanCacheMonitor = NULL;
	}

	if (NULL != _freeCacheMonitor) {
		omrthread_monitor_destroy(_freeCacheMonitor);
		_freeCacheMonitor = NULL;
	}

	J9HookInterface** mmOmrHooks = J9_HOOK_INTERFACE(_extensions->omrHookInterface);
	/* Unregister hook for global GC end. */
	(*mmOmrHooks)->J9HookUnregister(mmOmrHooks, J9HOOK_MM_OMR_GLOBAL_GC_START, hookGlobalCollectionStart, (void *)this);
	(*mmOmrHooks)->J9HookUnregister(mmOmrHooks, J9HOOK_MM_OMR_GLOBAL_GC_END, hookGlobalCollectionComplete, (void *)this);
}

/****************************************
 * Initialization routines
 ****************************************
 */

void
MM_Scavenger::setupForGC(MM_EnvironmentBase *env)
{
	/* Make sure the backout state is cleared */
	setBackOutFlag(env, false);

	_rescanThreadsForRememberedObjects = false;
}

void
MM_Scavenger::masterSetupForGC(MM_EnvironmentStandard *env)
{
	_doneIndex = 0;

	/* Reinitialize the copy scan caches */
	Assert_MM_true(_scavengeCacheFreeList.areAllCachesReturned());
	Assert_MM_true(0 == _cachedEntryCount);
	_extensions->copyScanRatio.reset(env, true);

	/* Cache heap ranges for fast "valid object" checks (this can change in an expanding heap situation, so we refetch every cycle) */
	_heapBase = _extensions->heap->getHeapBase();
	_heapTop = _extensions->heap->getHeapTop();

	/* ensure heap base is aligned to region size */
	uintptr_t regionSize = _extensions->heap->getHeapRegionManager()->getRegionSize();
	Assert_MM_true((0 != regionSize) && (0 == ((uintptr_t)_heapBase % regionSize)));

	/* Clear the gc statistics */
	clearGCStats(env);

	/* invoke collector language interface callback */
	_cli->scavenger_masterSetupForGC(env);

	/* Allow expansion in the tenure area on failed promotions (but no resizing on the semispace) */
	_expandTenureOnFailedAllocate = true;
	_activeSubSpace = (MM_MemorySubSpaceSemiSpace *)(env->_cycleState->_activeSubSpace);
	_cachedSemiSpaceResizableFlag = _activeSubSpace->setResizable(false);

	/* Reset the minimum failure sizes */
	_minTenureFailureSize = UDATA_MAX;
	_minSemiSpaceFailureSize = UDATA_MAX;

	/* Find tenure memory sub spaces for collection ( allocate and survivor are context specific) */
	/* Find the allocate, survivor and tenure memory sub spaces for collection */
	_evacuateMemorySubSpace = _activeSubSpace->getMemorySubSpaceAllocate();
	_survivorMemorySubSpace = _activeSubSpace->getMemorySubSpaceSurvivor();
	_tenureMemorySubSpace = _activeSubSpace->getTenureMemorySubSpace();
	
	/* Accumulate pre-scavenge allocation statistics */
	MM_HeapStats heapStatsSemiSpace;
	MM_HeapStats heapStatsTenureSpace;
	MM_ScavengerStats* scavengerStats = &_extensions->scavengerStats;
	_activeSubSpace->mergeHeapStats(&heapStatsSemiSpace);
	_tenureMemorySubSpace->mergeHeapStats(&heapStatsTenureSpace);
	scavengerStats->_tenureSpaceAllocBytesAcumulation += heapStatsTenureSpace._allocBytes;
	scavengerStats->_semiSpaceAllocBytesAcumulation += heapStatsSemiSpace._allocBytes;

	/* Record the tenure mask */
	_tenureMask = calculateTenureMask();
	
	_activeSubSpace->masterSetupForGC(env);
	
	/* evacuate range for GC is what allocate space is for allocation */
	GC_MemorySubSpaceRegionIterator allocateRegionIterator(_evacuateMemorySubSpace);
	MM_HeapRegionDescriptor* region = allocateRegionIterator.nextRegion();
	Assert_MM_true(NULL != region);
	Assert_MM_true(NULL == allocateRegionIterator.nextRegion());
	_evacuateSpaceBase = region->getLowAddress();
	_evacuateSpaceTop = region->getHighAddress();

	/* cache survivor ranges */
	GC_MemorySubSpaceRegionIterator survivorRegionIterator(_survivorMemorySubSpace);
	region = survivorRegionIterator.nextRegion();
	Assert_MM_true(NULL != region);
	Assert_MM_true(NULL == survivorRegionIterator.nextRegion());
	_survivorSpaceBase = region->getLowAddress();
	_survivorSpaceTop = region->getHighAddress();

	/* assume that value of RS Overflow flag will not be changed until scavengeRememberedSet() call, so handle it first */
	_isRememberedSetInOverflowAtTheBeginning = isRememberedSetInOverflowState();
	_extensions->rememberedSet.startProcessingSublist();
}

void
MM_Scavenger::workerSetupForGC(MM_EnvironmentStandard *env)
{
	/* Clear local stats */
	memset((void *)&(env->_scavengerStats), 0, sizeof(MM_ScavengerStats));

	/* Clear the worker hot field statistics */
	clearHotFieldStats(env);

	/* Clear local language-specific stats */
	_cli->scavenger_workerSetupForGC_clearEnvironmentLangStats(env);

	/* record that this thread is participating in this cycle */
	env->_scavengerStats._gcCount = _extensions->scavengerStats._gcCount;

	/* Reset the local remembered set fragment */
	env->_scavengerRememberedSet.count = 0;
	env->_scavengerRememberedSet.fragmentCurrent = NULL;
	env->_scavengerRememberedSet.fragmentTop = NULL;
	env->_scavengerRememberedSet.fragmentSize = (uintptr_t)J9_SCV_REMSET_FRAGMENT_SIZE;
	env->_scavengerRememberedSet.parentList = &_extensions->rememberedSet;

	/* caches should all be reset */
	Assert_MM_true(NULL == env->_survivorCopyScanCache);
	Assert_MM_true(NULL == env->_tenureCopyScanCache);
	Assert_MM_true(NULL == env->_scanCache);
	Assert_MM_true(NULL == env->_deferredScanCache);
	Assert_MM_true(NULL == env->_deferredCopyCache);
	Assert_MM_true(NULL == env->_tenureTLHRemainderBase);
	Assert_MM_true(NULL == env->_tenureTLHRemainderTop);
	Assert_MM_false(env->_loaAllocation);
	Assert_MM_true(NULL == env->_survivorTLHRemainderBase);
	Assert_MM_true(NULL == env->_survivorTLHRemainderTop);
}

/**
 * Run a scavenge.
 */
void
MM_Scavenger::scavenge(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	MM_ParallelScavengeTask scavengeTask(env, _dispatcher, this, env->_cycleState);
	_dispatcher->run(env, &scavengeTask);

	/* remove all scan caches temporary allocated in Heap */
	_scavengeCacheFreeList.removeAllHeapAllocatedChunks(env);

	Assert_MM_true(_scavengeCacheFreeList.areAllCachesReturned());
	Assert_MM_true(0 == _cachedEntryCount);
}

void
MM_Scavenger::reportScavengeStart(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	TRIGGER_J9HOOK_MM_PRIVATE_SCAVENGE_START(
		_extensions->privateHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_PRIVATE_SCAVENGE_START
	);
}

void
MM_Scavenger::reportScavengeEnd(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	bool scavengeSuccessful = scavengeCompletedSuccessfully(env);
	_cli->scavenger_reportScavengeEnd(env, scavengeSuccessful);

	_extensions->scavengerStats._tiltRatio = calculateTiltRatio();

	Trc_MM_Tiltratio(env->getLanguageVMThread(), _extensions->scavengerStats._tiltRatio);

	TRIGGER_J9HOOK_MM_PRIVATE_SCAVENGE_END(
		_extensions->privateHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_PRIVATE_SCAVENGE_END,
		env->_cycleState->_activeSubSpace
	);
}

void
MM_Scavenger::reportGCStart(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	/* TODO CRGTMP deprecate this trace point and add a new one */
	Trc_MM_LocalGCStart(env->getLanguageVMThread(),
		_extensions->globalGCStats.gcCount,
		_extensions->scavengerStats._gcCount,
		0, /* used to be weak reference count */
		0, /* used to be soft reference count */
		0, /* used to be phantom reference count */
		0
	);

	Trc_OMRMM_LocalGCStart(env->getOmrVMThread(),
	_extensions->globalGCStats.gcCount,
	        _extensions->scavengerStats._gcCount,
	        0, /* used to be weak reference count */
	        0, /* used to be soft reference count */
	        0, /* used to be phantom reference count */
	        0
	);

	TRIGGER_J9HOOK_MM_OMR_LOCAL_GC_START(
		_extensions->omrHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_OMR_LOCAL_GC_START,
		_extensions->globalGCStats.gcCount,
		_extensions->scavengerStats._gcCount
	);
}

void
MM_Scavenger::reportGCEnd(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);

	Trc_MM_LocalGCEnd(env->getLanguageVMThread(),
		_extensions->scavengerStats._rememberedSetOverflow,
		_extensions->scavengerStats._causedRememberedSetOverflow,
		_extensions->scavengerStats._scanCacheOverflow,
		_extensions->scavengerStats._failedFlipCount,
		_extensions->scavengerStats._failedFlipBytes,
		_extensions->scavengerStats._failedTenureCount,
		_extensions->scavengerStats._failedTenureBytes,
		_extensions->scavengerStats._flipCount,
		_extensions->scavengerStats._flipBytes,
		_extensions->heap->getApproximateActiveFreeMemorySize(MEMORY_TYPE_NEW),
		_extensions->heap->getActiveMemorySize(MEMORY_TYPE_NEW),
		_extensions->heap->getApproximateActiveFreeMemorySize(MEMORY_TYPE_OLD),
		_extensions->heap->getActiveMemorySize(MEMORY_TYPE_OLD),
		(_extensions-> largeObjectArea ? _extensions->heap->getApproximateActiveFreeLOAMemorySize(MEMORY_TYPE_OLD) : (uintptr_t)0 ),
		(_extensions-> largeObjectArea ? _extensions->heap->getActiveLOAMemorySize(MEMORY_TYPE_OLD) : (uintptr_t)0 ),
		_extensions->scavengerStats._tenureAge
	);

	Trc_OMRMM_LocalGCEnd(env->getOmrVMThread(),
		_extensions->scavengerStats._rememberedSetOverflow,
		_extensions->scavengerStats._causedRememberedSetOverflow,
		_extensions->scavengerStats._scanCacheOverflow,
		_extensions->scavengerStats._failedFlipCount,
		_extensions->scavengerStats._failedFlipBytes,
		_extensions->scavengerStats._failedTenureCount,
		_extensions->scavengerStats._failedTenureBytes,
		_extensions->scavengerStats._flipCount,
		_extensions->scavengerStats._flipBytes,
		_extensions->heap->getApproximateActiveFreeMemorySize(MEMORY_TYPE_NEW),
		_extensions->heap->getActiveMemorySize(MEMORY_TYPE_NEW),
		_extensions->heap->getApproximateActiveFreeMemorySize(MEMORY_TYPE_OLD),
		_extensions->heap->getActiveMemorySize(MEMORY_TYPE_OLD),
		(_extensions-> largeObjectArea ? _extensions->heap->getApproximateActiveFreeLOAMemorySize(MEMORY_TYPE_OLD) : (uintptr_t)0 ),
		(_extensions-> largeObjectArea ? _extensions->heap->getActiveLOAMemorySize(MEMORY_TYPE_OLD) : (uintptr_t)0 ),
		_extensions->scavengerStats._tenureAge
	);

	TRIGGER_J9HOOK_MM_OMR_LOCAL_GC_END(
		_extensions->omrHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_OMR_LOCAL_GC_END,
		env->_cycleState->_activeSubSpace,
		_extensions->globalGCStats.gcCount,
		_extensions->scavengerStats._gcCount,
		_extensions->scavengerStats._rememberedSetOverflow,
		_extensions->scavengerStats._causedRememberedSetOverflow,
		_extensions->scavengerStats._scanCacheOverflow,
		_extensions->scavengerStats._failedFlipCount,
		_extensions->scavengerStats._failedFlipBytes,
		_extensions->scavengerStats._failedTenureCount,
		_extensions->scavengerStats._failedTenureBytes,
		_extensions->scavengerStats._backout,
		_extensions->scavengerStats._flipCount,
		_extensions->scavengerStats._flipBytes,
		_extensions->scavengerStats._tenureAggregateCount,
		_extensions->scavengerStats._tenureAggregateBytes,
		_extensions->tiltedScavenge ? 1 : 0,
		_extensions->heap->getApproximateActiveFreeMemorySize(MEMORY_TYPE_NEW),
		_extensions->heap->getActiveMemorySize(MEMORY_TYPE_NEW),
		_extensions->heap->getApproximateActiveFreeMemorySize(MEMORY_TYPE_OLD),
		_extensions->heap->getActiveMemorySize(MEMORY_TYPE_OLD),
		(_extensions->largeObjectArea ? 1 : 0),
		(_extensions->largeObjectArea ? _extensions->heap->getApproximateActiveFreeLOAMemorySize(MEMORY_TYPE_OLD) : 0),
		(_extensions->largeObjectArea ? _extensions->heap->getActiveLOAMemorySize(MEMORY_TYPE_OLD) :0),
		_extensions->scavengerStats._tenureAge,
		_extensions->heap->getMemorySize()
	);
}

/**
 * Clears Master hot field statistics, if tracing hot fields is enabled.
 */
void
MM_Scavenger::masterClearHotFieldStats()
{
	if (_extensions->scavengerTraceHotFields) {
		_extensions->scavengerHotFieldStats.clear();
	}
}

/**
 * Reports master hot field statistics, if tracing hot fields is enabled.
 * Assumes all the worker thread statistics have been merged.
 */
void
MM_Scavenger::masterReportHotFieldStats()
{
	if (_extensions->scavengerTraceHotFields) {
		_extensions->scavengerHotFieldStats.reportStats(_omrVM);
	}
}

/**
 * Clears hot field statistics for worker, if tracing hot fields is enabled.
 */
void
MM_Scavenger::clearHotFieldStats(MM_EnvironmentStandard *env)
{
	if (_extensions->scavengerTraceHotFields) {
		getHotFieldStats(env)->clear();
	}
}

/**
 * Merges hot field statistics for worker into master, if tracing hot fields is enabled.
 */
void
MM_Scavenger::mergeHotFieldStats(MM_EnvironmentStandard *env)
{
	if (_extensions->scavengerTraceHotFields) {
		_extensions->scavengerHotFieldStats.mergeStats(getHotFieldStats(env));
	}
}

/**
 * Clear any global stats associated to the scavenger.
 */
void
MM_Scavenger::clearGCStats(MM_EnvironmentStandard *env)
{
	_extensions->scavengerStats.clear();
}

/**
 * Merge the current threads scavenge stats into the global scavenge stats.
 */
void
MM_Scavenger::mergeGCStats(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_OMRVM(_omrVM);

	/* Protect the merge with the mutex (this is done by multiple threads in the parallel collector) */
	omrthread_monitor_enter(_extensions->gcStatsMutex);

	MM_ScavengerStats *finalGCStats, *scavStats;
	finalGCStats = &_extensions->scavengerStats;
	scavStats = &env->_scavengerStats;

	finalGCStats->_rememberedSetOverflow |= scavStats->_rememberedSetOverflow;
	finalGCStats->_causedRememberedSetOverflow |= scavStats->_causedRememberedSetOverflow;
	finalGCStats->_scanCacheOverflow |= scavStats->_scanCacheOverflow;
	finalGCStats->_scanCacheAllocationFromHeap |= scavStats->_scanCacheAllocationFromHeap;
	finalGCStats->_scanCacheAllocationDurationDuringSavenger = OMR_MAX(finalGCStats->_scanCacheAllocationDurationDuringSavenger, scavStats->_scanCacheAllocationDurationDuringSavenger);

	finalGCStats->_backout |= scavStats->_backout;
	finalGCStats->_tenureAggregateCount += scavStats->_tenureAggregateCount;
	finalGCStats->_tenureAggregateBytes += scavStats->_tenureAggregateBytes;
#if defined(OMR_GC_LARGE_OBJECT_AREA)
	finalGCStats->_tenureLOACount += scavStats->_tenureLOACount;
	finalGCStats->_tenureLOABytes += scavStats->_tenureLOABytes;
#endif /* OMR_GC_LARGE_OBJECT_AREA */
	finalGCStats->_flipCount += scavStats->_flipCount;
	finalGCStats->_flipBytes += scavStats->_flipBytes;
	finalGCStats->_failedTenureCount += scavStats->_failedTenureCount;
	finalGCStats->_failedTenureBytes += scavStats->_failedTenureBytes;
	finalGCStats->_failedTenureLargest = OMR_MAX(scavStats->_failedTenureLargest,
											 finalGCStats->_failedTenureLargest);
	finalGCStats->_failedFlipCount += scavStats->_failedFlipCount;
	finalGCStats->_failedFlipBytes += scavStats->_failedFlipBytes;

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	finalGCStats->_acquireFreeListCount += scavStats->_acquireFreeListCount;
	finalGCStats->_releaseFreeListCount += scavStats->_releaseFreeListCount;
	finalGCStats->_acquireScanListCount += scavStats->_acquireScanListCount;
	finalGCStats->_releaseScanListCount += scavStats->_releaseScanListCount;
	finalGCStats->_acquireListLockCount += scavStats->_acquireListLockCount;
	finalGCStats->_aliasToCopyCacheCount += scavStats->_aliasToCopyCacheCount;
	finalGCStats->_arraySplitCount += scavStats->_arraySplitCount;
	finalGCStats->_arraySplitAmount += scavStats->_arraySplitAmount;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */

	finalGCStats->_flipDiscardBytes += scavStats->_flipDiscardBytes;
	finalGCStats->_tenureDiscardBytes += scavStats->_tenureDiscardBytes;

	finalGCStats->_survivorTLHRemainderCount += scavStats->_survivorTLHRemainderCount;
	finalGCStats->_tenureTLHRemainderCount += scavStats->_tenureTLHRemainderCount;

	finalGCStats->_semiSpaceAllocationCountLarge += scavStats->_semiSpaceAllocationCountLarge;
	finalGCStats->_semiSpaceAllocationCountSmall += scavStats->_semiSpaceAllocationCountSmall;

	finalGCStats->_tenureSpaceAllocationCountLarge += scavStats->_tenureSpaceAllocationCountLarge;
	finalGCStats->_tenureSpaceAllocationCountSmall += scavStats->_tenureSpaceAllocationCountSmall;

	if (env->isMasterThread()) {
		finalGCStats->getFlipHistory(0)->_tenureMask = _tenureMask;
		uintptr_t tenureAge = 0;
		for (tenureAge = 0; tenureAge <= OBJECT_HEADER_AGE_MAX; ++tenureAge) {
			if (_tenureMask & ((uintptr_t)1 << tenureAge)) {
				break;
			}
		}
		finalGCStats->_tenureAge = tenureAge;

		MM_ScavengerStats::FlipHistory* flipHistoryPrevious = finalGCStats->getFlipHistory(1);
		flipHistoryPrevious->_flipBytes[0] = finalGCStats->_semiSpaceAllocBytesAcumulation;
		flipHistoryPrevious->_tenureBytes[0] = finalGCStats->_tenureSpaceAllocBytesAcumulation;

		finalGCStats->_semiSpaceAllocBytesAcumulation = 0;
		finalGCStats->_tenureSpaceAllocBytesAcumulation = 0;
	}

	for (int i = 1; i <= OBJECT_HEADER_AGE_MAX+1; ++i) {
		finalGCStats->getFlipHistory(0)->_flipBytes[i] += scavStats->getFlipHistory(0)->_flipBytes[i];
		finalGCStats->getFlipHistory(0)->_tenureBytes[i] += scavStats->getFlipHistory(0)->_tenureBytes[i];
	}

	finalGCStats->_tenureExpandedBytes += scavStats->_tenureExpandedBytes;
	finalGCStats->_tenureExpandedCount += scavStats->_tenureExpandedCount;
	finalGCStats->_tenureExpandedTime += scavStats->_tenureExpandedTime;

	for (uintptr_t i = 0; i < OMR_SCAVENGER_DISTANCE_BINS; i++) {
		finalGCStats->_copy_distance_counts[i] += scavStats->_copy_distance_counts[i];
	}
	for (uintptr_t i = 0; i < OMR_SCAVENGER_CACHESIZE_BINS; i++) {
		finalGCStats->_copy_cachesize_counts[i] += scavStats->_copy_cachesize_counts[i];
	}
	finalGCStats->_leafObjectCount += scavStats->_leafObjectCount;
	finalGCStats->_copy_cachesize_sum += scavStats->_copy_cachesize_sum;
	finalGCStats->_workStallTime += scavStats->_workStallTime;
	finalGCStats->_completeStallTime += scavStats->_completeStallTime;
	finalGCStats->_syncStallTime += scavStats->_syncStallTime;
	finalGCStats->_workStallCount += scavStats->_workStallCount;
	finalGCStats->_completeStallCount += scavStats->_completeStallCount;
	_extensions->scavengerStats._syncStallCount += scavStats->_syncStallCount;

	if (_extensions->scavengerTraceHotFields) {
		_extensions->scavengerHotFieldStats.mergeStats(&(env->_hotFieldStats));
	}

	/* Merge language specific statistics */
	_cli->scavenger_mergeGCStats_mergeLangStats(env);

	omrthread_monitor_exit(_extensions->gcStatsMutex);

	/* record the thread-specific parallelism stats in the trace buffer. This aprtially duplicates info in -Xtgc:parallel */
	Trc_MM_ParallelScavenger_parallelStats(
		env->getLanguageVMThread(),
		(uint32_t)env->getSlaveID(),
		(uint32_t)omrtime_hires_delta(0, scavStats->_workStallTime, OMRPORT_TIME_DELTA_IN_MILLISECONDS),
		(uint32_t)omrtime_hires_delta(0, scavStats->_completeStallTime, OMRPORT_TIME_DELTA_IN_MILLISECONDS),
		(uint32_t)omrtime_hires_delta(0, scavStats->_syncStallTime, OMRPORT_TIME_DELTA_IN_MILLISECONDS),
		(uint32_t)scavStats->_workStallCount,
		(uint32_t)scavStats->_completeStallCount,
		(uint32_t)scavStats->_syncStallCount,
		scavStats->_acquireFreeListCount,
		scavStats->_releaseFreeListCount,
		scavStats->_acquireScanListCount,
		scavStats->_releaseScanListCount);
}

/**
 * Determine whether GC stats should be calculated for this round.
 * @return true if GC stats should be calculated for this round, false otherwise.
 */
bool
MM_Scavenger::canCalcGCStats(MM_EnvironmentStandard *env)
{
	/* If no backout and we actually did a scavenge this time around then it's safe to gather stats */
	return !backOutFlagRaised() && (0 < _extensions->heap->getPercolateStats()->getScavengesSincePercolate());
}

/**
 * Calculate any GC stats after a collection.
 */
void
MM_Scavenger::calcGCStats(MM_EnvironmentStandard *env)
{
	/* Do not calculate stats unless we actually collected */
	if (canCalcGCStats(env)) {
		MM_ScavengerStats *scavengerGCStats;
		scavengerGCStats = &_extensions->scavengerStats;
		uintptr_t initialFree = env->_cycleState->_activeSubSpace->getActualActiveFreeMemorySize();

		/* First collection  ? */
		if (scavengerGCStats->_gcCount > 1 ) {
			scavengerGCStats->_avgInitialFree = (uintptr_t)MM_Math::weightedAverage((float)scavengerGCStats->_avgInitialFree, (float)initialFree, INITIAL_FREE_HISTORY_WEIGHT);
			scavengerGCStats->_avgTenureBytes = (uintptr_t)MM_Math::weightedAverage((float)scavengerGCStats->_avgTenureBytes, (float)scavengerGCStats->_tenureAggregateBytes, TENURE_BYTES_HISTORY_WEIGHT);
#if defined(OMR_GC_LARGE_OBJECT_AREA)
			scavengerGCStats->_avgTenureSOABytes = (uintptr_t)MM_Math::weightedAverage((float)scavengerGCStats->_avgTenureSOABytes,
																		(float)(scavengerGCStats->_tenureAggregateBytes - scavengerGCStats->_tenureLOABytes),
																		TENURE_BYTES_HISTORY_WEIGHT);
			scavengerGCStats->_avgTenureLOABytes = (uintptr_t)MM_Math::weightedAverage((float)scavengerGCStats->_avgTenureLOABytes,
																		(float)scavengerGCStats->_tenureLOABytes,
																		TENURE_BYTES_HISTORY_WEIGHT);

#endif /* OMR_GC_LARGE_OBJECT_AREA */
		} else {
			scavengerGCStats->_avgInitialFree = initialFree;
			scavengerGCStats->_avgTenureBytes = scavengerGCStats->_tenureAggregateBytes;
#if defined(OMR_GC_LARGE_OBJECT_AREA)
			scavengerGCStats->_avgTenureSOABytes = scavengerGCStats->_tenureAggregateBytes - scavengerGCStats->_tenureLOABytes;
			scavengerGCStats->_avgTenureLOABytes = scavengerGCStats->_tenureLOABytes;
#endif /* OMR_GC_LARGE_OBJECT_AREA */
		}
	}
}

/****************************************
 * Copy/forward routines
 ****************************************
 */

/**
 * Calculate optimum copyscancache size.
 * @return the optimum copyscancache size
 */
MMINLINE uintptr_t
MM_Scavenger::calculateOptimumSurvivorSpaceCopyScanCacheSize(MM_EnvironmentStandard *env)
{
	/* scale down maximal scan cache size using wait/copy/scan factor and round up to nearest tlh size */
	uintptr_t scaleSize = (uintptr_t)(_extensions->copyScanRatio.getScalingFactor(env) * _extensions->scavengerScanCacheMaximumSize);
	uintptr_t cacheSize = MM_Math::roundToCeiling(_extensions->tlhMinimumSize, scaleSize);

	/* Fit result into allowable cache size range */
	if (cacheSize < _extensions->scavengerScanCacheMinimumSize) {
		cacheSize = _extensions->scavengerScanCacheMinimumSize;
	} else if (cacheSize > _extensions->scavengerScanCacheMaximumSize) {
		cacheSize = _extensions->scavengerScanCacheMaximumSize;
	}

	env->_scavengerStats.countCopyCacheSize(cacheSize, _extensions->scavengerScanCacheMaximumSize);

#if defined(OMR_SCAVENGER_TRACE)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	omrtty_printf("{SCAV: scanCacheSize %zu}\n", cacheSize);
#endif /* OMR_SCAVENGER_TRACE */

	return cacheSize;
}

MMINLINE MM_CopyScanCacheStandard *
MM_Scavenger::reserveMemoryForAllocateInSemiSpace(MM_EnvironmentStandard *env, omrobjectptr_t objectToEvacuate, uintptr_t objectReserveSizeInBytes)
{
	void* addrBase = NULL;
	void* addrTop = NULL;
	MM_CopyScanCacheStandard *copyCache = NULL;
	uintptr_t cacheSize = objectReserveSizeInBytes;

	Assert_MM_objectAligned(env, objectReserveSizeInBytes);

	/*
	 * Please note that condition like (top >= start + size) might cause wrong functioning due overflow
	 * so to be safe (top - start >= size) must be used
	 */
	if ((NULL != env->_survivorCopyScanCache) && (((uintptr_t)env->_survivorCopyScanCache->cacheTop - (uintptr_t)env->_survivorCopyScanCache->cacheAlloc) >= cacheSize)) {
		/* A survivor copy scan cache exists and there is a room, use the current copy cache */
		copyCache = env->_survivorCopyScanCache;
	} else {
		/* The copy cache was null or did not have enough room */
		/* Try and allocate room for the copy - if successful, flush the old cache */
		bool allocateResult = false;
		if (objectReserveSizeInBytes < _minSemiSpaceFailureSize) {
			/* try to use TLH remainder from previous discard */
			if (((uintptr_t)env->_survivorTLHRemainderTop - (uintptr_t)env->_survivorTLHRemainderBase) >= cacheSize) {
				Assert_MM_true(NULL != env->_survivorTLHRemainderBase);
				allocateResult = true;
				addrBase = env->_survivorTLHRemainderBase;
				addrTop = env->_survivorTLHRemainderTop;
				env->_survivorTLHRemainderBase = NULL;
				env->_survivorTLHRemainderTop = NULL;
			} else if (_extensions->tlhSurvivorDiscardThreshold < cacheSize) {
				MM_AllocateDescription allocDescription(cacheSize, 0, false, true);

				addrBase = _survivorMemorySubSpace->collectorAllocate(env, this, &allocDescription);
				if(NULL != addrBase) {
					addrTop = (void *)(((uint8_t *)addrBase) + cacheSize);
					/* Check that there is no overflow */
					Assert_MM_true(addrTop >= addrBase);
					allocateResult = true;
				}
				env->_scavengerStats._semiSpaceAllocationCountLarge += 1;
			} else {
				MM_AllocateDescription allocDescription(0, 0, false, true);
				/* Update the optimum scan cache size */
				uintptr_t scanCacheSize = calculateOptimumSurvivorSpaceCopyScanCacheSize(env);
				allocateResult = (NULL != _survivorMemorySubSpace->collectorAllocateTLH(env, this, &allocDescription, scanCacheSize, addrBase, addrTop));
				env->_scavengerStats._semiSpaceAllocationCountSmall += 1;
			}
		}

		if(allocateResult) {
			/* A new chunk has been allocated - refresh the copy cache */

			/* release local cache first. along the path we may realize that a cache structure can be re-used */
			MM_CopyScanCacheStandard *cacheToReuse = releaseLocalCopyCache(env, env->_survivorCopyScanCache);

			if (NULL == cacheToReuse) {
				/* So, we need a new cache - try to get reserved one*/
				copyCache = getFreeCache(env);
			} else {
				copyCache = cacheToReuse;
			}

			if (NULL != copyCache) {
#if defined(OMR_SCAVENGER_TRACE)
				OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
				omrtty_printf("{SCAV: Semispace cache allocated (%p) %p-%p}\n", copyCache, addrBase, addrTop);
#endif /* OMR_SCAVENGER_TRACE */

				/* clear all flags except "allocated in heap" might be set already*/
				copyCache->flags &= OMR_SCAVENGER_CACHE_TYPE_HEAP;
				copyCache->flags |= OMR_SCAVENGER_CACHE_TYPE_SEMISPACE | OMR_SCAVENGER_CACHE_TYPE_COPY;
				reinitCache(copyCache, addrBase, addrTop);
			} else {
				/* can not allocate a copyCache header, release allocated memory */
				/* return memory to pool */
				_survivorMemorySubSpace->abandonHeapChunk(addrBase, addrTop);
			}

			env->_survivorCopyScanCache = copyCache;
		} else {
			/* Can not allocate requested memory in survivor subspace */
			/* Record size to reduce multiple failure attempts
			 * NOTE: Since this is used across multiple threads there is a race condition between checking and setting
			 * the minimum.  This means that this value may not actually be the lowest value, or may increase.
			 */
			if (cacheSize < _minSemiSpaceFailureSize) {
				_minSemiSpaceFailureSize = cacheSize;
			}

			/* Record stats */
			env->_scavengerStats._failedFlipCount += 1;
			env->_scavengerStats._failedFlipBytes += objectReserveSizeInBytes;
		}
	}

	return copyCache;
}

MM_CopyScanCacheStandard *
MM_Scavenger::reserveMemoryForAllocateInTenureSpace(MM_EnvironmentStandard *env, omrobjectptr_t objectToEvacuate, uintptr_t objectReserveSizeInBytes)
{
	void* addrBase = NULL;
	void* addrTop = NULL;
	MM_CopyScanCacheStandard *copyCache = NULL;
	bool satisfiedInLOA = false;
	uintptr_t cacheSize = objectReserveSizeInBytes;

	Assert_MM_objectAligned(env, objectReserveSizeInBytes);

	/*
	 * Please note that condition like (top >= start + size) might cause wrong functioning due overflow
	 * so to be safe (top - start >= size) must be used
	 */
	if ((NULL != env->_tenureCopyScanCache) && (((uintptr_t)env->_tenureCopyScanCache->cacheTop - (uintptr_t)env->_tenureCopyScanCache->cacheAlloc) >= cacheSize)) {
		/* A tenure copy scan cache exists and there is a room, use the current copy cache */
		copyCache = env->_tenureCopyScanCache;
	} else {
		/* The copy cache was null or did not have enough room */
		/* Try and allocate room for the copy - if successful, flush the old cache */
		bool allocateResult = false;
		if (cacheSize < _minTenureFailureSize) {
			/* try to use TLH remainder from previous discard. */
			if (((uintptr_t)env->_tenureTLHRemainderTop - (uintptr_t)env->_tenureTLHRemainderBase) >= cacheSize) {
				Assert_MM_true(NULL != env->_tenureTLHRemainderBase);
				allocateResult = true;
				addrBase = env->_tenureTLHRemainderBase;
				addrTop = env->_tenureTLHRemainderTop;
				satisfiedInLOA = env->_loaAllocation;
				env->_tenureTLHRemainderBase = NULL;
				env->_tenureTLHRemainderTop = NULL;
				env->_loaAllocation = false;
			} else if (_extensions->tlhTenureDiscardThreshold < cacheSize) {
				MM_AllocateDescription allocDescription(cacheSize, 0, false, true);
				allocDescription.setCollectorAllocateExpandOnFailure(true);
				addrBase = _tenureMemorySubSpace->collectorAllocate(env, this, &allocDescription);
				if(NULL != addrBase) {
					addrTop = (void *)(((uint8_t *)addrBase) + cacheSize);
					/* Check that there is no overflow */
					Assert_MM_true(addrTop >= addrBase);
					allocateResult = true;

#if defined(OMR_GC_LARGE_OBJECT_AREA)
					if (allocDescription.isLOAAllocation()) {
						satisfiedInLOA = true;
					}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
				}
				env->_scavengerStats._tenureSpaceAllocationCountLarge += 1;
			} else {
				MM_AllocateDescription allocDescription(0, 0, false, true);
				allocDescription.setCollectorAllocateExpandOnFailure(true);
				allocateResult = (NULL != _tenureMemorySubSpace->collectorAllocateTLH(env, this, &allocDescription, _extensions->tlhMaximumSize, addrBase, addrTop));

#if defined(OMR_GC_LARGE_OBJECT_AREA)
				if (allocateResult && allocDescription.isLOAAllocation()) {
					satisfiedInLOA = true;
				}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
				env->_scavengerStats._tenureSpaceAllocationCountSmall += 1;
			}
		}

		if(allocateResult) {
			/* A new chunk has been allocated - refresh the copy cache */

			/* release local cache first. along the path we may realize that a cache structure can be re-used */
			MM_CopyScanCacheStandard *cacheToReuse = releaseLocalCopyCache(env, env->_tenureCopyScanCache);

			if (NULL == cacheToReuse) {
				/* So, we need a new cache - try to get reserved one*/
				copyCache = getFreeCache(env);
			} else {
				copyCache = cacheToReuse;
			}

			if (NULL != copyCache) {
#if defined(OMR_SCAVENGER_TRACE)
				OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
				omrtty_printf("{SCAV: Tenure cache allocated (%p) %p-%p}\n", copyCache, addrBase, addrTop);
#endif /* OMR_SCAVENGER_TRACE */

				/* clear all flags except "allocated in heap" might be set already*/
				copyCache->flags &= OMR_SCAVENGER_CACHE_TYPE_HEAP;
				copyCache->flags |= OMR_SCAVENGER_CACHE_TYPE_TENURESPACE | OMR_SCAVENGER_CACHE_TYPE_COPY;

#if defined(OMR_GC_LARGE_OBJECT_AREA)
				if (satisfiedInLOA) {
					copyCache->flags |= OMR_SCAVENGER_CACHE_TYPE_LOA;
				}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
				reinitCache(copyCache, addrBase, addrTop);
			} else {
				/* can not allocate a copyCache header, release allocated memory */
				/* return memory to pool */
				_tenureMemorySubSpace->abandonHeapChunk(addrBase, addrTop);
			}

			env->_tenureCopyScanCache = copyCache;

		} else {
			/* Can not allocate requested memory in tenure subspace */
			/* Record size to reduce multiple failure attempts
			 * NOTE: Since this is used across multiple threads there is a race condition between checking and setting
			 * the minimum.  This means that this value may not actually be the lowest value, or may increase.
			 */
			if (cacheSize < _minTenureFailureSize) {
				_minTenureFailureSize = cacheSize;
			}

			/* Record stats */
			env->_scavengerStats._failedTenureCount += 1;
			env->_scavengerStats._failedTenureBytes += objectReserveSizeInBytes;
			env->_scavengerStats._failedTenureLargest = OMR_MAX(objectReserveSizeInBytes,
			env->_scavengerStats._failedTenureLargest);
		}
	}

	return copyCache;
}

/**
 * Update the given slot to point at the new location of the object, after copying
 * the object if it was not already.
 * Attempt to copy (either flip or tenure) the object and install a forwarding
 * pointer at the new location. The object may have already been copied. In
 * either case, update the slot to point at the new location of the object.
 *
 * @param objectPtrIndirect the slot to be updated
 * @return true if the new location of the object is in new space
 * @return false otherwise
 */
MMINLINE bool
MM_Scavenger::copyAndForward(MM_EnvironmentStandard *env, volatile omrobjectptr_t *objectPtrIndirect)
{
	bool toReturn = false;

	/* clear effectiveCopyCache to support aliasing check -- will be updated if copy actually takes place */
	env->_effectiveCopyScanCache = NULL;

	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if (NULL != objectPtr) {
		if (isObjectInEvacuateMemory(objectPtr)) {
			/* Object needs to be copy and forwarded.  Check if the work has already been done */
			MM_ForwardedHeader forwardHeader(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
			omrobjectptr_t forwardPtr = forwardHeader.getForwardedObject();

			if (NULL != forwardPtr) {
				/* Object has been copied - update the forwarding information and return */
				*objectPtrIndirect = forwardPtr;
				toReturn = isObjectInNewSpace(forwardPtr);
			} else {
				omrobjectptr_t destinationObjectPtr = copy(env, &forwardHeader);
				if (NULL == destinationObjectPtr) {
					/* Failure - the scavenger must back out the work it has done. */
					/* raise the alert and return (true - must look like a new object was handled) */
					toReturn = true;
				} else {
					/* Update the slot */
					*objectPtrIndirect = destinationObjectPtr;
					toReturn = isObjectInNewSpace(destinationObjectPtr);
				}
			}
		} else if (isObjectInNewSpace(objectPtr)) {
#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
			MM_ForwardedHeader forwardHeader(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
			Assert_MM_true(!forwardHeader.isForwardedPointer());
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */
			/* When slot has been scanned before, and is already copied or forwarded
			 * for example when the partial scan state of a cache has been lost in scan cache overflow
			 */
			toReturn = true;
#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
		} else {
			Assert_MM_true(_extensions->isOld(objectPtr));
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */
		}
	}

	return toReturn;
}

/**
 * Update the given slot to point at the new location of the object, after copying
 * the object if it was not already.
 * Attempt to copy (either flip or tenure) the object and install a forwarding
 * pointer at the new location. The object may have already been copied. In
 * either case, update the slot to point at the new location of the object.
 *
 * @param slotObject the slot to be updated
 * @return true if the new location of the object is in new space
 * @return false otherwise
 */
MMINLINE bool
MM_Scavenger::copyAndForward(MM_EnvironmentStandard *env, GC_SlotObject *slotObject)
{
	omrobjectptr_t slot = slotObject->readReferenceFromSlot();
	bool result = copyAndForward(env, &slot);
	slotObject->writeReferenceToSlot(slot);

	if (NULL != env->_effectiveCopyScanCache) {
		env->_scavengerStats.countCopyDistance((uintptr_t)slotObject->readAddressFromSlot(), (uintptr_t)slotObject->readReferenceFromSlot());
	}

	return result;
}

bool
MM_Scavenger::copyObjectSlot(MM_EnvironmentStandard *env, volatile omrobjectptr_t *slotPtr)
{
	return copyAndForward(env, slotPtr);
}

bool
MM_Scavenger::copyObjectSlot(MM_EnvironmentStandard *env, GC_SlotObject *slotObject)
{
	return copyAndForward(env, slotObject);
}

omrobjectptr_t
MM_Scavenger::copyObject(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader)
{
	return copy(env, forwardedHeader);
}

omrobjectptr_t
MM_Scavenger::copy(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader)
{
	omrobjectptr_t destinationObjectPtr;
	uintptr_t objectCopySizeInBytes, objectReserveSizeInBytes;
	uintptr_t hotFieldsDescriptor = 0;
#if defined(J9VM_INTERP_NATIVE_SUPPORT)
	uintptr_t hotFieldsAlignment = 0;
	uintptr_t* hotFieldPadBase = NULL;
	uintptr_t hotFieldPadSize = 0;
#endif /* defined(J9VM_INTERP_NATIVE_SUPPORT) */
	MM_CopyScanCacheStandard *copyCache;
	void *newCacheAlloc;

	/* Try and find memory for the object based on its age */
	uintptr_t objectAge = _extensions->objectModel.getPreservedAge(forwardedHeader);
	uintptr_t oldObjectAge = objectAge;

	/* Object is in the evacuate space but not forwarded. */
	_extensions->objectModel.calculateObjectDetailsForCopy(forwardedHeader, &objectCopySizeInBytes, &objectReserveSizeInBytes, &hotFieldsDescriptor);

	Assert_MM_objectAligned(env, objectReserveSizeInBytes);

	if (0 == (((uintptr_t)1 << objectAge) & _tenureMask)) {
		/* The object should be flipped - try to reserve room in the semi space */
		copyCache = reserveMemoryForAllocateInSemiSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
		if (NULL != copyCache) {
			/* Adjust the age value*/
			if(objectAge < OBJECT_HEADER_AGE_MAX) {
				objectAge += 1;
			}
		} else {
			Trc_MM_Scavenger_semispaceAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, "yes");

			uintptr_t spaceAvailableForObject = _activeSubSpace->getMaxSpaceForObjectInEvacuateMemory(forwardedHeader->getObject());
			Assert_GC_true_with_message4(env, objectCopySizeInBytes <= spaceAvailableForObject,
					"Corruption in Evacuate at %p: calculated object size %zu larger then available %zu, Forwarded Header at %p\n",
					forwardedHeader->getObject(), objectCopySizeInBytes, spaceAvailableForObject, forwardedHeader);

			copyCache = reserveMemoryForAllocateInTenureSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
			if (NULL != copyCache) {
				/* Clear age and set the old bit */
				objectAge = STATE_NOT_REMEMBERED;
			} else {
				Trc_MM_Scavenger_tenureAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, env->_scavengerStats._failedTenureLargest, "no");
			}
		}
	} else {
		/* Move straight to tenuring on the object */
#if defined(J9VM_INTERP_NATIVE_SUPPORT)
		/* adjust the reserved object's size if we are aligning hot fields and this class has a known hot field */
		if (_extensions->scavengerAlignHotFields && HOTFIELD_SHOULD_ALIGN(hotFieldsDescriptor)) {
			/* this optimization is a source of fragmentation (alloc request size always assumes maximum padding,
			 * but free entry created by sweep in tenure could be less than that (since some of unused padding can overlap with next copied object)).
			 * we limit this optimization for arrays up to the size of 2 cache lines, beyond which the benefits of the optimization are believed to be non-existant */
            if (!_extensions->objectModel.isIndexable(forwardedHeader) || (objectReserveSizeInBytes <= 2 * _cacheLineAlignment)) {
				/* set the descriptor field if we should be aligning (since assuming that 0 means no is not safe) */
				hotFieldsAlignment = hotFieldsDescriptor;
				/* for simplicity, add the maximum padding we could need (and back off after allocation) */
				objectReserveSizeInBytes += (_cacheLineAlignment - _objectAlignmentInBytes);
				Assert_MM_objectAligned(env, objectReserveSizeInBytes);
            }
		}
#endif /* J9VM_INTERP_NATIVE_SUPPORT */
		copyCache = reserveMemoryForAllocateInTenureSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
		if (NULL != copyCache) {
			/* Clear age and set the old bit */
			objectAge = STATE_NOT_REMEMBERED;
		} else {
			Trc_MM_Scavenger_tenureAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, env->_scavengerStats._failedTenureLargest, "yes");

			uintptr_t spaceAvailableForObject = _activeSubSpace->getMaxSpaceForObjectInEvacuateMemory(forwardedHeader->getObject());
			Assert_GC_true_with_message4(env, objectCopySizeInBytes <= spaceAvailableForObject,
					"Corruption in Evacuate at %p: calculated object size %zu larger then available %zu, Forwarded Header at %p\n",
					forwardedHeader->getObject(), objectCopySizeInBytes, spaceAvailableForObject, forwardedHeader);

			copyCache = reserveMemoryForAllocateInSemiSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
			if (NULL != copyCache) {
				/* Adjust the age value*/
				if(objectAge < OBJECT_HEADER_AGE_MAX) {
					objectAge += 1;
				} else {
					Trc_MM_Scavenger_semispaceAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, "no");
				}
			}
		}
	}

	/* Check if memory was reserved successfully */
	if(NULL == copyCache) {
		/* Failure - the scavenger must back out the work it has done. */
		/* raise the alert and return (with NULL) */
		setBackOutFlag(env, true);
		omrthread_monitor_enter(_scanCacheMonitor);
		if(_waitingCount) {
			omrthread_monitor_notify_all(_scanCacheMonitor);
		}
		omrthread_monitor_exit(_scanCacheMonitor);
		return NULL;
	}

	/* Memory has been reserved */
	destinationObjectPtr = (omrobjectptr_t)copyCache->cacheAlloc;
	/* now correct for the hot field alignment */
#if defined(J9VM_INTERP_NATIVE_SUPPORT)
	if (0 != hotFieldsAlignment) {
		uintptr_t remainingInCacheLine = _cacheLineAlignment - ((uintptr_t)destinationObjectPtr % _cacheLineAlignment);
		uintptr_t alignmentBias = HOTFIELD_ALIGNMENT_BIAS(hotFieldsAlignment, _objectAlignmentInBytes);
		/* do alignment only if the object cannot fit in the remaining space in the cache line */
		if ((remainingInCacheLine < objectCopySizeInBytes) && (alignmentBias < remainingInCacheLine)) {
			hotFieldPadSize = ((remainingInCacheLine + _cacheLineAlignment) - (alignmentBias % _cacheLineAlignment)) % _cacheLineAlignment;
			hotFieldPadBase = (uintptr_t *)destinationObjectPtr;
			/* now fix the object pointer so that the hot field is aligned */
			destinationObjectPtr = (omrobjectptr_t)((uintptr_t)destinationObjectPtr + hotFieldPadSize);
		}
		/* and update the reserved size so that we "un-reserve" the extra memory we said we might need.  This is done by
		 * removing the excess reserve since we already accounted for the hotFieldPadSize by bumping the destination pointer
		 * and now we need to revert to the amount needed for the object allocation and its array alignment so the rest of
		 * the method continues to function without needing to know about this extra alignment calculation
		 */
		objectReserveSizeInBytes = objectReserveSizeInBytes - (_cacheLineAlignment - _objectAlignmentInBytes);
	}
#endif /* J9VM_INTERP_NATIVE_SUPPORT */

	/* and correct for the double array alignment */
	newCacheAlloc = (void *) (((uint8_t *)destinationObjectPtr) + objectReserveSizeInBytes);

	/* Try to swap the forwarding pointer to the destination copy array into the source object */
	omrobjectptr_t originalDestinationObjectPtr = destinationObjectPtr;
	destinationObjectPtr = forwardedHeader->setForwardedObject(destinationObjectPtr);
	if (destinationObjectPtr == originalDestinationObjectPtr) {
		/* Succeeded in forwarding the object - copy and adjust the age value */
#if defined(OMR_SCAVENGER_TRACE_COPY)
		OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
		omrtty_printf("{SCAV: Copied %p -> %p}\n", objectPtr, destinationObjectPtr);
#endif /* OMR_SCAVENGER_TRACE_COPY */

#if defined(J9VM_INTERP_NATIVE_SUPPORT)
		if (NULL != hotFieldPadBase) {
			/* lay down a hole (XXX:  This assumes that we are using AOL (address-ordered-list)) */
			MM_HeapLinkedFreeHeader::fillWithHoles(hotFieldPadBase, hotFieldPadSize);
		}
#endif /* J9VM_INTERP_NATIVE_SUPPORT */

		memcpy((void *)destinationObjectPtr, forwardedHeader->getObject(), objectCopySizeInBytes);

		_extensions->objectModel.fixupForwardedObject(forwardedHeader, destinationObjectPtr, objectAge);

		/* Move the cache allocate pointer to reflect the consumed memory */
		assume0(copyCache->cacheAlloc <= copyCache->cacheTop);
		copyCache->cacheAlloc = newCacheAlloc;
		assume0(copyCache->cacheAlloc <= copyCache->cacheTop);

		/* object has been copied so if scanning hierarchically set effectiveCopyCache to support aliasing check */
		env->_effectiveCopyScanCache = copyCache;

		/* Update the stats */
		MM_ScavengerStats *scavStats = &env->_scavengerStats;
		if(copyCache->flags & OMR_SCAVENGER_CACHE_TYPE_TENURESPACE) {
			scavStats->_tenureAggregateCount += 1;
			scavStats->_tenureAggregateBytes += objectCopySizeInBytes;
			scavStats->getFlipHistory(0)->_tenureBytes[oldObjectAge + 1] += objectReserveSizeInBytes;
#if defined(OMR_GC_LARGE_OBJECT_AREA)
			if (copyCache->flags & OMR_SCAVENGER_CACHE_TYPE_LOA) {
				scavStats->_tenureLOACount += 1;
				scavStats->_tenureLOABytes += objectCopySizeInBytes;
			}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
		} else {
			Assert_MM_true(copyCache->flags & OMR_SCAVENGER_CACHE_TYPE_SEMISPACE);
			scavStats->_flipCount += 1;
			scavStats->_flipBytes += objectCopySizeInBytes;
			scavStats->getFlipHistory(0)->_flipBytes[oldObjectAge + 1] += objectReserveSizeInBytes;
		}
	} else {
		/* We have not used the reserved space now, but we will for subsequent allocations. If this space was reserved for an individual object,
		 * we might have created a TLH remainder from previous cache just before reserving this space. This space eventaully can create another remainder.
		 * At that point, ideally (to recycle as much memory as possibly) we could enqueue this remainder, but as a simple solution we will now abandon
		 * the current remainder (we assert across the code, there is at most one at a give point of time).
		 * If we see large amount of discards even with low discard threshold, we may reconsider enqueueing discarded TLHs.
		 */
		if (copyCache->flags & OMR_SCAVENGER_CACHE_TYPE_TENURESPACE) {
			abandonTenureTLHRemainder(env);
		} else if (copyCache->flags & OMR_SCAVENGER_CACHE_TYPE_SEMISPACE) {
			abandonSurvivorTLHRemainder(env);
		} else {
			Assert_MM_unreachable();
		}
	}
	/* return value for updating the slot */
	return destinationObjectPtr;
}

/****************************************
 * Object scan and copy routines
 ****************************************
 */

uintptr_t
MM_Scavenger::getArraySplitAmount(MM_EnvironmentStandard *env, uintptr_t sizeInElements)
{
	uintptr_t scvArraySplitAmount = 0;

	if (!backOutFlagRaised()) {
		/* pointer arrays are split into segments to improve parallelism. split amount is proportional to array size.
		 * the less busy we are, the smaller the split amount, while obeying specified minimum and maximum.
		 * but for single-threaded backout, do not split arrays.
		 */
		scvArraySplitAmount = sizeInElements / (_dispatcher->activeThreadCount() + 2 * _waitingCount);
		scvArraySplitAmount = OMR_MAX(scvArraySplitAmount, _extensions->scvArraySplitMinimumAmount);
		scvArraySplitAmount = OMR_MIN(scvArraySplitAmount, _extensions->scvArraySplitMaximumAmount);
	}

	return scvArraySplitAmount;
}

bool
MM_Scavenger::splitIndexableObjectScanner(MM_EnvironmentStandard *env, GC_ObjectScanner *objectScanner, uintptr_t startIndex, omrobjectptr_t *rememberedSetSlot)
{
	bool result = false;

	Assert_MM_true(objectScanner->isIndexableObject());
	GC_IndexableObjectScanner *indexableScanner = (GC_IndexableObjectScanner *)objectScanner;
	uintptr_t maxIndex = indexableScanner->getIndexableRange();

	uintptr_t scvArraySplitAmount = getArraySplitAmount(env, maxIndex - startIndex);
	uintptr_t endIndex = startIndex + scvArraySplitAmount;

	if (endIndex < maxIndex) {
		/* try to split the remainder into a new copy cache */
		MM_CopyScanCacheStandard* splitCache = getFreeCache(env);
		if (NULL != splitCache) {
			/* set up the split copy cache and clone the object scanner into the cache */
			omrarrayptr_t arrayPtr = (omrarrayptr_t)objectScanner->getParentObject();
			void* arrayTop = (void*)((uintptr_t)arrayPtr + _extensions->indexableObjectModel.getSizeInBytesWithHeader(arrayPtr));
			reinitCache(splitCache, (omrobjectptr_t)arrayPtr, arrayTop);
			splitCache->cacheAlloc = splitCache->cacheTop;
			splitCache->_arraySplitIndex = endIndex;
			splitCache->_arraySplitRememberedSlot = rememberedSetSlot;
			splitCache->flags &= OMR_SCAVENGER_CACHE_TYPE_HEAP;
			splitCache->flags |= OMR_SCAVENGER_CACHE_TYPE_SPLIT_ARRAY;
			indexableScanner->splitTo(env, splitCache->getObjectScanner(), scvArraySplitAmount);
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
			env->_scavengerStats._arraySplitCount += 1;
			env->_scavengerStats._arraySplitAmount += scvArraySplitAmount;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
			addCacheEntryToScanListAndNotify(env, splitCache);
			result = true;
		}
	}

	return result;
}

/**
 * GC thread calls this to update its slots copied/scanned counts after scanning a range of objects. This may trigger
 * an update to the scaling factor that resets this threads counts.
 *
 * @param[in] slotsScanned number of slots scanned on thread from MM_CopyScanCache since last call to this method
 * @param[in] slotsCopied number of slots copied on thread from MM_CopyScanCache since last call to this method
 */
MMINLINE void
MM_Scavenger::updateCopyScanCounts(MM_EnvironmentBase* env, uint64_t slotsScanned, uint64_t slotsCopied)
{
	env->_scavengerStats._slotsScanned += slotsScanned;
	env->_scavengerStats._slotsCopied += slotsCopied;
	uint64_t updateResult = _extensions->copyScanRatio.update(env, &(env->_scavengerStats._slotsScanned), &(env->_scavengerStats._slotsCopied), _waitingCount);
	if (0 != updateResult) {
		_extensions->copyScanRatio.majorUpdate(env, updateResult, _cachedEntryCount, _scavengeCacheScanList.getApproximateEntryCount());
	}
}

MMINLINE bool
MM_Scavenger::scavengeObjectSlots(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *scanCache, omrobjectptr_t objectPtr, uintptr_t flags, omrobjectptr_t *rememberedSetSlot)
{
	GC_ObjectScanner *objectScanner = NULL;
	GC_ObjectScannerState objectScannerState;
	/* scanCache will be NULL if called from outside completeScan() */
	if ((NULL == scanCache) || !scanCache->isSplitArray()) {
		/* try to get a new scanner instance from the cli */
		objectScanner = _cli->scavenger_getObjectScanner(env, objectPtr, (void *) &objectScannerState, flags);
		if ((NULL == objectScanner) || objectScanner->isLeafObject()) {
			/* Object scanner will be NULL if object not scannable by cli (eg, empty pointer array, primitive array) */
			if (NULL != objectScanner) {
				/* Otherwise this is a leaf object -- contains no reference slots */
				env->_scavengerStats._leafObjectCount += 1;
			}
			return false;
		}
	} else {
		/* use scanner cloned into this split array scan cache */
		objectScanner = scanCache->getObjectScanner();
	}

#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
	Assert_MM_true(objectPtr == objectScanner->getParentObject());
	if (NULL != scanCache) {
		Assert_MM_true(objectScanner->isIndexableObject() == (scanCache->isSplitArray() && (0 < scanCache->_arraySplitIndex)));
		Assert_MM_true(rememberedSetSlot == scanCache->_arraySplitRememberedSlot);
	}
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */

	MM_ScavengerHotFieldStats *hotFieldStats = NULL;
	if (objectScanner->isIndexableObject()) {
		/* set scanning bounds for this scanner; if non-empty tail, clone scanner into split array cache and add cache to worklist */
		uintptr_t splitIndex = (NULL != scanCache) ? scanCache->_arraySplitIndex : 0;
		if (!splitIndexableObjectScanner(env, objectScanner, splitIndex, rememberedSetSlot)) {
			/* scan to end of array if can't split */
			((GC_IndexableObjectScanner *)objectScanner)->scanToLimit();
		}
	} else if (_extensions->scavengerTraceHotFields) {
		/* maintain hotness of fields copied from this object */
		hotFieldStats = getHotFieldStats(env);
		hotFieldStats->_objectPtr = objectPtr;
		hotFieldStats->clearHotnessOfField();
	}

	uint64_t slotsCopied = 0;
	uint64_t slotsScanned = 0;
	bool shouldRemember = false;
	GC_SlotObject *slotObject = NULL;
	bool isParentInNewSpace = isObjectInNewSpace(objectPtr);
	MM_CopyScanCacheStandard **copyCache = &(env->_effectiveCopyScanCache);
	while (NULL != (slotObject = objectScanner->getNextSlot())) {
		bool isSlotObjectInNewSpace = copyAndForward(env, slotObject);
		shouldRemember |= isSlotObjectInNewSpace;
		if (NULL != *copyCache) {
			if (NULL != hotFieldStats) {
				hotFieldStats->setHotnessOfField(slotObject->readAddressFromSlot(), objectScanner->getHotFieldsDescriptor());
				hotFieldStats->updateStats(isParentInNewSpace, isSlotObjectInNewSpace, slotObject->readReferenceFromSlot());
			}
			slotsCopied += 1;
		}
		slotsScanned += 1;
	}
	updateCopyScanCounts(env, slotsScanned, slotsCopied);

	if (NULL != hotFieldStats) {
		hotFieldStats->_objectPtr = NULL;
		hotFieldStats->clearHotnessOfField();
	}
	if (shouldRemember && (NULL != rememberedSetSlot)) {
		Assert_MM_true(!isObjectInNewSpace(objectPtr));
		Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));
		Assert_MM_true(objectPtr == (omrobjectptr_t)((uintptr_t)*rememberedSetSlot & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG));
		/* Set the remembered set slot to the object pointer in case it was still marked for removal. */
		*rememberedSetSlot = objectPtr;
	}
	return shouldRemember;
}

/**
 * Scans the slots of a non-indexable object, remembering objects as required. Scanning is interrupted
 * as soon as there is a copy cache that is preferred to the current scan cache. This is returned
 * in nextScanCache.
 *
 * @param scanCache current cache being scanned
 * @param objectPtr current object being scanned
 * @param nextScanCache the updated scanCache after re-aliasing.
 */
MMINLINE void
MM_Scavenger::incrementalScavengeObjectSlots(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr, MM_CopyScanCacheStandard *scanCache, MM_CopyScanCacheStandard **nextScanCache)
{
	/* Get an object scanner from the CLI if not resuming from a scan cache that was previously suspended */
	GC_ObjectScanner *objectScanner = NULL;
	if (!scanCache->_hasPartiallyScannedObject) {
		if (!scanCache->isSplitArray()) {
			/* try to get a new scanner instance from the cli */
			objectScanner = _cli->scavenger_getObjectScanner(env, objectPtr, (void *) scanCache->getObjectScanner(), GC_ObjectScanner::scanHeap);
			if ((NULL == objectScanner) || objectScanner->isLeafObject()) {
				/* Object scanner will be NULL if object not scannable by cli (eg, empty pointer array, primitive array) */
				if (NULL != objectScanner) {
					/* Otherwise this is a leaf object -- contains no reference slots */
					env->_scavengerStats._leafObjectCount += 1;
				}
				return;
			}
		} else {
			/* reuse scanner cloned into this split array scan cache */
			objectScanner = scanCache->getObjectScanner();
		}
		if (objectScanner->isIndexableObject()) {
			/* set scanning bounds for this scanner; if non-empty tail, add split array cache to worklist and clone this indexableScanner into split cache */
			if (!splitIndexableObjectScanner(env, objectScanner, scanCache->_arraySplitIndex, scanCache->_arraySplitRememberedSlot)) {
				/* scan to end of array if can't split */
				((GC_IndexableObjectScanner *)objectScanner)->scanToLimit();
			}
		}
		scanCache->_shouldBeRemembered = false;
	} else {
		/* resume suspended object scanner */
		objectScanner = scanCache->getObjectScanner();
	}

#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
	if (scanCache->isSplitArray()) {
		Assert_MM_true(objectScanner->isIndexableObject());
		Assert_MM_true(objectPtr == objectScanner->getParentObject());
		Assert_MM_true(0 < scanCache->_arraySplitIndex);
	} else {
		Assert_MM_true(0 == scanCache->_arraySplitIndex);
		Assert_MM_true(NULL == scanCache->_arraySplitRememberedSlot);
	}
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */

	/* Set up for maintaining hot field stats for scalar objects */
	MM_ScavengerHotFieldStats *hotFieldStats = NULL;
	if (!objectScanner->isIndexableObject() && _extensions->scavengerTraceHotFields) {
		/* maintain hotness of fields copied from this object */
		hotFieldStats = getHotFieldStats(env);
		hotFieldStats->_objectPtr = objectPtr;
		hotFieldStats->clearHotnessOfField();
	}

	*nextScanCache = NULL;
	GC_SlotObject *slotObject;
	uint64_t slotsCopied = 0;
	uint64_t slotsScanned = 0;
	bool isParentInNewSpace = isObjectInNewSpace(objectPtr);
	MM_CopyScanCacheStandard **copyCache = &(env->_effectiveCopyScanCache);
	while (NULL != (slotObject = objectScanner->getNextSlot())) {
		/* If the object should be remembered and it is in old space, remember it */
		bool isSlotObjectInNewSpace = copyAndForward(env, slotObject);
		scanCache->_shouldBeRemembered |= isSlotObjectInNewSpace;
		slotsScanned += 1;
		/* Copy cache will be set only if a referent object is copied (ie, if not previously forwarded) */
		if (NULL != *copyCache) {
			slotsCopied += 1;
			if (NULL != hotFieldStats) {
				hotFieldStats->setHotnessOfField(slotObject->readAddressFromSlot(), objectScanner->getHotFieldsDescriptor());
				hotFieldStats->updateStats(isParentInNewSpace, isSlotObjectInNewSpace, slotObject->readReferenceFromSlot());
			}
			*nextScanCache = aliasToCopyCache(env, slotObject, scanCache, *copyCache);
			/* alias and switch to nextScanCache if it was selected */
			if (NULL != *nextScanCache) {
				updateCopyScanCounts(env, slotsScanned, slotsCopied);
				return;
			}
		}
	}
	updateCopyScanCounts(env, slotsScanned, slotsCopied);

	if (NULL != hotFieldStats) {
		hotFieldStats->_objectPtr = NULL;
		hotFieldStats->clearHotnessOfField();
	}
	scanCache->_hasPartiallyScannedObject = false;
	if (scanCache->_shouldBeRemembered) {
		if (NULL != scanCache->_arraySplitRememberedSlot) {
			Assert_MM_true(!isObjectInNewSpace(objectPtr));
			Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));
			Assert_MM_true(objectPtr == (omrobjectptr_t)((uintptr_t)*(scanCache->_arraySplitRememberedSlot) & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG));
			/* Set the remembered set slot to the object pointer in case it was still marked for removal. */
			*(scanCache->_arraySplitRememberedSlot) = objectPtr;
		} else {
			rememberObject(env, objectPtr);
		}
		scanCache->_shouldBeRemembered = false;
	}
}

/****************************************
 * Scan completion routines
 ****************************************
 */

MMINLINE void
MM_Scavenger::flushBuffersForGetNextScanCache(MM_EnvironmentStandard *env)
{
	_cli->scavenger_flushReferenceObjects(env);
	flushRememberedSet(env);
}

MM_CopyScanCacheStandard *
MM_Scavenger::getNextScanCache(MM_EnvironmentStandard *env)
{
	MM_CopyScanCacheStandard *cache = NULL;
	bool doneFlag = false;
	volatile uintptr_t doneIndex = _doneIndex;

	/* Preference is to use survivor copy cache */
	if (NULL != (cache = getSurvivorCopyCache(env))) {
		return cache;
	}

	/* Otherwise the tenure copy cache */
	if (isWorkAvailableInCache(env->_tenureCopyScanCache)) {
		return env->_tenureCopyScanCache;
	}

	if (NULL != env->_deferredScanCache) {
		/* there is deferred scanning to do from partial depth first scanning */
		cache = env->_deferredScanCache;
		env->_deferredScanCache = NULL;
		return cache;
	}

	if (NULL != (cache = getDeferredCopyCache(env))) {
		/* deferred copy caches are used to merge memory-contiguous caches that got chopped up due to large objects not fitting and resuing remainder.
		 * we want to delay scanning them as much as possible (up to the size of the original cache size being chopped up),
		 * but we still want to do it before we synchronizing on scan queue and realizing no more work is awailable */
		Assert_MM_true(cache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY);
		cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
		env->_deferredCopyCache = NULL;
		return cache;
	}

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	env->_scavengerStats._acquireScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
#if defined(OMR_SCAVENGER_TRACE) || defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
#endif /* OMR_SCAVENGER_TRACE || J9MODRON_TGC_PARALLEL_STATISTICS */
 	while (!doneFlag && !backOutFlagRaised()) {
 		while (_cachedEntryCount > 0) {
 			cache = getNextScanCacheFromList(env);

			if (NULL != cache) {
 				/* Check if there are threads waiting that should be notified because of pending entries */
 				if((_cachedEntryCount > 0) && _waitingCount) {
					if (0 == omrthread_monitor_try_enter(_scanCacheMonitor)) {
						if(0 != _waitingCount) {
							omrthread_monitor_notify(_scanCacheMonitor);
						}
						omrthread_monitor_exit(_scanCacheMonitor);
					}
				}

#if defined(OMR_SCAVENGER_TRACE)
				omrtty_printf("{SCAV: Scan cache from list (%p)}\n", cache);
#endif /* OMR_SCAVENGER_TRACE */

				return cache;
			}
		}

		omrthread_monitor_enter(_scanCacheMonitor);
		_waitingCount += 1;

		if(doneIndex == _doneIndex) {
			if((env->_currentTask->getThreadCount() == _waitingCount) && (0 == _cachedEntryCount)) {
				_waitingCount = 0;
				_doneIndex += 1;
				flushBuffersForGetNextScanCache(env);
				_extensions->copyScanRatio.reset(env, false);
				omrthread_monitor_notify_all(_scanCacheMonitor);
			} else {
				while((0 == _cachedEntryCount) && (doneIndex == _doneIndex) && !backOutFlagRaised()) {
					flushBuffersForGetNextScanCache(env);
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
					uint64_t waitEndTime, waitStartTime;
					waitStartTime = omrtime_hires_clock();
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
					omrthread_monitor_wait(_scanCacheMonitor);
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
					waitEndTime = omrtime_hires_clock();
					if (doneIndex != _doneIndex) {
						env->_scavengerStats.addToCompleteStallTime(waitStartTime, waitEndTime);
					} else {
						env->_scavengerStats.addToWorkStallTime(waitStartTime, waitEndTime);
					}
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
				}
			}
		}

		/* Set the local done flag and if we are done and the waiting count is 0 (last thread) exit */
		doneFlag = (doneIndex != _doneIndex);
		if(!doneFlag) {
			_waitingCount -= 1;
		}

		omrthread_monitor_exit(_scanCacheMonitor);
	}

	return cache;
}

/**
 * Scans all the objects to scan in the env->_scanCache, remembering objects as required,
 * and flushing the cache at the end.
 */
void
MM_Scavenger::completeScanCache(MM_EnvironmentStandard *env)
{
	omrobjectptr_t objectPtr;

	/* mark that cache is in use as a scan cache */
	MM_CopyScanCacheStandard* scanCache = (MM_CopyScanCacheStandard*)env->_scanCache;
	Assert_MM_true(0 == (scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_SCAN));
	scanCache->flags |= OMR_SCAVENGER_CACHE_TYPE_SCAN;

	if (scanCache->isSplitArray()) {
		/* Advance the scan pointer to the top of the cache to signify that this has been scanned */
		objectPtr = (omrobjectptr_t)scanCache->scanCurrent;
		scanCache->scanCurrent = scanCache->cacheAlloc;
		bool shouldBeRemembered = scavengeObjectSlots(env, scanCache, objectPtr, GC_ObjectScanner::scanHeap, scanCache->_arraySplitRememberedSlot);
		if (shouldBeRemembered) {
			rememberObject(env, objectPtr);
		}
	} else {
		while(isWorkAvailableInCache(scanCache)) {
			GC_ObjectHeapIteratorAddressOrderedList heapChunkIterator(
				_extensions,
				(omrobjectptr_t)scanCache->scanCurrent,
				(omrobjectptr_t)scanCache->cacheAlloc, false);
			/* Advance the scan pointer to the top of the cache to signify that this has been scanned */
			scanCache->scanCurrent = scanCache->cacheAlloc;
			/* Scan the chunk for all live objects */
			while((objectPtr = heapChunkIterator.nextObjectNoAdvance()) != NULL) {
				/* If the object should be remembered and it is in old space, remember it */
				bool shouldBeRemembered = scavengeObjectSlots(env, scanCache, objectPtr, GC_ObjectScanner::scanHeap, NULL);
				if (shouldBeRemembered) {
					rememberObject(env, objectPtr);
				}
			}
		}
	}
#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
	Assert_MM_true(0 != (scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_SCAN));
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */
	/* mark cache as no longer in use for scanning */
	scanCache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_SCAN;
	/* Done with the cache - build a free list entry in the hole, release the cache to the free list (if not used), and continue */
	flushCache(env, scanCache);
}


 /**
 * Scans the objects to scan in the env->_scanCache, remembering objects as required.
 * Slots are scanned until there is an opportunity to alias the scan cache to a copy cache. When
 * this happens, scanning is interrupted and the present scan cache is either pushed onto the scan
 * list or "deferred" in thread-local storage. Deferring is done to reduce contention due to the
 * increased need to change scan cache. If the cache is scanned completely without interruption
 * the cache is flushed at the end.
 */
void
MM_Scavenger::incrementalScanCacheBySlot(MM_EnvironmentStandard *env)
{
	MM_CopyScanCacheStandard* scanCache = env->_scanCache;
	MM_CopyScanCacheStandard* nextScanCache = scanCache;

	nextCache:
	/* mark that cache is in use as a scan cache */
	Assert_MM_true(0 == (scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_SCAN));
	scanCache->flags |= OMR_SCAVENGER_CACHE_TYPE_SCAN;
	while (isWorkAvailableInCache(scanCache)) {
		void *cacheAlloc = scanCache->cacheAlloc;
		GC_ObjectHeapIteratorAddressOrderedList heapChunkIterator(
			_extensions,
			(omrobjectptr_t)scanCache->scanCurrent,
			(omrobjectptr_t)cacheAlloc,
			false);

		omrobjectptr_t objectPtr;
		/* Scan the chunk for live objects, incrementally slot by slot */
		while ((objectPtr = heapChunkIterator.nextObjectNoAdvance()) != NULL) {
			incrementalScavengeObjectSlots(env, objectPtr, scanCache, &nextScanCache);

			/* object was not completely scanned in order to interrupt scan */
			if (scanCache->_hasPartiallyScannedObject) {
#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
				Assert_MM_true(0 != (scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_SCAN));
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */
				/* interrupt scan, save scan state of cache before deferring */
				scanCache->scanCurrent = objectPtr;
				/* Only save scan cache if it is not a copy cache, and then don't add to scanlist - this
				 * can cause contention, just defer to later time on same thread
				 * if deferred cache is occupied, then queue current scan cache on scan list
				 */
				scanCache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_SCAN;
				if (!(scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY)) {
					if (NULL == env->_deferredScanCache) {
						env->_deferredScanCache = scanCache;
					} else {
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
						env->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
						addCacheEntryToScanListAndNotify(env, scanCache);
					}
				}
				env->_scanCache = scanCache = nextScanCache;
				goto nextCache;
			}
		}
		/* Advance the scan pointer for the objects that were scanned */
		scanCache->scanCurrent = cacheAlloc;
	}
#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
	Assert_MM_true(0 != (scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_SCAN));
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */
	/* mark cache as no longer in use for scanning */
	scanCache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_SCAN;
	/* Done with the cache - build a free list entry in the hole, release the cache to the free list (if not used), and continue */
	flushCache(env, scanCache);
}

bool
MM_Scavenger::completeScan(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	/* take a snapshot of ID of this scan cycle (which will change in getNextScanCache() once all threads agree to leave the scan loop) */
	uintptr_t doneIndex = _doneIndex;

	if (_extensions->_forceRandomBackoutsAfterScan) {
		if (0 == (rand() % _extensions->_forceRandomBackoutsAfterScanPeriod)) {
			omrtty_printf("Forcing backout at workUnitIndex: %zu lastSyncPointReached: %s\n", env->getWorkUnitIndex(), env->_lastSyncPointReached);
			setBackOutFlag(env, true);
			omrthread_monitor_enter(_scanCacheMonitor);
			if(_waitingCount) {
				omrthread_monitor_notify_all(_scanCacheMonitor);
			}
			omrthread_monitor_exit(_scanCacheMonitor);
		}
	}

	env->_scavengerStats.resetCopyScanCounts();

	while((env->_scanCache = getNextScanCache(env)) != NULL) {
#if defined(OMR_SCAVENGER_TRACE)
		omrtty_printf("{SCAV: Completing scan (%p) %p-%p-%p}\n", env->_scanCache, env->_scanCache->cacheBase, env->_scanCache->cacheAlloc, env->_scanCache->cacheTop);
#endif /* OMR_SCAVENGER_TRACE */

		assume0(env->_scanCache->cacheBase <= env->_scanCache->cacheAlloc);
		assume0(env->_scanCache->cacheAlloc <= env->_scanCache->cacheTop);
		assume0(env->_scanCache->scanCurrent <= env->_scanCache->cacheAlloc);

		switch (_extensions->scavengerScanOrdering) {
		case MM_GCExtensionsBase::OMR_GC_SCAVENGER_SCANORDERING_BREADTH_FIRST:
			completeScanCache(env);
			break;
		case MM_GCExtensionsBase::OMR_GC_SCAVENGER_SCANORDERING_HIERARCHICAL:
			incrementalScanCacheBySlot(env);
			break;
		default:
			Assert_MM_unreachable();
			break;
		}
	}

	env->_scavengerStats.resetCopyScanCounts();

	/* A slow  thread can see backOutFlag raised by a fast thread aborting in the next scan cycle.
	 * By checking that thread local doneIndex of the current scan cycle matches the doneIndex from scan cycle that raised the flag,
	 * we ensure consistent behavior (return of backOutRaised flag) of all threads within this scan cycle, so that all threads proceed
	 * consistently to the next step (being just another scan cycle, or backout procedure).
	 */
	bool backOutRaisedThisScanCycle = backOutFlagRaised() && (doneIndex == _backOutDoneIndex);

	Assert_MM_true(backOutRaisedThisScanCycle || (0 == env->_scavengerRememberedSet.count));

	return !backOutRaisedThisScanCycle;
}

void
MM_Scavenger::workThreadGarbageCollect(MM_EnvironmentStandard *env)
{
	/* GC init (set up per-invocation values) */
	workerSetupForGC(env);

	/*
	 * There is a hidden assumption that RS Overflow flag would not be changed between beginning of scavenge and this point,
	 * otherwise it is hang situation when one group of threads might wait for more work in complete scan and another group of threads
	 * be got stuck on sync point in scavengeRememberedSetOverflow()
	 *
	 * So scavenge Remembered Set right away
	 */
	MM_ScavengerRootScanner rootScanner(env, this);

	rootScanner.scavengeRememberedSet(env);

	rootScanner.scanRoots(env);

	if(completeScan(env)) {
		if (_rescanThreadsForRememberedObjects) {
			rootScanner.rescanThreadSlots(env);
			flushRememberedSet(env);
		}
		rootScanner.scanClearable(env);
	}
	rootScanner.flush(env);

	addCopyCachesToFreeList(env);
	abandonTLHRemainders(env);

	/* If -Xgc:fvtest=forceScavengerBackout has been specified, set backout flag every 3rd scavenge */
	if(_extensions->fvtest_forceScavengerBackout) {
		if (env->_currentTask->synchronizeGCThreadsAndReleaseMaster(env, UNIQUE_ID)) {
			if (2 <= _extensions->fvtest_backoutCounter) {
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
				OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
				omrtty_printf("{SCAV: Forcing back out(%p)}\n", env->getLanguageVMThread());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
				setBackOutFlag(env, true);
				_extensions->fvtest_backoutCounter = 0;
			} else {
				_extensions->fvtest_backoutCounter += 1;
			}
			env->_currentTask->releaseSynchronizedGCThreads(env);
		}
	}

	if(backOutFlagRaised()) {
		env->_scavengerStats._backout = 1;
		completeBackOut(env);
	} else {
		/* pruning */
		rootScanner.pruneRememberedSet(env);
	}

	/* No matter what happens, always sum up the gc stats */
	mergeGCStats(env);
}

/****************************************
 * Remembered set support
 ****************************************
 */
void
MM_Scavenger::clearRememberedSetLists(MM_EnvironmentStandard *env)
{
	_extensions->rememberedSet.clear(env);
}

void
MM_Scavenger::addAllRememberedObjectsToOverflow(MM_EnvironmentStandard *env, MM_RSOverflow *overflow)
{
	/* Walk the heap finding all old objects that are flagged as remembered */
	MM_HeapRegionDescriptorStandard *region = NULL;
	GC_MemorySubSpaceRegionIteratorStandard regionIterator(_tenureMemorySubSpace);
	while((region = regionIterator.nextRegion()) != NULL) {
		GC_ObjectHeapIteratorAddressOrderedList objectIterator(_extensions, region, false);
		omrobjectptr_t objectPtr;
		while((objectPtr = objectIterator.nextObject()) != NULL) {
			if(_extensions->objectModel.isRemembered(objectPtr)) {
				/* mark remembered objects */
				overflow->addObject(objectPtr);
			}
		}
	}
}

void
MM_Scavenger::addToRememberedSetFragment(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	Assert_MM_true(NULL != objectPtr);
	Assert_MM_true(!isObjectInNewSpace(objectPtr));
	Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));

	if(env->_scavengerRememberedSet.fragmentCurrent >= env->_scavengerRememberedSet.fragmentTop) {
		/* There wasn't enough room in the current fragment - allocate a new one */
		if(allocateMemoryForSublistFragment(env->getOmrVMThread(), (J9VMGC_SublistFragment*)&env->_scavengerRememberedSet)) {
			/* Failed to allocate a fragment - set the remembered set overflow state and exit */
			if(!isRememberedSetInOverflowState()) {
				env->_scavengerStats._causedRememberedSetOverflow = 1;
			}
			setRememberedSetOverflowState();
			return ;
		}
	}

	/* There is at least 1 free entry in the fragment - use it */
#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	omrtty_printf("{SCAV: Add to remembered set %p}\n", objectPtr);
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

	env->_scavengerRememberedSet.count++;
	uintptr_t *rememberedSetEntry = env->_scavengerRememberedSet.fragmentCurrent++;
	*rememberedSetEntry = (uintptr_t)objectPtr;
}

void
MM_Scavenger::rememberObject(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	/* Try to set the REMEMBERED bit in the flags field (if it hasn't already been set) */
	if(!isObjectInNewSpace(objectPtr)) {
		if(_extensions->objectModel.atomicSetRemembered(objectPtr)) {
			/* The object has been successfully marked as REMEMBERED - allocate an entry in the remembered set */
			addToRememberedSetFragment(env, objectPtr);
		}
	}
}

bool
MM_Scavenger::isRememberedThreadReference(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	Assert_MM_true(NULL != objectPtr);
	Assert_MM_true(!isObjectInNewSpace(objectPtr));
	Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));

	bool result = false;

	/* Check for thread-referenced objects. These need to be remembered as
	 * long as they're still referenced by a thread or stack slot
	 */
	uintptr_t age = _extensions->objectModel.getRememberedBits(objectPtr);
	switch (age) {
	case OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED:
	case OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED:
		result = true;
		break;
	case STATE_REMEMBERED:
		/* normal remembered object -- do nothing */
		break;
	case STATE_NOT_REMEMBERED:
	default:
		Assert_MM_unreachable();
	}

	return result;
}

bool
MM_Scavenger::processRememberedThreadReference(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	Assert_MM_true(NULL != objectPtr);
	Assert_MM_true(!isObjectInNewSpace(objectPtr));
	Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));

	bool result = false;

	/* Check for thread-referenced objects. These need to be remembered as
	 * long as they're still referenced by a thread or stack slot
	 */
	uintptr_t age = _extensions->objectModel.getRememberedBits(objectPtr);
	switch (age) {
	case OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED:
		result = true;
		/* downgrade state */
		_extensions->objectModel.setRememberedBits(objectPtr, OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED);
		break;
	case OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED:
		result = true;
		/* downgrade state */
		_extensions->objectModel.setRememberedBits(objectPtr, STATE_REMEMBERED);
		break;
	case STATE_REMEMBERED:
		/* normal remembered object -- do nothing */
		break;
	case STATE_NOT_REMEMBERED:
	default:
		Assert_MM_unreachable();
	}

	return result;
}

/********************************************************************
 * Object Scan Routines for Remembered Set Overflow (RSO) conditions
 * All objects taken as input MUST be in Tenured (Old) Space
 ********************************************************************/
bool
MM_Scavenger::walkObjectSlotsForRSO(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	Assert_MM_true((NULL != objectPtr) && (!isObjectInNewSpace(objectPtr)));

	bool shouldBeRemembered = false;
	GC_ObjectScannerState objectScannerState;
	GC_ObjectScanner *objectScanner = _cli->scavenger_getObjectScanner(env, objectPtr, &objectScannerState, GC_ObjectScanner::scanRoots);

	if (NULL != objectScanner) {
		GC_SlotObject *slotPtr;
		while (NULL != (slotPtr = objectScanner->getNextSlot())) {
			omrobjectptr_t slotObjectPtr = slotPtr->readReferenceFromSlot();
			if (NULL != slotObjectPtr) {
				if (isObjectInNewSpace(slotObjectPtr)) {
					Assert_MM_true(!isObjectInEvacuateMemory(slotObjectPtr));
					shouldBeRemembered = true;
				}
			}
		}
	}

	return shouldBeRemembered;
}

/**
 * Scans all the slots of the given object, treating REFERENCE_MIXED as MIXED. Also scan indirectly
 * referenced objects in object class.
 * @return true if object should be remembered at the end of scanning.
 */
MMINLINE bool
MM_Scavenger::scavengeRememberedObject(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	bool shouldBeBemembered = scavengeObjectSlots(env, NULL, objectPtr, GC_ObjectScanner::scanRoots, NULL);
	if (_extensions->objectModel.hasIndirectObjectReferents(env->getLanguageVMThread(), objectPtr)) {
		shouldBeBemembered |= _cli->scavenger_scavengeIndirectObjectSlots(env, objectPtr);
	}
	return shouldBeBemembered;
}

void
MM_Scavenger::scavengeRememberedSetOverflow(MM_EnvironmentStandard *env)
{
	/* Reset the local remembered set fragment */
	env->_scavengerRememberedSet.fragmentCurrent = NULL;
	env->_scavengerRememberedSet.fragmentTop = NULL;
	env->_scavengerRememberedSet.fragmentSize = (uintptr_t)J9_SCV_REMSET_FRAGMENT_SIZE;
	env->_scavengerRememberedSet.parentList = &_extensions->rememberedSet;

	if (env->_currentTask->synchronizeGCThreadsAndReleaseMaster(env, UNIQUE_ID)) {

#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
		OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
		omrtty_printf("{SCAV: Scavenge remembered set overflow}\n");
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

		clearRememberedSetLists(env);

		/* Creation of this class will Abort Global Collector */
		MM_RSOverflow rememberedSetOverflow(env);

		addAllRememberedObjectsToOverflow(env, &rememberedSetOverflow);

		/*
		 * Scan any remembered objects, but don't adjust their remembered bit.
		 * Objects that no longer need remembering will be pruned at the end of the scavenge.
		 */
		omrobjectptr_t objectPtr = NULL;
		while (NULL != (objectPtr = rememberedSetOverflow.nextObject())) {
			scavengeRememberedObject(env, objectPtr);
		}

		env->_currentTask->releaseSynchronizedGCThreads(env);
	}
}

MMINLINE void
MM_Scavenger::flushRememberedSet(MM_EnvironmentStandard *env)
{
	MM_SublistFragment::flush((J9VMGC_SublistFragment*)&env->_scavengerRememberedSet);
}

void
MM_Scavenger::pruneRememberedSet(MM_EnvironmentStandard *env)
{
	if(isRememberedSetInOverflowState()) {
		pruneRememberedSetOverflow(env);
	} else {
		pruneRememberedSetList(env);
	}
}

void
MM_Scavenger::pruneRememberedSetOverflow(MM_EnvironmentStandard *env)
{
	/* Reset the local remembered set fragment */
	env->_scavengerRememberedSet.fragmentCurrent = NULL;
	env->_scavengerRememberedSet.fragmentTop = NULL;
	env->_scavengerRememberedSet.fragmentSize = (uintptr_t)J9_SCV_REMSET_FRAGMENT_SIZE;
	env->_scavengerRememberedSet.parentList = &_extensions->rememberedSet;

	if (env->_currentTask->synchronizeGCThreadsAndReleaseMaster(env, UNIQUE_ID)) {

#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
		OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
		omrtty_printf("{SCAV: Prune remembered set overflow}\n");
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

		/* Clear the overflow state. Probability is high that we'll wind up re-overflowing. */
		clearRememberedSetOverflowState();
		clearRememberedSetLists(env);

		/* Walk the heap finding all old objects that are flagged as remembered */
		MM_HeapRegionDescriptorStandard *region = NULL;
		GC_MemorySubSpaceRegionIteratorStandard regionIterator(_tenureMemorySubSpace);
		while((region = regionIterator.nextRegion()) != NULL) {
			GC_ObjectHeapIteratorAddressOrderedList objectIterator(_extensions, region, false);
			omrobjectptr_t objectPtr;

			while((objectPtr = objectIterator.nextObject()) != NULL) {
				if(_extensions->objectModel.isRemembered(objectPtr)) {
					bool shouldBeRemembered = false;

					/* Re-scan the tenured for objects that should be remembered.
					 * No copying will be done. */
					shouldBeRemembered = walkObjectSlotsForRSO(env, objectPtr);

					/* The remembered state of a class object also depends on the class statics */
					if (_extensions->objectModel.hasIndirectObjectReferents(env->getLanguageVMThread(), objectPtr)) {
						shouldBeRemembered |= _cli->scavenger_hasIndirectReferentsInNewSpace(env, objectPtr);
					}

					/* VMDESIGN 2048 : unconditionally remember any recently referenced objects */
					if (processRememberedThreadReference(env, objectPtr)) {
						Trc_MM_ParallelScavenger_scavengeRememberedSet_keepingRememberedObject(env->getLanguageVMThread(), objectPtr, _extensions->objectModel.getRememberedBits(objectPtr));
						shouldBeRemembered = true;
					}

					/* Re-remember the object if necessary. This will add it to the list if possible. */
					if(shouldBeRemembered) {
						addToRememberedSetFragment(env, objectPtr);
					} else {
						_extensions->objectModel.clearRemembered(objectPtr);
						/* Inform interested parties that an object has been removed from the remembered set */
						TRIGGER_J9HOOK_MM_PRIVATE_OBJECT_REMOVED_FROM_REMEMBERED_SET(_extensions->privateHookInterface, env->getOmrVMThread(), objectPtr);
					}
				}
			}
		}

#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
		if(isRememberedSetInOverflowState()) {
			omrtty_printf"{SCAV: Pruned remembered set still in overflow}\n");
		} else {
			omrtty_printf("{SCAV: Pruned remembered set no longer in overflow}\n");
		}
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

		/* Objects may have been remembered during scan, fragment must be flushed */
		flushRememberedSet(env);

		env->_currentTask->releaseSynchronizedGCThreads(env);
	}
}

void
MM_Scavenger::pruneRememberedSetList(MM_EnvironmentStandard *env)
{
	/* Remembered set walk */
	omrobjectptr_t *slotPtr;
	omrobjectptr_t objectPtr;
	MM_SublistPuddle *puddle;

#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	omrtty_printf("{SCAV: Prune remembered set list}\n");
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

	GC_SublistIterator remSetIterator(&(_extensions->rememberedSet));
	while((puddle = remSetIterator.nextList()) != NULL) {
		if(J9MODRON_HANDLE_NEXT_WORK_UNIT(env)) {
			GC_SublistSlotIterator remSetSlotIterator(puddle);
			while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
				objectPtr = *slotPtr;

				if (NULL == objectPtr) {
					remSetSlotIterator.removeSlot();
				} else if((uintptr_t)objectPtr & DEFERRED_RS_REMOVE_FLAG) {
					/* Is slot flagged for deferred removal ? */
					/* Yes..so first remove tag bit from object address */
					objectPtr = (omrobjectptr_t)((uintptr_t)objectPtr & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG);
#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
					omrtty_printf("{SCAV: REMOVED remembered set object %p}\n", objectPtr);
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

					/* A simple mask out can be used - we are guaranteed to be the only manipulator of the object */
					_extensions->objectModel.clearRemembered(objectPtr);
					remSetSlotIterator.removeSlot();

					/* Inform interested parties that an object has been removed from the remembered set */
					TRIGGER_J9HOOK_MM_PRIVATE_OBJECT_REMOVED_FROM_REMEMBERED_SET(_extensions->privateHookInterface, env->getOmrVMThread(), objectPtr);

				} else {
					/* Retain remembered object */
#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
					omrtty_printf("{SCAV: Remembered set object %p}\n", objectPtr);
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

					if (processRememberedThreadReference(env, objectPtr)) {
						/* the object was tenured from the stack on a previous scavenge -- keep it around for a bit longer */
						Trc_MM_ParallelScavenger_scavengeRememberedSet_keepingRememberedObject(env->getLanguageVMThread(), objectPtr, _extensions->objectModel.getRememberedBits(objectPtr));
					}
				}
			} /* while non-null slots */
		}
	}
}

void
MM_Scavenger::scavengeRememberedSetList(MM_EnvironmentStandard *env)
{
	Trc_MM_ParallelScavenger_scavengeRememberedSetList_Entry(env->getLanguageVMThread());

	/* Remembered set walk */
	MM_SublistPuddle *puddle = NULL;
	while (NULL != (puddle = _extensions->rememberedSet.popPreviousPuddle(puddle))) {
		Trc_MM_ParallelScavenger_scavengeRememberedSetList_startPuddle(env->getLanguageVMThread(), puddle);
		uintptr_t numElements = 0;
		GC_SublistSlotIterator remSetSlotIterator(puddle);
		omrobjectptr_t *slotPtr;
		while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
			omrobjectptr_t objectPtr = *slotPtr;

			if(NULL != objectPtr) {
				Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));
				numElements += 1;

				/* First assume the object will not be remembered.
				 * This is helpful for work completion ordering of split arrays.
				 * Flag slot for later removal if we complete scavenge OK
				 */
				*slotPtr = (omrobjectptr_t)((uintptr_t)*slotPtr | DEFERRED_RS_REMOVE_FLAG);
				bool shouldBeRemembered = scavengeObjectSlots(env, NULL, objectPtr, GC_ObjectScanner::scanRoots, slotPtr);
				if (_extensions->objectModel.hasIndirectObjectReferents(env->getLanguageVMThread(), objectPtr)) {
					shouldBeRemembered |= _cli->scavenger_scavengeIndirectObjectSlots(env, objectPtr);
				}
				shouldBeRemembered |= isRememberedThreadReference(env, objectPtr);

				if (shouldBeRemembered) {
					/* We want to remember this object after all; clear the flag for removal. */
					*slotPtr = (omrobjectptr_t)((uintptr_t)*slotPtr & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG);
				}
			} else {
				remSetSlotIterator.removeSlot();
			}
		}

		Trc_MM_ParallelScavenger_scavengeRememberedSetList_donePuddle(env->getLanguageVMThread(), puddle, numElements);
	}

	Trc_MM_ParallelScavenger_scavengeRememberedSetList_Exit(env->getLanguageVMThread());
}

/* NOTE - only  scavengeRememberedSetOverflow ends with a sync point.
 * Callers of this function must not assume that there is a sync point
 */
void
MM_Scavenger::scavengeRememberedSet(MM_EnvironmentStandard *env)
{
	if (_isRememberedSetInOverflowAtTheBeginning) {
		env->_scavengerStats._rememberedSetOverflow = 1;
		scavengeRememberedSetOverflow(env);
	} else {
		scavengeRememberedSetList(env);
	}
}

void
MM_Scavenger::copyAndForwardThreadSlot(MM_EnvironmentStandard *env, omrobjectptr_t *objectPtrIndirect)
{
	/* auto-remember stack- and thread-referenced objects */
	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if(NULL != objectPtr) {
		if (isObjectInEvacuateMemory(objectPtr)) {
			bool isInNewSpace = copyAndForward(env, objectPtrIndirect);
			if (!isInNewSpace) {
				Trc_MM_ParallelScavenger_copyAndForwardThreadSlot_deferRememberObject(env->getLanguageVMThread(), *objectPtrIndirect);
				/* the object was tenured while it was referenced from the stack. Undo the forward, and process it in the rescan pass. */
				_rescanThreadsForRememberedObjects = true;
				*objectPtrIndirect = objectPtr;
			}
		} else {
			if (_extensions->isOld(objectPtr)) {
				if(_extensions->objectModel.atomicSetObjectCurrentlyReferenced(objectPtr)) {
					Trc_MM_ParallelScavenger_copyAndForwardThreadSlot_renewingRememberedObject(env->getLanguageVMThread(), objectPtr,
							OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED);
				}
			}
		}
	}
}

void
MM_Scavenger::rescanThreadSlot(MM_EnvironmentStandard *env, omrobjectptr_t *objectPtrIndirect)
{
	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if(NULL != objectPtr) {
		if (isObjectInEvacuateMemory(objectPtr)) {
			/* the slot is still pointing at evacuate memory. This means that it must have been left unforwarded
			 * in the first pass so that we would process it here.
			 */
			MM_ForwardedHeader forwardedHeader(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
			omrobjectptr_t tenuredObjectPtr = forwardedHeader.getForwardedObject();

			Trc_MM_ParallelScavenger_rescanThreadSlot_rememberedObject(env->getLanguageVMThread(), tenuredObjectPtr);

			Assert_MM_true(NULL != tenuredObjectPtr);
			Assert_MM_true(!isObjectInNewSpace(tenuredObjectPtr));

			*objectPtrIndirect = tenuredObjectPtr;
			rememberObject(env, tenuredObjectPtr);
			_extensions->objectModel.setObjectCurrentlyReferenced(tenuredObjectPtr);
		}
	}
}

/****************************************
 * Copy-Scan Cache management
 ****************************************
 * TODO: move all the CopyScanCache methods into the CopyScanCache class.
 */

/*
 * definition of the following method is earlier in the file to ensure inlining.
 *
 * MM_Scavenger::reinitCache(MM_CopyScanCacheStandard *cache, void *base, void *top)
 */

MMINLINE void
MM_Scavenger::reinitCache(MM_CopyScanCacheStandard *cache, void *base, void *top)
{
	cache->cacheBase = base;
	cache->cacheAlloc = base;
	cache->scanCurrent = base;
	cache->_arraySplitIndex = 0;
	cache->_arraySplitAmountToScan = 0;
	cache->_arraySplitRememberedSlot = NULL;
	cache->_hasPartiallyScannedObject = false;
	cache->_shouldBeRemembered = false;
	cache->cacheTop = top;
}

/**
 * @return whether there is scanning work on the given cache
 */
MMINLINE bool
MM_Scavenger::isWorkAvailableInCache(MM_CopyScanCacheStandard *cache)
{
	return ((NULL != cache) && (cache->scanCurrent < cache->cacheAlloc));
}

MMINLINE MM_CopyScanCacheStandard *
MM_Scavenger::getFreeCache(MM_EnvironmentStandard *env)
{
	/* Check the free list */
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	env->_scavengerStats._acquireFreeListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */

	MM_CopyScanCacheStandard *cache = _scavengeCacheFreeList.popCache(env);

	if (NULL == cache) {	
		env->_scavengerStats._scanCacheOverflow = 1;
		OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
		uint64_t duration = omrtime_current_time_millis();

		omrthread_monitor_enter(_freeCacheMonitor);
		bool result = _scavengeCacheFreeList.resizeCacheEntries(env, 1+_scavengeCacheFreeList.getAllocatedCacheCount(), 0);
		omrthread_monitor_exit(_freeCacheMonitor);
		if (result) {
			cache = _scavengeCacheFreeList.popCache(env);
		}
		if (NULL == cache) {
			/* Still need a new cache and nothing left reserved - create it in Heap */
			cache = createCacheInHeap(env);
		}
		duration = omrtime_current_time_millis() - duration;
		env->_scavengerStats._scanCacheAllocationDurationDuringSavenger += duration;

	}

	return cache;
}

MM_CopyScanCacheStandard *
MM_Scavenger::createCacheInHeap(MM_EnvironmentStandard *env)
{
#if defined(OMR_SCAVENGER_TRACE)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
#endif /* OMR_SCAVENGER_TRACE */

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	env->_scavengerStats._acquireFreeListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */

	omrthread_monitor_enter(_freeCacheMonitor);

	/* Recheck the free list - avoids a potential timing hole here after releasing the lock previously.
	 * This keeps the lock ordering (scan is an outer lock of free)
	 * wrt/the api (can enter this call with the scan lock held)
	 */
	MM_CopyScanCacheStandard *cache = _scavengeCacheFreeList.popCache(env);

	if (NULL == cache) {
		env->_scavengerStats._scanCacheAllocationFromHeap = 1;
		/* try to create a temporary chunk of scanCaches in Survivor */
		cache = _scavengeCacheFreeList.appendCacheEntriesInHeap(env, _survivorMemorySubSpace, this);
		if (NULL != cache) {
		/* temporary chunk of scanCaches is created in Survivor */
#if defined(OMR_SCAVENGER_TRACE)
			omrtty_printf("{SCAV: Temporary chunk of scan cache headers allocated in Survivor (%p)}\n", cache);
#endif /* OMR_SCAVENGER_TRACE */
		}
	}

	if (NULL == cache) {
		/* try to create a temporary chunk of scanCaches in Tenure */
		cache = _scavengeCacheFreeList.appendCacheEntriesInHeap(env, _tenureMemorySubSpace, this);
		if (NULL != cache) {
			/* temporary chunk of scanCaches is created in Tenure */
#if defined(OMR_SCAVENGER_TRACE)
			omrtty_printf("{SCAV: Temporary chunk of scan cache headers allocated in Tenure (%p)}\n", cache);
#endif /* OMR_SCAVENGER_TRACE */
		}
	}

	if (NULL == cache) {
#if defined(OMR_SCAVENGER_TRACE)
		omrtty_printf("{SCAV: Temporary chunk of scan cache headers can not be allocated in Heap!}\n");
#endif /* OMR_SCAVENGER_TRACE */
	}

	omrthread_monitor_exit(_freeCacheMonitor);

	return cache;
}

void
MM_Scavenger::flushCache(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *cache)
{
	if (0 == (cache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY)) {
#if defined(OMR_SCAVENGER_TRACE)
		OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
		omrtty_printf("{SCAV: Flushing cache (%p) %p-%p-%p}\n", cache, cache->cacheBase, cache->cacheAlloc, cache->cacheTop);
#endif /* OMR_SCAVENGER_TRACE */
		if (0 == (cache->flags & OMR_SCAVENGER_CACHE_TYPE_CLEARED)) {
			clearCache(env, cache);
		}

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
		env->_scavengerStats._releaseFreeListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
		_scavengeCacheFreeList.pushCache(env, cache);
	}
}

bool
MM_Scavenger::canLocalCacheBeReused(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *cache)
{
	/* has not been scanned and no scan work to do - so it can be reused */
	return (NULL != cache) && !cache->isCurrentlyBeingScanned() && !cache->isScanWorkAvailable();
}

MM_CopyScanCacheStandard *
MM_Scavenger::releaseLocalCopyCache(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *cache)
{
	MM_CopyScanCacheStandard *cacheToReuse = NULL;

	if (NULL != cache) {
		/* Clear the current entry in the cache */
		bool remainderCreated = clearCache(env, cache);

		/* Handle an existing cache and return a new (virgin) cache */
		/* Check if the cache contains elements that need to be scanned - if not, just reuse the cache */
		if (cache->isCurrentlyBeingScanned()) {
			/* Since it is being scanned, cannot reuse and should not add to scan list */
			/* Mark the cache entry as unused as a copy destination */
			cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
		} else {

			if (NULL != env->_deferredCopyCache) {
				/* Deferred copy cache already exists. Check if should be merged with current cache */
				/* Deferred copy cache should also never be a scan cache */
				Assert_MM_false(env->_deferredCopyCache->flags & OMR_SCAVENGER_CACHE_TYPE_SCAN);
				/* We have two copy caches (deferred and current). Do they create contiguous memory with no objects (fully or partially) scanned in between? */
				if ((env->_deferredCopyCache->cacheAlloc == cache->scanCurrent) && !cache->_hasPartiallyScannedObject) {
					/* append current copy cache to the deferred one. yet, decide whether to keep deferring it or push it for scanning */
					Assert_MM_true((cache->flags & ~OMR_SCAVENGER_CACHE_TYPE_HEAP) == (env->_deferredCopyCache->flags & ~OMR_SCAVENGER_CACHE_TYPE_HEAP));
					Assert_MM_false(cache->flags & OMR_SCAVENGER_CACHE_TYPE_SPLIT_ARRAY);
					if (remainderCreated) {
						/* keep deferring the joint copy cache, there might be more appends to come */
						env->_deferredCopyCache->cacheAlloc = cache->cacheAlloc;
						cacheToReuse = cache;
						cache = NULL;
					} else {
						/* this was last possible append. we want to finally add the deferred cache to the scan list */
						/* we use deferredCopyCache for merged one. this way we preserve partial scanned object info, if any exists */
						env->_deferredCopyCache->cacheAlloc = cache->cacheAlloc;
						env->_deferredCopyCache->cacheTop = cache->cacheTop;
						cacheToReuse = cache;
						cache = env->_deferredCopyCache;
						env->_deferredCopyCache = NULL;
					}
				} else {
					/* deferred and current copy caches are discontiguous. pushing the current one, if scan work available */
					if (!cache->isScanWorkAvailable()) {
						cacheToReuse = cache;
						cache = NULL;
					}
				}
			} else {
				/* No deferred cache exists. Decide what to do with current one (defer, push for scanning, or just ignore) */
				if (cache->isScanWorkAvailable()) {
					/* make the current cache the deferred-copy one */
					if (remainderCreated) {
						env->_deferredCopyCache = cache;
						cache = NULL;
					}
					/* else, we have something to push onto the scan queue */
				} else {
					/* nothing to push, we can reuse this cache */
					cacheToReuse = cache;
					cache = NULL;
				}
			}

			if (cache) {
				/* Preceding code made sure that if there is a cache to push, it has some scan work */
				Assert_MM_true(cache->isScanWorkAvailable());

				/* assert that deferred or scan cache is not this cache */
				Assert_MM_true(cache != env->_scanCache);
				Assert_MM_true(cache != env->_deferredScanCache);

				/* It is not being scanned, and it does have entries to scan - add to scan list */
				/* Mark the cache entry as unused as a copy destination */
				Assert_MM_true(cache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY);
				cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
				/* must not have local references still in use before adding to global list */
				Assert_MM_true(cache->cacheBase <= cache->cacheAlloc);
				Assert_MM_true(cache->cacheAlloc <= cache->cacheTop);
				Assert_MM_true(cache->scanCurrent <= cache->cacheAlloc);
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
				env->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
				addCacheEntryToScanListAndNotify(env, cache);
			}
		}
	}

	return cacheToReuse;
}

bool
MM_Scavenger::clearCache(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *cache)
{
	MM_MemorySubSpace *allocSubSpace = NULL;
	uintptr_t discardSize = (uintptr_t)cache->cacheTop - (uintptr_t)cache->cacheAlloc;
	Assert_MM_false(cache->flags & OMR_SCAVENGER_CACHE_TYPE_CLEARED);
	bool remainderCreated = false;

	if (cache->flags & OMR_SCAVENGER_CACHE_TYPE_TENURESPACE) {
		allocSubSpace = _tenureMemorySubSpace;

		if(discardSize < env->getExtensions()->tlhTenureDiscardThreshold) {
			env->_scavengerStats._tenureDiscardBytes += discardSize;
			/* Abandon the current entry in the cache */
			allocSubSpace->abandonHeapChunk(cache->cacheAlloc, cache->cacheTop);
		} else {
			remainderCreated = true;
			env->_scavengerStats._tenureTLHRemainderCount += 1;
			Assert_MM_true(NULL == env->_tenureTLHRemainderBase);
			Assert_MM_true(NULL == env->_tenureTLHRemainderTop);
			env->_tenureTLHRemainderBase = cache->cacheAlloc;
			env->_tenureTLHRemainderTop = cache->cacheTop;
			env->_loaAllocation = (OMR_SCAVENGER_CACHE_TYPE_LOA == (cache->flags & OMR_SCAVENGER_CACHE_TYPE_LOA));
		}
	} else if (cache->flags & OMR_SCAVENGER_CACHE_TYPE_SEMISPACE) {
		allocSubSpace = _survivorMemorySubSpace;
		if(discardSize < env->getExtensions()->tlhSurvivorDiscardThreshold) {
			env->_scavengerStats._flipDiscardBytes += discardSize;
			allocSubSpace->abandonHeapChunk(cache->cacheAlloc, cache->cacheTop);
		} else {
			remainderCreated = true;
			env->_scavengerStats._survivorTLHRemainderCount += 1;
			Assert_MM_true(NULL == env->_survivorTLHRemainderBase);
			Assert_MM_true(NULL == env->_survivorTLHRemainderTop);
			env->_survivorTLHRemainderBase = cache->cacheAlloc;
			env->_survivorTLHRemainderTop = cache->cacheTop;
		}
	} else {
		Assert_MM_true(cache->flags & OMR_SCAVENGER_CACHE_TYPE_SPLIT_ARRAY);
		Assert_MM_true(0 == discardSize);
	}

	/* Broadcast details of that portion of memory within which objects have been allocated */
	TRIGGER_J9HOOK_MM_PRIVATE_CACHE_CLEARED(_extensions->privateHookInterface, env->getOmrVMThread(), allocSubSpace,
									cache->cacheBase, cache->cacheAlloc, cache->cacheTop);

	cache->flags |= OMR_SCAVENGER_CACHE_TYPE_CLEARED;

	return remainderCreated;
}

void
MM_Scavenger::abandonSurvivorTLHRemainder(MM_EnvironmentStandard *env)
{
	if (NULL != env->_survivorTLHRemainderBase) {
		Assert_MM_true(NULL != env->_survivorTLHRemainderTop);
		env->_scavengerStats._flipDiscardBytes += (uintptr_t)env->_survivorTLHRemainderTop - (uintptr_t)env->_survivorTLHRemainderBase;
		_survivorMemorySubSpace->abandonHeapChunk(env->_survivorTLHRemainderBase, env->_survivorTLHRemainderTop);
		env->_survivorTLHRemainderBase = NULL;
		env->_survivorTLHRemainderTop = NULL;
	}
}

void
MM_Scavenger::abandonTenureTLHRemainder(MM_EnvironmentStandard *env)
{
	if (NULL != env->_tenureTLHRemainderBase) {
		Assert_MM_true(NULL != env->_tenureTLHRemainderTop);
		env->_scavengerStats._tenureDiscardBytes += (uintptr_t)env->_tenureTLHRemainderTop - (uintptr_t)env->_tenureTLHRemainderBase;
		_tenureMemorySubSpace->abandonHeapChunk(env->_tenureTLHRemainderBase, env->_tenureTLHRemainderTop);
		env->_tenureTLHRemainderBase = NULL;
		env->_tenureTLHRemainderTop = NULL;
		env->_loaAllocation = false;
	}
}

void
MM_Scavenger::abandonTLHRemainders(MM_EnvironmentStandard *env)
{
	abandonSurvivorTLHRemainder(env);
	abandonTenureTLHRemainder(env);
}

void
MM_Scavenger::addCopyCachesToFreeList(MM_EnvironmentStandard *env)
{
	/* Should be already handled at this point */
	Assert_MM_true(NULL == env->_deferredScanCache);

	if(NULL != env->_survivorCopyScanCache) {
		env->_survivorCopyScanCache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
		flushCache(env, env->_survivorCopyScanCache);
		env->_survivorCopyScanCache = NULL;
	}
	if(NULL != env->_deferredCopyCache) {
		env->_deferredCopyCache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
		flushCache(env, env->_deferredCopyCache);
		env->_deferredCopyCache = NULL;
	}
	if(NULL != env->_tenureCopyScanCache) {
		env->_tenureCopyScanCache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
		flushCache(env, env->_tenureCopyScanCache);
		env->_tenureCopyScanCache = NULL;
	}
}

MMINLINE void
MM_Scavenger::addCacheEntryToScanListAndNotify(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *newCacheEntry)
{
	_scavengeCacheScanList.pushCache(env, newCacheEntry);
	if (0 != _waitingCount) {
		/* Added an entry to the list - notify any other threads that a new entry has appeared on the list */
		if (0 == omrthread_monitor_try_enter(_scanCacheMonitor)) {
			if (0 != _waitingCount) {
				omrthread_monitor_notify(_scanCacheMonitor);
			}
			omrthread_monitor_exit(_scanCacheMonitor);
		}
	}
}

MMINLINE uintptr_t
MM_Scavenger::scanCacheDistanceMetric(MM_CopyScanCacheStandard* scanCache, GC_SlotObject *scanSlot)
{
	/* distance from referring slot to prospective referent copy location */
	return ((uintptr_t)scanCache->cacheAlloc - (uintptr_t)scanSlot->readAddressFromSlot());
}

MMINLINE uintptr_t
MM_Scavenger::copyCacheDistanceMetric(MM_CopyScanCacheStandard* copyCache)
{
	/* (best estimate) distance from first reference slot to prospective referent copy location */
	return ((uintptr_t)copyCache->cacheAlloc - ((uintptr_t)copyCache->scanCurrent + J9_GC_MINIMUM_OBJECT_SIZE));
}

MMINLINE MM_CopyScanCacheStandard *
MM_Scavenger::aliasToCopyCache(MM_EnvironmentStandard *env, GC_SlotObject *scannedSlot, MM_CopyScanCacheStandard* scanCache, MM_CopyScanCacheStandard* copyCache)
{
	/* Only alias a copy cache IF there are 0 threads waiting.  If the current thread is the only producer and
	 * it aliases a copy cache then it will be the only thread able to consume.  This will alleviate the stalling issues
	 * described in VMDESIGN 1359.
	 */
	if (0 == _waitingCount) {
		/* was the object received by an unaliased copy cache or aliased scan cache? */
		if (scanCache != copyCache) {
			/* unaliased copy cache received the object; alias and switch to it if possible */
			if ((0 == (scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY))
				|| (copyCacheDistanceMetric(copyCache) < scanCacheDistanceMetric(scanCache, scannedSlot))
			) {
				env->_scavengerStats._aliasToCopyCacheCount += 1;
				scanCache->_hasPartiallyScannedObject = true;
				return copyCache;
			}
		} else {
			/* scan cache is aliased and received the object so identify the other copy cache */
			if (0 == (scanCache->flags & OMR_SCAVENGER_CACHE_TYPE_SEMISPACE)) {
				copyCache = env->_survivorCopyScanCache;
			} else {
				copyCache = env->_tenureCopyScanCache;
			}
			/* alias and switch to copy cache if it has scan work available and a shorter copy distance */
			if ((NULL != copyCache)
				&& (copyCacheDistanceMetric(copyCache) < scanCacheDistanceMetric(scanCache, scannedSlot))
				&& (copyCache->cacheAlloc != copyCache->scanCurrent)
			) {
				env->_scavengerStats._aliasToCopyCacheCount += 1;
				scanCache->_hasPartiallyScannedObject = true;
				return copyCache;
			}
		}
	}
	return NULL;
		}

MMINLINE MM_CopyScanCacheStandard *
MM_Scavenger::getNextScanCacheFromList(MM_EnvironmentStandard *env)
{
	return _scavengeCacheScanList.popCache(env);
	}

MMINLINE MM_CopyScanCacheStandard *
MM_Scavenger::getDeferredCopyCache(MM_EnvironmentStandard *env)
{
	return env->_deferredCopyCache;
	}

MMINLINE MM_CopyScanCacheStandard *
MM_Scavenger::getSurvivorCopyCache(MM_EnvironmentStandard *env)
{
	MM_CopyScanCacheStandard *cache = NULL;

	if (isWorkAvailableInCache(env->_survivorCopyScanCache)) {
		cache = env->_survivorCopyScanCache;
	}

	return cache;
}

/**
 * Determine whether a scavenge that has been started did complete successfully.
 * @return true if the scavenge completed successfully, false otherwise.
 */
bool
MM_Scavenger::scavengeCompletedSuccessfully(MM_EnvironmentStandard *env)
{
	return !backOutFlagRaised();
}

/****************************************
 * Scavenge back out impl
 ****************************************
 */

/**
 * Inform consumers of Scavenger backout status.
 * Change the value of _backOutFlag and inform consumers.
 */
void
MM_Scavenger::setBackOutFlag(MM_EnvironmentBase *env, bool value)
{
	/* Skip triggering of trace point and hook if we trying to set flag to true multiple times */
	if (!(_backOutFlag && value)) {
		_backOutDoneIndex = _doneIndex;
		_backOutFlag = value;
		/* Might be an overkill, but ensure that other CPUs see correct _backOutDoneIndex, by the time they see _backOutFlag is set */
		MM_AtomicOperations::writeBarrier();
		Trc_MM_ScavengerBackout(env->getLanguageVMThread(), value ? "true" : "false");
		TRIGGER_J9HOOK_MM_PRIVATE_SCAVENGER_BACK_OUT(_extensions->privateHookInterface, env->getOmrVM(), value);
	}
}

bool
MM_Scavenger::backOutFixSlotWithoutCompression(volatile omrobjectptr_t *slotPtr)
{
	omrobjectptr_t objectPtr = *slotPtr;

	if(NULL != objectPtr) {
		MM_ForwardedHeader forwardHeader(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
		Assert_MM_false(forwardHeader.isForwardedPointer());
		if (forwardHeader.isReverseForwardedPointer()) {
			*(omrobjectptr_t*)slotPtr = forwardHeader.getReverseForwardedPointer();
			return true;
		}
	}
	return false;
}

bool
MM_Scavenger::backOutFixSlot(GC_SlotObject *slotObject)
{
	omrobjectptr_t objectPtr = slotObject->readReferenceFromSlot();

	if(NULL != objectPtr) {
		MM_ForwardedHeader forwardHeader(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
		Assert_MM_false(forwardHeader.isForwardedPointer());
		if (forwardHeader.isReverseForwardedPointer()) {
			omrobjectptr_t fwdObjectPtr = forwardHeader.getReverseForwardedPointer();
			slotObject->writeReferenceToSlot(fwdObjectPtr);
	return true;
}
	}
	return false;
}

void
MM_Scavenger::backOutObjectScan(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	GC_SlotObject *slotObject = NULL;
	GC_ObjectScannerState objectScannerState;
	GC_ObjectScanner *objectScanner = _cli->scavenger_getObjectScanner(env, objectPtr, (void *) &objectScannerState, GC_ObjectScanner::scanRoots);
	if (NULL != objectScanner) {
		while (NULL != (slotObject = objectScanner->getNextSlot())) {
			backOutFixSlot(slotObject);
		}
	}

	if (_extensions->objectModel.hasIndirectObjectReferents(env->getLanguageVMThread(), objectPtr)) {
		_cli->scavenger_backOutIndirectObjectSlots(env, objectPtr);
	}
}

void
MM_Scavenger::backoutFixupAndReverseForwardPointersInSurvivor(MM_EnvironmentStandard *env)
{
	GC_MemorySubSpaceRegionIteratorStandard evacuateRegionIterator(_activeSubSpace);
	MM_HeapRegionDescriptorStandard* rootRegion = NULL;

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
	while(NULL != (rootRegion = evacuateRegionIterator.nextRegion())) {
		/* skip survivor regions */
		if (isObjectInEvacuateMemory((omrobjectptr_t )rootRegion->getLowAddress())) {
			/* tell the object iterator to work on the given region */
			GC_ObjectHeapIteratorAddressOrderedList evacuateHeapIterator(_extensions, rootRegion, false);
			omrobjectptr_t objectPtr = NULL;

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
			omrtty_printf("{SCAV: Reverse forward pointers in [%p->%p]}\n", rootRegion->getLowAddress(), rootRegion->getHighAddress());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

			while((objectPtr = evacuateHeapIterator.nextObjectNoAdvance()) != NULL) {
				MM_ForwardedHeader header(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
				_cli->scavenger_reverseForwardedObject(env, &header);
			}
		}
	}

#if defined (OMR_INTERP_COMPRESSED_OBJECT_HEADER)
	GC_MemorySubSpaceRegionIteratorStandard evacuateRegionIterator1(_activeSubSpace);
	while(NULL != (rootRegion = evacuateRegionIterator1.nextRegion())) {
		if (isObjectInEvacuateMemory((omrobjectptr_t )rootRegion->getLowAddress())) {
			/*
			 * CMVC 179190:
			 * The call to "reverseForwardedObject", above, destroys our ability to detect if this object needs its destroyed slot fixed up (but
			 * the above loop must complete before we have the information with which to fixup the destroyed slot).  Fixing up a slot in dark
			 * matter could crash, though, since the slot could point to contracted memory or could point to corrupted data updated in a previous
			 * backout.  The simple work-around for this problem is to check if the slot points at a readable part of the heap (specifically,
			 * tenure or survivor - the only locations which would require us to fix up the slot) and only read and fixup the slot in those cases.
			 * This means that we could still corrupt the slot but we will never crash during fixup and nobody else should be trusting slots found
			 * in dead objects.
			 */
			GC_ObjectHeapIteratorAddressOrderedList evacuateHeapIterator(_extensions, rootRegion, false);
			omrobjectptr_t objectPtr = NULL;

			while((objectPtr = evacuateHeapIterator.nextObjectNoAdvance()) != NULL) {
				MM_ForwardedHeader header(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
				_cli->scavenger_fixupDestroyedSlot(env, &header, _activeSubSpace);
			}
		}
	}
#endif /* defined (OMR_INTERP_COMPRESSED_OBJECT_HEADER) */
}

void
MM_Scavenger::completeBackOut(MM_EnvironmentStandard *env)
{
	/* Work to be done:
	 * 1) Flush copy scan caches
	 * 2) Walk the evacuate space, fixing up objects and installing reverse forward pointers in survivor space
	 * 3) Restore the remembered set
	 * 4) Client language completion of back out
	 */

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

	/* Ensure we've pushed all references from buffers out to the lists */
	_cli->scavenger_flushReferenceObjects(env);

	/* Must synchronize to be sure all private caches have been flushed */
	if (env->_currentTask->synchronizeGCThreadsAndReleaseMaster(env, UNIQUE_ID)) {
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
		omrtty_printf("{SCAV: Complete back out(%p)}\n", env->getLanguageVMThread());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

		/* 1) Flush copy scan caches */
		MM_CopyScanCacheStandard *cache = NULL;

		while (NULL != (cache = _scavengeCacheScanList.popCache(env))) {
			flushCache(env, cache);
		}
		Assert_MM_true(0 == _cachedEntryCount);

		/* 2
		 * a) Mark the overflow scan as invalid (backing out of objects moved into old space)
		 * b) If the remembered set is in an overflow state,
		 *    i) Unremember any objects that moved from new space to old
		 *    ii) Walk old space and build up the overflow list
		 */
		_extensions->scavengerRsoScanUnsafe = true;

		if(isRememberedSetInOverflowState()) {
			GC_MemorySubSpaceRegionIterator evacuateRegionIterator(_activeSubSpace);
			MM_HeapRegionDescriptor* rootRegion;

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
			omrtty_printf("{SCAV: Handle RS overflow}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

			/* i) Unremember any objects that moved from new space to old */
			while(NULL != (rootRegion = evacuateRegionIterator.nextRegion())) {
				/* skip survivor regions */
				if (isObjectInEvacuateMemory((omrobjectptr_t)rootRegion->getLowAddress())) {
					/* tell the object iterator to work on the given region */
					GC_ObjectHeapIteratorAddressOrderedList evacuateHeapIterator(_extensions, rootRegion, false);
					omrobjectptr_t objectPtr = NULL;
					omrobjectptr_t fwdObjectPtr = NULL;
					while((objectPtr = evacuateHeapIterator.nextObjectNoAdvance()) != NULL) {
						MM_ForwardedHeader header(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET);
						fwdObjectPtr = header.getForwardedObject();
						if (NULL != fwdObjectPtr) {
							if(_extensions->objectModel.isRemembered(fwdObjectPtr)) {
								_extensions->objectModel.clearRemembered(fwdObjectPtr);
							}
							/* Move to the next object - the heap iterator is incapable of dealing with
							 * tagged class pointers.
							 */
							/* If this scavenge was the first move of a hashed object, the sizes don't match.
							 * This approach won't work if the flags of the original (i.e. evacuate space)
							 * object are destroyed.
							 */
							if(_extensions->objectModel.isObjectJustHasBeenMoved(fwdObjectPtr)) {
								uintptr_t size = _extensions->objectModel.getSizeInBytesWithHeader(fwdObjectPtr);
								size = _extensions->objectModel.adjustSizeInBytes(size);
								evacuateHeapIterator.advance(size);
							} else
								evacuateHeapIterator.advance(_extensions->objectModel.getConsumedSizeInBytesWithHeader(fwdObjectPtr));
						}
					}
				}
			}

			/* ii) Walk old space and build up the overflow list */
			/* the list is built because after reverse fwd ptrs are installed, the heap becomes unwalkable */
			clearRememberedSetLists(env);

			MM_RSOverflow rememberedSetOverflow(env);
			addAllRememberedObjectsToOverflow(env, &rememberedSetOverflow);

			/*
			 * 2.c)Walk the evacuate space, fixing up objects and installing reverse forward pointers in survivor space
			 */
			backoutFixupAndReverseForwardPointersInSurvivor(env);

			/* 3) Walk the remembered set, updating list pointers as well as remembered object ptrs */
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
			omrtty_printf("{SCAV: Back out RS overflow}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

			/* Walk the remembered set overflow list built earlier */
			omrobjectptr_t objectOverflow;
			while (NULL != (objectOverflow = rememberedSetOverflow.nextObject())) {
				backOutObjectScan(env, objectOverflow);
			}

			/* Walk all classes that are flagged as remembered */
			_cli->scavenger_backOutIndirectObjects(env);
		} else {
			/*
			 * 2.c)Walk the evacuate space, fixing up objects and installing reverse forward pointers in survivor space
			 */
			backoutFixupAndReverseForwardPointersInSurvivor(env);

			/* Walk the remembered set removing any tagged entries (back out of a tenured copy that is remembered)
			 * and scanning remembered objects for reverse fwd info
			 */
			omrobjectptr_t *slotPtr;
			omrobjectptr_t objectPtr;
			MM_SublistPuddle *puddle;

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
			omrtty_printf("{SCAV: Back out RS list}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

			GC_SublistIterator remSetIterator(&(_extensions->rememberedSet));
			while((puddle = remSetIterator.nextList()) != NULL) {
				GC_SublistSlotIterator remSetSlotIterator(puddle);
				while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
					/* clear any remove pending flags */
					*slotPtr = (omrobjectptr_t)((uintptr_t)*slotPtr & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG);
					objectPtr = *slotPtr;

					if(objectPtr) {
						if (MM_ForwardedHeader(objectPtr, OMR_OBJECT_METADATA_SLOT_OFFSET).isReverseForwardedPointer()) {
							remSetSlotIterator.removeSlot();
						} else {
							backOutObjectScan(env, objectPtr);
						}
					} else {
						remSetSlotIterator.removeSlot();
					}
				}
			}
		}

		MM_ScavengerBackOutScanner backOutScanner(env, true, this);
		backOutScanner.scanAllSlots(env);

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
		omrtty_printf("{SCAV: Done back out}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
		env->_currentTask->releaseSynchronizedGCThreads(env);
	}
}

/**
 * Setup, execute and complete a scavenge.
 */
void
MM_Scavenger::masterThreadGarbageCollect(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	Trc_MM_Scavenger_masterThreadGarbageCollect_Entry(env->getLanguageVMThread());

	if (_extensions->trackMutatorThreadCategory) {
		/* This thread is doing GC work, account for the time spent into the GC bucket */
		omrthread_set_category(env->getOmrVMThread()->_os_thread, J9THREAD_CATEGORY_SYSTEM_GC_THREAD, J9THREAD_TYPE_SET_GC);
	}

	if (_extensions->processLargeAllocateStats) {
		processLargeAllocateStatsBeforeGC(env);
	}

	reportGCCycleStart(env);
	reportGCStart(env);
	reportGCIncrementStart(env);
	reportScavengeStart(env);

	_extensions->scavengerStats._startTime = omrtime_hires_clock();

	/* Perform any master-specific setup */
	masterSetupForGC(env);

	/* And perform the scavenge */
	scavenge(env);

	/* defer to collector language interface */
	_cli->scavenger_masterThreadGarbageCollect_scavengeComplete(env);

	/* Record the completion time of the scavenge */
	_extensions->scavengerStats._endTime = omrtime_hires_clock();

	reportScavengeEnd(env);

	/* Reset the resizable flag of the semi space.
	 * NOTE: Must be done before we attempt to resize the new space.
	 */
	_activeSubSpace->setResizable(_cachedSemiSpaceResizableFlag);

	if(scavengeCompletedSuccessfully(env)) {
		/* Merge sublists in the remembered set (if necessary) */
		_extensions->rememberedSet.compact(env);

		/* Must report object events before memory spaces are flipped */
		_cli->scavenger_reportObjectEvents(env);
		
		/* If -Xgc:fvtest=forcePoisonEvacuate has been specified, poison(fill poison pattern) evacuate space */
		if(_extensions->fvtest_forcePoisonEvacuate) {
			_activeSubSpace->poisonEvacuateSpace();
		}

		/* Build free list in evacuate profile */
		_activeSubSpace->rebuildFreeListForEvacuate(env);

		/* Flip the memory space allocate profile */
		_activeSubSpace->flip();

		/* Let know MemorySpace about new default MemorySubSpace */
		_activeSubSpace->getMemorySpace()->setDefaultMemorySubSpace(_activeSubSpace->getDefaultMemorySubSpace());

		/* Adjust memory between the semi spaces where applicable */
		_activeSubSpace->checkResize(env);
		_activeSubSpace->performResize(env);

		/* Defer to collector language interface */
		_cli->scavenger_masterThreadGarbageCollect_scavengeSuccess(env);

		if(_extensions->scvTenureStrategyAdaptive) {
			/* Adjust the tenure age based on the percentage of new space used.  Also, avoid / by 0 */
			uintptr_t newSpaceTotalSize = _activeSubSpace->getActiveMemorySize();
			uintptr_t newSpaceConsumedSize = newSpaceTotalSize - _activeSubSpace->getActualActiveFreeMemorySize();
			uintptr_t newSpaceSizeScale = newSpaceTotalSize / 100;

			if((newSpaceConsumedSize < (_extensions->scvTenureRatioLow * newSpaceSizeScale)) && (_extensions->scvTenureAdaptiveTenureAge < OBJECT_HEADER_AGE_MAX)) {
				_extensions->scvTenureAdaptiveTenureAge++;
			} else {
				if((newSpaceConsumedSize > (_extensions->scvTenureRatioHigh * newSpaceSizeScale)) && (_extensions->scvTenureAdaptiveTenureAge > OBJECT_HEADER_AGE_MIN)) {
					_extensions->scvTenureAdaptiveTenureAge--;
				}
			}
		}
	} else {
		/* Build free list in survivor profile - the scavenge was unsuccessful, so rebuild the free list */
		_activeSubSpace->rebuildFreeListForBackout(env);
	}

	/* Restart the allocation caches associated to all threads */
	{
		GC_OMRVMThreadListIterator threadListIterator(_omrVM);
		OMR_VMThread *walkThread;
		while((walkThread = threadListIterator.nextOMRVMThread()) != NULL) {
			MM_EnvironmentBase *walkEnv = MM_EnvironmentBase::getEnvironment(walkThread);
			walkEnv->_objectAllocationInterface->restartCache(env);
		}
	}

	_extensions->heap->resetHeapStatistics(false);

	/* If there was a failed tenure of a size greater than the threshold, set the flag. */
	/* The next attempt to scavenge will result in a global collect */
	if (_extensions->scavengerStats._failedTenureCount > 0) {
		if (_extensions->scavengerStats._failedTenureBytes >= _extensions->scavengerFailedTenureThreshold) {
			Trc_MM_Scavenger_masterThreadGarbageCollect_setFailedTenureFlag(env->getLanguageVMThread(), _extensions->scavengerStats._failedTenureLargest);
			setFailedTenureThresholdFlag();
			setFailedTenureLargestObject(_extensions->scavengerStats._failedTenureLargest);
		}
	}
	if (_extensions->processLargeAllocateStats) {
		processLargeAllocateStatsAfterGC(env);
	}
	
	reportGCCycleFinalIncrementEnding(env);
	reportGCIncrementEnd(env);
	reportGCEnd(env);
	reportGCCycleEnd(env);

	if (_extensions->processLargeAllocateStats) {
		/* reset tenure processLargeAllocateStats after TGC */ 
		resetTenureLargeAllocateStats(env);
	}
	_extensions->allocationStats.clear();

	if (_extensions->trackMutatorThreadCategory) {
		/* Done doing GC, reset the category back to the old one */
		omrthread_set_category(env->getOmrVMThread()->_os_thread, 0, J9THREAD_TYPE_SET_GC);
	}

	Trc_MM_Scavenger_masterThreadGarbageCollect_Exit(env->getLanguageVMThread());
}

void
MM_Scavenger::processLargeAllocateStatsBeforeGC(MM_EnvironmentBase *env) 
{
	MM_MemorySpace *defaultMemorySpace = _extensions->heap->getDefaultMemorySpace();
	MM_MemorySubSpace *defaultMemorySubspace = defaultMemorySpace->getDefaultMemorySubSpace();
	MM_MemorySubSpace *tenureMemorySubspace = defaultMemorySpace->getTenureMemorySubSpace();

	/* merge largeObjectAllocateStats in nursery space */
	if (defaultMemorySubspace->isPartOfSemiSpace()) {
		/* SemiSpace stats include only Mutator stats (no Collector stats during flipping) */
		defaultMemorySubspace->getTopLevelMemorySubSpace(MEMORY_TYPE_NEW)->mergeLargeObjectAllocateStats(env);
	}
	
	/* TODO: remove the below 2 lines(resetLargeObjectAllocateStats), so that we do not loose direct mutator allocation info */ 
	MM_MemoryPool *tenureMemoryPool = tenureMemorySubspace->getMemoryPool();
	tenureMemoryPool->resetLargeObjectAllocateStats();
}

void 
MM_Scavenger::processLargeAllocateStatsAfterGC(MM_EnvironmentBase *env) 
{
	MM_MemorySpace *defaultMemorySpace = _extensions->heap->getDefaultMemorySpace();
	MM_MemorySubSpace *tenureMemorySubspace = defaultMemorySpace->getTenureMemorySubSpace();
	MM_MemoryPool *memoryPool = tenureMemorySubspace->getMemoryPool();

	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	uint64_t startTime = omrtime_hires_clock();

	/* merge and average largeObjectAllocateStats in tenure space */
	memoryPool->mergeLargeObjectAllocateStats();
	memoryPool->mergeTlhAllocateStats();
	/* TODO: need to consider allocation form mutators for averaging later, currently only average allocation from collectors */
	memoryPool->averageLargeObjectAllocateStats(env, _extensions->scavengerStats._tenureAggregateBytes);
	/* merge FreeEntry AllocateStats in tenure space */
	memoryPool->mergeFreeEntryAllocateStats();
	MM_LargeObjectAllocateStats *stats = memoryPool->getLargeObjectAllocateStats();
	stats->setTimeMergeAverage(omrtime_hires_clock() - startTime);

	stats->verifyFreeEntryCount(memoryPool->getActualFreeEntryCount());
	/* estimate Fragmentation */
	if (LOCALGC_ESTIMATE_FRAGMENTATION == (_extensions->estimateFragmentation & LOCALGC_ESTIMATE_FRAGMENTATION)) {
		stats->estimateFragmentation(env);
	} else {
		stats->resetRemainingFreeMemoryAfterEstimate();
	}
}


void
MM_Scavenger::reportGCCycleFinalIncrementEnding(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	MM_CommonGCData commonData;

	TRIGGER_J9HOOK_MM_OMR_GC_CYCLE_END(
		_extensions->omrHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_OMR_GC_CYCLE_END,
		_extensions->getHeap()->initializeCommonGCData(env, &commonData),
		env->_cycleState->_type,
		omrgc_condYieldFromGC
	);
}

/**
 * Re-size all structures which are dependent on the current size of the heap.
 *
 * @param size The amount of memory added to the heap
 * @param lowAddress The base address of the memory added to the heap
 * @param highAddress The top address (non-inclusive) of the memory added to the heap
 * @return true if operation completes with success
 */
bool
MM_Scavenger::heapAddRange(MM_EnvironmentBase *env, MM_MemorySubSpace *subspace, uintptr_t size, void *lowAddress, void *highAddress)
{
	return true;
}

/**
 * Re-size all structures which are dependent on the current size of the heap.
 *
 * @param size The amount of memory added to the heap
 * @param lowAddress The base address of the memory added to the heap
 * @param highAddress The top address (non-inclusive) of the memory added to the heap
 * @param lowValidAddress The first valid address previous to the lowest in the heap range being removed
 * @param highValidAddress The first valid address following the highest in the heap range being removed
 * @return true if operation completes with success
 */
bool
MM_Scavenger::heapRemoveRange(
	MM_EnvironmentBase *env, MM_MemorySubSpace *subspace, uintptr_t size, void *lowAddress, void *highAddress,
	void *validLowAddress, void *validHighAddress)
{
	return true;
}

/**
 * Report API for when an expansion has occurred during a collection.
 * @seealso MM_Collector::collectorExpanded(MM_EnvironmentBase *, MM_MemorySubSpace *, uintptr_t)
 * @note This call is NOT made pre/post collection, when actual user type allocation requests are being statisfied.
 * @note an expandSize of 0 represents a failed expansion attempt
 * @param subSpace memory subspace that has expanded.
 * @param expandSize number of bytes the subspace was expanded by.
 */
void
MM_Scavenger::collectorExpanded(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, uintptr_t expandSize)
{
	MM_Collector::collectorExpanded(env, subSpace, expandSize);

	if(0 == expandSize) {
		/* Cause a ggc on next scav as expand of tenure failed */
		setExpandFailedFlag();

		/* Expand failed so stop subsequent attempts during this scavenge */
		_expandTenureOnFailedAllocate = false;
	} else {
		MM_HeapResizeStats *resizeStats = _extensions->heap->getResizeStats();
		Assert_MM_true(SATISFY_COLLECTOR == resizeStats->getLastExpandReason());
		Assert_MM_true(MEMORY_TYPE_OLD == subSpace->getTypeFlags());

		env->_scavengerStats._tenureExpandedCount += 1;
		env->_scavengerStats._tenureExpandedBytes += expandSize;
		env->_scavengerStats._tenureExpandedTime += resizeStats->getLastExpandTime();

		uintptr_t totalActiveCacheCount = _extensions->heap->getActiveMemorySize(MEMORY_TYPE_NEW) / _extensions->scavengerScanCacheMinimumSize;

		/* TODO: can fail? */
		_scavengeCacheFreeList.resizeCacheEntries(env, totalActiveCacheCount, 0);
	}
}

/**
 * Answer whether the subspace can expand on a collector-invoked allocate request.
 * The query is made only on collectorAllocate() type requests when the allocation fails, and a decision on whether
 * to expand the subspace to satisfy the allocate is being made.
 * @seealso MM_Collector::canCollectorExpand(MM_EnvironmentBase *, MM_MemorySubSpace *, uintptr_t)
 * @note Call is only made during collection and during a collectorAllocate() type request to the subspace.
 * @return true if the subspace is allowed to expand, false otherwise.
 */
bool
MM_Scavenger::canCollectorExpand(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, uintptr_t expandSize)
{
	/* the only known caller is SubSpaceFlat */
	Assert_MM_true(subSpace == _tenureMemorySubSpace->getParent());
	return  _expandTenureOnFailedAllocate;
}

/**
 * Returns requested expand size.
 * The query is made only on collectorAllocate() type requests when the allocation fails, and a decision
 * on how much to expand the subspace to satisfy the allocate is being made.
 * @seealso MM_Collector::getCollectorExpandSize(MM_EnvironmentBase *)
 * @note Call is only made during collection and during a collectorAllocate() type request to the subspace.
 * @return size to subspace expand by.
 * @ingroup GC_Modron_base methodGroup
 */
uintptr_t
MM_Scavenger::getCollectorExpandSize(MM_EnvironmentBase *env)
{
	MM_ScavengerStats *scavengerGCStats= &_extensions->scavengerStats;
	uintptr_t expandSize =  ( (uintptr_t)(scavengerGCStats->_avgTenureBytes * _extensions->scavengerCollectorExpandRatio));
	expandSize = OMR_MIN(_extensions->scavengerMaximumCollectorExpandSize, expandSize);
	return expandSize;
}

/**
 * Perform any pre-collection work as requested by the garbage collection invoker.
 */
void
MM_Scavenger::internalPreCollect(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription, uint32_t gcCode)
{
	_cycleState = MM_CycleState();
	env->_cycleState = &_cycleState;
	env->_cycleState->_gcCode = MM_GCCode(gcCode);
	env->_cycleState->_type = _cycleType;
	env->_cycleState->_collectionStatistics = &_collectionStatistics;

	/* If we are in an excessiveGC level beyond normal then an aggressive GC is
	 * conducted to free up as much space as possible
	 */
	if (!env->_cycleState->_gcCode.isExplicitGC()) {
		if(excessive_gc_normal != _extensions->excessiveGCLevel) {
			/* convert the current mode to excessive GC mode */
			env->_cycleState->_gcCode = MM_GCCode(J9MMCONSTANT_IMPLICIT_GC_EXCESSIVE);
		}
	}

	/* Flush any VM level changes to prepare for a safe slot walk */
	GC_OMRVMInterface::flushCachesForGC(env);
}

/**
 * Perform any post-collection work as requested by the garbage collection invoker.
 */
void
MM_Scavenger::internalPostCollect(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace)
{
	calcGCStats((MM_EnvironmentStandard*)env);

	return ;
}

/**
 * Internal API for invoking a garbage collect.
 * @return true if the collection completed successfully, false otherwise.
 */
bool
MM_Scavenger::internalGarbageCollect(MM_EnvironmentBase *envBase, MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription)
{
	MM_EnvironmentStandard *env = (MM_EnvironmentStandard *)envBase;
	MM_ScavengerStats *scavengerGCStats= &_extensions->scavengerStats;
	MM_MemorySubSpace *tenureMemorySubSpace = ((MM_MemorySubSpaceSemiSpace *)subSpace)->getTenureMemorySubSpace();

	/* First, if the previous scavenge had a failed tenure of a size greater than the threshold,
	 * ask parent MSS to try a collect.
	 */
	if (failedTenureThresholdReached()) {
		Trc_MM_Scavenger_percolate_failedTenureThresholdReached(env->getLanguageVMThread(), getFailedTenureLargestObject(), _extensions->heap->getPercolateStats()->getScavengesSincePercolate());

		/* Create an allocate description to describe the size of the
		 * largest chunk we need in the tenure space.
		 */
		MM_AllocateDescription percolateAllocDescription(getFailedTenureLargestObject(), OMR_GC_ALLOCATE_OBJECT_TENURED, false, true);

		/* We do an aggressive percolate if the last scavenge also percolated */
		uint32_t aggressivePercolate = _extensions->heap->getPercolateStats()->getScavengesSincePercolate() <= 1 ? J9MMCONSTANT_IMPLICIT_GC_PERCOLATE_AGGRESSIVE : J9MMCONSTANT_IMPLICIT_GC_PERCOLATE;

		/* Percolate the collect to parent MSS */
		bool result = percolateGarbageCollect(env, subSpace, &percolateAllocDescription, FAILED_TENURE, aggressivePercolate);

		/* Global GC must be executed */
		Assert_MM_true(result);

		/* Should have been reset by globalCollectionComplete() broadcast event */
		Assert_MM_true(!failedTenureThresholdReached());
		return true;
	}

	/*
	 * Second, if the previous scavenge failed to expand tenure, ask parent MSS to try a collect.
	 */
	if (expandFailed()) {
		Trc_MM_Scavenger_percolate_expandFailed(env->getLanguageVMThread());
	
		/* We do an aggressive percolate if the last scavenge also percolated */
		uint32_t aggressivePercolate = _extensions->heap->getPercolateStats()->getScavengesSincePercolate() <= 1 ? J9MMCONSTANT_IMPLICIT_GC_PERCOLATE_AGGRESSIVE : J9MMCONSTANT_IMPLICIT_GC_PERCOLATE;

		/* Aggressive percolate the collect to parent MSS */
		bool result = percolateGarbageCollect(env, subSpace, NULL, EXPAND_FAILED, aggressivePercolate);

		/* Global GC must be executed */
		Assert_MM_true(result);

		/* Should have been reset by globalCollectionComplete() broadcast event */
		Assert_MM_true(!expandFailed());
		return true;
	}

	/* If the tenure MSS is not expandable and/or  there is insufficent space left to tenure
	 * the average number of bytes tenured by a scavenge then percolate the collect to avoid
	 * an aborted scavenge and its associated time consuming backout
	 */
	if ((tenureMemorySubSpace->maxExpansionInSpace(env) + tenureMemorySubSpace->getApproximateActiveFreeMemorySize()) < scavengerGCStats->_avgTenureBytes ) {
		Trc_MM_Scavenger_percolate_insufficientTenureSpace(env->getLanguageVMThread(), tenureMemorySubSpace->maxExpansionInSpace(env), tenureMemorySubSpace->getApproximateActiveFreeMemorySize(), scavengerGCStats->_avgTenureBytes);

		/* Percolate the collect to parent MSS */
		bool result = percolateGarbageCollect(env, subSpace, NULL, INSUFFICIENT_TENURE_SPACE, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);

		/* Global GC must be executed */
		Assert_MM_true(result);

		return true;
	}

	/* If it has been too long since a global GC, execute one instead of a scavenge. */
	//TODO Probably should rename this -Xgc option as it may not always result in a ggc
	//in futre, e.g if we implement multiple generations.
	if (_extensions->maxScavengeBeforeGlobal) {
		if (_countSinceForcingGlobalGC++ >= _extensions->maxScavengeBeforeGlobal) {
			Trc_MM_Scavenger_percolate_maxScavengeBeforeGlobal(env->getLanguageVMThread(), _extensions->maxScavengeBeforeGlobal);

			/* Percolate the collect to parent MSS */
			bool result = percolateGarbageCollect(env, subSpace, NULL, MAX_SCAVENGES, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);

			/* Global GC must be executed */
			Assert_MM_true(result);

			/* Should have been reset by globalCollectionComplete() broadcast event */
			Assert_MM_true(_countSinceForcingGlobalGC == 0);
			return true;
		}
	}

	/**
	 * Language percolation trigger	
	 * Allow the CollectorLanguageInterface to advise if percolation should occur.
	 */
	PercolateReason percolateReason = NONE_SET;
	uint32_t gcCode = J9MMCONSTANT_IMPLICIT_GC_DEFAULT;

	bool shouldPercolate = _cli->scavenger_internalGarbageCollect_shouldPercolateGarbageCollect(env, & percolateReason, & gcCode);

	if (shouldPercolate) {
		bool didPercolate = percolateGarbageCollect(env, subSpace, NULL, percolateReason, gcCode);
		/* Percolation must occur if required by the cli. */
		if (didPercolate) {
			return true;
		}
	}

	/* Check if there is an RSO and the heap is not safely walkable */
	if(isRememberedSetInOverflowState() && _extensions->scavengerRsoScanUnsafe) {
		/* NOTE: No need to set that the collect was unsuccessful - we will actually execute
		 * the scavenger after percolation.
		 */

		Trc_MM_Scavenger_percolate_rememberedSetOverflow(env->getLanguageVMThread());

		/* Percolate the collect to parent MSS */
		percolateGarbageCollect(env, subSpace, NULL, RS_OVERFLOW, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);
	}

	_extensions->heap->getPercolateStats()->incrementScavengesSincePercolate();

	_extensions->scavengerStats._gcCount += 1;
	env->_cycleState->_activeSubSpace = subSpace;
	_collectorExpandedSize = 0;

	masterClearHotFieldStats();

	masterThreadGarbageCollect(env);

	masterReportHotFieldStats();

	/* If we know now that the next scavenge will cause a peroclate broadcast
	 * the fact so other parties can react, e.g concurrrent can adjust KO threshold
	 */

	if (failedTenureThresholdReached()
		|| expandFailed()
		|| (_extensions->maxScavengeBeforeGlobal && _countSinceForcingGlobalGC == _extensions->maxScavengeBeforeGlobal)
		|| ((tenureMemorySubSpace->maxExpansionInSpace(env) + tenureMemorySubSpace->getApproximateActiveFreeMemorySize()) < scavengerGCStats->_avgTenureBytes)) {
		_extensions->scavengerStats._nextScavengeWillPercolate = true;
	}

	return true;
}
/**
 * Percolate the garbage collect to the parent memory sub space
 *
 * @param allocate descriptor describing the allocation request the collector should aim to satify
 * @param percolateReason code for the percolate
 * @param gcCode GC code requested
 * @return true if Global GC was executed, false if concurrent kickoff forced or Global GC is not possible
 */
bool
MM_Scavenger::percolateGarbageCollect(MM_EnvironmentBase *envModron,  MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription, PercolateReason percolateReason, uint32_t gcCode)
{
	/* save the cycle state since we are about to call back into the collector to start a new global cycle */
	MM_CycleState *scavengeCycleState = envModron->_cycleState;
	Assert_MM_true(NULL != scavengeCycleState);
	envModron->_cycleState = NULL;

	/* Set last percolate reason */
	_extensions->heap->getPercolateStats()->setLastPercolateReason(percolateReason);

	/* Percolate the collect to parent MSS */
	bool result = subSpace->percolateGarbageCollect(envModron, allocDescription, gcCode);

	/* Reset last Percolate reason */
	_extensions->heap->getPercolateStats()->resetLastPercolateReason();

	if (result) {
		_extensions->heap->getPercolateStats()->clearScavengesSincePercolate();
	}

	/* restore the cycle state to maintain symmetry */
	Assert_MM_true(NULL == envModron->_cycleState);
	envModron->_cycleState = scavengeCycleState;
	return result;
}


/**
 * Re-size all structures which are dependent on the current size of the heap.
 * No new memory has been added to a heap reconfiguration.  This call typically is the result
 * of having segment range changes (memory redistributed between segments) or the meaning of
 * memory changed.
 *
 */
void
MM_Scavenger::heapReconfigured(MM_EnvironmentBase *env)
{
}

void
MM_Scavenger::globalCollectionStart(MM_EnvironmentBase *env)
{
	/* Hold on to allocation stats that are useful but cleared on global collects. */
	MM_ScavengerStats* scavengerStats = &_extensions->scavengerStats;
	MM_HeapStats heapStatsSemiSpace;
	MM_HeapStats heapStatsTenureSpace;

	MM_MemorySpace* space = _extensions->heap->getDefaultMemorySpace();
	Assert_MM_true(NULL != space);

	MM_MemorySubSpace* semiSpace = space->getDefaultMemorySubSpace();
	MM_MemorySubSpace* tenureSpace = space->getTenureMemorySubSpace();

	Assert_MM_true(NULL != semiSpace);
	Assert_MM_true(NULL != tenureSpace);

	semiSpace->mergeHeapStats(&heapStatsSemiSpace);
	tenureSpace->mergeHeapStats(&heapStatsTenureSpace);

	scavengerStats->_semiSpaceAllocBytesAcumulation += heapStatsSemiSpace._allocBytes;
	scavengerStats->_tenureSpaceAllocBytesAcumulation += heapStatsTenureSpace._allocBytes;
}

void
MM_Scavenger::globalCollectionComplete(MM_EnvironmentBase *env)
{
	/* A global collection has occurred so if already set clear any
	 * flags which may force a global gc on next scavenge.
	 */
	clearFailedTenureThresholdFlag();
	clearExpandFailedFlag();
	_extensions->scavengerStats._nextScavengeWillPercolate = false;
	setFailedTenureLargestObject(0);
	_countSinceForcingGlobalGC = 0;
}

void
MM_Scavenger::reportGCCycleStart(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	MM_CommonGCData commonData;

	TRIGGER_J9HOOK_MM_OMR_GC_CYCLE_START(
		_extensions->omrHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_OMR_GC_CYCLE_START,
		_extensions->getHeap()->initializeCommonGCData(env, &commonData),
		env->_cycleState->_type);
}

void
MM_Scavenger::reportGCCycleEnd(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	MM_GCExtensionsBase* extensions = env->getExtensions();
	MM_CommonGCData commonData;

	TRIGGER_J9HOOK_MM_PRIVATE_GC_POST_CYCLE_END(
		extensions->privateHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_PRIVATE_GC_POST_CYCLE_END,
		extensions->getHeap()->initializeCommonGCData(env, &commonData),
		env->_cycleState->_type,
		extensions->globalGCStats.workPacketStats.getSTWWorkStackOverflowOccured(),
		extensions->globalGCStats.workPacketStats.getSTWWorkStackOverflowCount(),
		extensions->globalGCStats.workPacketStats.getSTWWorkpacketCountAtOverflow(),
		extensions->globalGCStats.fixHeapForWalkReason,
		extensions->globalGCStats.fixHeapForWalkTime
	);
}

uintptr_t
MM_Scavenger::calculateTiltRatio()
{
	/*
	 * Calculation of tilt ratio in percents:
	 *
	 * 								New_memory_size
	 * 	tilt_ratio =  ------------------------------------------ * 100
	 * 								Nursery size
	 *
	 *  To avoid of using of uint64_t and prevent an overflow of uintptr_t on 32-bit platforms change formula to
	 *
	 * 								New_memory_size
	 * 	tilt_ratio =  --------------------------------------------------
	 * 							Nursery size / 100
	 *
	 * Quality of calculation if good enough because Nursery size is a large number (at least grater then 1K)
	 */

	/* Calculate bottom part first */
	uintptr_t tmp = _extensions->heap->getActiveMemorySize(MEMORY_TYPE_NEW) / 100;

	/* Size of (Total - Tenure) can not be smaller then 100 bytes */
	Assert_MM_true (tmp > 0);

	/* allocate size = nursery size - survivor size */
	uintptr_t allocateSize = _extensions->heap->getActiveMemorySize(MEMORY_TYPE_NEW) - _extensions->heap->getActiveSurvivorMemorySize(MEMORY_TYPE_NEW);
	return allocateSize / tmp;
}

void
MM_Scavenger::reportGCIncrementStart(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	MM_CollectionStatisticsStandard *stats = (MM_CollectionStatisticsStandard *)env->_cycleState->_collectionStatistics;
	stats->collectCollectionStatistics(env, stats);
	stats->_startTime = omrtime_hires_clock();

	intptr_t rc = omrthread_get_process_times(&stats->_startProcessTimes);
	switch (rc){
	case -1: /* Error: Function un-implemented on architecture */
	case -2: /* Error: getrusage() or GetProcessTimes() returned error value */
		stats->_startProcessTimes._userTime = I_64_MAX;
		stats->_startProcessTimes._systemTime = I_64_MAX;
		break;
	case  0:
		break; /* Success */
	default:
		Assert_MM_unreachable();
	}

	TRIGGER_J9HOOK_MM_PRIVATE_GC_INCREMENT_START(
		_extensions->privateHookInterface,
		env->getOmrVMThread(),
		stats->_startTime,
		J9HOOK_MM_PRIVATE_GC_INCREMENT_START,
		stats);
}

void
MM_Scavenger::reportGCIncrementEnd(MM_EnvironmentStandard *env)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	MM_CollectionStatisticsStandard *stats = (MM_CollectionStatisticsStandard *)env->_cycleState->_collectionStatistics;
	stats->collectCollectionStatistics(env, stats);

	intptr_t rc = omrthread_get_process_times(&stats->_endProcessTimes);
	switch (rc){
	case -1: /* Error: Function un-implemented on architecture */
	case -2: /* Error: getrusage() or GetProcessTimes() returned error value */
		stats->_endProcessTimes._userTime = 0;
		stats->_endProcessTimes._systemTime = 0;
		break;
	case  0:
		break; /* Success */
	default:
		Assert_MM_unreachable();
	}

	stats->_endTime = omrtime_hires_clock();

	TRIGGER_J9HOOK_MM_PRIVATE_GC_INCREMENT_END(
		_extensions->privateHookInterface,
		env->getOmrVMThread(),
		stats->_endTime,
		J9HOOK_MM_PRIVATE_GC_INCREMENT_END,
		stats);
}

uintptr_t
MM_Scavenger::calculateTenureMask()
{
	/* always tenure objects which have reached the maximum age */
	uintptr_t newMask = ((uintptr_t)1 << OBJECT_HEADER_AGE_MAX);

	/* Delegate tenure mask calculations to the active strategies. */
	if (_extensions->scvTenureStrategyFixed) {
		newMask |= calculateTenureMaskUsingFixed(_extensions->scvTenureFixedTenureAge);
	}
	if (_extensions->scvTenureStrategyAdaptive) {
		newMask |= calculateTenureMaskUsingFixed(_extensions->scvTenureAdaptiveTenureAge);
	}
	if (_extensions->scvTenureStrategyLookback) {
		newMask |= calculateTenureMaskUsingLookback(_extensions->scvTenureStrategySurvivalThreshold);
	}
	if (_extensions->scvTenureStrategyHistory) {
		newMask |= calculateTenureMaskUsingHistory(_extensions->scvTenureStrategySurvivalThreshold);
	}

	return newMask;
}

uintptr_t
MM_Scavenger::calculateTenureMaskUsingLookback(double minimumSurvivalRate)
{
	Assert_MM_true(0.0 <= minimumSurvivalRate);
	Assert_MM_true(1.0 >= minimumSurvivalRate);

	MM_ScavengerStats *stats = &_extensions->scavengerStats;
	uintptr_t mask = 0;

	/* We need a normalized representation of the initial generation size over history. */

	/* We start by getting the average initial generation size. */
	double accumulatedGenerationSizes = 0.0;
	uintptr_t count = 0;
	for (uintptr_t index = 1; index < SCAVENGER_FLIP_HISTORY_SIZE; index++) {
		uintptr_t initialGenerationSize = stats->getFlipHistory(index)->_flipBytes[1] + stats->getFlipHistory(index)->_tenureBytes[1];
		if (initialGenerationSize > 0) {
			accumulatedGenerationSizes += (double)initialGenerationSize;
			count += 1;
		}
	}
	double averageInitialGenerationSize;
	if (0 == count) {
		averageInitialGenerationSize = 0;
	} else {
		averageInitialGenerationSize = accumulatedGenerationSizes / (double)count;
	}

	/* Second, we calculate the standard deviation of the initial generation size over history. */
	double accumulatedSquareDeltas = 0.0;
	for (uintptr_t index = 1; index < SCAVENGER_FLIP_HISTORY_SIZE; index++) {
		uintptr_t initialGenerationSize = stats->getFlipHistory(index)->_flipBytes[1] + stats->getFlipHistory(index)->_tenureBytes[1];
		if (initialGenerationSize > 0) {
			double delta = (double)initialGenerationSize - averageInitialGenerationSize;
			accumulatedSquareDeltas += (delta * delta);
		}
	}
	double standardDeviationOfInitialGenerationSize;
	if (0 == count) {
		standardDeviationOfInitialGenerationSize = 0;
	} else {
		standardDeviationOfInitialGenerationSize = sqrt(accumulatedSquareDeltas / (double)count);
	}

	/* This normalized initial generation size (calculated using the standard
	 * deviation) is used for determining how far back in history we should be looking.
	 * The larger this is, the shallower we look back.
	 */
	uintptr_t normalizedInitialGenerationSize = (uintptr_t)OMR_MAX(0.0, averageInitialGenerationSize - standardDeviationOfInitialGenerationSize);

	for (uintptr_t age = 0; age <= OBJECT_HEADER_AGE_MAX + 1; ++age) {
		/* skip the first row (it's the current scavenge, and is all zero right now).
		 * Also skip the last row in the history (there's no previous to compare it to)
		 */
		bool shouldTenureThisAge = true;
		uintptr_t currentGenerationBytes = stats->getFlipHistory(1)->_flipBytes[age];

		/* The lookback distance is determined by the size of the generation.
		 * We use a simple logarithmic heuristic.
		 * If the generation would occupy at least half of survivor space, only look back one collection.
		 * If the generation would occupy at least a quarter of survivor space, look back two collections.
		 * If the generation would occupy at least an eighth of survivor space, look back three collections.
		 * . . .
		 */
		const uintptr_t maximumLookback = SCAVENGER_FLIP_HISTORY_SIZE - 1;
		uintptr_t requiredLookback = 1;
		uintptr_t minimumBytesForRequiredLookback = normalizedInitialGenerationSize;
		while ((requiredLookback < maximumLookback) && (currentGenerationBytes < minimumBytesForRequiredLookback)) {
			requiredLookback += 1;
			minimumBytesForRequiredLookback /= 2;
		}

		if (requiredLookback >= age) {
			/* this generation is too young to have enough history to satisfy the lookback */
			shouldTenureThisAge = false;
		} else {
			Assert_MM_true(1 <= requiredLookback);
			Assert_MM_true(requiredLookback < SCAVENGER_FLIP_HISTORY_SIZE);
			for (uintptr_t lookback = 1; (lookback <= requiredLookback) && shouldTenureThisAge; lookback++) {
				Assert_MM_true((age+1) >= lookback);
				uintptr_t currentAgeIndex = age - lookback + 1;
				uintptr_t previousAgeIndex = age - lookback;
				uintptr_t currentFlipBytes = stats->getFlipHistory(lookback)->_flipBytes[currentAgeIndex];
				uintptr_t currentTotalBytes = currentFlipBytes + stats->getFlipHistory(lookback)->_tenureBytes[currentAgeIndex];
				uintptr_t previousFlipBytes = stats->getFlipHistory(lookback+1)->_flipBytes[previousAgeIndex];

				if (0 != previousFlipBytes) {
					if (0 == currentFlipBytes) {
						/* There are no bytes in this age, don't bother tenuring. */
						shouldTenureThisAge = false;
					} else if (((double)currentTotalBytes / (double)previousFlipBytes) < minimumSurvivalRate) {
						/* Not enough objects are surviving, don't tenure. */
						shouldTenureThisAge = false;
					}
				}
			}
		}

		if (shouldTenureThisAge) {
			/* Objects in this age are historically still dying at a rate of less than 1%. Tenure them. */
			mask |= ((uintptr_t)1 << age);
		}
	}

	return mask;
}

uintptr_t
MM_Scavenger::calculateTenureMaskUsingHistory(double minimumSurvivalRate)
{
	Assert_MM_true(0.0 <= minimumSurvivalRate);
	Assert_MM_true(1.0 >= minimumSurvivalRate);

	MM_ScavengerStats* stats = &_extensions->scavengerStats;
	uintptr_t mask = 0;

	for (uintptr_t age = 0; age < OBJECT_HEADER_AGE_MAX; ++age) {
		bool shouldTenureThisAge = true;

		/* Skip the first row in the history (it's the current scavenge, and is all zero right now).
		 * Also skip the last row in the history (there's no previous to compare it to).
		 */
		for (uintptr_t lookback = 1; lookback < SCAVENGER_FLIP_HISTORY_SIZE - 1; lookback++) {
			uintptr_t currentBytes = stats->getFlipHistory(lookback + 1)->_flipBytes[age];
			uintptr_t nextBytes = stats->getFlipHistory(lookback)->_flipBytes[age+1] + stats->getFlipHistory(lookback)->_tenureBytes[age+1];
			if (0 == currentBytes) {
				shouldTenureThisAge = false;
				break;
			} else if (((double)nextBytes / (double)currentBytes) < minimumSurvivalRate) {
				shouldTenureThisAge = false;
				break;
			}
		}

		if (shouldTenureThisAge) {
			/* Objects in this age historically survive at a rate above the minimum survival rate. Tenure them. */
			mask |= ((uintptr_t)1 << age);
		}
	}

	return mask;
}

uintptr_t
MM_Scavenger::calculateTenureMaskUsingFixed(uintptr_t tenureAge)
{
	Assert_MM_true(tenureAge <= OBJECT_HEADER_AGE_MAX);
	uintptr_t mask = 0;
	for (uintptr_t i = tenureAge; i <= OBJECT_HEADER_AGE_MAX; ++i) {
		mask |= (uintptr_t)1 << i;
	}
	return mask;
}

void 
MM_Scavenger::resetTenureLargeAllocateStats(MM_EnvironmentBase *env)
{
	MM_MemorySpace *defaultMemorySpace = _extensions->heap->getDefaultMemorySpace();
	MM_MemorySubSpace *tenureMemorySubspace = defaultMemorySpace->getTenureMemorySubSpace();
	MM_MemoryPool *tenureMemoryPool = tenureMemorySubspace->getMemoryPool();
	tenureMemoryPool->resetLargeObjectAllocateStats();
}

#endif /* OMR_GC_MODRON_SCAVENGER */