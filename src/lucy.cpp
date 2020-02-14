#include <cassert>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include "lucy.h"


namespace lucy
{


static_assert(sizeof(u8) == 1);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(u64) == 8);


template <typename T, int count> constexpr int countOf(const T (&)[count])
{
	return count;
};


namespace Fiber
{


#ifdef _WIN32
	using Event = HANDLE;
	typedef void* Handle;
	typedef void(__stdcall *FiberProc)(void*);
#else 
	typedef ucontext_t Handle;
	typedef void (*FiberProc)(void*);
#endif
constexpr void* INVALID_FIBER = nullptr;


void initThread(FiberProc proc, Handle* handle);
Handle create(int stack_size, FiberProc proc, void* parameter);
void destroy(Handle fiber);
void switchTo(Handle* from, Handle fiber);


void initThread(FiberProc proc, Handle* out)
{
	*out = ConvertThreadToFiber(nullptr);
	proc(nullptr);
}


Handle create(int stack_size, FiberProc proc, void* parameter)
{
	return CreateFiber(stack_size, proc, parameter);
}


void destroy(Handle fiber)
{
	DeleteFiber(fiber);
}


void switchTo(Handle* from, Handle fiber)
{
	SwitchToFiber(fiber);
}


} // namespace Fiber


#ifdef _WIN32
	using Event = HANDLE;
	using Thread = HANDLE;
	void trigger(Event e) { ::SetEvent(e); }
	void reset(Event e) { ::ResetEvent(e); }
	Event createEvent() { return ::CreateEvent(nullptr, TRUE, FALSE, nullptr); }

	void waitMultiple(Event e0, Event e1, u32 timeout_ms) { 
		const HANDLE handles[2] = { e0, e1 };
		::WaitForMultipleObjects(2, handles, false, timeout_ms);
	}
#else 
	struct Event
	{
		pthread_mutex_t mutex;
		pthread_cond_t cond;
		bool signaled;
		bool manual_reset;
	};
#endif


enum { 
	HANDLE_ID_MASK = 0xffFF,
	HANDLE_GENERATION_MASK = 0xffFF0000 
};


struct Job
{
	void (*task)(void*) = nullptr;
	void* data = nullptr;
	SignalHandle dec_on_finish;
	SignalHandle precondition;
	u8 worker_index;
};


struct Signal {
	volatile int value;
	u32 generation;
	Job next_job;
	SignalHandle sibling;
};


struct WorkerTask;


struct FiberDecl
{
	int idx;
	Fiber::Handle fiber;
	Job current_job;
};


struct System
{
	System() 
	{
		m_signals_pool.resize(4096);
		m_free_queue.resize(4096);
		for(int i = 0; i < 4096; ++i) {
			m_free_queue[i] = i;
			m_signals_pool[i].sibling = INVALID_HANDLE;
			m_signals_pool[i].generation = 0;
		}
	}


	std::mutex m_sync;
	std::mutex m_job_queue_sync;
	Event m_work_signal;
	std::vector<WorkerTask*> m_workers;
	std::vector<Job> m_job_queue;
	std::vector<Signal> m_signals_pool;
	std::vector<FiberDecl*> m_free_fibers;
	std::vector<FiberDecl*> m_ready_fibers;
	std::vector<u32> m_free_queue;
	FiberDecl m_fiber_pool[512];
};


static System* g_system = nullptr;
static thread_local WorkerTask* g_worker = nullptr;

#pragma optimize( "", off )
WorkerTask* getWorker()
{
	return g_worker;
}
#pragma optimize( "", on )


struct WorkerTask
{
	WorkerTask(System& system, u8 worker_index) 
		: m_system(system)
		, m_worker_index(worker_index)
	{
		m_work_signal = createEvent();
		reset(m_work_signal);
	}


	static u32 run(WorkerTask* task)
	{
		return task->task();
	}


	u32 task()
	{
		g_worker = this;
		Fiber::initThread(start, &m_primary_fiber);
		m_is_finished = true;
		return 0;
	}


	#ifdef _WIN32
		static void __stdcall start(void* data)
	#else
		static void start(void* data)
	#endif
	{
		g_system->m_sync.lock();
		FiberDecl* fiber = g_system->m_free_fibers.back();
		g_system->m_free_fibers.pop_back();
		getWorker()->m_current_fiber = fiber;
		Fiber::switchTo(&getWorker()->m_primary_fiber, fiber->fiber);
	}


	bool m_finished = false;
	FiberDecl* m_current_fiber = nullptr;
	Fiber::Handle m_primary_fiber;
	System& m_system;
	std::vector<Job> m_job_queue;
	std::vector<FiberDecl*> m_ready_fibers;
	u8 m_worker_index;
	bool m_is_finished = false;
	Event m_work_signal;
	std::thread m_thread;
};


