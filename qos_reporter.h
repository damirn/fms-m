#pragma once

namespace fms
{
	class media_host;
	class stream_registry;
	class rtmp_app_manager;

	// The once-a-second QoS gather + report, lifted out of video_bcast_application's
	// timer. Flushes the per-subscriber stats accumulated lock-free on the fan-out path
	// into the shared netstream stats, and emits an onQOS notify to each QoS-stream
	// subscriber. Called under the app's media-routing lock (caller-holds).
	class qos_reporter
	{
	public:
		qos_reporter(media_host &host, stream_registry &registry, rtmp_app_manager *manager)
			: m_host(host), m_registry(registry), m_manager(manager)
		{}

		void report();

	private:
		media_host &m_host;
		stream_registry &m_registry;
		rtmp_app_manager *m_manager;
	};
}
