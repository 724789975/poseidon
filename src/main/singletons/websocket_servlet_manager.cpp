#include "../../precompiled.hpp"
#include "websocket_servlet_manager.hpp"
#include <map>
#include <boost/noncopyable.hpp>
#include <boost/ref.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/locks.hpp>
#include "../log.hpp"
#include "../exception.hpp"
using namespace Poseidon;

class Poseidon::WebSocketServlet : boost::noncopyable,
	public boost::enable_shared_from_this<WebSocketServlet>
{
private:
	const std::string m_uri;
	const boost::weak_ptr<const void> m_dependency;
	const WebSocketServletCallback m_callback;

public:
	WebSocketServlet(const std::string &uri,
		const boost::weak_ptr<const void> &dependency, const WebSocketServletCallback &callback)
		: m_uri(uri), m_dependency(dependency), m_callback(callback)
	{
		LOG_INFO("Created WebSocket servlet for URI ", m_uri);
	}
	~WebSocketServlet(){
		LOG_INFO("Destroyed WebSocket servlet for URI ", m_uri);
	}

public:
	boost::shared_ptr<const WebSocketServletCallback>
		lock(boost::shared_ptr<const void> &lockedDep) const
	{
		if((m_dependency < boost::weak_ptr<void>()) || (boost::weak_ptr<void>() < m_dependency)){
			lockedDep = m_dependency.lock();
			if(!lockedDep){
				return NULLPTR;
			}
		}
		return boost::shared_ptr<const WebSocketServletCallback>(shared_from_this(), &m_callback);
	}
};

namespace {

boost::shared_mutex g_mutex;
std::map<std::string, boost::weak_ptr<const WebSocketServlet> > g_servlets;

}

void WebSocketServletManager::start(){
}
void WebSocketServletManager::stop(){
	LOG_INFO("Unloading all WebSocket servlets...");

	g_servlets.clear();
}

boost::shared_ptr<const WebSocketServlet>
	WebSocketServletManager::registerServlet(const std::string &uri,
		const boost::weak_ptr<const void> &dependency, const WebSocketServletCallback &callback)
{
	AUTO(newServlet, boost::make_shared<WebSocketServlet>(
		boost::ref(uri), boost::ref(dependency), boost::ref(callback)));
	{
		const boost::unique_lock<boost::shared_mutex> ulock(g_mutex);
		AUTO_REF(servlet, g_servlets[uri]);
		if(!servlet.expired()){
			DEBUG_THROW(Exception, "Duplicate protocol servlet.");
		}
		servlet = newServlet;
	}
	return newServlet;
}

boost::shared_ptr<const WebSocketServletCallback>
	WebSocketServletManager::getServlet(boost::shared_ptr<const void> &lockedDep, const std::string &uri)
{
	const boost::shared_lock<boost::shared_mutex> slock(g_mutex);
	const AUTO(it, g_servlets.find(uri));
	if(it == g_servlets.end()){
		return NULLPTR;
	}
	const AUTO(servlet, it->second.lock());
	if(!servlet){
		return NULLPTR;
	}
	return servlet->lock(lockedDep);
}