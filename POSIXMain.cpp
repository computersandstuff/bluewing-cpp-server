/* vim: set noet ts=4 sw=4 sts=4 ft=cpp:
 *
 * Created by Darkwire Software.
 *
 * This example server file is available unlicensed; the MIT license of liblacewing/Lacewing Relay does not apply to this file.
*/
#include <iostream>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <vector>
#include "ConsoleColors.hpp"
#include "Lacewing/Lacewing.h"
#include <signal.h>
#include <termios.h>
#include <unistd.h>


#include <iomanip>
#include <cstdlib>
#include <config/libconfig.h++>

#include <cstdint>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/stdx.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/instance.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/array.hpp>


using bsoncxx::builder::stream::close_array;
using bsoncxx::builder::stream::close_document;
using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize;
using bsoncxx::builder::stream::open_array;
using bsoncxx::builder::stream::open_document;
mongocxx::instance instance{};// don't put inside main


using namespace std::string_view_literals;
using namespace libconfig;

// Define if you want Flash hosted. Policy file will automatically be generated.
#define FLASH_ENABLED

// Upload limit for ENTIRE SERVER, TCP + UDP, in bytes
// UDP messages received above this limit will be discarded
// TCP messages received above this limit are still delivered. See TCP_CLIENT_UPLOAD_CAP.
// #define TOTAL_UPLOAD_CAP 500000

// TCP upload limit for single clients, per second, in bytes.
// TCP messages received above this limit will send the client an error message
// and disconnect them.
// UDP upload limit is not defined.
// #define TCP_CLIENT_UPLOAD_CAP 3000

// Set this to 0 for the app to ask the user what port it is, on bootup;
// or to another number to use that by default
static const int FIXEDPORT = 6121;
// Set this to 0 for the app to disable websocket on either or both http/https variants.
// websocketSecure will not work without certificate loading before websocket host is called.
// WebSocket expects ./fullchain.pem and ./privkey.pem files, with no password, in same folder as executable.
static int websocketNonSecure = 8087, websocketSecure = 4437;



// Declarations - Lacewing handlers
void OnConnectRequest(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> client);
void OnDisconnect(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> client);
void OnTimerTick(lacewing::timer timer);
void OnError(lacewing::relayserver& server, lacewing::error error);
void OnServerMessage(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> senderclient,
	bool blasted, lw_ui8 subchannel, std::string_view data, lw_ui8 variant);
void OnChannelMessage(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> senderclient,
	std::shared_ptr<lacewing::relayserver::channel> channel,
	bool blasted, lw_ui8 subchannel, std::string_view data, lw_ui8 variant);
void OnPeerMessage(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> senderclient,
	std::shared_ptr<lacewing::relayserver::channel> viachannel, std::shared_ptr<lacewing::relayserver::client> receiverclient,
	bool blasted, lw_ui8 subchannel, std::string_view data, lw_ui8 variant);

// Declarations - functions
void GenerateFlashPolicy(int port);
void Shutdown();
void UpdateTitle(size_t clientCount);
void CloseHandler(int sig);

// Global variables
lacewing::eventpump globalpump;
lacewing::timer globalmsgrecvcounttimer;
lacewing::relayserver* globalserver;
std::string flashpolicypath;
bool deleteFlashPolicyAtEndOfApp;
static char timeBuffer[10];

// In case of idiocy
struct BanEntry
{
	std::string ip;
	int disconnects;
	std::string reason;
	time_t resetAt;
	BanEntry(std::string ip, int disconnects, std::string reason, time_t resetAt) :
		ip(ip), disconnects(disconnects), reason(reason), resetAt(resetAt)
	{
		// yay
	}
};
static std::vector<BanEntry> banIPList;

static std::uint64_t totalNumMessagesIn = 0, totalNumMessagesOut = 0;
static std::uint64_t totalBytesIn = 0, totalBytesOut = 0;
static size_t maxClients = 0, maxChannels = 0;
static size_t maxNumMessagesIn = 0, maxNumMessagesOut = 0;
static size_t maxBytesInInOneSec = 0, maxBytesOutInOneSec = 0;

