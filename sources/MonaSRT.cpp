/*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; If not, see <http://www.gnu.org/licenses/>
*/

#include <algorithm>
#include <functional>

#include "SRTIn.h"

#include "MonaSRT.h"
#include "SRTOut.h"
#include "MonaSRTVersion.h"
#include "Mona/Logs.h"
#include "Mona/MapWriter.h"
#include "Mona/MapReader.h"
#include "Mona/HTTP/HTTPWriter.h"

#include "json11.hpp"

namespace {
struct url
{
	url(const std::string& url_s)
	{
		const std::string prot_end("://");
		std::string::const_iterator prot_i = std::search(
			url_s.begin(), url_s.end(),
			prot_end.begin(), prot_end.end());
		_protocol.reserve(std::distance(url_s.begin(), prot_i));
		std::transform(url_s.begin(), prot_i,
			std::back_inserter(_protocol),
			::tolower);
		if(prot_i == url_s.end())
			return;
		std::advance(prot_i, prot_end.length());
		std::string::const_iterator path_i = std::find(
			prot_i, url_s.end(), '/');
		_host.reserve(std::distance(prot_i, path_i));
		std::transform(prot_i, path_i,
			std::back_inserter(_host),
			::tolower);
		std::string::const_iterator query_i = std::find(
			path_i, url_s.end(), '?');
		_path.assign(path_i, query_i);
		std::string::const_iterator app_i = std::find(
			std::next(path_i, 1), url_s.end(), '/');
		_host.reserve(std::distance(path_i, app_i));
		std::transform(path_i, app_i,
			std::back_inserter(_app),
			::tolower);
		if( query_i != url_s.end() )
			++query_i;
		_query.assign(query_i, url_s.end());
	}
	std::string _protocol, _host, _app, _path, _query;
};
} //anonymous

using namespace std;

