# Lucy Job System
Fiber-based job system with extremely simple API. 

It's a standalone version of job system used in [Lumix Engine](https://github.com/nem0/lumixengine).
Only Windows is supported now, although the Lumix Engine version has Linux support, I have to copy it here yet.

## Demo 
Create a solution with ```create_solution_vs2017.bat```. The solution is in ```build/tmp/```. Open the solution, compile and run.

## Integration
Just put lucy.h and lucy.cpp in your project and you are good to go.

## Docs

See ```src/main.cpp``` for an example.

### Initialization / shutdown

```lucy::init()``` must be called before any other function from lucy namespace
```lucy::shutdown()``` call this when you don't want to run any more jobs. Make sure all jobs are finished before calling this, since it does not wait for jobs to finish.

### Jobs

Job is a function pointer with associated void data pointer. When job is executed, the function is called and the data pointer is passed as a parameter to this function.

```lucy::run``` push a job to global queue

```cpp
	int value = 42;
	lucy::run(&value, [](void* data){
		printf("%d", *(int*)data);
	}, nullptr);
```

This prints ```42```. Eventually, after the job is finished, a signal can be triggered, see signals for more information.

### Signals

```cpp
	lucy::SignalHandle signal = lucy::INVALID_HANDLE;
	lucy::wait(signal); // does not wait, since signal is invalid
	lucy::incSignal(&signal);
	lucy::wait(signal); // wait until someone calls lucy::decSignal, execute other jobs in the meantime
```

```cpp
	lucy::SignalHandle signal = lucy::INVALID_HANDLE;
	for (int i = 0; i < N; ++i) {
		lucy::incSignal(&signal);
	}
	lucy::wait(signal); // wait until lucy::decSignal is called N-times
```

If a signal is passed to ```lucy::run``` (3rd parameter), then the signal is automatically incremented. It is decremented once all the job is finished.

```cpp
	lucy::SignalHandle finished = lucy::INVALID_HANDLE;
	for(int i = 0; i < 10; ++i) {
		lucy::run(nullptr, job_fn, &finished);
	}
	lucy::wait(finished); // wait for all 10 jobs to finish
```

There's no need to destroy signals, it's "garbage collected".

Signals can be used as preconditions to run a job. It means a job starts only after the signal is signalled:

```cpp
	lucy::SignalHandle precondition = getPrecondition();
	lucy::runEx(nullptr, job_fn, nullptr, precondition, lucy::ANY_WORKER);
```

is functionally equivalent to:

```cpp
	void job_fn(void*) { 
		lucy::wait(precondition);
		dowork();
	}
	lucy::run(nullptr, job_fn, nullptr);
```

However, the ```lucy::runEx``` version has better performance.

Finally, a job can be pinned to specific worker thread. This is useful for calling APIs which must be called from the same thread, e.g. OpenGL functions or WinAPI UI functions.

```cpp
	lucy::runEx(nullptr, job_fn, nullptr, lucy::INVALID_HANDLE, 3); // run on worker thread 3
```