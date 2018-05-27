#ifndef MS_SETTINGS_H
#define	MS_SETTINGS_H

#include "common.h"
#include <map>
#include <string>

class Settings
{
public:
	// Struct holding the configuration.
	struct Configuration
	{
		unsigned int logLevel            { MS_LOG_LEVEL_DEBUG };
		std::string rtcListenIPv4;
		std::string rtcListenIPv6;
		MS_PORT     rtcMinPort           { 10000 };
		MS_PORT     rtcMaxPort           { 59999 };
		std::string dtlsCertificateFile;
		std::string dtlsPrivateKeyFile;
		// Private fields.
		bool        hasIPv4              { false };
		bool        hasIPv6              { false };
	};

public:
	static void SetConfiguration(int argc, char* argv[]);
	static void PrintConfiguration();

private:
	static void SetDefaultRtcListenIP(int requested_family);
	static void SetLogLevel(std::string &level);
	static void SetRtcListenIPv4(const std::string &ip);
	static void SetRtcListenIPv6(const std::string &ip);
	static void SetRtcPorts();
	static void SetDtlsCertificateAndPrivateKeyFiles();

public:
	static struct Configuration configuration;

private:
	static std::map<std::string, unsigned int> string2LogLevel;
	static std::map<unsigned int, std::string> logLevel2String;
};

#endif
