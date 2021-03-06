/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */

// This file is included inside the Group class.

private:

Process *
findProcessWithStickySessionId(unsigned int id) const {
	ProcessList::const_iterator it, end = enabledProcesses.end();
	for (it = enabledProcesses.begin(); it != end; it++) {
		Process *process = it->get();
		if (process->getStickySessionId() == id) {
			return process;
		}
	}
	return NULL;
}

Process *
findProcessWithStickySessionIdOrLowestBusyness(unsigned int id) const {
	int leastBusyProcessIndex = -1;
	int lowestBusyness = 0;
	unsigned int i, size = enabledProcessBusynessLevels.size();
	const int *enabledProcessBusynessLevels = &this->enabledProcessBusynessLevels[0];

	for (i = 0; i < size; i++) {
		Process *process = enabledProcesses[i].get();
		if (process->getStickySessionId() == id) {
			return process;
		} else if (leastBusyProcessIndex == -1 || enabledProcessBusynessLevels[i] < lowestBusyness) {
			leastBusyProcessIndex = i;
			lowestBusyness = enabledProcessBusynessLevels[i];
		}
	}

	if (leastBusyProcessIndex == -1) {
		return NULL;
	} else {
		return enabledProcesses[leastBusyProcessIndex].get();
	}
}

Process *
findProcessWithLowestBusyness(const ProcessList &processes) const {
	if (processes.empty()) {
		return NULL;
	}

	int lowestBusyness = -1;
	Process *leastBusyProcess = NULL;
	ProcessList::const_iterator it;
	ProcessList::const_iterator end = processes.end();
	for (it = processes.begin(); it != end; it++) {
		Process *process = (*it).get();
		int busyness = process->busyness();
		if (lowestBusyness == -1 || lowestBusyness > busyness) {
			lowestBusyness = busyness;
			leastBusyProcess = process;
		}
	}
	return leastBusyProcess;
}

/**
 * Cache-optimized version of findProcessWithLowestBusyness() for the common case.
 */
Process *findEnabledProcessWithLowestBusyness() const {
	if (enabledProcesses.empty()) {
		return NULL;
	}

	int leastBusyProcessIndex = -1;
	int lowestBusyness = 0;
	unsigned int i, size = enabledProcessBusynessLevels.size();
	const int *enabledProcessBusynessLevels = &this->enabledProcessBusynessLevels[0];

	for (i = 0; i < size; i++) {
		if (leastBusyProcessIndex == -1 || enabledProcessBusynessLevels[i] < lowestBusyness) {
			leastBusyProcessIndex = i;
			lowestBusyness = enabledProcessBusynessLevels[i];
		}
	}
	return enabledProcesses[leastBusyProcessIndex].get();
}

/**
 * Removes a process to the given list (enabledProcess, disablingProcesses, disabledProcesses).
 * This function does not fix getWaitlist invariants or other stuff.
 */
void
removeProcessFromList(const ProcessPtr &process, ProcessList &source) {
	ProcessPtr p = process; // Keep an extra reference count just in case.

	source.erase(source.begin() + process->getIndex());
	process->setIndex(-1);

	switch (process->enabled) {
	case Process::ENABLED:
		assert(&source == &enabledProcesses);
		enabledCount--;
		if (process->isTotallyBusy()) {
			nEnabledProcessesTotallyBusy--;
		}
		break;
	case Process::DISABLING:
		assert(&source == &disablingProcesses);
		disablingCount--;
		break;
	case Process::DISABLED:
		assert(&source == &disabledProcesses);
		disabledCount--;
		break;
	case Process::DETACHED:
		assert(&source == &detachedProcesses);
		break;
	default:
		P_BUG("Unknown 'enabled' state " << (int) process->enabled);
	}

	// Rebuild indices
	ProcessList::iterator it, end = source.end();
	unsigned int i = 0;
	for (it = source.begin(); it != end; it++, i++) {
		const ProcessPtr &process = *it;
		process->setIndex(i);
	}

	// Rebuild enabledProcessBusynessLevels
	if (&source == &enabledProcesses) {
		enabledProcessBusynessLevels.clear();
		for (it = source.begin(); it != end; it++, i++) {
			const ProcessPtr &process = *it;
			enabledProcessBusynessLevels.push_back(process->busyness());
		}
		enabledProcessBusynessLevels.shrink_to_fit();
	}
}

