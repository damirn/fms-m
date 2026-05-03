#include "pch.h"
#include "authentication_manager.h"

#ifndef WIN32
#include <dlfcn.h>
#endif

namespace fms
{
	authentication_manager::authentication_manager()
		: m_auth_plugin(new no_authentication_plugin)
		, m_plugin_loaded(false)
	{
	}

	authentication_manager::authentication_manager(const std::string &plugin_name)
		: m_auth_plugin(nullptr)
		, m_plugin_loaded(false)
		, m_destroy(nullptr)
	{
		init_plugin(plugin_name);
	}

	authentication_manager::~authentication_manager()
	{
		if (!m_plugin_loaded)
			delete m_auth_plugin;
		else
			m_destroy(m_auth_plugin);
	}

	boost::tribool authentication_manager::authenticate(auth_data_ptr data, std::function<void (bool, auth_data_ptr)>)
	{
		return m_auth_plugin->authenticate(data);
	}

	void authentication_manager::init_plugin(const std::string &plugin_name)
	{
		if (plugin_name.empty())
			m_auth_plugin = new no_authentication_plugin;
		else
			load_plugin(plugin_name);
	}

	void authentication_manager::load_plugin(const std::string &plugin_name)
	{
#ifdef WIN32
		HMODULE h = ::LoadLibrary(plugin_name.c_str());
		if (h == 0)
			throw std::runtime_error("Cannot load authentication plugin");

		create c = reinterpret_cast<create>(GetProcAddress(h, "create_plugin"));
		destroy d = reinterpret_cast<destroy>(GetProcAddress(h, "destroy_plugin"));
#else
		void *h = ::dlopen(plugin_name.c_str(), RTLD_LAZY);
		if (h == nullptr)
			throw std::runtime_error("Cannot load authentication plugin");
		create c = reinterpret_cast<create>(::dlsym(h, "create_plugin"));
		destroy d = reinterpret_cast<destroy>(::dlsym(h, "destroy_plugin"));
#endif
		// Fail loudly on a malformed plugin rather than leaving m_auth_plugin
		// null (later deref) or silently defaulting to allow-all.
		if (c == nullptr || d == nullptr)
			throw std::runtime_error("Authentication plugin missing create_plugin/destroy_plugin");

		m_auth_plugin = c();
		m_destroy = d;
		m_plugin_loaded = true;   // dtor must use m_destroy, not plain delete
	}
}
