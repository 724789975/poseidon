#include "../../precompiled.hpp"
#include "mysql_daemon.hpp"
#include <list>
#include <boost/thread.hpp>
#include <boost/scoped_ptr.hpp>
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <unistd.h>
#include "config_file.hpp"
#include "../mysql/object_base.hpp"
#include "../atomic.hpp"
#include "../exception.hpp"
#include "../log.hpp"
#include "../job_base.hpp"
using namespace Poseidon;

namespace {

sql::SQLString g_databaseServer			= "tcp://localhost:3306";
sql::SQLString g_databaseUsername		= "root";
sql::SQLString g_databasePassword		= "root";
sql::SQLString g_databaseName			= "test";

std::size_t g_databaseSaveDelay			= 5000;
std::size_t g_databaseMaxReconnDelay	= 60000;

class AsyncLoadCallbackJob : public JobBase {
private:
	MySqlAsyncLoadCallback m_callback;
	boost::shared_ptr<MySqlObjectBase> m_object;

public:
	AsyncLoadCallbackJob(MySqlAsyncLoadCallback callback,
		boost::shared_ptr<MySqlObjectBase> object)
	{
		m_callback.swap(callback);
		m_object.swap(object);
	}

protected:
	void perform() const {
		m_callback(m_object);
	}
};

struct MySQLThreadInitializer : boost::noncopyable {
	MySQLThreadInitializer(){
		LOG_INFO("Initializing MySQL thread...");
		::get_driver_instance()->threadInit();
	}
	~MySQLThreadInitializer(){
		LOG_INFO("Uninitializing MySQL thread...");
		::get_driver_instance()->threadEnd();
	}
};

struct AsyncSaveItem {
	boost::shared_ptr<const MySqlObjectBase> object;
	unsigned long long timeStamp;

	void swap(AsyncSaveItem &rhs) throw() {
		object.swap(rhs.object);
		std::swap(timeStamp, rhs.timeStamp);
	}
};

struct AsyncLoadItem {
	boost::shared_ptr<MySqlObjectBase> object;
	std::string filter;
	MySqlAsyncLoadCallback callback;

	void swap(AsyncLoadItem &rhs) throw() {
		object.swap(rhs.object);
		filter.swap(rhs.filter);
		callback.swap(rhs.callback);
	}
};

volatile bool g_running = false;
boost::thread g_thread;

boost::mutex g_mutex;
std::list<AsyncSaveItem> g_saveQueue;
std::list<AsyncSaveItem> g_savePool;
std::list<AsyncLoadItem> g_loadQueue;
std::list<AsyncLoadItem> g_loadPool;
boost::condition_variable g_newObjectAvail;
boost::condition_variable g_queueEmpty;

void getMySqlConnection(boost::scoped_ptr<sql::Connection> &connection){
	LOG_INFO("Connecting to MySQL server...");

	std::size_t reconnectDelay = 0;
	for(;;){
		try {
			connection.reset(::get_driver_instance()->connect(
				g_databaseServer, g_databaseUsername, g_databasePassword));
			connection->setSchema(g_databaseName);
		} catch(sql::SQLException &e){
			LOG_ERROR("Error connecting to MySQL server: code = ", e.getErrorCode(),
				", state = ", e.getSQLState(), ", what = ", e.what());
			connection.reset();
		}
		if(connection){
			break;
		}
		if(reconnectDelay == 0){
			reconnectDelay = 1;
		} else {
			LOG_INFO("Will retry after ", reconnectDelay, " milliseconds.");
			::usleep(reconnectDelay * 1000);

			reconnectDelay <<= 1;
			if(reconnectDelay > g_databaseMaxReconnDelay){
				reconnectDelay = g_databaseMaxReconnDelay;
			}
		}
	}

	LOG_INFO("Successfully connected to MySQL server.");
}

void threadProc(){
	LOG_INFO("MySQL daemon started.");

	g_databaseServer =
		ConfigFile::get<std::string>("database_server", g_databaseServer);
	LOG_DEBUG("MySQL server = ", g_databaseServer);

	g_databaseUsername =
		ConfigFile::get<std::string>("database_username", g_databaseUsername);
	LOG_DEBUG("MySQL username = ", g_databaseUsername);

	g_databasePassword =
		ConfigFile::get<std::string>("database_password", g_databasePassword);
	LOG_DEBUG("MySQL password = ", g_databasePassword);

	g_databaseName =
		ConfigFile::get<std::string>("database_name", g_databaseName);
	LOG_DEBUG("MySQL database name = ", g_databaseName);

	g_databaseSaveDelay =
		ConfigFile::get<std::size_t>("database_save_delay", g_databaseSaveDelay);
	LOG_DEBUG("MySQL save delay = ", g_databaseSaveDelay);

	g_databaseMaxReconnDelay =
		ConfigFile::get<std::size_t>("database_max_reconn_delay", g_databaseMaxReconnDelay);
	LOG_DEBUG("MySQL max reconnect delay = ", g_databaseMaxReconnDelay);

	const MySQLThreadInitializer initializer;

	boost::scoped_ptr<sql::Connection> connection;
	for(;;){
		try {
			if(!connection){
				getMySqlConnection(connection);
			}

			AsyncSaveItem asi;
			AsyncLoadItem ali;
			{
				boost::mutex::scoped_lock lock(g_mutex);
				for(;;){
					if(!g_saveQueue.empty()){
						AUTO_REF(head, g_saveQueue.front());
						if(head.timeStamp > getMonoClock()){
							goto skip;
						}
						if(atomicLoad(head.object->m_context) != &head){
							AsyncSaveItem().swap(head);
						} else {
							asi.swap(head);
						}
						g_savePool.splice(g_savePool.begin(), g_saveQueue, g_saveQueue.begin());
						if(!asi.object){
							goto skip;
						}
						break;
					}
				skip:
					if(!g_loadQueue.empty()){
						ali.swap(g_loadQueue.front());
						g_loadPool.splice(g_loadPool.begin(), g_loadQueue, g_loadQueue.begin());
						break;
					}
					if(!atomicLoad(g_running)){
						break;
					}
					g_newObjectAvail.timed_wait(lock, boost::posix_time::seconds(1));
				}
			}
			if(asi.object){
				asi.object->syncSave(connection.get());
			} else if(ali.object){
				ali.object->syncLoad(connection.get(), ali.filter.c_str());
				ali.object->enableAutoSaving();

				if(ali.callback){
					boost::make_shared<AsyncLoadCallbackJob>(
						boost::ref(ali.callback), boost::ref(ali.object))->pend();
				}
			} else {
				break;
			}
		} catch(sql::SQLException &e){
			LOG_ERROR("SQLException thrown in MySQL daemon: code = ", e.getErrorCode(),
				", state = ", e.getSQLState(), ", what = ", e.what());

			LOG_INFO("The connection was left in an indeterminate state. Free it.");
			connection.reset();
		} catch(Exception &e){
			LOG_ERROR("Exception thrown in MySQL daemon: file = ", e.file(),
				", line = ", e.line(), ", what = ", e.what());
		} catch(std::exception &e){
			LOG_ERROR("std::exception thrown in MySQL daemon: what = ", e.what());
		} catch(...){
			LOG_ERROR("Unknown exception thrown in MySQL daemon.");
		}
	}

	LOG_INFO("MySQL daemon stopped.");
}

}