/**
 * Adds a process to the given list (enabledProcess, disablingProcesses, disabledProcesses)
 * and sets the process->enabled flag accordingly.
 * The process must currently not be in any list. This function does not fix
 * getWaitlist invariants or other stuff.
 */
void
addProcessToList(const ProcessPtr &process, ProcessList &destination) {
	destination.push_back(process);
	process->setIndex(destination.size() - 1);
	if (&destination == &enabledProcesses) {
		process->enabled = Process::ENABLED;
		enabledCount++;
		enabledProcessBusynessLevels.push_back(process->busyness());
		if (process->isTotallyBusy()) {
			nEnabledProcessesTotallyBusy++;
		}
	} else if (&destination == &disablingProcesses) {
		process->enabled = Process::DISABLING;
		disablingCount++;
	} else if (&destination == &disabledProcesses) {
		assert(process->sessions == 0);
		process->enabled = Process::DISABLED;
		disabledCount++;
	} else if (&destination == &detachedProcesses) {
		assert(process->isAlive());
		process->enabled = Process::DETACHED;
		callAbortLongRunningConnectionsCallback(process);
	} else {
		P_BUG("Unknown destination list");
	}
}

void
enableAllDisablingProcesses(boost::container::vector<Callback> &postLockActions) {
	P_DEBUG("Enabling all DISABLING processes with result DR_ERROR");
	deque<DisableWaiter>::iterator it, end = disableWaitlist.end();
	for (it = disableWaitlist.begin(); it != end; it++) {
		const DisableWaiter &waiter = *it;
		const ProcessPtr process = waiter.process;
		// A process can appear multiple times in disableWaitlist.
		assert(process->enabled == Process::DISABLING
			|| process->enabled == Process::ENABLED);
		if (process->enabled == Process::DISABLING) {
			removeProcessFromList(process, disablingProcesses);
			addProcessToList(process, enabledProcesses);
			P_DEBUG("Enabled process " << process->inspect());
		}
	}
	clearDisableWaitlist(DR_ERROR, postLockActions);
}

void
removeFromDisableWaitlist(const ProcessPtr &p, DisableResult result,
	boost::container::vector<Callback> &postLockActions)
{
	deque<DisableWaiter>::const_iterator it, end = disableWaitlist.end();
	deque<DisableWaiter> newList;
	for (it = disableWaitlist.begin(); it != end; it++) {
		const DisableWaiter &waiter = *it;
		const ProcessPtr process = waiter.process;
		if (process == p) {
			postLockActions.push_back(boost::bind(waiter.callback, p, result));
		} else {
			newList.push_back(waiter);
		}
	}
	disableWaitlist = newList;
}

void
clearDisableWaitlist(DisableResult result,
	boost::container::vector<Callback> &postLockActions)
{
	// This function may be called after processes in the disableWaitlist
	// have been disabled or enabled, so do not assume any value for
	// waiter.process->enabled in this function.
	postLockActions.reserve(postLockActions.size() + disableWaitlist.size());
	while (!disableWaitlist.empty()) {
		const DisableWaiter &waiter = disableWaitlist.front();
		postLockActions.push_back(boost::bind(waiter.callback, waiter.process, result));
		disableWaitlist.pop_front();
	}
}


/********************************************
 * Public methods
 ********************************************/

public:

/**
 * Attaches the given process to this Group and mark it as enabled. This
 * function doesn't touch `getWaitlist` so be sure to fix its invariants
 * afterwards if necessary, e.g. by calling `assignSessionsToGetWaiters()`.
 */