static SignalHandle allocateSignal()
{
	const u32 handle = g_system->m_free_queue.back();
	Signal& w = g_system->m_signals_pool[handle & HANDLE_ID_MASK];
	w.value = 1;
	w.sibling = INVALID_HANDLE;
	w.next_job.task = nullptr;
	g_system->m_free_queue.pop_back();

	return handle & HANDLE_ID_MASK | w.generation;
}


static void pushJob(const Job& job)
{
	if (job.worker_index != ANY_WORKER) {
		WorkerTask* worker = g_system->m_workers[job.worker_index % g_system->m_workers.size()];
		worker->m_job_queue.push_back(job);
		trigger(worker->m_work_signal);
		return;
	}
	g_system->m_job_queue.push_back(job);
	trigger(g_system->m_work_signal);
}


void trigger(SignalHandle handle)
{
	std::lock_guard lock(g_system->m_sync);
	
	Signal& counter = g_system->m_signals_pool[handle & HANDLE_ID_MASK];
	--counter.value;
	if (counter.value > 0) return;

	SignalHandle iter = handle;
	while (isValid(iter)) {
		Signal& signal = g_system->m_signals_pool[iter & HANDLE_ID_MASK];
		if(signal.next_job.task) {
			std::lock_guard lock(g_system->m_job_queue_sync);
			pushJob(signal.next_job);
		}
		signal.generation = (((signal.generation >> 16) + 1) & 0xffFF) << 16;
		g_system->m_free_queue.push_back(iter & HANDLE_ID_MASK | signal.generation);
		signal.next_job.task = nullptr;
		iter = signal.sibling;
	}
}


static bool isSignalZero(SignalHandle handle, bool lock)
{
	if (!isValid(handle)) return true;

	const u32 gen = handle & HANDLE_GENERATION_MASK;
	const u32 id = handle & HANDLE_ID_MASK;
	
	if (lock) g_system->m_sync.lock();
	Signal& counter = g_system->m_signals_pool[id];
	bool is_zero = counter.generation != gen || counter.value == 0;
	if (lock) g_system->m_sync.unlock();
	return is_zero;
}


static void runInternal(void* data
	, void (*task)(void*)
	, SignalHandle precondition
	, bool lock
	, SignalHandle* on_finish
	, u8 worker_index)
{
	Job j;
	j.data = data;
	j.task = task;
	j.worker_index = worker_index;
	j.precondition = precondition;

	if (lock) g_system->m_sync.lock();
	j.dec_on_finish = [&]() -> SignalHandle {
		if (!on_finish) return INVALID_HANDLE;
		if (isValid(*on_finish) && !isSignalZero(*on_finish, false)) {
			++g_system->m_signals_pool[*on_finish & HANDLE_ID_MASK].value;
			return *on_finish;
		}
		return allocateSignal();
	}();
	if (on_finish) *on_finish = j.dec_on_finish;

	if (!isValid(precondition) || isSignalZero(precondition, false)) {
		std::lock_guard lock(g_system->m_job_queue_sync);
		pushJob(j);
	}
	else {
		Signal& counter = g_system->m_signals_pool[precondition & HANDLE_ID_MASK];
		if(counter.next_job.task) {
			const SignalHandle ch = allocateSignal();
			Signal& c = g_system->m_signals_pool[ch & HANDLE_ID_MASK];
			c.next_job = j;
			c.sibling = counter.sibling;
			counter.sibling = ch;
		}
		else {
			counter.next_job = j;
		}
	}

	if (lock) g_system->m_sync.unlock();
}


void incSignal(SignalHandle* signal)
{
	assert(signal);
	std::lock_guard lock(g_system->m_sync);
	
	if (isValid(*signal) && !isSignalZero(*signal, false)) {
		++g_system->m_signals_pool[*signal & HANDLE_ID_MASK].value;
	}
	else {
		*signal = allocateSignal();
	}
}


void decSignal(SignalHandle signal)
{
	trigger(signal);
}


void run(void* data, void(*task)(void*), SignalHandle* on_finished)
{
	runInternal(data, task, INVALID_HANDLE, true, on_finished, ANY_WORKER);
}


void runEx(void* data, void(*task)(void*), SignalHandle* on_finished, SignalHandle precondition, u8 worker_index)
{
	runInternal(data, task, precondition, true, on_finished, worker_index);
}


#ifdef _WIN32
	static void __stdcall manage(void* data)
#else
	static void manage(void* data)
