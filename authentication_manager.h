#pragma once

#include <string>
#include <functional>
#include <boost/logic/tribool.hpp>
#include <boost/noncopyable.hpp>
#include <memory>

#include "authentication_plugin.h"

typedef intertalk::authentication_plugin *(*create)();
typedef void *(*destroy)(intertalk::authentication_plugin *);

namespace intertalk
{
	class authentication_plugin;

	class authentication_manager : private boost::noncopyable
	{
	public:
		authentication_manager();
		authentication_manager(const std::string &);
		~authentication_manager();

		boost::tribool authenticate(auth_data_ptr, std::function<void (bool, auth_data_ptr)>);

	protected:
		void init_plugin(const std::string &);
		void load_plugin(const std::string &);

		authentication_plugin *m_auth_plugin;
		bool m_plugin_loaded;
		destroy m_destroy;
	};
}
