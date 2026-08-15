#pragma once

#include "channel_manager.h"
#include "net_client.h"
#include "rtmp_message.h"
#include "rtmp_parser.h"

#include <list>
#include <map>
#include <memory>
#include <queue>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

namespace fms::rtmp_client
{
	class net_stream;
	using net_stream_ptr = std::shared_ptr<net_stream>;

	class net_connection_event_handler
	{
	public:
		virtual ~net_connection_event_handler()= default;
		virtual void on_status(const std::string &) = 0;
	};

	class result_handler;
	using result_handler_ptr = std::shared_ptr<result_handler>;

	class result_handler
	{
	public:
		using callback_t = std::function<void (rtmp_message_invoke_ptr, result_handler_ptr)>;

		explicit result_handler(callback_t cb)
			: m_callback(std::move(cb)){}

		virtual ~result_handler()= default;

		callback_t m_callback;
	};

	using invoke_handler_t = std::function<void (rtmp_message_invoke_ptr)>;

	// Composes an rtmp_parser and is its message sink.
	class net_connection : public rtmp_message_sink, boost::noncopyable, public std::enable_shared_from_this<net_connection>
	{
	public:
		net_connection(boost::asio::io_context &, net_connection_event_handler &, bool);

		// Called by net_stream to open its RTMP stream on this connection.
		void add_net_stream(const net_stream_ptr&);
		void create_stream(const net_stream_ptr&);

		void connect(const std::string &);
		void connect(const std::string &, const amf0_type_ptr&);
		void connect(const std::string &, const std::list<amf0_type_ptr> &);

		void close_stream(const net_stream_ptr&);

		// FMLE/rtmpdump-style publish handshake: releaseStream + FCPublish before
		// createStream, FCUnpublish + deleteStream on teardown.
		void release_stream(const std::string &name);
		void fc_publish(const std::string &name);
		void fc_unpublish(const std::string &name);
		void delete_stream(std::uint32_t stream_id);

		void send_rtmp_message(const rtmp_message_ptr& msg)
		{
			send_message(msg);
		}

		// Announce a larger outbound chunk size (default 128). Sends Set Chunk Size
		// and frames our subsequent chunks at n -- real publishers do this to avoid
		// a chunk header every 128 bytes.
		void set_output_chunk_size(std::uint32_t n);

		void close()
		{
			m_client->close_socket();
		}

		void call(const std::string &);
		std::uint32_t call(const std::string &, result_handler_ptr);

		void call(const std::string &, const std::list<amf0_type_ptr> &);
		std::uint32_t call(const std::string &, const std::list<amf0_type_ptr> &, result_handler_ptr);

		void register_invoke_handler(const std::string &, invoke_handler_t);

		bool connected() const
		{
			return m_state == eReady;
		}

		const std::string &uri() const
		{
			return m_uri;
		}

	private:
		void handle_connect(bool, const std::string &);

	protected:
		// False on CSPRNG failure: never ship a partially-built handshake.
		[[nodiscard]] bool prepare_handshake();
		[[nodiscard]] bool write_signed_c2(const std::uint8_t *s1);   // FP9 signed C2 from the received S1
		void send_connect_invoke();

		void read_data(std::size_t = 1);
		void write_data();
		void handle_read_data(bool, std::size_t);
		void handle_write_data(bool, std::size_t);
		void update_bytes_read(std::size_t);

		void send_message(const rtmp_message_ptr&);
		void serialize_message(const rtmp_message_ptr&, const rtmp_channel_ptr&);

		void handle_message(rtmp_channel_ptr, rtmp_message_ptr) override;
		void handle_internal_message(rtmp_message_ptr) override;

		void handle_win_ack(const rtmp_message_window_acknowledgement_size_ptr&);
		void handle_set_peer_bandwidth(const rtmp_message_set_peer_bandwidth_ptr&);
		void handle_ping(const rtmp_message_ping_ptr&);
		void handle_invoke(const rtmp_message_invoke_ptr&);
		void handle_notify(const rtmp_message_notify_ptr&);
		void handle_invoke_without_handler(const rtmp_message_invoke_ptr&);
		void handle_invoke_with_result(const rtmp_message_invoke_ptr&);
		void handle_aggregate(const rtmp_message_aggregate_ptr&);
		void handle_audio(const rtmp_message_audio_data_ptr&);
		void handle_video(const rtmp_message_video_data_ptr&);

		std::uint32_t get_invoke_id()
		{
			return m_invoke_id++;
		}

		void send_named_command(const char *command, const std::string &name);

		class create_stream_result_handler : public result_handler
		{
		public:
			create_stream_result_handler(callback_t cb, std::uint32_t id)
				: result_handler(std::move(cb))
				  , m_id(id)
			{}

			const std::uint32_t &id() const
			{
				return m_id;
			}

		protected:
			std::uint32_t m_id;
		};

		using create_stream_result_handler_ptr = std::shared_ptr<create_stream_result_handler>;

		void send_message(const rtmp_message_invoke_ptr&, result_handler_ptr);
		void handle_connect_result(const rtmp_message_invoke_ptr&, const result_handler_ptr&);
		void handle_create_stream_result(const rtmp_message_invoke_ptr&, const result_handler_ptr&);

		static std::optional<std::string> get_code(const rtmp_message_invoke_ptr&);

		std::unordered_map<std::uint32_t, result_handler_ptr> m_result_handlers;

		using net_stream_map_t = std::unordered_map<std::uint32_t, net_stream_ptr>;
		net_stream_map_t m_tmp_net_streams;
		net_stream_map_t m_net_streams;

		boost::asio::io_context &m_io_context;

		net_client_ptr m_client;

		net_connection_event_handler &m_event_handler;
		bool m_use_fp9_hs;
		std::uint8_t m_hs_scheme{1};   // digest scheme used for the FP9 handshake
		std::string m_app;
		std::string m_uri;
		std::uint32_t m_invoke_id{1};
		std::uint32_t m_stream_cnt{0};

		std::list<amf0_type_ptr> m_additional_connect_params;

		// Invoke handlers
		std::map<std::string, invoke_handler_t> m_invoke_handlers;

		// Inbound RTMP chunk parser, fed by handle_read_data, delivering to us as its
		// sink. m_channel_manager is shared with the serialize path (per-channel
		// header state, both directions) and is declared first so the parser's
		// reference to it is valid.
		channel_manager m_channel_manager;
		rtmp_parser m_parser{m_channel_manager, *this};

		// I/O buffers
		byte_writer *m_input_buffer;
		byte_writer *m_output_buffer;

		std::uint32_t m_messages_read{0};
		std::uint32_t m_messages_written{0};
		std::uint16_t m_out_chunk_size{128};   // our outbound chunk size (Set Chunk Size)
		std::uint32_t m_bytes_read{0};
		std::uint32_t m_ack_size{eAckSize};
		std::uint32_t m_ack_size_next{eAckSize};

		using message_queue_t = std::queue<rtmp_message_ptr>;
		message_queue_t m_queue;

		enum state { eNotConnected, eConnected, eHandshake1Sent, eHandshake2Sent, eReady };
		static constexpr std::size_t  eHandshakeSize = 1536;
		static constexpr std::uint8_t ePlainMagic    = 0x03;
		static constexpr std::uint8_t eCryptoMagic   = 0x06;
		static constexpr std::uint32_t eAckSize      = 2500000;

		state m_state{eNotConnected};
	};

	using net_connection_ptr = std::shared_ptr<net_connection>;
}
