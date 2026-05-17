#pragma once

#include <string>
#include <memory>
#include <boost/logic/tribool.hpp>

namespace fms
{
	struct auth_data
	{
		std::uint32_t m_id;
		std::uint32_t m_id_provider;
		std::string m_user;
		std::string m_pass;
		std::string m_display_name;
		std::string m_auth_user;
	};

	using auth_data_ptr = std::shared_ptr<auth_data>;

	class authentication_plugin
	{
	public:
		virtual ~authentication_plugin()= default;
		virtual boost::tribool authenticate(auth_data_ptr) = 0;
	};

	class no_authentication_plugin : public authentication_plugin
	{
	public:
		boost::tribool authenticate(auth_data_ptr) override
		{
			return true;
		}
	};
}
