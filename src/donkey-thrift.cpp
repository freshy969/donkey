#include "donkey-signal.h"
#include <future>
#include <mutex>
#include <boost/log/utility/setup/console.hpp>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/concurrency/ThreadManager.h>
#define DONKEY_USE_POSIX_THREADS 1
#ifndef DONKEY_USE_POSIX_THREADS
#include <thrift/concurrency/StdThreadFactory.h>
#else
#include <thrift/concurrency/PosixThreadFactory.h>
#endif
#include <thrift/server/TThreadPoolServer.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>
#include "donkey.h"
#include "Donkey.h"

namespace donkey {

using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace apache::thrift::concurrency;
using namespace apache::thrift::server;

static int first_start_time = 0;
static int restart_count = 0;
static std::mutex global_mutex;

class DonkeyHandler : virtual public api::DonkeyIf {
    Service *server;
    int last_start_time;

    void protect (function<void()> const &callback) {
        try {
            callback();
        }
        catch (Error const &e) {
            api::DonkeyException ae;
            ae.what = e.code();
            ae.why = e.what();
            throw ae;
        }
        catch (std::exception const &e) {
            api::DonkeyException ae;
            ae.what = ErrorCode_Unknown;
            ae.why = e.what();
            throw ae;
        }
        catch (...) {
            api::DonkeyException ae;
            ae.what = ErrorCode_Unknown;
            throw ae;
        }
    }

 public:
  DonkeyHandler(Service *s): server(s) {
    // Your initialization goes here
        last_start_time = time(NULL);
        std::lock_guard<std::mutex> lock(global_mutex);
        if (restart_count == 0) {
            first_start_time = last_start_time;
        }
        ++restart_count;
  }

  void ping(api::PingResponse& response, const api::PingRequest& request) {
      response.first_start_time = first_start_time;
      response.last_start_time = last_start_time;
      response.restart_count = restart_count;
  }

  void search(api::SearchResponse& response, const api::SearchRequest& request) {
        protect([this, &response, request](){
            SearchRequest req;
            req.db = request.db;
            req.raw = request.raw;
            req.url = request.url;
            req.content = request.content;
            req.type = request.type;
            req.K = request.__isset.K ? request.K : -1;
            req.R = request.__isset.R ? request.R : NAN;
            req.hint_K = request.__isset.hint_K ? request.hint_K: -1;
            req.hint_R = request.__isset.hint_R ? request.hint_R: NAN;

            SearchResponse resp;

            server->search(req, &resp);
            response.time=resp.time;
            response.load_time=resp.load_time;
            response.filter_time=resp.filter_time;
            response.rank_time=resp.rank_time;
            response.hits.clear();
            for (auto const &hit: resp.hits) {
                api::Hit h;
                h.key = hit.key;
                h.meta = hit.meta;
                h.details = hit.details;
                h.score = hit.score;
                response.hits.push_back(h);
            }
        });
  }

  void insert(api::InsertResponse& response, const api::InsertRequest& request) {
      protect([this, &response, request](){
        InsertRequest req;
        req.db = request.db;
        req.key = request.key;
        req.meta = request.meta;
        req.raw = request.raw;
        req.url = request.url;
        req.content = request.content;
        req.type = request.type;

        InsertResponse resp;
        server->insert(req, &resp);
        response.time=resp.time;
        response.load_time=resp.load_time;
        response.journal_time=resp.journal_time;
        response.index_time=resp.index_time;
     });
  }

