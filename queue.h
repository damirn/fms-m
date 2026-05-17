#pragma once

#include <queue>
#include <boost/noncopyable.hpp>
#include <mutex>
#include <condition_variable>

namespace fms
{
	template<typename T>
	class queue : boost::noncopyable
	{
	public:
		void push(const T &value)
		{
			std::unique_lock lock(m_mutex);
			m_queue.push(value);
			lock.unlock();
			m_condition.notify_one();
		}

		bool empty() const
		{
			std::unique_lock lock(m_mutex);
			return m_queue.empty();
		}

		void clear()
		{
			std::unique_lock lock(m_mutex);
			while (!m_queue.empty())
				m_queue.pop();
		}

		bool try_pop(T &value)
		{
			std::unique_lock const lock(m_mutex);
			if(m_queue.empty())
			{
				return false;
			}

			value = m_queue.front();
			m_queue.pop();
			return true;
		}

		void wait_and_pop(T& value)
		{
			std::unique_lock lock(m_mutex);
			while(m_queue.empty())
			{
				m_condition.wait(lock);
			}

			value = m_queue.front();
			m_queue.pop();
		}

		std::size_t size()
		{
			std::unique_lock lock(m_mutex);
			return m_queue.size();
		}

		std::size_t trim(std::size_t elements = 1)
		{
			std::size_t cnt = 0;
			std::unique_lock lock(m_mutex);
			while (m_queue.size() > elements)
			{
				m_queue.pop();
				++cnt;
			}
			return cnt;
		}

	private:
		std::queue<T> m_queue;
		mutable std::mutex m_mutex;
		std::condition_variable m_condition;
	};
}
