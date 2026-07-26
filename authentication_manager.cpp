#include "pch.h"
#include "authentication_manager.h"

#include <stdexcept>
#include <utility>

#include <dlfcn.h>

namespace fms
{
	namespace
	{
		void close_handle(void *h)
		{
			if (h != nullptr)
				::dlclose(h);
		}

		void delete_plugin(authentication_plugin *p)
		{
			delete p;
		}
	}

	authentication_manager::authentication_manager()
		: m_handle(nullptr, &close_handle)
		, m_auth_plugin(new no_authentication_plugin, &delete_plugin)
	{
	}

	authentication_manager::authentication_manager(const std::string &plugin_name)
		: m_handle(nullptr, &close_handle)
		, m_auth_plugin(nullptr, &delete_plugin)
	{
		init_plugin(plugin_name);
	}

	boost::tribool authentication_manager::authenticate(auth_data_ptr data)
	{
		return m_auth_plugin->authenticate(std::move(data));
	}

	void authentication_manager::init_plugin(const std::string &plugin_name)
	{
		if (plugin_name.empty())
			m_auth_plugin = plugin_ptr(new no_authentication_plugin, &delete_plugin);
		else
			load_plugin(plugin_name);
	}

	void authentication_manager::load_plugin(const std::string &plugin_name)
	{
		dl_handle h(::dlopen(plugin_name.c_str(), RTLD_LAZY), &close_handle);
		if (!h)
			throw std::runtime_error("Cannot load authentication plugin");

		auto const c = reinterpret_cast<create_plugin_fn>(::dlsym(h.get(), "create_plugin"));
		auto const d = reinterpret_cast<destroy_plugin_fn>(::dlsym(h.get(), "destroy_plugin"));
		// Fail loudly on a malformed plugin rather than leaving m_auth_plugin
		// null (later deref) or silently defaulting to allow-all.
		if (c == nullptr || d == nullptr)
			throw std::runtime_error("Authentication plugin missing create_plugin/destroy_plugin");

		authentication_plugin *const p = c();
		if (p == nullptr)
			throw std::runtime_error("Authentication plugin create_plugin returned null");

		m_handle = std::move(h);
		m_auth_plugin = plugin_ptr(p, d);   // destroyed by the plugin's own destroy_plugin
	}
}
