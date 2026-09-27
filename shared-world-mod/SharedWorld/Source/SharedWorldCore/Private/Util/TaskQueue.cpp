#include "SharedWorldCore/Util/TaskQueue.h"

namespace sw
{
	SerialQueue::SerialQueue() : Worker([this]() { Run(); }) {}

	SerialQueue::~SerialQueue() { Shutdown(); }

	void SerialQueue::Post(std::function<void()> Task)
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (bStopping) return;
			Tasks.push_back(std::move(Task));
		}
		Cv.notify_one();
	}

	bool SerialQueue::Busy() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return bRunning || !Tasks.empty();
	}

	void SerialQueue::Drain()
	{
		std::unique_lock<std::mutex> Lock(Mutex);
		IdleCv.wait(Lock, [this]() { return (!bRunning && Tasks.empty()) || bStopping; });
	}

	void SerialQueue::Shutdown()
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (bStopping && !Worker.joinable()) return;
			bStopping = true;
			Tasks.clear();
		}
		Cv.notify_all();
		IdleCv.notify_all();
		if (Worker.joinable() && Worker.get_id() != std::this_thread::get_id())
		{
			Worker.join();
		}
	}

	void SerialQueue::Run()
	{
		while (true)
		{
			std::function<void()> Task;
			{
				std::unique_lock<std::mutex> Lock(Mutex);
				Cv.wait(Lock, [this]() { return bStopping || !Tasks.empty(); });
				if (bStopping) return;
				Task = std::move(Tasks.front());
				Tasks.pop_front();
				bRunning = true;
			}
			Task();
			{
				std::lock_guard<std::mutex> Lock(Mutex);
				bRunning = false;
			}
			IdleCv.notify_all();
		}
	}
}