  void misc(api::MiscResponse& response, const api::MiscRequest& request) {
      protect([this, &response, request](){
        MiscRequest req;
        MiscResponse resp;
        req.method = request.method;
        req.db = request.db;
        server->misc(req, &resp);
        response.code = resp.code;
        response.text = resp.text;
     });
  }
};


bool run_server (Config const &config, Service *svr) {
    LOG(info) << "Starting the server...";
    boost::shared_ptr<TProtocolFactory> apiFactory(new TBinaryProtocolFactory());
    boost::shared_ptr<DonkeyHandler> handler(new DonkeyHandler(svr));
    boost::shared_ptr<TProcessor> processor(new api::DonkeyProcessor(handler));
    boost::shared_ptr<TServerTransport> serverTransport(new TServerSocket(config.get<int>("donkey.thrift.server.port", 50052)));
    boost::shared_ptr<TTransportFactory> transportFactory(new TBufferedTransportFactory());
    boost::shared_ptr<ThreadManager> threadManager = ThreadManager::newSimpleThreadManager(config.get<int>("donkey.thrift.server.threads", 8));
#ifndef DONKEY_USE_POSIX_THREADS
    boost::shared_ptr<StdThreadFactory> threadFactory = boost::shared_ptr<StdThreadFactory>(new StdThreadFactory());
#else
    boost::shared_ptr<PosixThreadFactory> threadFactory = boost::shared_ptr<PosixThreadFactory>(new PosixThreadFactory());
#endif
    threadManager->threadFactory(threadFactory);
    threadManager->start();
    TThreadPoolServer server(processor, serverTransport, transportFactory, apiFactory, threadManager);
    std::future<void> ret = std::async(std::launch::async, [&server](){server.serve();});
    WaitSignal ws;
    int sig = 0;
    ws.wait(&sig);
    server.stop();
    ret.get();
    return sig == SIGHUP;
}


class DonkeyClientImpl: public Service {
    NetworkAddress address;
    mutable boost::shared_ptr<TTransport> socket;
    mutable boost::shared_ptr<TTransport> transport;
    mutable boost::shared_ptr<TProtocol> protocol;
    mutable api::DonkeyClient client;

    void protect (function<void()> const &callback) {
        try {
            callback();
        }
        catch (api::DonkeyException const &ae) {
            throw Error(ae.why, ae.what);
        }
        catch (...) {
            throw Error("unknown error");
        }
    }

    void protect (function<void()> const &callback) const {
        try {
            callback();
        }
        catch (api::DonkeyException const &ae) {
            throw Error(ae.why, ae.what);
        }
        catch (...) {
            throw Error("unknown error");
        }
    }

public:
    DonkeyClientImpl (Config const &config)
        : address(config.get<string>("donkey.thrift.client.server", "127.0.0.1")),
        socket(new TSocket(address.host("127.0.0.1"), address.port(50052))),
        transport(new TBufferedTransport(socket)),
        protocol(new TBinaryProtocol(transport)),
        client(protocol)
    {
        transport->open();
    }

    DonkeyClientImpl (string const &addr)
        : address(addr),
        socket(new TSocket(address.host("127.0.0.1"), address.port(50052))),
        transport(new TBufferedTransport(socket)),
        protocol(new TBinaryProtocol(transport)),
        client(protocol)
    {
        transport->open();
    }

    ~DonkeyClientImpl () {
        transport->close();
    }

    void ping (PingResponse *r) {
        api::PingRequest req;
        api::PingResponse resp;
        client.ping(resp, req);
        r->last_start_time = resp.last_start_time;
        r->first_start_time = resp.first_start_time;
        r->restart_count = resp.restart_count;
    }

    void insert (InsertRequest const &request, InsertResponse *response) {
        protect([this, &response, request](){
            api::InsertRequest req;
            api::InsertResponse resp;
            req.db = request.db;
            req.raw = request.raw;
            req.url = request.url;
            req.content = request.content;
            req.type = request.type;
            req.key = request.key;
            req.meta = request.meta;
            client.insert(resp, req);
            response->time = resp.time;
            response->load_time = resp.load_time;
            response->journal_time = resp.journal_time;
            response->index_time = resp.index_time;
        });
    }

    void search (SearchRequest const &request, SearchResponse *response) {
        protect([this, &response, request](){
            api::SearchRequest req;
            api::SearchResponse resp;
            req.db = request.db;
            req.raw = request.raw;
            req.url = request.url;
            req.content = request.content;
            req.type = request.type;
            req.__set_K(request.K);
            req.__set_R(request.R);
            req.__set_hint_K(request.hint_K);
            req.__set_hint_R(request.hint_R);
            client.search(resp, req);
            response->time = resp.time;
            response->load_time = resp.load_time;
            response->filter_time = resp.filter_time;
            response->rank_time = resp.rank_time;
            response->hits.clear();
            for (auto const &h: resp.hits) {
                Hit hit;
                hit.key = h.key;
                hit.meta = h.meta;
                hit.score = h.score;
                hit.details = h.details;
                response->hits.push_back(hit);
            }
        });
    }

    void misc (MiscRequest const &request, MiscResponse *response) {
        protect([this, &response, request](){
            api::MiscRequest req;
            api::MiscResponse resp;
            req.method = request.method;
            req.db = request.db;
            client.misc(resp, req);
            response->code = resp.code;
            response->text = resp.text;
            });
    }
};

Service *make_client (Config const &config) {
    return new DonkeyClientImpl(config);
}

Service *make_client (string const &address) {
    return new DonkeyClientImpl(address);
}


}
