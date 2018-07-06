///
/// Copyright 2015-2018 Oliver Giles
///
/// This file is part of Laminar
///
/// Laminar is free software: you can redistribute it and/or modify
/// it under the terms of the GNU General Public License as published by
/// the Free Software Foundation, either version 3 of the License, or
/// (at your option) any later version.
///
/// Laminar is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU General Public License for more details.
///
/// You should have received a copy of the GNU General Public License
/// along with Laminar.  If not, see <http://www.gnu.org/licenses/>
///
#include "server.h"
#include "interface.h"
#include "laminar.capnp.h"
#include "resources.h"
#include "log.h"

#include <capnp/ez-rpc.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.capnp.h>
#include <kj/async-io.h>
#include <kj/threadlocal.h>

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signal.h>
#include <sys/signalfd.h>

#include <rapidjson/document.h>

// Size of buffer used to read from file descriptors. Should be
// a multiple of sizeof(struct signalfd_siginfo) == 128
#define PROC_IO_BUFSIZE 4096

namespace {

// Used for returning run state to RPC clients
LaminarCi::JobResult fromRunState(RunState state) {
    switch(state) {
    case RunState::SUCCESS: return LaminarCi::JobResult::SUCCESS;
    case RunState::FAILED:  return LaminarCi::JobResult::FAILED;
    case RunState::ABORTED: return LaminarCi::JobResult::ABORTED;
    default:
        return LaminarCi::JobResult::UNKNOWN;
    }
}

}

// This is the implementation of the Laminar Cap'n Proto RPC interface.
// As such, it implements the pure virtual interface generated from
// laminar.capnp with calls to the LaminarInterface
class RpcImpl : public LaminarCi::Server, public LaminarWaiter {
public:
    RpcImpl(LaminarInterface& l) :
        LaminarCi::Server(),
        laminar(l)
    {
        laminar.registerWaiter(this);
    }

    ~RpcImpl() override {
        laminar.deregisterWaiter(this);
    }

    // Queue a job, without waiting for it to start
    kj::Promise<void> queue(QueueContext context) override {
        std::string jobName = context.getParams().getJobName();
        LLOG(INFO, "RPC queue", jobName);
        ParamMap params;
        for(auto p : context.getParams().getParams()) {
            params[p.getName().cStr()] = p.getValue().cStr();
        }
        LaminarCi::MethodResult result = laminar.queueJob(jobName, params)
                ? LaminarCi::MethodResult::SUCCESS
                : LaminarCi::MethodResult::FAILED;
        context.getResults().setResult(result);
        return kj::READY_NOW;
    }

    // Start a job, without waiting for it to finish
    kj::Promise<void> start(StartContext context) override {
        std::string jobName = context.getParams().getJobName();
        LLOG(INFO, "RPC start", jobName);
        ParamMap params;
        for(auto p : context.getParams().getParams()) {
            params[p.getName().cStr()] = p.getValue().cStr();
        }
        std::shared_ptr<Run> run = laminar.queueJob(jobName, params);
        if(Run* r = run.get()) {
            return r->started.promise.then([context,r]() mutable {
                context.getResults().setResult(LaminarCi::MethodResult::SUCCESS);
                context.getResults().setBuildNum(r->build);
            });
        } else {
            context.getResults().setResult(LaminarCi::MethodResult::FAILED);
            return kj::READY_NOW;
        }
    }

    // Start a job and wait for the result
    kj::Promise<void> run(RunContext context) override {
        std::string jobName = context.getParams().getJobName();
        LLOG(INFO, "RPC run", jobName);
        ParamMap params;
        for(auto p : context.getParams().getParams()) {
            params[p.getName().cStr()] = p.getValue().cStr();
        }
        std::shared_ptr<Run> run = laminar.queueJob(jobName, params);
        if(const Run* r = run.get()) {
            runWaiters[r].emplace_back(kj::newPromiseAndFulfiller<RunState>());
            return runWaiters[r].back().promise.then([context,run](RunState state) mutable {
                context.getResults().setResult(fromRunState(state));
                context.getResults().setBuildNum(run->build);
            });
        } else {
            context.getResults().setResult(LaminarCi::JobResult::UNKNOWN);
            return kj::READY_NOW;
        }
    }

