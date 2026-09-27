#pragma once
// A serial background queue: tasks run one at a time, in order, on one
// worker thread, never on the game thread. Destruction drains nothing: it
// sets the cancel flag, lets the running task finish and joins.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace sw
{
	class SerialQueue
	{
	public:
		SerialQueue();
		~SerialQueue();
		SerialQueue(const SerialQueue&) = delete;
		SerialQueue& operator=(const SerialQueue&) = delete;

		/** Enqueues Task; ignored after Shutdown. */
		void Post(std::function<void()> Task);
		/** True while a task is running or queued. */
		bool Busy() const;
		/** Blocks until the queue is empty and idle (tests). */
		void Drain();
		/** Stops accepting work, cancels, joins the worker. */
		void Shutdown();
		/** Long-running tasks should poll this and return early. */
		bool Cancelled() const { return bStopping.load(); }

	private:
		void Run();
		mutable std::mutex Mutex;
		std::condition_variable Cv;
		std::condition_variable IdleCv;
		std::deque<std::function<void()>> Tasks;
		bool bRunning = false;
		std::atomic<bool> bStopping{false};
		std::thread Worker;
	};
}