static size_t numMessagesIn = 0, numMessagesOut = 0;
static size_t bytesIn = 0, bytesOut = 0;
struct clientstats
{
	std::shared_ptr<lacewing::relayserver::client> c;
	size_t totalBytesIn;
	size_t totalNumMessagesIn;
	size_t wastedServerMessages;
#ifdef TCP_CLIENT_UPLOAD_CAP
	size_t bytesIn;
	size_t numMessagesIn;
	bool exceeded;
	clientstats(std::shared_ptr<lacewing::relayserver::client> _c) : c(_c), totalBytesIn(0), totalNumMessagesIn(0),
		wastedServerMessages(0), bytesIn(0), numMessagesIn(0), exceeded(false) {}
#else
	clientstats(std::shared_ptr<lacewing::relayserver::client> _c) : c(_c), totalBytesIn(0), totalNumMessagesIn(0),
		wastedServerMessages(0) {}
#endif
};
static std::vector<std::shared_ptr<clientstats>> clientdata;

static termios oldt;

const char* sslPathCertChain = "./fullchain.pem";
const char* sslPathPrivKey = "./privkey.pem";

int main()
{
	//Read configuration file
	Config cfg;

	try {
		cfg.readFile("/etc/example.cfg");
	}
	catch (const FileIOException& fioex) {
		std::cerr << "I/O error while reading file." << std::endl;
		return EXIT_FAILURE;
	}
	catch (const ParseException& pex) {
		std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
			<< " - " << pex.getError() << std::endl;
		return EXIT_FAILURE;
	}

	// Extract and print the 'port' value
	try {
		int port = cfg.lookup("port");
		std::cout << "Port: " << port << std::endl;
	}
	catch (const SettingNotFoundException& nfex) {
		std::cerr << "No 'port' setting in configuration file." << std::endl;
	}

	//mongocxx::instance instance{}; // This should be done only once.
	mongocxx::uri uri("mongodb://10.0.0.30:27017");
	mongocxx::client client(uri);
	mongocxx::database db = client["mydb"];
	mongocxx::collection coll = db["mycollection"];

	bsoncxx::builder::stream::document document{};
	document << "Data" << "hello";
	coll.insert_one(document.view());

	// Disable console input
	if (tcgetattr(STDIN_FILENO, &oldt) == -1)
	{
		std::cout << "Couldn't read console mode (error "sv << errno << ")."sv;

		if (errno != ENOTTY)
		{
			std::cout << " Aborting server startup.\r\n"sv;
			return errno;
		}
		std::cout << " 25 = not terminal; probably run in simulated terminal. Server startup continues.\r\n"sv;
	}
	termios newt = oldt;
	newt.c_lflag &= ~ECHO;
	tcsetattr(STDIN_FILENO, TCSANOW, &newt);

	// Handle closing nicely
	signal(SIGABRT, CloseHandler);
	signal(SIGFPE, CloseHandler);
	signal(SIGILL, CloseHandler);
	signal(SIGINT, CloseHandler);
	signal(SIGSEGV, CloseHandler);
	signal(SIGTERM, CloseHandler);

	// We don't use C-style printf(), so desync.
	// It's unclear whether cout or printf is faster; and some say cout is faster only with a fast locale.
	std::ios_base::sync_with_stdio(false);

	// Block some IPs by default
	//banIPList.push_back(BanEntry("75.128.140.10"sv, 4, "IP banned. Contact Phi on Clickteam Discord."sv, (time(NULL) + 24 * 60 * 60)));
	//banIPList.push_back(BanEntry("127.0.0.1"sv, 4, "IP banned. Contact Phi on Clickteam Discord."sv, (time(NULL) + 24 * 60 * 60)));

	globalpump = lacewing::eventpump_new();
	globalserver = new lacewing::relayserver(globalpump);
	globalmsgrecvcounttimer = lacewing::timer_new(globalpump);
	lacewing::error error = nullptr;

	{
		char message[256];
#ifdef _DEBUG
		sprintf(message, "This is a Bluewing Server build %i. Currently under debug testing. "
			"You may be disconnected randomly as server is restarted.", lacewing::relayserver::buildnum);
#elif TCP_CLIENT_UPLOAD_CAP
		sprintf(message, "This is a Bluewing Server build %i. An upload cap is in place. Please pay "
			"attention to Sent server -> peer text messages on subchannels 0 and 1, or you may be banned.",
			lacewing::relayserver::buildnum);
#else
		sprintf(message, "This is a Bluewing Server build %i.", lacewing::relayserver::buildnum);
#endif
		globalserver->setwelcomemessage(message);
	}

	// Initialise hooks
	globalserver->onconnect(OnConnectRequest);
	globalserver->ondisconnect(OnDisconnect);
	globalserver->onmessage_server(OnServerMessage);
	globalserver->onmessage_channel(OnChannelMessage);
	globalserver->onmessage_peer(OnPeerMessage);
	globalserver->onerror(OnError);
	globalmsgrecvcounttimer->on_tick(OnTimerTick);

	// Allow all letters, all numbers, all marks like accents, all punctuation, and char 32 i.e. space
	globalserver->setcodepointsallowedlist(lacewing::relayserver::codepointsallowlistindex::ClientNames, "L*,M*,N*,P*,32");
	globalserver->setcodepointsallowedlist(lacewing::relayserver::codepointsallowlistindex::ChannelNames, "L*,M*,N*,P*,32");
	// globalserver->setcodepointsallowedlist(lacewing::relayserver::codepointsallowlistindex::MessagesSentToClients, "L*,M*,N*,P*,32");
	globalserver->setcodepointsallowedlist(lacewing::relayserver::codepointsallowlistindex::MessagesSentToServer, "L*,M*,N*,P*,32");

	UpdateTitle(0); // Update console title with 0 clients

	// Check port settings
	int port = FIXEDPORT;
	if constexpr (FIXEDPORT == 0)
	{
		std::cout << "Enter port number to begin (default 6121):"sv;

		{
			std::string portStr;
			std::getline(std::cin, portStr);
			std::stringstream lazy(portStr); lazy >> port;
			port = port <= 0 || port > 0xFFFF ? 6121 : port;
		}
	}
#ifdef FLASH_ENABLED
	GenerateFlashPolicy(port);
#endif

	// Update the current time in case host() errors, or try to connect before first tick
	OnTimerTick(globalmsgrecvcounttimer);

	// Host the thing
	std::cout << green << "Host started. Port "sv << port << ", build "sv << globalserver->buildnum << ". "sv <<
		(flashpolicypath.empty() ? "Flash not hosting"sv : "Flash policy hosting on TCP port 843"sv) << '.' <<
		std::string(flashpolicypath.empty() ? 30 : 5, ' ') << "\r\n"sv << yellow;

	if (websocketSecure)
	{
		if (!lw_file_exists(sslPathCertChain))
		{
			std::cout << yellow << "Couldn't find TLS certficate files - expecting \"fullchain.pem\" and \"privkey.pem\" in app folder.\r\n"
				"Will continue webserver with just insecure websocket.\r\n"sv;
			websocketSecure = 0;
		}
		else if (!globalserver->websocket->load_cert_file(sslPathCertChain, sslPathPrivKey, ""))
		{
			std::cout << red << "Found but couldn't load TLS certificate files \"fullchain.pem\" and \"privkey.pem\". Aborting server.\r\n"sv;
			goto cleanup;
		}
	}

	if (websocketNonSecure || websocketSecure)
	{
		std::cout << green << "WebSocket hosting. Port "sv;
		if (websocketNonSecure)
			std::cout << websocketNonSecure << " (non-secure, ws://xx)"sv;
		if (websocketNonSecure && websocketSecure)
			std::cout << " and port "sv;
		if (websocketSecure)
			std::cout << websocketSecure << " (secure, wss://xx)"sv;
		std::cout << ".\r\n"sv << yellow;
	}
	std::cout.flush();

	globalserver->host((lw_ui16)port);

	if (!flashpolicypath.empty())
		globalserver->flash->host(flashpolicypath.c_str());

	if (websocketNonSecure || websocketSecure)
		globalserver->host_websocket((lw_ui16)websocketNonSecure, (lw_ui16)websocketSecure);

	// Update messages received/sent line every 1 sec
	globalmsgrecvcounttimer->start(1000L);

	// Start main event loop
#ifdef _DEBUG
	error = globalpump->start_eventloop();
#else
	try {
		error = globalpump->start_eventloop();
	}
	catch (...)
	{
		error = lacewing::error_new();
		error->add("Crash happened.");
	}
#endif

	if (error)
		std::cout << red << "\r\n"sv << timeBuffer << " | Error occurred in pump: "sv << error->tostring() << "\r\n"sv;

cleanup:
	// Cleanup time
	clientdata.clear();
	lacewing::timer_delete(globalmsgrecvcounttimer);
	globalserver->unhost();
	globalserver->flash->unhost();
	globalserver->websocket->unhost();
	globalserver->websocket->unhost_secure();
	delete globalserver;
	lacewing::pump_delete(globalpump);

	if (!flashpolicypath.empty() && deleteFlashPolicyAtEndOfApp)
		remove(flashpolicypath.c_str());

	// Lacewing uses a sync inside lw_trace, which is singleton and never freed.
	// lw_trace() is a no-op if _lacewing_debug isn't defined.
	// To let garbage collector not see it as a leak:
#if defined(_CRTDBG_MAP_ALLOC) && defined(_lacewing_debug)
	extern lw_sync lw_trace_sync;
	lw_sync_delete(lw_trace_sync);
#endif

	std::cout << green << timeBuffer << " | Program completed.\r\n"sv;
	std::cout << timeBuffer << " | Total bytes: "sv << totalBytesIn << " in, "sv << totalBytesOut << " out.\r\n"sv;
	std::cout << timeBuffer << " | Total msgs: "sv << totalNumMessagesIn << " in, "sv << totalNumMessagesOut << " out.\r\n"sv;
	std::cout << timeBuffer << " | Max msgs in 1 sec: "sv << maxNumMessagesIn << " in, "sv << maxNumMessagesOut << " out.\r\n"sv;
	std::cout << timeBuffer << " | Max bytes in 1 sec: "sv << maxBytesInInOneSec << " in, "sv << maxBytesOutInOneSec << " out.\r\n"sv;
	std::cout << timeBuffer << " | Press any key to exit.\r\n"sv;

	// Clear any keypress the user did before we waited
	std::cin.clear();
	std::cin.ignore();
	std::cin.get(); // wait for user keypress

	std::cout << "\x1B[0m"; // reset console color
	tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // restore console input mode
	return 0;
}

