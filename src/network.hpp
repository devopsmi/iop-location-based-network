#ifndef __LOCNET_ASIO_NETWORK_H__
#define __LOCNET_ASIO_NETWORK_H__

#include <functional>
#include <memory>
#include <thread>

#define ASIO_STANDALONE
#include "asio.hpp"

#include "messaging.hpp"



namespace LocNet
{



class IoService
{
    static IoService _instance;
    
    asio::io_service         _asioService;
    asio::io_service::strand _fastStrand;
    asio::io_service::strand _slowStrand;
    
protected:
    
    IoService();
    IoService(const IoService &other) = delete;
    IoService& operator=(const IoService &other) = delete;
    
public:
    
    static IoService& Instance();

    void Shutdown();
    
    asio::io_service& AsioService();
    asio::io_service::strand& FastStrand();
    asio::io_service::strand& SlowStrand();
};



// Abstract TCP server that accepts clients asynchronously on a specific port number
// and has a customizable client accept callback to customize concrete provided service.
class TcpServer
{
protected:
    
    asio::ip::tcp::acceptor _acceptor;
    
    virtual void AsyncAcceptHandler( std::shared_ptr<asio::ip::tcp::socket> socket,
                                     const asio::error_code &ec ) = 0;
public:

    TcpServer(TcpPort portNumber);
    virtual ~TcpServer();
};



// Interface of a network connection that allows sending and receiving protobuf messages
// (either request or response).
// TODO this would be more independent if would send/receive byte arrays,
//      but receiving a message we have to be aware of the message header
//      to know how many bytes to read, it cannot be determined in advance.
class INetworkConnection
{
public:
    
    virtual ~INetworkConnection() {}
    
    virtual const SessionId& id() const = 0;
    virtual const Address& remoteAddress() const = 0;
    
    virtual std::unique_ptr<iop::locnet::Message> ReceiveMessage() = 0;
    virtual void SendMessage(std::unique_ptr<iop::locnet::Message> &&message) = 0;
    
// TODO Would be nice and more convenient to implement using these methods,
//      but they do not seem to nicely fit ASIO
//     virtual void KeepAlive() = 0;
//     virtual bool IsAlive() const = 0;
//     virtual void Close() = 0;
};



// TODO should we create an interface for possibly different session implementations?
class ProtoBufNetworkSession
{
    std::shared_ptr<INetworkConnection> _connection;
    
    uint32_t _nextMessageId;
    std::unordered_map< uint32_t, std::promise< std::unique_ptr<iop::locnet::Response> > > _pendingRequests;
    std::mutex _pendingRequestsMutex;
    
public:
    
    ProtoBufNetworkSession(std::shared_ptr<INetworkConnection> connection);
    virtual ~ProtoBufNetworkSession() {}
    
    virtual const SessionId& id() const;
    
    virtual std::future< std::unique_ptr<iop::locnet::Response> > SendRequest(
        std::unique_ptr<iop::locnet::Message> &&requestMessage);
    virtual void ResponseArrived( std::unique_ptr<iop::locnet::Message> &&responseMessage);
};



// Factory interface to create a dispatcher object for a session.
// Implemented specifically for the keepalive feature, otherwise would not be needed.
class IBlockingRequestDispatcherFactory
{
public:
    
    virtual ~IBlockingRequestDispatcherFactory() {}
    
    virtual std::shared_ptr<IBlockingRequestDispatcher> Create(
        std::shared_ptr<ProtoBufNetworkSession> session ) = 0;
};



// Tcp server implementation that serves protobuf requests for accepted clients.
class DispatchingTcpServer : public TcpServer
{
protected:
    
    std::shared_ptr<IBlockingRequestDispatcherFactory> _dispatcherFactory;
    
    void AsyncAcceptHandler( std::shared_ptr<asio::ip::tcp::socket> socket,
                             const asio::error_code &ec ) override;
public:
    
    DispatchingTcpServer( TcpPort portNumber,
        std::shared_ptr<IBlockingRequestDispatcherFactory> dispatcherFactory );
};



// Request dispatcher to serve incoming requests from clients.
// Implemented specifically for the keepalive feature.
class LocalServiceRequestDispatcherFactory : public IBlockingRequestDispatcherFactory
{
    std::shared_ptr<ILocalServiceMethods> _iLocal;
    
public:
    
    LocalServiceRequestDispatcherFactory(std::shared_ptr<ILocalServiceMethods> iLocal);
    
    std::shared_ptr<IBlockingRequestDispatcher> Create(
        std::shared_ptr<ProtoBufNetworkSession> session ) override;
};



// Dispatcher factory that ignores the session and returns a simple dispatcher
class StaticBlockingDispatcherFactory : public IBlockingRequestDispatcherFactory
{
    std::shared_ptr<IBlockingRequestDispatcher> _dispatcher;
    
public:
    