void MySqlDaemon::start(){
	if(atomicExchange(g_running, true) != false){
		LOG_FATAL("Only one daemon is allowed at the same time.");
		std::abort();
	}
	LOG_INFO("Starting MySQL daemon...");

	boost::thread(threadProc).swap(g_thread);
}
void MySqlDaemon::stop(){
	LOG_INFO("Stopping MySQL daemon...");

	atomicStore(g_running, false);
	{
		const boost::mutex::scoped_lock lock(g_mutex);
		g_newObjectAvail.notify_all();
	}
	if(g_thread.joinable()){
		g_thread.join();
	}
}

void MySqlDaemon::waitForAllAsyncOperations(){
	boost::mutex::scoped_lock lock(g_mutex);
	while(!(g_saveQueue.empty() && g_loadQueue.empty())){
		g_queueEmpty.wait(lock);
	}
}

void MySqlDaemon::pendForSaving(boost::shared_ptr<const MySqlObjectBase> object){
	const boost::mutex::scoped_lock lock(g_mutex);
	if(g_savePool.empty()){
		g_savePool.push_front(AsyncSaveItem());
	}
	g_saveQueue.splice(g_saveQueue.end(), g_savePool, g_savePool.begin());

	AUTO_REF(asi, g_saveQueue.back());
	asi.object.swap(object);
	asi.timeStamp = getMonoClock() + g_databaseSaveDelay * 1000;
	atomicStore(asi.object->m_context, &asi);

	g_newObjectAvail.notify_all();
}
void MySqlDaemon::pendForLoading(boost::shared_ptr<MySqlObjectBase> object,
	std::string filter, MySqlAsyncLoadCallback callback)
{
	const boost::mutex::scoped_lock lock(g_mutex);
	if(g_loadPool.empty()){
		g_loadPool.push_front(AsyncLoadItem());
	}
	g_loadQueue.splice(g_loadQueue.end(), g_loadPool, g_loadPool.begin());

	AUTO_REF(ali, g_loadQueue.back());
	ali.object.swap(object);
	ali.filter.swap(filter);
	ali.callback.swap(callback);

	g_newObjectAvail.notify_all();
}