    // Set a parameter on a running build
    kj::Promise<void> set(SetContext context) override {
        std::string jobName = context.getParams().getJobName();
        uint buildNum = context.getParams().getBuildNum();
        LLOG(INFO, "RPC set", jobName, buildNum);

        LaminarCi::MethodResult result = laminar.setParam(jobName, buildNum,
            context.getParams().getParam().getName(), context.getParams().getParam().getValue())
                ? LaminarCi::MethodResult::SUCCESS
                : LaminarCi::MethodResult::FAILED;
        context.getResults().setResult(result);
        return kj::READY_NOW;
    }

    // Take a named lock
    kj::Promise<void> lock(LockContext context) override {
        std::string lockName = context.getParams().getLockName();
        LLOG(INFO, "RPC lock", lockName);
        auto& lockList = locks[lockName];
        lockList.emplace_back(kj::newPromiseAndFulfiller<void>());
        if(lockList.size() == 1)
            lockList.front().fulfiller->fulfill();
        return std::move(lockList.back().promise);
    }

    // Release a named lock
    kj::Promise<void> release(ReleaseContext context) override {
        std::string lockName = context.getParams().getLockName();
        LLOG(INFO, "RPC release", lockName);
        auto& lockList = locks[lockName];
        if(lockList.size() == 0) {
            LLOG(INFO, "Attempt to release unheld lock", lockName);
            return kj::READY_NOW;
        }
        lockList.erase(lockList.begin());
        if(lockList.size() > 0)
            lockList.front().fulfiller->fulfill();
        return kj::READY_NOW;
    }
private:
    // Implements LaminarWaiter::complete
    void complete(const Run* r) override {
        for(kj::PromiseFulfillerPair<RunState>& w : runWaiters[r])
            w.fulfiller->fulfill(RunState(r->result));
        runWaiters.erase(r);
    }
private:
    LaminarInterface& laminar;
    std::unordered_map<std::string, std::list<kj::PromiseFulfillerPair<void>>> locks;
    std::unordered_map<const Run*, std::list<kj::PromiseFulfillerPair<RunState>>> runWaiters;
};

// This is the implementation of the HTTP/Websocket interface. It creates
// websocket connections as LaminarClients and registers them with the
// LaminarInterface so that status messages will be delivered to the client.
// On opening a websocket connection, it delivers a status snapshot message
// (see LaminarInterface::sendStatus)
class HttpImpl : public kj::HttpService {
public:
    HttpImpl(LaminarInterface& laminar, kj::HttpHeaderTable&tbl) :
        laminar(laminar),
        responseHeaders(tbl)
    {}
    virtual ~HttpImpl() {}

private:
    // Implements LaminarClient and holds the Websocket connection object.
    // Automatically destructed when the promise created in request() resolves
    // or is cancelled
    class WebsocketClient : public LaminarClient {
    public:
        WebsocketClient(LaminarInterface& laminar, kj::Own<kj::WebSocket>&& ws) :
            laminar(laminar),
            ws(kj::mv(ws))
        {}
        ~WebsocketClient() override {
            laminar.deregisterClient(this);
        }
        virtual void sendMessage(std::string payload) override {
            messages.push_back(kj::mv(payload));
            // sendMessage might be called several times before the event loop
            // gets a chance to act on the fulfiller. So store the payload here
            // where it can be fetched later and don't pass the payload with the
            // fulfiller because subsequent calls to fulfill() are ignored.
            fulfiller->fulfill();
        }
        LaminarInterface& laminar;
        kj::Own<kj::WebSocket> ws;
        std::list<std::string> messages;
        kj::Own<kj::PromiseFulfiller<void>> fulfiller;
    };

