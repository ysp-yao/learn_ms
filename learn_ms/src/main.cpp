#define MS_CLASS "main"

#include "common.h"
#include "Settings.h"
#include "DepLibUV.h"
#include "DepOpenSSL.h"
#include "DepLibSRTP.h"
#include "DepUsrSCTP.h"
#include "Utils.h"
#include "RTC/UDPSocket.h"
#include "RTC/TCPServer.h"
#include "RTC/DTLSHandler.h"
#include "RTC/SRTPSession.h"
#include "Loop.h"
#include "MediaSoupError.h"
#include "Logger.h"
#include <map>
#include <string>
#include <csignal>  // sigaction()
#include <cerrno>
#include <cstdint>  // uint8_t, etc  // TODO: remove?

static void init();
static void ignoreSignals();
static void destroy();

int main(int argc, char* argv[])
{
	// TODO: set the process name/id.
	Logger::Init("main");

	#if defined(MS_LITTLE_ENDIAN)
		MS_DEBUG("detected Little-Endian CPU");
	#elif defined(MS_BIG_ENDIAN)
		MS_DEBUG("detected Big-Endian CPU");
	#endif

	#if defined(INTPTR_MAX) && defined(INT32_MAX) && (INTPTR_MAX == INT32_MAX)
		MS_DEBUG("detected 32 bits architecture");
	#elif defined(INTPTR_MAX) && defined(INT64_MAX) && (INTPTR_MAX == INT64_MAX)
		MS_DEBUG("detected 64 bits architecture");
	#else
		MS_NOTICE("cannot determine whether the architecture is 32 or 64 bits");
	#endif

	try
	{
		Settings::SetConfiguration(argc, argv);
	}
	catch (const MediaSoupError &error)
	{
		MS_EXIT_FAILURE("%s", error.what());
	}

	MS_NOTICE("starting MediaSoup");

	// Print configuration.
	Settings::PrintConfiguration();

	try
	{
		init();

		// Run the Loop.
		Loop loop;
	}
	catch (const MediaSoupError &error)
	{
		destroy();
		MS_EXIT_FAILURE("%s", error.what());
	}

	destroy();
	MS_EXIT_SUCCESS("MediaSoup ends");
}

void init()
{
	MS_TRACE();

	ignoreSignals();

	// Initialize static stuff.
	DepLibUV::ClassInit();
	DepOpenSSL::ClassInit();
	DepLibSRTP::ClassInit();
	DepUsrSCTP::ClassInit();
	RTC::UDPSocket::ClassInit();
	RTC::TCPServer::ClassInit();
	RTC::DTLSHandler::ClassInit();
	RTC::SRTPSession::ClassInit();
	Utils::Crypto::ClassInit();
}

void ignoreSignals()
{
	MS_TRACE();

	int err;
	struct sigaction act;
	// NOTE: Here we ignore also signals that will be later handled in Loop
	// (the libuv signal handler overrides it).
	std::map<std::string, int> ignored_signals =
	{
		{ "INT",  SIGINT  },
		{ "TERM", SIGTERM },
		{ "HUP",  SIGHUP  },
		{ "ALRM", SIGALRM },
		{ "USR1", SIGUSR2 },
		{ "USR2", SIGUSR1 }
	};

	act.sa_handler = SIG_IGN;
	act.sa_flags = 0;
	err = sigfillset(&act.sa_mask);
	if (err)
		MS_THROW_ERROR("sigfillset() failed: %s", std::strerror(errno));

	for (auto it = ignored_signals.begin(); it != ignored_signals.end(); ++it)
	{
		std::string sig_name = it->first;
		int sig_id = it->second;

		err = sigaction(sig_id, &act, nullptr);
		if (err)
			MS_THROW_ERROR("sigaction() failed for signal %s: %s", sig_name.c_str(), std::strerror(errno));
	}
}

void destroy()
{
	MS_TRACE();

	// Free static stuff.
	DepLibUV::ClassDestroy();
	DepOpenSSL::ClassDestroy();
	DepLibSRTP::ClassDestroy();
	DepUsrSCTP::ClassDestroy();
	RTC::DTLSHandler::ClassDestroy();
	Utils::Crypto::ClassDestroy();
}
