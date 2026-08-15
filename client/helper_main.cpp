// fms_helper -- the external relay tool fms-m spawns as --helper-app.
//
// When a client plays "stream@rtmp://origin/app" on fms-m and there is no local
// publisher (and no recorded VOD), the server forks the configured helper as:
//
//     fms_helper -r <origin_url> -l rtmp://localhost:<port>/<app> -s <stream>
//
// (see media_application::spawn_helper). This tool fulfils that contract:
// it plays <stream> from the remote origin (-r) and republishes it to the local
// fms-m (-l) under the same name, so the waiting local subscriber gets fed. It
// is a pure pull-relay -- no disk, no reconnect (fms-m re-spawns on the next
// play attempt), and it exits when the source ends.

#include "pch.h"
#include "net_connection.h"
#include "net_stream.h"
#include "rtmp_message.h"

#include <iostream>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace
{
	using namespace fms::rtmp_client;

	struct relay;

	// Forwards every frame the remote play stream delivers straight into the
	// local publish stream, unchanged except for the republish channel ids.
	struct relay_sink : av_sink
	{
		relay *r{nullptr};
		void on_video_frame(fms::rtmp_message_video_data_ptr) override;
		void on_audio_frame(fms::rtmp_message_audio_data_ptr) override;
		void on_meta_data(fms::rtmp_message_notify_ptr) override;
		void close() override {}
	};

	// --- local endpoint: publish <stream> to fms-m (-l) ---------------------
	struct publish_stream_handler : net_stream_event_handler
	{
		relay *r{nullptr};
		net_stream_ptr ns;
		void on_status(const std::string &) override;
	};

	struct publish_conn_handler : net_connection_event_handler
	{
		relay *r{nullptr};
		publish_stream_handler ns_h;
		void on_status(const std::string &) override;
	};

	// --- remote endpoint: play <stream> from the origin (-r) ----------------
	struct play_stream_handler : net_stream_event_handler
	{
		relay *r{nullptr};
		net_stream_ptr ns;
		void on_status(const std::string &) override;
	};

	struct play_conn_handler : net_connection_event_handler
	{
		relay *r{nullptr};
		play_stream_handler ns_h;
		void on_status(const std::string &) override;
	};

	struct relay
	{
		boost::asio::io_context io;
		std::string remote, local, stream;

		publish_conn_handler pub_h;
		play_conn_handler     play_h;
		relay_sink            sink;

		net_connection_ptr local_conn, remote_conn;

		relay()
		{
			pub_h.r = play_h.r = sink.r = this;
			pub_h.ns_h.r = play_h.ns_h.r = this;
		}

		void run()
		{
			// Establish the local publish first: the origin's very first frames
			// are the AVC/AAC sequence headers, and they must land in a publish
			// that is already live or downstream playback can't be decoded.
			local_conn = std::make_shared<net_connection>(io, pub_h, true);
			local_conn->connect(local);
			io.run();
		}

		// publish is live -> now pull from the origin
		void on_publish_ready()
		{
			remote_conn = std::make_shared<net_connection>(io, play_h, true);
			remote_conn->connect(remote);
		}

		void stop() { io.stop(); }
	};

	void publish_conn_handler::on_status(const std::string &s)
	{
		if (s == "NetConnection.Connect.Success")
		{
			ns_h.ns = std::make_shared<net_stream>(r->local_conn, ns_h);
			ns_h.ns->publish(r->stream);
		}
		else if (s == "NetConnection.Connect.Failed" || s == "NetConnection.Connect.Closed")
		{
			std::cerr << "fms_helper: local connection lost (" << s << ")\n";
			r->stop();
		}
	}

	void publish_stream_handler::on_status(const std::string &s)
	{
		if (s == "NetStream.Publish.Start")
			r->on_publish_ready();
	}

	void play_conn_handler::on_status(const std::string &s)
	{
		if (s == "NetConnection.Connect.Success")
		{
			ns_h.ns = std::make_shared<net_stream>(r->remote_conn, ns_h);
			ns_h.ns->play(r->stream);
		}
		else if (s == "NetConnection.Connect.Failed" || s == "NetConnection.Connect.Closed")
		{
			std::cerr << "fms_helper: remote connection lost (" << s << ")\n";
			r->stop();
		}
	}

	void play_stream_handler::on_status(const std::string &s)
	{
		if (s == "NetStream.Play.Start")
			ns->set_sink(&r->sink);
		else if (s == "NetStream.Play.Stop" || s == "NetStream.Play.UnpublishNotify")
			r->stop();   // source ended -> we're done
	}

	void relay_sink::on_video_frame(fms::rtmp_message_video_data_ptr v)
	{
		if (r->pub_h.ns_h.ns)   // guard: publish stream exists
		{
			v->set_channel_id(6);
			r->pub_h.ns_h.ns->send_msg(v);
		}
	}

	void relay_sink::on_audio_frame(fms::rtmp_message_audio_data_ptr a)
	{
		if (r->pub_h.ns_h.ns)
		{
			a->set_channel_id(4);
			r->pub_h.ns_h.ns->send_msg(a);
		}
	}

	void relay_sink::on_meta_data(fms::rtmp_message_notify_ptr m)
	{
		if (r->pub_h.ns_h.ns)
			r->pub_h.ns_h.ns->send_msg(m);
	}
}

int main(int argc, char *argv[])
{
	relay rel;

	po::options_description desc("fms_helper -- RTMP pull-relay (fms-m --helper-app)");
	desc.add_options()
		("help,h", "show this help and exit")
		("remote,r", po::value<std::string>(&rel.remote)->required(), "remote source URL, e.g. rtmp://origin/app")
		("local,l",  po::value<std::string>(&rel.local)->required(),  "local destination URL, e.g. rtmp://localhost:1935/app")
		("stream,s", po::value<std::string>(&rel.stream)->required(), "stream name to relay");

	try
	{
		po::variables_map vm;
		po::store(po::parse_command_line(argc, argv, desc), vm);
		if (vm.contains("help"))
		{
			std::cout << desc << "\n";
			return 0;
		}
		po::notify(vm);   // enforces the required options
	}
	catch (const po::error &e)
	{
		std::cerr << "fms_helper: " << e.what() << "\n\n" << desc << "\n";
		return 2;
	}

	try
	{
		std::cout << "fms_helper: relaying '" << rel.stream << "' from " << rel.remote
		          << " to " << rel.local << std::endl;
		rel.run();
	}
	catch (const std::exception &e)
	{
		std::cerr << "fms_helper: fatal: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