    kj::Promise<void> websocketRead(WebsocketClient& lc)
    {
        return lc.ws->receive().then([&lc,this](kj::WebSocket::Message&& message) {
            KJ_SWITCH_ONEOF(message) {
                KJ_CASE_ONEOF(str, kj::String) {
                    rapidjson::Document d;
                    d.ParseInsitu(const_cast<char*>(str.cStr()));
                    if(d.HasMember("page") && d["page"].IsInt()) {
                        int page = d["page"].GetInt();
                        lc.scope.page = page;
                        laminar.sendStatus(&lc);
                        return websocketRead(lc);
                    }
                }
                KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
                    // clean socket shutdown
                    return lc.ws->close(close.code, close.reason);
                }
                KJ_CASE_ONEOF_DEFAULT {}
            }
            // unhandled/unknown message
            return lc.ws->disconnect();
        });
    }

    kj::Promise<void> websocketWrite(WebsocketClient& lc)
    {
        auto paf = kj::newPromiseAndFulfiller<void>();
        lc.fulfiller = kj::mv(paf.fulfiller);
        return paf.promise.then([this,&lc]{
            kj::Promise<void> p = kj::READY_NOW;
            std::list<std::string> messages = kj::mv(lc.messages);
            for(std::string& s : messages) {
                p = p.then([&s,&lc]{
                    kj::String str = kj::str(s);
                    return lc.ws->send(str).attach(kj::mv(str));
                });
            }
            return p.attach(kj::mv(messages)).then([this,&lc]{
                return websocketWrite(lc);
            });
        });
    }

    kj::Promise<void> websocketUpgraded(WebsocketClient& lc, std::string resource) {
        // convert the requested URL to a MonitorScope
        if(resource.substr(0, 5) == "/jobs") {
            if(resource.length() == 5) {
                lc.scope.type = MonitorScope::ALL;
            } else {
                resource = resource.substr(5);
                size_t split = resource.find('/',1);
                std::string job = resource.substr(1,split-1);
                if(!job.empty()) {
                    lc.scope.job = job;
                    lc.scope.type = MonitorScope::JOB;
                }
                if(split != std::string::npos) {
                    size_t split2 = resource.find('/', split+1);
                    std::string run = resource.substr(split+1, split2-split);
                    if(!run.empty()) {
                        lc.scope.num = static_cast<uint>(atoi(run.c_str()));
                        lc.scope.type = MonitorScope::RUN;
                    }
                    if(split2 != std::string::npos && resource.compare(split2, 4, "/log") == 0) {
                        lc.scope.type = MonitorScope::LOG;
                    }
                }
            }
        }
        laminar.registerClient(&lc);
        kj::Promise<void> connection = websocketRead(lc).exclusiveJoin(websocketWrite(lc));
        // registerClient can happen after a successful websocket handshake.
        // However, the connection might not be closed gracefully, so the
        // corresponding deregister operation happens in the WebsocketClient
        // destructor rather than the close handler or a then() clause
        laminar.sendStatus(&lc);
        return connection;
    }

    virtual kj::Promise<void> request(kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
            kj::AsyncInputStream& requestBody, Response& response) override
    {
        std::string resource = url.cStr();
        if(headers.isWebSocket()) {
            responseHeaders.clear();
            kj::Own<WebsocketClient> lc = kj::heap<WebsocketClient>(laminar, response.acceptWebSocket(responseHeaders));
            return websocketUpgraded(*lc, resource).attach(kj::mv(lc));
        } else {
            // handle regular HTTP request
            const char* start, *end, *content_type;
            responseHeaders.clear();
            if(resource.compare(0, strlen("/archive/"), "/archive/") == 0) {
                std::string file(resource.substr(strlen("/archive/")));
                std::string content;
                if(laminar.getArtefact(file, content)) {
                    responseHeaders.add("Content-Transfer-Encoding", "binary");
                    auto stream = response.send(200, "OK", responseHeaders, content.size());
                    return stream->write(content.data(), content.size()).attach(kj::mv(stream));
                }
            } else if(resource.compare("/custom/style.css") == 0) {
                responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, "text/css; charset=utf-8");
                responseHeaders.add("Content-Transfer-Encoding", "binary");
                std::string css = laminar.getCustomCss();
                auto stream = response.send(200, "OK", responseHeaders, css.size());
                return stream->write(css.data(), css.size()).attach(kj::mv(stream));
            } else if(resources.handleRequest(resource, &start, &end, &content_type)) {
                responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, content_type);
                responseHeaders.add("Content-Encoding", "gzip");
                responseHeaders.add("Content-Transfer-Encoding", "binary");
                auto stream = response.send(200, "OK", responseHeaders, end-start);
                return stream->write(start, end-start).attach(kj::mv(stream));
            }
            return response.sendError(404, "Not Found", responseHeaders);
        }
    }

    LaminarInterface& laminar;
    Resources resources;
    kj::HttpHeaders responseHeaders;
};

