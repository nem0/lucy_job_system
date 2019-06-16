#include "lucy.h"
#include <condition_variable>
#include <cstdio>
#include <thread>
#include <mutex>


std::mutex g_mutex;
std::condition_variable g_finished;
std::mutex g_finished_mutex;


void print(const char* msg)
{
	std::lock_guard lock(g_mutex);
	printf(msg);
	printf("\n");
}


void jobB(void*)
{
	print("B begin");
	print("B end");
}


void jobA(void*)
{
	print("A begin");
	lucy::SignalHandle finished = lucy::INVALID_HANDLE;
	for(int i = 0; i < 10; ++i) {
		lucy::run(nullptr, jobB, &finished);
	}
	lucy::wait(finished);
	print("A end");
	g_finished.notify_all();
}


int main(int argc, char* argv[])
{
	lucy::init(std::thread::hardware_concurrency());
	lucy::run(nullptr, jobA, nullptr);
	std::unique_lock<std::mutex> lck(g_finished_mutex);
	g_finished.wait(lck);
	lucy::shutdown();
}