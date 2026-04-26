#pragma once

#include <list>
#include <set>
#include <cstdint>

namespace intertalk
{
	template <typename T>
	class seq_manager
	{
	protected:
		typedef std::set<T> set_t;
		typedef typename std::set<T>::iterator set_iterator;

	public:
		seq_manager()
			: m_csn(0)
			, m_sum(0)
		{}

		enum result { _eOK, _eDuplicate, _eGapClosed, _eProducesGap, _eGapStillPresent };

		result add_seq(const T &val)
		{
			if (val <= m_csn || m_sequences.find(val) != m_sequences.end())
			{
				std::cout << " duplicate, val " << val << " csn " << m_csn << " ";
				return _eDuplicate;
			}
			if (m_missing.find(val) != m_missing.end())
			{
				std::cout << " missing fragment has arrived, ";
				m_missing.erase(val);
				m_sum += val;
				if (m_sum == sum_n_elements()) // gap closed
				{
					update_csn(val);
					std::cout << "gap closed ";
					return _eGapClosed;
				}
				if (val == m_csn + 1)
				{
					++m_csn;
					set_iterator i = m_sequences.begin();
					while (i != m_sequences.end() && *i == (m_csn + 1))
					{
						++m_csn;
						m_sequences.erase(i);
						i = m_sequences.begin();
					}
				}
				else
					m_sequences.insert(val);
				std::cout << "gap still present ";
				return _eGapStillPresent;
			}
			m_sum += val;
			if (m_sum == sum_n_elements()) // gap closed
			{
				update_csn(val);
				return _eOK;
			}
			std::cout << " gap produced, sum " << m_sum << " val " << val << " ";
			m_sequences.insert(val);
			for (T i = m_csn + 1; i < val; ++i)
			{
				if (m_sequences.find(i) == m_sequences.end())
					m_missing.insert(i);
			}
			return _eProducesGap;
		}

		void add_sequences_until(const T &val)
		{
			if (val > m_csn)
			{
				m_csn = val;
				m_sum = m_csn * (m_csn + 1) / 2;

				set_iterator i = m_sequences.begin();
				while (i != m_sequences.end() && *i <= m_csn)
				{
					m_sequences.erase(i);
					i = m_sequences.begin();
				}
				while (i != m_sequences.end())
				{
					m_sum += *i;
					if (*i == (m_csn + 1))
					{
						++m_csn;
						m_sequences.erase(i);
						i = m_sequences.begin();
					}
					else
						++i;
				}
				i = m_missing.begin();
				while (i != m_missing.end() && *i <= m_csn)
				{
					m_missing.erase(i);
					i = m_missing.begin();
				}
				for (i = m_missing.begin(); i != m_missing.end(); ++i)
					m_sum += *i;
			}
		}

		bool has_gaps() const
		{
			return !m_missing.empty();
		}

		T get_range_ack(std::list<std::pair<T, T> > &list)
		{
			if (m_missing.empty())
			{
				return m_csn;
			}
			else
			{
				if (!m_sequences.empty())
				{
					set_iterator i = m_missing.begin();
					T ret = *i - 1;
					while (i != m_missing.end())
					{
						std::pair<T, T> pair = count_sequence(i);
						list.push_back(pair);
					}
					return ret;
				}
				return 0;
			}
		}

		const T &csn() const
		{
			return m_csn;
		}

	protected:
		void update_csn(const T &val)
		{
			if (m_sequences.size() >= 1)
			{
				m_csn = *--m_sequences.end();
				m_sequences.clear();
			}
			else
				m_csn = val;
		}

		std::uint64_t sum_n_elements()
		{
			std::uint64_t tmp = m_csn + m_sequences.size() + 1;
			return tmp * (tmp + 1) / 2;
		}

		std::pair<T, T> count_sequence(set_iterator &i)
		{
			T a = 0;
			T b = 0;
			T prev = *i;
			while (++i != m_missing.end())
			{
				if (*i == prev + 1)
				{
					++prev;
					++a;
				}
				else
					break;
			}
			++prev;
			if (i != m_missing.end())
			{
				while (prev < *i - 1)
				{
					++prev;
					++b;
				}
			}
			else
			{
				while (m_sequences.find(prev + 1) != m_sequences.end())
				{
					++prev;
					++b;
				}
			}
			return std::make_pair(a, b);
		}

		T m_csn;
		T m_sum;

		set_t m_sequences;
		set_t m_missing;
	};
}
