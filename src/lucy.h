#pragma once


#ifndef LUCY_API
#define LUCY_API
#endif


namespace lucy
{


using u8 = unsigned char;
using u32 = unsigned;
using u64 = unsigned long long;
using SignalHandle = u32;
constexpr u8 ANY_WORKER = 0xff;
constexpr SignalHandle INVALID_HANDLE = 0xffFFffFF;


LUCY_API bool init();
LUCY_API void shutdown();
LUCY_API int getWorkersCount();

LUCY_API void incSignal(SignalHandle* signal);
LUCY_API void decSignal(SignalHandle signal);

LUCY_API void run(void* data, void(*task)(void*), SignalHandle* on_finish);
LUCY_API void runEx(void* data, void (*task)(void*), SignalHandle* on_finish, SignalHandle precondition, u8 worker_index);
LUCY_API void wait(SignalHandle waitable);
LUCY_API inline bool isValid(SignalHandle waitable) { return waitable != INVALID_HANDLE; }


template <typename F>
void runOnWorkers(F& f)
{
	SignalHandle signal = JobSystem::INVALID_HANDLE;
	for(int i = 0, c = getWorkersCount(); i < c; ++i) {
		JobSystem::run(&f, [](void* data){
			(*(F*)data)();
		}, &signal);
	}
	wait(signal);
}


template <typename F>
void forEach(unsigned count, F& f)
{
	struct Data {
		F* f;
		volatile i32 offset = 0;
		uint count;
	} data;
	data.count = count;
	data.f = &f;
	
	SignalHandle signal = JobSystem::INVALID_HANDLE;
	for(uint i = 0; i < count; ++i) {
		JobSystem::run(&data, [](void* ptr){
			Data& data = *(Data*)ptr;
			for(;;) {
				const uint idx = MT::atomicIncrement(&data.offset) - 1;
				if(idx >= data.count) break;
				(*data.f)(idx);
			}
		}, &signal);
	}
	wait(signal);
}


} // namespace lucy