void UpdateTitle(size_t clientCount)
{
	size_t channelCount = globalserver->channelcount();
	char name[128];
	sprintf(name, "Bluewing C++ Server - %zu client%s connected in %zu channel%s",
		clientCount, clientCount == 1 ? "" : "s",
		channelCount, channelCount == 1 ? "" : "s");

	// suits aixterm, dtterm, linux, xterm consoles. Taken from .NET Core's Unix terminfo title format string.
	// cygwin: "\x1B];%p1%s\x07";
	// konsole: "\x1B]30;%p1%s\x07";
	// screen: "\x1Bk%p1%s\x1B";
	std::cout << "\033]0;"sv << name << '\007';

	if (maxClients < clientCount)
		maxClients = clientCount;
	if (maxChannels < channelCount)
		maxChannels = channelCount;
}

void OnConnectRequest(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> client)
{
	char addr[64];
	lw_addr_prettystring(client->getaddress().data(), addr, sizeof(addr));

	auto banEntry = std::find_if(banIPList.begin(), banIPList.end(), [&](const BanEntry& b) { return b.ip == addr; });
	if (banEntry != banIPList.end())
	{
		if (banEntry->resetAt < time(NULL))
			banIPList.erase(banEntry);
		else if (banEntry->disconnects > 3)
		{
			banEntry->resetAt = time(NULL) + (time_t)(((long long)(banEntry->disconnects++ << 2)) * 60 * 60);

			std::cout << green << '\r' << timeBuffer << " | Blocked connection attempt from IP "sv << addr << ", banned due to "sv
				<< banEntry->reason << '.'
				<< std::string(45, ' ') << "\r\n"sv << yellow;
			return server.connect_response(client, banEntry->reason.c_str());
		}
	}

	server.connect_response(client, std::string_view());
	UpdateTitle(server.clientcount());

	std::cout << green << '\r' << timeBuffer << " | New client ID "sv << client->id() << ", IP "sv << addr << " connected."sv
		<< std::string(45, ' ') << "\r\n"sv << yellow;
	clientdata.push_back(std::make_unique<clientstats>(client));
}
void OnDisconnect(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> client)
{
	UpdateTitle(server.clientcount());
	std::string name = client->name();
	name = !name.empty() ? name : "[unset]"sv;
	char addr[64];
	lw_addr_prettystring(client->getaddress().data(), addr, sizeof(addr));
	const auto a = std::find_if(clientdata.cbegin(), clientdata.cend(), [&](const auto& c) {
		return c->c == client; }
	);

	std::cout << green << '\r' << timeBuffer << " | Client ID "sv << client->id() << ", name "sv << name << ", IP "sv << addr << " disconnected."sv;
	if (a != clientdata.cend())
		std::cout << " Uploaded "sv << (**a).totalBytesIn << " bytes in "sv << (**a).totalNumMessagesIn << " msgs total."sv;
	else
		std::cout << std::string(25, ' ');
	std::cout << "\r\n"sv << yellow;

	if (a != clientdata.cend())
		clientdata.erase(a);
	if (!client->istrusted())
	{
		auto banEntry = std::find_if(banIPList.begin(), banIPList.end(), [&](const BanEntry& b) { return b.ip == addr; });
		if (banEntry == banIPList.end())
		{
			std::cout << yellow << '\r' << timeBuffer << " | Due to malformed protocol usage, created a IP ban entry."sv << std::string(25, ' ')
				<< "\r\n"sv << yellow;
			banIPList.push_back(BanEntry(addr, 1, "Broken Lacewing protocol", (time(NULL) + 30 * 60)));
		}
		else
		{
			std::cout << yellow << '\r' << timeBuffer << " | Due to malformed protocol usage, increased their ban likelihood."sv << std::string(25, ' ')
				<< "\r\n"sv << yellow;
			banEntry->disconnects++;
		}
	}
}