    StaticBlockingDispatcherFactory(std::shared_ptr<IBlockingRequestDispatcher> dispatcher);
    
    std::shared_ptr<IBlockingRequestDispatcher> Create(
        std::shared_ptr<ProtoBufNetworkSession> session ) override;
};



class CombinedBlockingRequestDispatcherFactory : public IBlockingRequestDispatcherFactory
{
    std::shared_ptr<Node> _node;
    
public:
    
    CombinedBlockingRequestDispatcherFactory(std::shared_ptr<Node> node);
    
    std::shared_ptr<IBlockingRequestDispatcher> Create(
        std::shared_ptr<ProtoBufNetworkSession> session ) override;
};


// Network connection that uses a blocking TCP stream for the easiest implementation.
// TODO ideally would use async networking, but it's hard in C++
//      to implement a simple (blocking) interface using async operations.
//      Maybe boost stackful coroutines could be useful here, but we shouldn't depend on boost.
class SyncTcpNetworkConnection : public INetworkConnection
{
    std::shared_ptr<asio::ip::tcp::socket>  _socket;
    SessionId                               _id;
    Address                                 _remoteAddress;
    std::mutex                              _socketWriteMutex;
    uint32_t                                _nextRequestId;
    
    // NOTE notification messages may be sent from different threads, but only the message loop reads them.
    //      This still may be useful for debugging if we have any doubts about this statement being true.
    //std::mutex                              _socketReadMutex;
    
public:

    // Server connection to client with accepted socket
    SyncTcpNetworkConnection(std::shared_ptr<asio::ip::tcp::socket> socket);
    // Client connection to server, endpoint resolution to be done
    SyncTcpNetworkConnection(const NetworkEndpoint &endpoint);
    ~SyncTcpNetworkConnection();

    const SessionId& id() const override;
    const Address& remoteAddress() const override;
    
    std::unique_ptr<iop::locnet::Message> ReceiveMessage() override;
    void SendMessage(std::unique_ptr<iop::locnet::Message> &&message) override;
    
// TODO implement these
//     void KeepAlive() override;
//     bool IsAlive() const override;
//     void Close() override;
};



// A protobuf request dispatcher that delivers requests through a network session
// and reads response messages from it.
class NetworkDispatcher : public IBlockingRequestDispatcher
{
    std::shared_ptr<ProtoBufNetworkSession> _session;
    
public:

    NetworkDispatcher(std::shared_ptr<ProtoBufNetworkSession> session);
    virtual ~NetworkDispatcher() {}
    
    std::unique_ptr<iop::locnet::Response> Dispatch(std::unique_ptr<iop::locnet::Request> &&request) override;
};



// Connection factory that creates a blocking TCP stream to communicate with remote node.
class TcpNodeConnectionFactory : public INodeConnectionFactory
{
    std::function<void(const Address&)> _detectedIpCallback;
    
public:
    
    std::shared_ptr<INodeMethods> ConnectTo(const NetworkEndpoint &address) override;
    
    void detectedIpCallback(std::function<void(const Address&)> detectedIpCallback);
};



// Factory implementation that creates ProtoBufTcpStreamChangeListener objects.
class TcpChangeListenerFactory : public IChangeListenerFactory
{
    std::shared_ptr<ProtoBufNetworkSession> _session;
    
public:
    
    TcpChangeListenerFactory(std::shared_ptr<ProtoBufNetworkSession> session);
    
    std::shared_ptr<IChangeListener> Create(
        std::shared_ptr<ILocalServiceMethods> localService) override;
};



// Listener implementation that translates node notifications to protobuf
// and uses a dispatcher to send them and notify a remote peer.
class TcpChangeListener : public IChangeListener
{
    SessionId                                   _sessionId;
    std::shared_ptr<ILocalServiceMethods>       _localService;
    // std::shared_ptr<IProtoBufRequestDispatcher> _dispatcher;
    std::shared_ptr<ProtoBufNetworkSession>     _session;
    
public:
    
    TcpChangeListener(
        std::shared_ptr<ProtoBufNetworkSession> session,
        std::shared_ptr<ILocalServiceMethods> localService );
        // std::shared_ptr<IProtoBufRequestDispatcher> dispatcher );
    ~TcpChangeListener();
    
    void Deregister();
    
    const SessionId& sessionId() const override;
    
    void OnRegistered() override;
    void AddedNode  (const NodeDbEntry &node) override;
    void UpdatedNode(const NodeDbEntry &node) override;
    void RemovedNode(const NodeDbEntry &node) override;
};



} // namespace LocNet


#endif // __LOCNET_ASIO_NETWORK_H__