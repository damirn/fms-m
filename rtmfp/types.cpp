#include "pch.h"
#include "types.h"
#include "byte_reader.h"
#include "byte_writer.h"

#include <memory>

namespace fms
{
	bool option::deserialize(byte_reader &buff)
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
				std::size_t const value_len = m_len - consumed;
				if (value_len > 0xFFFF)
					return false;   // an option value cannot exceed a datagram
				m_value.assign(buff.read_pos(), buff.read_pos() + value_len);
				buff.skip(value_len);
			}
			return true;
		}
		catch (fms::buffer_eof_exception &)
		{
			return false;
		}
	}

	std::uint16_t option::serialize(byte_writer &to) const
	{
		if (m_len == 0)
		{
			to.write_vlu(m_len);
			return 1;
		}
		// length is known up front (type VLU + value): write [len][type][value]
		std::size_t const start = to.size();
		vlu_t const size = byte_writer::vlu_size(m_type) + m_value.size();
		to.write_vlu(size);
		to.write_vlu(m_type);
		to.write(m_value.data(), m_value.size());
		return static_cast<std::uint16_t>(to.size() - start);
	}

	vlu_t option::value_as_vlu() const
	{
		byte_reader t(m_value.data(), m_value.size());
		return t.read_vlu();
	}

	bool option_list::deserialize(byte_reader &buffer)
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

	std::uint16_t option_list::serialize(byte_writer &to)
	{
		static vlu_t const end_marker = 0;
		std::uint16_t size = 0;
		for (auto & opt : m_options)
			size += opt->serialize(to);
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
		for (auto & opt : m_options)
			if (opt->m_type == type)
				return std::optional<option_ptr>(opt);
		return std::optional<option_ptr>();
	}
}