void OnTimerTick(lacewing::timer timer)
{
	std::time_t rawtime = std::time(NULL);
	std::time(&rawtime);
	std::tm* timeinfo = localtime(&rawtime);
	if (timeinfo)
		std::strftime(timeBuffer, sizeof(timeBuffer), "%T", timeinfo);
	else
		strcpy(timeBuffer, "XX:XX:XX");

	totalNumMessagesIn += numMessagesIn;
	totalNumMessagesOut += numMessagesOut;
	totalBytesIn += bytesIn;
	totalBytesOut += bytesOut;
	if (maxNumMessagesIn < numMessagesIn)
		maxNumMessagesIn = numMessagesIn;
	if (maxNumMessagesOut < numMessagesOut)
		maxNumMessagesOut = numMessagesOut;
	if (maxBytesInInOneSec < bytesIn)
		maxBytesInInOneSec = bytesIn;
	if (maxBytesOutInOneSec < bytesOut)
		maxBytesOutInOneSec = bytesOut;

	std::cout << timeBuffer << " | Last sec received "sv << numMessagesIn << " messages ("sv << bytesIn << " bytes), forwarded "sv
		<< numMessagesOut << " ("sv << bytesOut << " bytes)."sv << std::string(15, ' ') << '\r';
	std::cout.flush();
	numMessagesOut = numMessagesIn = 0U;
	bytesIn = bytesOut = 0U;

#ifdef TCP_CLIENT_UPLOAD_CAP
	for (auto& c : clientdata)
	{
		if (!c->exceeded)
		{
			c->bytesIn = 0;
			c->numMessagesIn = 0;
		}
	}
	// open clientdata as shared owner, or disconnect handler's erase may invalidate it while TimerTick is still using it
	for (auto c : clientdata)
	{
		if (!c->exceeded)
			continue;
		char addr[64];
		const char* ipAddress = c->c->getaddress().data();
		lw_addr_prettystring(ipAddress, addr, sizeof(addr));

		auto banEntry = std::find_if(banIPList.begin(), banIPList.end(), [&](const BanEntry& b) { return b.ip == addr; });
		if (banEntry == banIPList.end())
			banIPList.push_back(BanEntry(ipAddress, 1, "You have been banned for heavy TCP usage. Contact Phi on Clickteam Discord.", time(NULL) + 60));
		else
			++banEntry->disconnects;

		std::cout << red << '\r' << timeBuffer << " | Client ID "sv << c->c->id() << ", IP "sv << addr <<
			" dropped for heavy TCP upload ("sv << c->bytesIn << " bytes in "sv << c->numMessagesIn << " msgs)"sv << yellow << "\r\n"sv;
		c->c->send(1, "You have exceeded the TCP upload limit. Contact Phi on Clickteam Discord."sv, 0);
		c->c->send(0, "You have exceeded the TCP upload limit. Contact Phi on Clickteam Discord."sv, 0);
		c->c->disconnect();

		// disconnect() will usually call disconnect handler, but rarely won't.
		// If it does call the handler, the handler will delete the clientdata "c", so this for loop running through clientdata
		// is now invalid, so we have to break or we get exception from invalid iterator.
		// If it doesn't call the handler, we need to erase "c" or we'll get a disconnect re-attempted every timer tick.
		const auto a = std::find_if(clientdata.cbegin(), clientdata.cend(), [&](const auto& cd) {
			return cd->c == c->c; }
		);
		if (a != clientdata.cend())
			clientdata.erase(a);

		break;
	}
#endif
}