AttachResult
attach(const ProcessPtr &process,
	boost::container::vector<Callback> &postLockActions)
{
	TRACE_POINT();
	assert(process->getGroup() == NULL || process->getGroup() == this);
	assert(process->isAlive());
	assert(isAlive());

	if (processUpperLimitsReached()) {
		return AR_GROUP_UPPER_LIMITS_REACHED;
	} else if (poolAtFullCapacity()) {
		return AR_POOL_AT_FULL_CAPACITY;
	} else if (!isWaitingForCapacity() && anotherGroupIsWaitingForCapacity()) {
		return AR_ANOTHER_GROUP_IS_WAITING_FOR_CAPACITY;
	}

	process->initializeStickySessionId(generateStickySessionId());
	P_DEBUG("Attaching process " << process->inspect());
	addProcessToList(process, enabledProcesses);

	/* Now that there are enough resources, relevant processes in
	 * 'disableWaitlist' can be disabled.
	 */
	deque<DisableWaiter>::const_iterator it, end = disableWaitlist.end();
	deque<DisableWaiter> newDisableWaitlist;
	for (it = disableWaitlist.begin(); it != end; it++) {
		const DisableWaiter &waiter = *it;
		const ProcessPtr process2 = waiter.process;
		// The same process can appear multiple times in disableWaitlist.
		assert(process2->enabled == Process::DISABLING
			|| process2->enabled == Process::DISABLED);
		if (process2->sessions == 0) {
			if (process2->enabled == Process::DISABLING) {
				P_DEBUG("Disabling DISABLING process " << process2->inspect() <<
					"; disable command succeeded immediately");
				removeProcessFromList(process2, disablingProcesses);
				addProcessToList(process2, disabledProcesses);
			} else {
				P_DEBUG("Disabling (already disabled) DISABLING process " <<
					process2->inspect() << "; disable command succeeded immediately");
			}
			postLockActions.push_back(boost::bind(waiter.callback, process2, DR_SUCCESS));
		} else {
			newDisableWaitlist.push_back(waiter);
		}
	}
	disableWaitlist = newDisableWaitlist;

	// Update GC sleep timer.
	wakeUpGarbageCollector();

	postLockActions.push_back(boost::bind(&Group::runAttachHooks, this, process));

	return AR_OK;
}

/**
 * Detaches the given process from this Group. This function doesn't touch
 * getWaitlist so be sure to fix its invariants afterwards if necessary.
 * `pool->detachProcessUnlocked()` does that so you should usually use
 * that method over this one.
 */
void
detach(const ProcessPtr &process, boost::container::vector<Callback> &postLockActions) {
	TRACE_POINT();
	assert(process->getGroup() == this);
	assert(process->isAlive());
	assert(isAlive());

	if (process->enabled == Process::DETACHED) {
		P_DEBUG("Detaching process " << process->inspect() << ", which was already being detached");
		return;
	}

	const ProcessPtr p = process; // Keep an extra reference just in case.
	P_DEBUG("Detaching process " << process->inspect());

	if (process->enabled == Process::ENABLED || process->enabled == Process::DISABLING) {
		assert(enabledCount > 0 || disablingCount > 0);
		if (process->enabled == Process::ENABLED) {
			removeProcessFromList(process, enabledProcesses);
		} else {
			removeProcessFromList(process, disablingProcesses);
			removeFromDisableWaitlist(process, DR_NOOP, postLockActions);
		}
	} else {
		assert(process->enabled == Process::DISABLED);
		assert(!disabledProcesses.empty());
		removeProcessFromList(process, disabledProcesses);
	}

	addProcessToList(process, detachedProcesses);
	startCheckingDetachedProcesses(false);

	postLockActions.push_back(boost::bind(&Group::runDetachHooks, this, process));
}

/**
 * Detaches all processes from this Group. This function doesn't touch
 * getWaitlist so be sure to fix its invariants afterwards if necessary.
 */