// Context for an RPC connection
struct RpcConnection {
    RpcConnection(kj::Own<kj::AsyncIoStream>&& stream,
                  capnp::Capability::Client bootstrap,
                  capnp::ReaderOptions readerOpts) :
        stream(kj::mv(stream)),
        network(*this->stream, capnp::rpc::twoparty::Side::SERVER, readerOpts),
        rpcSystem(capnp::makeRpcServer(network, bootstrap))
    {
    }
    kj::Own<kj::AsyncIoStream> stream;
    capnp::TwoPartyVatNetwork network;
    capnp::RpcSystem<capnp::rpc::twoparty::VatId> rpcSystem;
};

Server::Server(LaminarInterface& li, kj::StringPtr rpcBindAddress,
               kj::StringPtr httpBindAddress) :
    rpcInterface(kj::heap<RpcImpl>(li)),
    laminarInterface(li),
    ioContext(kj::setupAsyncIo()),
    headerTable(),
    httpService(kj::heap<HttpImpl>(li, headerTable)),
    httpServer(kj::heap<kj::HttpServer>(ioContext.provider->getTimer(), headerTable, *httpService)),
    listeners(kj::heap<kj::TaskSet>(*this)),
    childTasks(*this),
    httpConnections(*this),
    httpReady(kj::newPromiseAndFulfiller<void>())
{
    // RPC task
    if(rpcBindAddress.startsWith("unix:"))
        unlink(rpcBindAddress.slice(strlen("unix:")).cStr());
    listeners->add(ioContext.provider->getNetwork().parseAddress(rpcBindAddress)
              .then([this](kj::Own<kj::NetworkAddress>&& addr) {
        return acceptRpcClient(addr->listen());
    }));

    // HTTP task
    if(httpBindAddress.startsWith("unix:"))
        unlink(httpBindAddress.slice(strlen("unix:")).cStr());
    listeners->add(ioContext.provider->getNetwork().parseAddress(httpBindAddress)
              .then([this](kj::Own<kj::NetworkAddress>&& addr) {
        // TODO: a better way? Currently used only for testing
        httpReady.fulfiller->fulfill();
        kj::Own<kj::ConnectionReceiver> listener = addr->listen();
        return httpServer->listenHttp(*listener).attach(kj::mv(listener));
    }));

    // handle SIGCHLD
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        int sigchld = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);
        auto event = ioContext.lowLevelProvider->wrapInputFd(sigchld, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
        auto buffer = kj::heapArrayBuilder<char>(PROC_IO_BUFSIZE);
        reapWatch = handleFdRead(event, buffer.asPtr().begin(), [this](const char* buf, size_t){
            const struct signalfd_siginfo* siginfo = reinterpret_cast<const struct signalfd_siginfo*>(buf);
            KJ_ASSERT(siginfo->ssi_signo == SIGCHLD);
            laminarInterface.reapChildren();
        }).attach(std::move(event)).attach(std::move(buffer));
    }

    // handle watched paths
    {
        inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        auto event = ioContext.lowLevelProvider->wrapInputFd(inotify_fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
        auto buffer = kj::heapArrayBuilder<char>(PROC_IO_BUFSIZE);
        pathWatch = handleFdRead(event, buffer.asPtr().begin(), [this](const char*, size_t){
            laminarInterface.notifyConfigChanged();
        }).attach(std::move(event)).attach(std::move(buffer));
    }
}

