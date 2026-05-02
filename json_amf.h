#pragma once

#include <memory>

#include "amf0_types.h"
#include "json/json.h"

namespace fms
{
	// JSON to amf0
	namespace detail
	{
		static amf0_type_ptr json_to_amf0(const json &val)
		{
			if (val.is_string())
				return std::make_shared<amf0_string>(val.get<std::string>());
			else if (val.is_boolean())
				return std::make_shared<amf0_boolean>(val.get<bool>());
			else if (val.is_null())
				return std::make_shared<amf0_null>();
			else if (val.is_number())
				return std::make_shared<amf0_number>(val.get<double>());
			else if (val.is_array())
			{
				amf0_strict_array_ptr a = std::make_shared<amf0_strict_array>();
				for (json::const_iterator i = val.cbegin(); i != val.cend(); ++i) {
					amf0_type_ptr v = json_to_amf0(*i);
					a->add_entry(v);
				}
				return a;
			}
			else if (val.is_object())
			{
				amf0_object_ptr o = std::make_shared<amf0_object>();
				for (json::const_iterator i = val.begin(); i != val.end(); ++i)
					o->add_entry(i.key(), json_to_amf0(i.value()));
				return o;
			}
			return std::make_shared<amf0_null>();
		}
	}

	struct json_amf
	{
		static amf0_type_ptr json_to_amf0(const json &val)
		{
			return detail::json_to_amf0(val);
		}

		static json amf0_to_json(amf0_type_ptr val)
		{
			switch (val->type())
			{
			case amf0_type::eAMF0Boolean:
			{
				amf0_boolean_ptr b = std::dynamic_pointer_cast<amf0_boolean>(val);
				return json(b->value());
			}
			case amf0_type::eAMF0Null:
				return json(nullptr);
			case amf0_type::eAMF0Number:
			{
				amf0_number_ptr n = std::dynamic_pointer_cast<amf0_number>(val);
				double d = n->value();
				double t;
				if (std::modf(d, &t) == 0.0) // integer
					return json(static_cast<std::int32_t >(d));
				return json(d);
			}
			case amf0_type::eAMF0String:
			{
				amf0_string_ptr s = std::dynamic_pointer_cast<amf0_string>(val);
				return json(s->value());
			}
			case amf0_type::eAMF0StrictArray:
			{
				amf0_strict_array_ptr a = std::dynamic_pointer_cast<amf0_strict_array>(val);
				json jarr = json::array();
				for (amf0_strict_array::array_t::iterator i = a->value().begin(); i != a->value().end(); ++i)
					jarr.push_back(amf0_to_json(*i));
				return jarr;
			}
			case amf0_type::eAMF0EcmaArray:
			{
				amf0_ecma_array_ptr a = std::dynamic_pointer_cast<amf0_ecma_array>(val);
				json jarr = json::array();
				for (amf0_ecma_array::array_t::iterator i = a->value().begin(); i != a->value().end(); ++i)
					jarr.push_back(amf0_to_json(i->second));
				return jarr;
			}
			case amf0_type::eAMF0Object:
			{
				amf0_object_ptr o = std::dynamic_pointer_cast<amf0_object>(val);
				json jobj = json::object();
				for (amf0_object::value_type::iterator i = o->value().begin(); i != o->value().end(); ++i)
					jobj[i->m_name] = amf0_to_json(i->m_value);
				return jobj;
			}
			default:
				return json();
			}
		}
	};
}
