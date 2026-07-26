#pragma once

#include "authentication_plugin.h"

#include <functional>
#include <memory>
#include <string>

#include <boost/logic/tribool.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	class authentication_plugin;

	// A plugin shared object exports these two.
	using create_plugin_fn = authentication_plugin *(*)();
	using destroy_plugin_fn = void (*)(authentication_plugin *);

	class authentication_manager : boost::noncopyable
	{
	public:
		authentication_manager();
		explicit authentication_manager(const std::string &);
		~authentication_manager() = default;

		boost::tribool authenticate(auth_data_ptr);

	protected:
		void init_plugin(const std::string &);
		void load_plugin(const std::string &);

		using plugin_ptr = std::unique_ptr<authentication_plugin, std::function<void (authentication_plugin *)>>;
		using dl_handle = std::unique_ptr<void, void (*)(void *)>;

		// Declaration order is destruction order reversed: the plugin is torn down
		// by destroy_plugin, which lives inside the shared object, so the plugin
		// must go before the handle that keeps that object mapped.
		dl_handle m_handle;
		plugin_ptr m_auth_plugin;
	};
}