#endif
{
	g_system->m_sync.unlock();

	FiberDecl* this_fiber = (FiberDecl*)data;
		
	WorkerTask* worker = getWorker();
	while (!worker->m_finished) {
		FiberDecl* fiber = nullptr;
		Job job;
		{
			std::lock_guard lock(g_system->m_job_queue_sync);

			if (!worker->m_ready_fibers.empty()) {
				fiber = worker->m_ready_fibers.back();
				worker->m_ready_fibers.pop_back();
				if (worker->m_ready_fibers.empty()) reset(worker->m_work_signal);
			}
			else if (!worker->m_job_queue.empty()) {
				job = worker->m_job_queue.back();
				worker->m_job_queue.pop_back();
				if (worker->m_job_queue.empty()) reset(worker->m_work_signal);
			}
			else if (!g_system->m_ready_fibers.empty()) {
				fiber = g_system->m_ready_fibers.back();
				g_system->m_ready_fibers.pop_back();
				if (g_system->m_ready_fibers.empty()) reset(g_system->m_work_signal);
			}
			else if(!g_system->m_job_queue.empty()) {
				job = g_system->m_job_queue.back();
				g_system->m_job_queue.pop_back();
				if (g_system->m_job_queue.empty()) reset(g_system->m_work_signal);
			}
		}

		if (fiber) {
			worker->m_current_fiber = fiber;

			g_system->m_sync.lock();
			g_system->m_free_fibers.push_back(this_fiber);
			Fiber::switchTo(&this_fiber->fiber, fiber->fiber);
			g_system->m_sync.unlock();

			worker = getWorker();
			worker->m_current_fiber = this_fiber;
			continue;
		}

		if (job.task) {
			this_fiber->current_job = job;
			job.task(job.data);
            this_fiber->current_job.task = nullptr;
			if (isValid(job.dec_on_finish)) {
				trigger(job.dec_on_finish);
			}
			worker = getWorker();
		}
		else 
		{
			waitMultiple(g_system->m_work_signal, worker->m_work_signal, 1);
		}
	}
	Fiber::switchTo(&getWorker()->m_current_fiber->fiber, getWorker()->m_primary_fiber);
}


bool init(u8 workers_count)
{
	assert(!g_system);

	g_system = new System;
	reset(g_system->m_work_signal);

	g_system->m_free_fibers.reserve(countOf(g_system->m_fiber_pool));
	for (FiberDecl& fiber : g_system->m_fiber_pool) {
		g_system->m_free_fibers.push_back(&fiber);
	}

	const int fiber_num = countOf(g_system->m_fiber_pool);
	for(int i = 0; i < fiber_num; ++i) { 
		FiberDecl& decl = g_system->m_fiber_pool[i];
		decl.fiber = Fiber::create(64 * 1024, manage, &g_system->m_fiber_pool[i]);
		decl.idx = i;
	}

	unsigned count = workers_count;
	if (count == 0) count = 1;
	for (unsigned i = 0; i < count; ++i) {
		WorkerTask* task = new WorkerTask(*g_system, i < 64 ? u64(1) << i : 0);
		
		try {
			std::thread t(&WorkerTask::run, task);
			task->m_thread.swap(t);
			g_system->m_workers.push_back(task);
		}
		catch(...) {
			assert(false);
			delete task;
		}
	}

	return !g_system->m_workers.empty();
}


int getWorkersCount()
{
	return (int)g_system->m_workers.size();
}


void shutdown()
{
	if (!g_system) return;

	for (WorkerTask* task : g_system->m_workers)
	{
		WorkerTask* wt = (WorkerTask*)task;
		wt->m_finished = true;
	}

	for (WorkerTask* task : g_system->m_workers)
	{
		while (!task->m_is_finished) trigger(g_system->m_work_signal);
		task->m_thread.join();
		delete task;
	}

	for (FiberDecl& fiber : g_system->m_fiber_pool)
	{
		Fiber::destroy(fiber.fiber);
	}

	delete g_system;
	g_system = nullptr;
}


void wait(SignalHandle handle)
{
	g_system->m_sync.lock();
	if (isSignalZero(handle, false)) {
		g_system->m_sync.unlock();
		return;
	}
	
	assert(getWorker());

	FiberDecl* this_fiber = getWorker()->m_current_fiber;

	runInternal(this_fiber, [](void* data){
		std::lock_guard lock(g_system->m_job_queue_sync);
		FiberDecl* fiber = (FiberDecl*)data;
		if (fiber->current_job.worker_index == ANY_WORKER) {
			g_system->m_ready_fibers.push_back(fiber);
			trigger(g_system->m_work_signal);
		}
		else {
			WorkerTask* worker = g_system->m_workers[fiber->current_job.worker_index % g_system->m_workers.size()];
			worker->m_ready_fibers.push_back(fiber);
			trigger(worker->m_work_signal);
		}
	}, handle, false, nullptr, 0);
		
	FiberDecl* new_fiber = g_system->m_free_fibers.back();
	g_system->m_free_fibers.pop_back();
	getWorker()->m_current_fiber = new_fiber;
	Fiber::switchTo(&this_fiber->fiber, new_fiber->fiber);
	getWorker()->m_current_fiber = this_fiber;
	g_system->m_sync.unlock();
		
	#ifndef NDEBUG
		g_system->m_sync.lock();
		assert(isSignalZero(handle, false));
		g_system->m_sync.unlock();
	#endif
}


} // namespace lucy