void
detachAll(boost::container::vector<Callback> &postLockActions) {
	assert(isAlive());
	P_DEBUG("Detaching all processes in group " << info.name);

	foreach (ProcessPtr process, enabledProcesses) {
		addProcessToList(process, detachedProcesses);
	}
	foreach (ProcessPtr process, disablingProcesses) {
		addProcessToList(process, detachedProcesses);
	}
	foreach (ProcessPtr process, disabledProcesses) {
		addProcessToList(process, detachedProcesses);
	}

	enabledProcesses.clear();
	disablingProcesses.clear();
	disabledProcesses.clear();
	enabledProcessBusynessLevels.clear();
	enabledCount = 0;
	disablingCount = 0;
	disabledCount = 0;
	nEnabledProcessesTotallyBusy = 0;
	clearDisableWaitlist(DR_NOOP, postLockActions);
	startCheckingDetachedProcesses(false);
}

/**
 * Marks the given process as enabled. This function doesn't touch getWaitlist
 * so be sure to fix its invariants afterwards if necessary.
 */
void
enable(const ProcessPtr &process, boost::container::vector<Callback> &postLockActions) {
	assert(process->getGroup() == this);
	assert(process->isAlive());
	assert(isAlive());

	if (process->enabled == Process::DISABLING) {
		P_DEBUG("Enabling DISABLING process " << process->inspect());
		removeProcessFromList(process, disablingProcesses);
		addProcessToList(process, enabledProcesses);
		removeFromDisableWaitlist(process, DR_CANCELED, postLockActions);
	} else if (process->enabled == Process::DISABLED) {
		P_DEBUG("Enabling DISABLED process " << process->inspect());
		removeProcessFromList(process, disabledProcesses);
		addProcessToList(process, enabledProcesses);
	} else {
		P_DEBUG("Enabling ENABLED process " << process->inspect());
	}
}

/**
 * Marks the given process as disabled. Returns DR_SUCCESS, DR_DEFERRED
 * or DR_NOOP. If the result is DR_DEFERRED, then the callback will be
 * called later with the result of this action.
 */
DisableResult
disable(const ProcessPtr &process, const DisableCallback &callback) {
	assert(process->getGroup() == this);
	assert(process->isAlive());
	assert(isAlive());

	if (process->enabled == Process::ENABLED) {
		P_DEBUG("Disabling ENABLED process " << process->inspect() <<
			"; enabledCount=" << enabledCount << ", process.sessions=" << process->sessions);
		assert(enabledCount >= 0);
		if (enabledCount == 1 && !allowSpawn()) {
			P_WARN("Cannot disable sole enabled process in group " << info.name <<
				" because spawning is not allowed according to the current" <<
				" configuration options");
			return DR_ERROR;
		} else if (enabledCount <= 1 || process->sessions > 0) {
			removeProcessFromList(process, enabledProcesses);
			addProcessToList(process, disablingProcesses);
			disableWaitlist.push_back(DisableWaiter(process, callback));
			if (enabledCount == 0) {
				/* All processes are going to be disabled, so in order
				 * to avoid blocking requests we first spawn a new process
				 * and disable this process after the other one is done
				 * spawning. We do this irregardless of resource limits
				 * because this is an exceptional situation.
				 */
				P_DEBUG("Spawning a new process to avoid the disable action from blocking requests");
				spawn();
			}
			P_DEBUG("Deferring disable command completion");
			return DR_DEFERRED;
		} else {
			removeProcessFromList(process, enabledProcesses);
			addProcessToList(process, disabledProcesses);
			P_DEBUG("Disable command succeeded immediately");
			return DR_SUCCESS;
		}
	} else if (process->enabled == Process::DISABLING) {
		assert(disablingCount > 0);
		disableWaitlist.push_back(DisableWaiter(process, callback));
		P_DEBUG("Disabling DISABLING process " << process->inspect() <<
			info.name << "; command queued, deferring disable command completion");
		return DR_DEFERRED;
	} else {
		assert(disabledCount > 0);
		P_DEBUG("Disabling DISABLED process " << process->inspect() <<
			info.name << "; disable command succeeded immediately");
		return DR_NOOP;
	}
}
