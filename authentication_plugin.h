#pragma once

#include <string>
#include <boost/cstdint.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/logic/tribool.hpp>

namespace intertalk
{
	struct auth_data
	{
		boost::uint32_t m_id;
		boost::uint32_t m_id_provider;
		std::string m_user;
		std::string m_pass;
		std::string m_display_name;
		std::string m_auth_user;
	};

	typedef boost::shared_ptr<auth_data> auth_data_ptr;

	class authentication_plugin
	{
	public:
		virtual ~authentication_plugin(){}
		virtual boost::tribool authenticate(auth_data_ptr) = 0;
	};

	class no_authentication_plugin : public authentication_plugin
	{
	public:
		virtual boost::tribool authenticate(auth_data_ptr)
		{
			return true;
		}
	};
}