static bool shutdowned = false;
void Shutdown()
{
	if (shutdowned)
		return;
	shutdowned = true;

	globalpump->post_eventloop_exit(); // end main loop
}
void OnError(lacewing::relayserver& server, lacewing::error error)
{
	std::string_view err = error->tostring();
	if (err.back() == '.')
		err.remove_suffix(1);
	std::cout << red << '\r' << timeBuffer << " | Error occured: "sv << err << ". Execution continues."sv
		<< std::string(25, ' ') << "\r\n"sv << yellow;
}

void OnServerMessage(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> senderclient,
	bool blasted, lw_ui8 subchannel, std::string_view data, lw_ui8 variant)
{
	++numMessagesIn;
	bytesIn += data.size();

	if (blasted || variant != 0 || subchannel != 0 && subchannel != 5)
	{
		char addr[64];
		lw_addr_prettystring(senderclient->getaddress().data(), addr, sizeof(addr));
		std::cout << red << '\r' << timeBuffer << " | Dropped server message from IP "sv << addr << ", invalid type."sv
			<< std::string(35, ' ') << "\r\n"sv << yellow;
		const auto cd = std::find_if(clientdata.cbegin(), clientdata.cend(), [&](const auto& b) { return b->c == senderclient; });
		if (cd != clientdata.end())
		{
			(**cd).totalBytesIn += data.size();
			++(**cd).totalNumMessagesIn;

			if ((**cd).wastedServerMessages++ > 5) {
				auto banEntry = std::find_if(banIPList.begin(), banIPList.end(), [&](const BanEntry& b) { return b.ip == addr; });
				if (banEntry == banIPList.end())
					banIPList.push_back(BanEntry(addr, 1, "Sending too many messages the server is not meant to handle.",
						time(NULL) + 60 * 60));
				else
					++banEntry->disconnects;
				senderclient->send(1, "You have been banned for sending too many server messages that the server is not designed to receive.\r\nContact Phi on Clickteam Discord."sv);
				senderclient->disconnect();
			}
		}
		return;
	}
	std::string name = senderclient->name();
	name = !name.empty() ? name : "[unset]"sv;

	std::cout << white << '\r' << timeBuffer << " | Message from client ID "sv << senderclient->id() << ", name "sv << name
		<< ":"sv << std::string(35, ' ') << "\r\n"sv
		<< data << "\r\n"sv << yellow;
	if (subchannel == 5)
	{
		//std::string cars[4] = { "Volvo", "BMW", "Ford", "Mazda" };
		//std::cout << cars[0];

		// { "foo" : [ 1, 2, 3 ] }

		using bsoncxx::builder::basic::kvp;
		using bsoncxx::builder::basic::sub_array;

		const auto elements = { 1, 2, 3 };
		auto doc = bsoncxx::builder::basic::document{};
		doc.append(kvp("foo", [&elements](sub_array child) {
			for (const auto& element : elements) {
				child.append(element);
			}
			}));
		std::cout << bsoncxx::to_json(doc) << std::endl;

	}
	if (data == "HI")
	{
		std::cout << "LOL IT WORKED\n";
	}
}
bool IncrementClient(std::shared_ptr<lacewing::relayserver::client> client, size_t size, bool blasted)
{
	const auto cd = std::find_if(clientdata.cbegin(), clientdata.cend(), [&](const auto& b) { return b->c == client; });
	if (cd != clientdata.cend())
	{
		(**cd).totalBytesIn += size;
		++(**cd).totalNumMessagesIn;

#ifdef TCP_CLIENT_UPLOAD_CAP
		if (!blasted)
		{
			(**cd).bytesIn += size;
			(**cd).exceeded = (**cd).exceeded || (**cd).bytesIn > TCP_CLIENT_UPLOAD_CAP;
			return !(**cd).exceeded;
		}
#endif
	}
	return true;
}
void OnPeerMessage(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> senderclient,
	std::shared_ptr<lacewing::relayserver::channel> viachannel, std::shared_ptr<lacewing::relayserver::client> receiverclient,
	bool blasted, lw_ui8 subchannel, std::string_view data, lw_ui8 variant)
{
	++numMessagesIn;
	bytesIn += data.size();
#ifdef TOTAL_UPLOAD_CAP
	if (bytesOut > 50000 && blasted)
	{
		server.clientmessage_permit(senderclient, viachannel, receiverclient, blasted, subchannel, data, variant, false);
		return;
	}
#endif

	// False means it's exceeded TCP limits (if TCP limit is off, this'll always return true)
	if (!IncrementClient(senderclient, data.size(), blasted))
	{
		server.clientmessage_permit(senderclient, viachannel, receiverclient, blasted, subchannel, data, variant, false);
		return;
	}

	++numMessagesOut;
	bytesOut += data.size();
	server.clientmessage_permit(senderclient, viachannel, receiverclient, blasted, subchannel, data, variant, true);
}

