#pragma once

#include <queue>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>

namespace intertalk
{
	template<typename T>
	class queue : private boost::noncopyable
	{
	public:
		void push(const T &value)
		{
			boost::mutex::scoped_lock lock(m_mutex);
			m_queue.push(value);
			lock.unlock();
			m_condition.notify_one();
		}

		bool empty() const
		{
			boost::mutex::scoped_lock lock(m_mutex);
			return m_queue.empty();
		}

		void clear()
		{
			boost::mutex::scoped_lock lock(m_mutex);
			while (!m_queue.empty())
				m_queue.pop();
		}

		bool try_pop(T &value)
		{
			boost::mutex::scoped_lock lock(m_mutex);
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
			boost::mutex::scoped_lock lock(m_mutex);
			while(m_queue.empty())
			{
				m_condition.wait(lock);
			}

			value = m_queue.front();
			m_queue.pop();
		}

		std::size_t size()
		{
			boost::mutex::scoped_lock lock(m_mutex);
			return m_queue.size();
		}

		std::size_t trim(std::size_t elements = 1)
		{
			std::size_t cnt = 0;
			boost::mutex::scoped_lock lock(m_mutex);
			while (m_queue.size() > elements)
			{
				m_queue.pop();
				++cnt;
			}
			return cnt;
		}

	private:
		std::queue<T> m_queue;
		mutable boost::mutex m_mutex;
		boost::condition_variable m_condition;
	};
}
