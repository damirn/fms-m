#include "pch.h"
#include "types.h"
#include "stream_array.h"

#include <memory>

namespace fms
{
	bool option::deserialize(stream_array &buff)
	{
		try
		{
			m_len = buff.read_vlu();
			if (m_len != 0)
			{
				std::uint8_t  const*here = buff.read_pos();
				m_type = buff.read_vlu();
				auto const consumed = static_cast<std::size_t>(buff.read_pos() - here);
				// the type VLU must fit inside m_len, and the remaining value must
				// fit inside the datagram — otherwise the length is malformed
				if (consumed > m_len || (m_len - consumed) > buff.available())
					return false;
				m_value_len = static_cast<std::uint16_t>(m_len - consumed);
				m_value = new std::uint8_t[m_value_len];
				std::memcpy(m_value, buff.read_pos(), m_value_len);
				buff.skip(m_value_len);
			}
			return true;
		}
		catch (fms::buffer_eof_exception &)
		{
			return false;
		}
	}

	std::uint16_t option::serialize(stream_array &to) const
	{
		if (m_len == 0)
		{
			to.write_vlu(m_len);
			return 1;
		}
		std::uint8_t  const*here = to.write_pos();
		vlu_t const size = stream_array::get_vlu_size(m_type) + m_value_len;
		to.mark_write();
		to.skip_write(stream_array::get_vlu_size(size));
		to.write_vlu(m_type);
		to.write(m_value, m_value_len);
		to.rewind_write();
		to.write_vlu(size);
		to.append();
		return to.write_pos() - here;
	}

	vlu_t option::value_as_vlu() const
	{
		stream_array t(m_value);
		t.update(m_value_len);
		return t.read_vlu();
	}

	bool option_list::deserialize(stream_array &buffer)
	{
		option_ptr opt = std::make_shared<option>();
		bool ret = opt->deserialize(buffer);
		while (ret && !opt->is_marker())
		{
			m_options.push_back(opt);
			opt = std::make_shared<option>();
			ret = opt->deserialize(buffer);
		}
		return ret;
	}

	std::uint16_t option_list::serialize(stream_array &to)
	{
		static vlu_t const end_marker = 0;
		std::uint16_t size = 0;
		for (auto & m_option : m_options)
			size += m_option->serialize(to);
		to.write_vlu(end_marker);
		return size + 1;
	}

	option_ptr option_list::create_option(std::uint8_t type, const std::uint8_t *value, const std::uint16_t &value_len)
	{
		option_ptr opt = std::make_shared<option>(type, value, value_len);
		m_options.push_back(opt);
		return opt;
	}

	option_ptr option_list::create_option(std::uint8_t type, const vlu_t &value)
	{
		option_ptr opt = std::make_shared<option>(type, value);
		m_options.push_back(opt);
		return opt;
	}

	std::optional<option_ptr> option_list::get_option(std::uint8_t type)
	{
		for (auto & m_option : m_options)
			if (m_option->m_type == type)
				return std::optional<option_ptr>(m_option);
		return std::optional<option_ptr>();
	}
}