void OnChannelMessage(lacewing::relayserver& server, std::shared_ptr<lacewing::relayserver::client> senderclient,
	std::shared_ptr<lacewing::relayserver::channel> channel,
	bool blasted, lw_ui8 subchannel, std::string_view data, lw_ui8 variant)
{
	++numMessagesIn;
	bytesIn += data.size();

#ifdef TOTAL_UPLOAD_CAP
	if (bytesOut > TOTAL_UPLOAD_CAP && blasted)
	{
		server.channelmessage_permit(senderclient, channel, blasted, subchannel, data, variant, false);
		++numMessagesIn;
		bytesIn += data.size();
		return;
	}
#endif

	// False means it's exceeded TCP limits (if TCP limit is off, this'll always return true)
	if (!IncrementClient(senderclient, data.size(), blasted))
	{
		server.channelmessage_permit(senderclient, channel, blasted, subchannel, data, variant, false);
		return;
	}

	server.channelmessage_permit(senderclient, channel, blasted, subchannel, data, variant, true);
	size_t numCli = channel->clientcount() - 1U;
	numMessagesOut += numCli;
	bytesOut += numCli * data.size();
}

// Until we have a better general error handler for Lacewing...
extern "C" void always_log(const char* c, ...)
{
	char output[1024];
	va_list v;
	va_start(v, c);
	int numChars = vsprintf(output, c, v);
	if (numChars <= 0)
		std::abort();
	std::cout << yellow << '\r' << timeBuffer << " | "sv << output << std::string(35, ' ') << "\r\n"sv;
	va_end(v);
}