Server::~Server() {
}

void Server::start() {
    // The eventfd is used to quit the server later since we need to trigger
    // a reaction from the event loop
    efd_quit = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK);
    kj::evalLater([this](){
        static uint64_t _;
        auto wakeEvent = ioContext.lowLevelProvider->wrapInputFd(efd_quit);
        return wakeEvent->read(&_, sizeof(uint64_t)).attach(std::move(wakeEvent));
    }).wait(ioContext.waitScope);
    // Execution arrives here when the eventfd is triggered (in stop())

    // Shutdown sequence:
    // 1. stop accepting new connections
    listeners = nullptr;
    // 2. abort current jobs. Most of the time this isn't necessary since
    // systemd stop or other kill mechanism will send SIGTERM to the whole
    // process group.
    laminarInterface.abortAll();
    // 3. wait for all children to close
    childTasks.onEmpty().wait(ioContext.waitScope);
    // 4. run the loop once more to send any pending output to websocket clients
    ioContext.waitScope.poll();
    // 5. return: websockets will be destructed
}

void Server::stop() {
    // This method is expected to be called in signal context, so an eventfd
    // is used to get the main loop to react. See run()
    eventfd_write(efd_quit, 1);
}

void Server::addDescriptor(int fd, std::function<void(const char*,size_t)> cb) {
    auto event = this->ioContext.lowLevelProvider->wrapInputFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    auto buffer = kj::heapArrayBuilder<char>(PROC_IO_BUFSIZE);
    childTasks.add(handleFdRead(event, buffer.asPtr().begin(), cb).attach(std::move(event)).attach(std::move(buffer)));
}

kj::Promise<void> Server::addTimeout(int seconds, std::function<void ()> cb) {
    return ioContext.lowLevelProvider->getTimer().afterDelay(seconds * kj::SECONDS).then([cb](){
        cb();
    }).eagerlyEvaluate(nullptr);
}

void Server::addWatchPath(const char* dpath) {
    inotify_add_watch(inotify_fd, dpath, IN_ONLYDIR | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE);
}

kj::Promise<void> Server::acceptRpcClient(kj::Own<kj::ConnectionReceiver>&& listener) {
    kj::ConnectionReceiver& cr = *listener.get();
    return cr.accept().then(kj::mvCapture(kj::mv(listener),
        [this](kj::Own<kj::ConnectionReceiver>&& listener, kj::Own<kj::AsyncIoStream>&& connection) {
            auto server = kj::heap<RpcConnection>(kj::mv(connection), rpcInterface, capnp::ReaderOptions());
            childTasks.add(server->network.onDisconnect().attach(kj::mv(server)));
            return acceptRpcClient(kj::mv(listener));
        }));
}

// returns a promise which will read a chunk of data from the file descriptor
// wrapped by stream and invoke the provided callback with the read data.
// Repeats until ::read returns <= 0
kj::Promise<void> Server::handleFdRead(kj::AsyncInputStream* stream, char* buffer, std::function<void(const char*,size_t)> cb) {
    return stream->tryRead(buffer, 1, PROC_IO_BUFSIZE).then([this,stream,buffer,cb](size_t sz) {
        if(sz > 0) {
            cb(buffer, sz);
            return handleFdRead(stream, kj::mv(buffer), cb);
        }
        return kj::Promise<void>(kj::READY_NOW);
    });
}

void Server::taskFailed(kj::Exception &&exception) {
    // An unexpected http connection close can cause an exception, so don't re-throw.
    // TODO: consider re-throwing selected exceptions
    LLOG(INFO, exception);
    //kj::throwFatalException(kj::mv(exception));
}
