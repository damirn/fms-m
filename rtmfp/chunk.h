#pragma once

#include "types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace fms
{
	class chunk
	{
	public:
		enum type_t
		{
			ePing = 0x01,
			eSessionClose = 0x0c,
			eForwardedInitiatorHello = 0x0f,
			eUserData = 0x10,
			eNextUserData = 0x11,
			eInitiatorHello = 0x30,
			eInitiatorInitialKeying = 0x38,
			ePingReply = 0x41,
			eSessionCloseAcknowledgement = 0x4c,
			eDataAcknowledgementRanges = 0x51,
			eFlowExceptionReportChunk = 0x5e,
			eResponderHello = 0x70,
			eResponderRedirect = 0x71,
			eResponderInitialKeying = 0x78
		};

		explicit chunk (type_t t)
			: m_type(t)
		{}

		virtual ~chunk()= default;

		const type_t &type() const
		{
			return m_type;
		}

		virtual bool deserialize(byte_reader &, std::uint16_t) = 0;
		virtual std::uint16_t serialize(byte_writer &) = 0;

	protected:
		std::uint16_t serialize_chunk_header(byte_writer &, std::size_t);

		type_t m_type;
		enum { eChunkHeaderSize = 3 };
	};

	class fihello_chunk : public chunk
	{
	public:
		fihello_chunk(const std::uint16_t &epd_len, const std::uint8_t *epd, const address &a, const std::uint16_t &tag_len, const std::uint8_t *tag)
			: chunk(eForwardedInitiatorHello)
			, m_epd_len(epd_len)
			, m_epd(const_cast<std::uint8_t *>(epd))
			, m_address(a)
			, m_tag_len(tag_len)
			, m_tag(const_cast<std::uint8_t *>(tag))
		{}

		~fihello_chunk() override= default;

		const vlu_t &epd_len() const
		{
			return m_epd_len;
		}

		const std::uint8_t *epd() const
		{
			return m_epd;
		}

		const std::uint16_t &tag_len() const
		{
			return m_tag_len;
		}

		const std::uint8_t *tag() const
		{
			return m_tag;
		}

		bool deserialize(byte_reader &, std::uint16_t) override
		{
			return true; // not implemented
		}
		std::uint16_t serialize(byte_writer &) override;

	protected:
		vlu_t m_epd_len;
		std::uint8_t *m_epd;
		address m_address;
		std::uint16_t m_tag_len;
		std::uint8_t *m_tag;
	};

	class ihello_chunk : public chunk
	{
	public:
		ihello_chunk()
			: chunk(eInitiatorHello)
		{}

		enum type { eServerIHello = 0x0a, eRemotePeerIHello = 0x0f };

		const vlu_t &epd_len() const
		{
			return m_epd_len;
		}

		const std::uint8_t *epd() const
		{
			return m_epd;
		}

		std::uint16_t tag_len() const
		{
			return static_cast<std::uint16_t>(m_tag.size());
		}

		const std::uint8_t *tag() const
		{
			return m_tag.data();
		}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override
		{
			return 0;
		} // not implemented

	protected:
		vlu_t m_epd_len{0};
		std::uint8_t *m_epd{nullptr};
		// Owns a copy: the RHello reply outlives the packet buffer.
		std::vector<std::uint8_t> m_tag;
	};

	class rhello_chunk : public chunk
	{
	public:
		rhello_chunk()
			: chunk(eResponderHello)
		{}

		rhello_chunk(const std::uint16_t &tag_len, const std::uint8_t *tag, const std::uint16_t &cookie_len, const std::uint8_t *cookie, const std::uint16_t &cert_len, const std::uint8_t *cert)
			: chunk(eResponderHello)
			, m_tag_len(tag_len)
			, m_tag(tag)
			, m_cookie_len(cookie_len)
			, m_cookie(cookie)
			, m_cert_len(cert_len)
			, m_cert(cert)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override
		{
			return true;
		}  // not implemented
		std::uint16_t serialize(byte_writer &) override;

	protected:
		std::uint16_t m_tag_len;
		const std::uint8_t *m_tag;
		std::uint16_t m_cookie_len;
		const std::uint8_t *m_cookie;
		std::uint16_t m_cert_len;
		const std::uint8_t *m_cert;
	};

	class redirect_chunk : public chunk
	{
	public:
		redirect_chunk(std::uint16_t tag_len, const std::uint8_t *tag)
			: chunk(eResponderRedirect)
			, m_tag_len(tag_len)
			, m_tag(new std::uint8_t[m_tag_len])
		{
			std::memcpy(const_cast<std::uint8_t *>(m_tag), tag, m_tag_len);
		}

		~redirect_chunk() override
		{
			delete[] m_tag;
		}

		const std::list<address> &addresses() const
		{
			return m_addresses;
		}

		std::list<address> &addresses()
		{
			return m_addresses;
		}

		bool deserialize(byte_reader &, std::uint16_t) override
		{
			return true;
		}  // not implemented
		std::uint16_t serialize(byte_writer &) override;

	protected:
		std::uint16_t m_tag_len;
		const std::uint8_t *m_tag;
		std::list<address> m_addresses;
	};

	class iikeying_chunk : public chunk
	{
	public:
		iikeying_chunk()
			: chunk(eInitiatorInitialKeying)
		{}

		const std::uint32_t &isid() const
		{
			return m_isid;
		}

		const vlu_t &cookie_len() const
		{
			return m_cookie_len;
		}

		const std::uint8_t *cookie_echo() const
		{
			return m_cookie_echo;
		}

		const vlu_t &cert_len() const
		{
			return m_cert_len;
		}

		const std::uint8_t *initator_cert() const
		{
			return m_initiator_cert;
		}

		const vlu_t &skic_len() const
		{
			return m_skic_len;
		}

		const std::uint8_t *skic() const
		{
			return m_skic;
		}

		const std::uint16_t &signature_len() const
		{
			return m_signature_len;
		}

		const std::uint8_t *signature() const
		{
			return m_signature;
		}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override
		{
			return 0;
		} // not implemented

	protected:
		std::uint32_t m_isid;
		vlu_t m_cookie_len;
		const std::uint8_t *m_cookie_echo;
		vlu_t m_cert_len;
		const std::uint8_t *m_initiator_cert;
		vlu_t m_skic_len;
		const std::uint8_t *m_skic;
		std::uint16_t m_signature_len;
		const std::uint8_t *m_signature;
	};

	class rikeying_chunk : public chunk
	{
	public:
		rikeying_chunk(std::uint32_t rsid, std::uint16_t skrc_len, const std::uint8_t *skrc)
			: chunk(eResponderInitialKeying)
			, m_rsid(rsid)
			, m_skrc_len(skrc_len)
			, m_skrc(skrc)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override
		{
			return true;
		}  // not implemented
		std::uint16_t serialize(byte_writer &) override;

	protected:
		static std::uint8_t m_marker;
		std::uint32_t m_rsid;
		std::uint16_t m_skrc_len;
		const std::uint8_t *m_skrc;
	};

	struct fragment;
	using fragment_ptr = std::shared_ptr<fragment>;

	class data_chunk
	{
	public:
		data_chunk()
			: m_flags(0)
			, m_user_data(nullptr)
			, m_user_data_len(0)
			, m_options_present(false)
			, m_abandon(false)
			, m_final(false)
		{}

		data_chunk(const std::uint8_t &flags, const std::uint8_t *user_data, const std::uint16_t &data_len)
			: m_flags(flags)
			, m_user_data(user_data)
			, m_user_data_len(data_len)
		{}

		const std::uint8_t &flags() const
		{
			return m_flags;
		}

		const option_list &options() const
		{
			return m_option_list;
		}

		option_list &options()
		{
			return m_option_list;
		}

		const bool &should_abandon() const
		{
			return m_abandon;
		}

		const bool &is_final() const
		{
			return m_final;
		}

		const std::uint8_t &frag_ctl() const
		{
			return m_frag_ctl;
		}

		const std::uint8_t *user_data() const
		{
			return m_user_data;
		}

		const std::uint16_t &user_data_len() const
		{
			return m_user_data_len;
		}

	protected:
		void parse_flags();
		void create_flags();

		std::uint8_t m_flags;
		option_list m_option_list;

		const std::uint8_t *m_user_data;
		std::uint16_t m_user_data_len;

		// flags
		bool m_options_present;
		bool m_abandon;
		bool m_final;
		std::uint8_t m_frag_ctl;
	};

	class user_data_chunk : public chunk, public data_chunk
	{
	public:
		user_data_chunk()
			: chunk(eUserData)
			, m_flow_id(0)
			, m_seq_number(0)
			, m_fsn_offset(0)
		{}

		user_data_chunk(const std::uint8_t &flags, const vlu_t &flow_id, const vlu_t &seq_no, const vlu_t &fsn_off, const std::uint8_t *user_data, const std::uint16_t &data_len)
			: chunk(eUserData)
			, data_chunk(flags, user_data, data_len)
			, m_flow_id(flow_id)
			, m_seq_number(seq_no)
			, m_fsn_offset(fsn_off)
		{}

		user_data_chunk(const fragment_ptr&, const vlu_t &, const vlu_t &);

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override;

		const vlu_t &flow_id() const
		{
			return m_flow_id;
		}

		const vlu_t &seq_number() const
		{
			return m_seq_number;
		}

		const vlu_t &fsn_offset() const
		{
			return m_fsn_offset;
		}

		vlu_t forward_seq_number() const
		{
			return m_seq_number - m_fsn_offset;
		}

	protected:
		vlu_t m_flow_id;
		vlu_t m_seq_number;
		vlu_t m_fsn_offset;
	};

	class next_user_data_chunk : public chunk, public data_chunk
	{
	public:
		next_user_data_chunk()
			: chunk(eNextUserData)
			 
		{}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override
		{
			return 0;
		} // not implemented
	};

	class range_ack_chunk : public chunk
	{
	public:
		range_ack_chunk()
			: chunk(eDataAcknowledgementRanges)
		{}

		range_ack_chunk(const vlu_t &flowid, const vlu_t &blocks_available, const vlu_t &cumulative_ack)
			: chunk(eDataAcknowledgementRanges)
			, m_flowid(flowid)
			, m_buff_blocks_available(blocks_available)
			, m_cumulative_ack(cumulative_ack)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override;

		const vlu_t &flow_id() const
		{
			return m_flowid;
		}

		const vlu_t &buff_blocks_available() const
		{
			return m_buff_blocks_available;
		}

		const vlu_t &cumulative_ack() const
		{
			return m_cumulative_ack;
		}

		using range_pair_t = std::pair<vlu_t, vlu_t>;
		using range_list_t = std::list<range_pair_t>;

		const range_list_t &ranges() const
		{
			return m_ranges;
		}

		range_list_t &ranges()
		{
			return m_ranges;
		}

		void add_range(const range_pair_t &p)
		{
			m_ranges.push_back(p);
		}

	protected:
		vlu_t m_flowid;
		vlu_t m_buff_blocks_available;
		vlu_t m_cumulative_ack;
		range_list_t m_ranges;
	};

	class flow_exception_report_chunk : public chunk
	{
	public:
		flow_exception_report_chunk()
			: chunk(eFlowExceptionReportChunk)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override
		{
			return 0;
		} // not implemented

		const vlu_t &flow_id() const
		{
			return m_flowid;
		}

		const vlu_t &exception() const
		{
			return m_exception;
		}

	protected:
		vlu_t m_flowid;
		vlu_t m_exception;
	};

	class ping_chunk : public chunk
	{
	public:
		ping_chunk()
			: chunk(ePing)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override
		{
			return 0;
		} // not implemented

		const std::uint16_t &data_len() const
		{
			return m_data_len;
		}

		const std::uint8_t *data() const
		{
			return m_data;
		}

	protected:
		std::uint16_t m_data_len;
		const std::uint8_t *m_data;
	};

	class ping_reply_chunk : public chunk
	{
	public:
		// Owns a copy: `data` points into the packet buffer, freed when parse()
		// returns, long before this reply is serialized.
		ping_reply_chunk(const std::uint8_t *data, const std::uint16_t &data_len)
			: chunk(ePingReply)
			, m_data(data, data + data_len)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override
		{
			return true;
		} // not implemented
		std::uint16_t serialize(byte_writer &) override;

	protected:
		std::vector<std::uint8_t> m_data;
	};

	// Session Close Request (RFC 7016 sec. 2.3.11): "please close this session".
	class close_chunk : public chunk
	{
	public:
		close_chunk()
			: chunk(eSessionClose)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override;
	};

	// Session Close Acknowledgement (RFC 7016 sec. 2.3.12): sent in reply to a
	// close request; also accepted as the peer's own close-and-done.
	class close_ack_chunk : public chunk
	{
	public:
		close_ack_chunk()
			: chunk(eSessionCloseAcknowledgement)
		{}

		bool deserialize(byte_reader &, std::uint16_t) override;
		std::uint16_t serialize(byte_writer &) override;
	};
}