#include <sys/stat.h>   // stat
#include <stdbool.h>    // bool type
#include <unistd.h>
#include <limits.h>

void GenerateFlashPolicy(int port)
{
	char filenameBuf[1024];
	// Get full path of EXE, including EXE filename + ext
	ssize_t len = ::readlink("/proc/self/exe", filenameBuf, sizeof(filenameBuf) - 1);
	if (len == -1) {
		std::cout << "Flash policy couldn't be created. Looking up current app folder failed.\r\n"sv;
		return;
	}
	filenameBuf[len] = '\0';

	// Strip EXE part
	std::string filename(filenameBuf);
	size_t lastSlash = filename.rfind('/');
	if (lastSlash == std::string::npos)
		lastSlash = filename.rfind('\\');
	if (lastSlash == std::string::npos)
	{
		std::cout << "Flash policy couldn't be created. Current app folder made no sense.\r\n"sv;
		return;
	}

	filename = filename.substr(0U, lastSlash + 1U) + "FlashPlayerPolicy.xml";

	// File already exists; just use it
	struct stat buffer;
	if (stat(filename.c_str(), &buffer) == 0)
	{
		flashpolicypath = filename;
		return;
	}

	FILE* forWriting = fopen(filename.c_str(), "wb");
	if (forWriting == NULL)
	{
		std::cout << "Flash policy couldn't be created. Opening file "sv << filename << " for writing in current app folder failed.\r\n"sv;
		return;
	}

	deleteFlashPolicyAtEndOfApp = true;

	std::stringstream flashPolicy;
	flashPolicy << "<?xml version=\"1.0\"?>\r\n"sv
		"<!DOCTYPE cross-domain-policy SYSTEM \"/xml/dtds/cross-domain-policy.dtd\">\r\n"sv
		"<cross-domain-policy>\r\n"sv
		"\t<site-control permitted-cross-domain-policies=\"master-only\"/>\r\n"sv
		"\t<allow-access-from domain=\"*\" to-ports=\"843," << port << ",583\" secure=\"false\" />\r\n"sv
		"</cross-domain-policy>"sv;
	const std::string policyStr = flashPolicy.str();
	if (fwrite(policyStr.c_str(), 1, policyStr.size(), forWriting) != policyStr.size())
	{
		std::cout << "Flash policy couldn't be created. Writing to file "sv << filename << " failed.\r\n"sv;
		fclose(forWriting);
		remove(filename.c_str());
		return;
	}

	fclose(forWriting);
	flashpolicypath = filename;
}