namespace Mona {

bool SRTOpenParams::SetFromURL(const std::string& url)
{
	return true;
}

//// External Publish ////



//// Server Events /////
void MonaSRT::onStart() {
#if 0
	_appsByName["/srt"] = new SRTOut(*this);
	if (getBoolean<false>("SRT")) {
		_srtIn = new SRTIn(*this, *this);
		_srtIn->load();
	}
#endif
}

void MonaSRT::manage() {

	std::lock_guard<std::mutex> lock(_appsMutex);
	// manage application!
	for (auto& it : _appsByName)
		it.second->manage();
}

void MonaSRT::onStop() {
	std::lock_guard<std::mutex> lock(_appsMutex);
	// delete applications
	for (auto& it : _appsByName)
		delete it.second;
	_appsByName.clear();
	_appsById.clear();

	if (_srtIn) {
		delete _srtIn;
		_srtIn = nullptr;
	}

	// unblock ctrl+c waiting
	_terminateSignal.set();
}

//// Client Events /////
SocketAddress& MonaSRT::onHandshake(const string& path, const string& protocol, const SocketAddress& address, const Parameters& properties, SocketAddress& redirection) {
	DEBUG(protocol, " ", address, " handshake to ", path.empty() ? "/" : path);

	if (!String::ICompare(protocol, "HTTP")/* && !String::ICompare(path, "/srt")*/)
		return redirection;

	std::lock_guard<std::mutex> lock(_appsMutex);
	const auto& it(_appsByName.find(path));
	return it == _appsByName.end() ? redirection : it->second->onHandshake(protocol, address, properties, redirection);
}

void MonaSRT::onConnection(Exception& ex, Client& client, DataReader& parameters, DataWriter& response) {
	DEBUG(client.protocol, " ", client.address, " connects to ", client.path.empty() ? "/" : client.path)

	if (!String::ICompare(client.protocol, "HTTP"))
		return;

	std::lock_guard<std::mutex> lock(_appsMutex);
	const auto& it(_appsByName.find(client.path));
	if (it == _appsByName.end())
		return;
	client.setCustomData<App::Client>(it->second->newClient(ex,client,parameters,response));
}

void MonaSRT::onDisconnection(Client& client) {
	DEBUG(client.protocol, " ", client.address, " disconnects from ", client.path.empty() ? "/" : client.path);
	if (client.hasCustomData()) {
		std::lock_guard<std::mutex> lock(_appsMutex);
		const auto& it(_appsByName.find(client.path));
		if (it != _appsByName.end())
			it->second->deleteClient(client.getCustomData<App::Client>());
		else
			delete client.getCustomData<App::Client>();
		client.setCustomData<App::Client>(NULL);
	}
}

void MonaSRT::onAddressChanged(Client& client, const SocketAddress& oldAddress) {
	if (client.hasCustomData())
		client.getCustomData<App::Client>()->onAddressChanged(oldAddress);
}

static void makeJsonResponse(std::string& response,
		const std::string& status, const std::string& msg = std::string())
{
	std::map<std::string, std::string> m;

	m["Status"] = status;
	if (!msg.empty()) {
		m["Details"] = msg;
	}
	const json11::Json json(m);
	json.dump(response);
}

bool MonaSRT::onInvocation(Exception& ex, Client& client, const string& name, DataReader& arguments, UInt8 responseType) {
	// on client message, returns "false" if "name" message is unknown
	const std::string &method = client.properties().getString("type");
	DEBUG(name," call from ",client.protocol," to ",
		client.path.empty() ? "/" : client.path, " type ", responseType,
		" method ", method.empty() ? "UNKNOWN" : method)

	HTTPWriter * phttpWriter = dynamic_cast<HTTPWriter*>(&client.writer());
	if (phttpWriter != nullptr) {
		std::string response, msg;
		const std::string body((const char*)arguments->data(),
			arguments->size());

		if (method == "GET") {
			std::vector<std::string> parts;
			Mona::String::Split(client.path, "/", parts,
				SPLIT_TRIM | SPLIT_IGNORE_EMPTY);
			if (parts.size() == 0) {
				if (name == "channels") {
					std::map<std::string, Mona::Parameters&> channels;
					for (auto it = _appsById.begin(); it != _appsById.end(); it++) {
						channels.emplace(it->first, *it->second);
					}
					const json11::Json json = json11::Json::object({
						{ "channels", channels }
					});
					json.dump(response);
				} else if (name == "status") {
					Exception ex;
					std::vector<Mona::IPAddress> ips = Mona::IPAddress::Locals(ex);
					std::vector<Mona::IPAddress>::const_iterator it = ips.begin();
					while (it < ips.end()) {
						if (it->family() != Mona::IPAddress::IPv4
								|| it->isLoopback() || it->isLinkLocal()) {
							it = ips.erase(it);
						} else {
							it++;
						}
					}
					const json11::Json json = json11::Json::object({
						{ "version", MONASRT_VERSION_STRING },
						{ "hostIPv4", ips }
					});
					json.dump(response);
				}
			} else if (parts.size() == 2 && parts[0] == "channels") {
				const std::string &channelId = parts[1];
				auto chanIt = _appsById.find(channelId);
				if (chanIt != _appsById.end()) {
					App* app = chanIt->second;
					if (!app->onHTTPRequest(method, name, body, response))
						return false;
				}
			}
		} else if (method == "POST" && !client.path.empty() && !name.empty()) {
			std::vector<std::string> parts;
			Mona::String::Split(client.path, "/", parts,
				SPLIT_TRIM | SPLIT_IGNORE_EMPTY);
			if (parts.size() == 2 && parts[0] == "channels") {
				const std::string &channelId = parts[1];
				auto chanIt = _appsById.find(channelId);
				if (name == "start") {
					if (chanIt != _appsById.end()) {
						msg = "Channel " + channelId + " already started";
						ERROR(msg);
						makeJsonResponse(response, "Fail", msg);
						goto httpout;
					}
					DEBUG("Start ", channelId, " channel");

					std::string err;
					json11::Json json = json11::Json::parse(body, err);

					if (!err.empty()) {
						msg = "Error parsing JSON command: " + err;
						ERROR(msg);
						makeJsonResponse(response, "Fail", msg);
						goto httpout;
					}

					std::string appName;
					const std::string& inputUrl = json["inputUrl"].string_value();
					::url inurl(inputUrl);
					if (inurl._protocol == "rtmp" || inurl._protocol == "rtmps") {
						if (inurl._app.empty()) {
							msg = "Invalid RTMP application";
							ERROR(msg);
							makeJsonResponse(response, "Fail", msg);
							goto httpout;
						}
						appName = inurl._app;
					} else if (inurl._protocol == "udp") {
						appName = channelId;
						// TODO: Start UDP stream
					} else {
							msg = "Unsupported input protocol " + inurl._protocol;
							ERROR(msg);
							makeJsonResponse(response, "Fail", msg);
							goto httpout;
					}

					const std::string& outputUrl = json["outputUrl"].string_value();
					::url outurl(outputUrl);
					if (outurl._protocol != "srt") {
							msg = "Unsupported output protocol " + inurl._protocol;
							ERROR(msg);
							makeJsonResponse(response, "Fail", msg);
							goto httpout;
					}
					
					if (_appsByName.find(appName) != _appsByName.end()) {
						msg = "Existing applicaion";
						ERROR(msg);
						makeJsonResponse(response, "Fail", msg);
						goto httpout;
					}

					Mona::Parameters params;
					const json11::Json::object &object = json.object_items();
					for (auto &it : object) {
						if (it.second.is_string()) {
							const std::string& key = it.first;
							const std::string& val = it.second.string_value();

							if ((true)) {
								DEBUG(key, " = ", val);
							}
							params.setString(key, val);
						}
					}
					SRTOut * app = new SRTOut(params);
					_appsMutex.lock();
					_appsByName[appName] = app;
					_appsById[channelId] = app;
					_appsMutex.unlock();
					msg = "Channel " + channelId + " started";
					makeJsonResponse(response, "Success", msg);
				} else if (name == "stop") {
					auto abiIt = _appsById.find(channelId);
					if (abiIt != _appsById.end()) {
						DEBUG("Stop ", channelId, " channel");

						App* app = abiIt->second;
						app->closeClients();
						_appsMutex.lock();
						_appsById.erase(abiIt);
						for (auto abnIt = _appsByName.begin();
								abnIt != _appsByName.end(); abnIt++) {
							if (abnIt->second == app) {
								_appsByName.erase(abnIt);
								break;
							}
						}
						_appsMutex.unlock();
						delete app;
						makeJsonResponse(response, "Success");
					} else {
						msg = "Channel " + channelId + " does not exist";
						ERROR(msg);
						makeJsonResponse(response, "Fail", msg);
					}
				}
			}
		}
httpout:
		if (!response.empty()) {
			DataWriter& dataWriter(phttpWriter->writeResponse("json"));
			BinaryWriter& binaryWriter(*dataWriter);
			binaryWriter.clear();
			binaryWriter.write(response);
			binaryWriter.write("\r\n");

			return true;
		}
	} else if (client.hasCustomData())
		return client.getCustomData<App::Client>()->onInvocation(ex, name, arguments,responseType);

	return false;
} 


bool MonaSRT::onFileAccess(Exception& ex, File::Mode mode, Path& file, DataReader& arguments, DataWriter& properties, Client* pClient) {
	// on client file access, returns "false" if acess if forbiden
	if(pClient) {
		DEBUG(file.name(), " file access from ", pClient->protocol, " to ", pClient->path.empty() ? "/" : pClient->path);
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onFileAccess(ex, mode, file, arguments, properties);
	} else
		DEBUG(file.name(), " file access to ", file.parent().empty() ? "/" : file.parent());
	// arguments.read(properties); to test HTTP page properties (HTTP parsing!)
	return true;
}


//// Publication Events /////

bool MonaSRT::onPublish(Exception& ex, Publication& publication, Client* pClient) {
	if (pClient) {
		NOTE("Client publish ", publication.name());
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onPublish(ex, publication);
	} else
		NOTE("Publish ",publication.name())

	return false; // "true" to allow, "false" to forbid
}

void MonaSRT::onUnpublish(Publication& publication, Client* pClient) {
	if (pClient) {
		NOTE("Client unpublish ", publication.name());
		if(pClient->hasCustomData())
			pClient->getCustomData<App::Client>()->onUnpublish(publication);
	} else
		NOTE("Unpublish ",publication.name())
}

bool MonaSRT::onSubscribe(Exception& ex, const Subscription& subscription, const Publication& publication, Client* pClient) {
	if (pClient) {

		INFO(pClient->protocol, " ", pClient->address, " subscribe to ", publication.name());
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onSubscribe(ex, subscription, publication);
	} else
		INFO("Subscribe to ", publication.name());
	return true; // "true" to allow, "false" to forbid
}

void MonaSRT::onUnsubscribe(const Subscription& subscription, const Publication& publication, Client* pClient) {
	if (pClient) {
		INFO(pClient->protocol, " ", pClient->address, " unsubscribe to ", publication.name());
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onUnsubscribe(subscription, publication);
	} else
		INFO("Unsubscribe to ", publication.name());
}

} // namespace Mona