void CloseHandler(int sig)
{
	std::cout << red << '\r' << timeBuffer << " | "sv;

	// Catch exceptions
	switch (sig)
	{
	case SIGABRT:
		std::cout << "Caught SIGABRT: usually caused by an abort() or assert()                   \r\n"sv;
		break;
	case SIGFPE:
		std::cout << "Caught SIGFPE: arithmetic exception, such as divide by zero                \r\n"sv;
		break;
	case SIGILL:
		std::cout << "Caught SIGILL: illegal instruction                                         \r\n"sv;
		break;
	case SIGINT:
		std::cout << "Caught SIGINT: interactive attention signal, probably a ctrl+c             \r\n"sv;
		break;
	case SIGSEGV:
		std::cout << "Caught SIGSEGV: segfault                                                   \r\n"sv;
		break;
	case SIGTERM:
	default:
		std::cout << "Caught SIGTERM: a termination request was sent to the program              \r\n"sv;
		break;
	}


	if (!shutdowned)
	{
		std::cout << red << '\r' << timeBuffer << " | Got Ctrl-C or Close, ending the app."sv << std::string(30, ' ') << "\r\n"sv << yellow;
		Shutdown();
	}

	// Every other command will likely kill the program after end of this handler
	if (sig != SIGINT)
	{
		std::cout << red << '\r' << timeBuffer << " | Aborting instantly from signal "sv << sig << '.' << std::string(40, ' ') << "\r\n"sv;
		std::cout << "\x1B[0m"; // reset console color

		tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // restore console input mode

		if (!flashpolicypath.empty() && deleteFlashPolicyAtEndOfApp)
			remove(flashpolicypath.c_str());

		// Cleanup time
		clientdata.clear();
		lacewing::timer_delete(globalmsgrecvcounttimer);
		globalserver->unhost();
		globalserver->flash->unhost();
		globalserver->websocket->unhost();
		globalserver->websocket->unhost_secure();
		delete globalserver;
		lacewing::pump_delete(globalpump);

		exit(EXIT_FAILURE);
	}
}
