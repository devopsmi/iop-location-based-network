#include <chrono>
#include <functional>

#include <easylogging++.h>

#include "config.hpp"
#include "network.hpp"

using namespace std;
using namespace asio::ip;



namespace LocNet
{


static const size_t ThreadPoolSize = 1;
static const size_t MaxMessageSize = 1024 * 1024;
static const size_t MessageHeaderSize = 5;
static const size_t MessageSizeOffset = 1;


static chrono::duration<uint32_t> GetNormalStreamExpirationPeriod()
    { return Config::Instance().isTestMode() ? chrono::seconds(60) : chrono::seconds(15); }

//static const chrono::duration<uint32_t> KeepAliveStreamExpirationPeriod = chrono::hours(168);


bool NetworkEndpoint::isLoopback() const
{
    try { return address::from_string(_address).is_loopback(); }
    catch (...) { return false; }
}

Address NodeContact::AddressFromBytes(const std::string &bytes)
{
    if ( bytes.empty() )
        { return Address(); }
    else if ( bytes.size() == sizeof(address_v4::bytes_type) )
    {
        address_v4::bytes_type v4AddrBytes;
        copy( &bytes.front(), &bytes.front() + v4AddrBytes.size(), v4AddrBytes.data() );
        address_v4 ipv4(v4AddrBytes);
        return ipv4.to_string();
    }
    else if ( bytes.size() == sizeof(address_v6::bytes_type) )
    {
        address_v6::bytes_type v6AddrBytes;
        copy( &bytes.front(), &bytes.front() + v6AddrBytes.size(), v6AddrBytes.data() );
        address_v6 ipv6(v6AddrBytes);
        return ipv6.to_string();
    }
    else { throw LocationNetworkError(ErrorCode::ERROR_INVALID_DATA, "Invalid ip address bytearray size: " + bytes.size()); }
}


string NodeContact::AddressToBytes(const Address &addr)
{
    string result;
    if ( addr.empty() )
        { return result; }
    
    auto ipAddress( address::from_string(addr) );
    if ( ipAddress.is_v4() )
    {
        auto bytes = ipAddress.to_v4().to_bytes();
        for (uint8_t byte : bytes)
            { result.push_back(byte); }
    }
    else if ( ipAddress.is_v6() )
    {
        auto bytes = ipAddress.to_v6().to_bytes();
        for (uint8_t byte : bytes)
            { result.push_back(byte); }
    }
    else { throw LocationNetworkError(ErrorCode::ERROR_INVALID_DATA, "Unknown type of address: " + addr); }
    return result;
}

string  NodeContact::AddressBytes() const
    { return AddressToBytes(_address); }



TcpServer::TcpServer(TcpPort portNumber) :
    _ioService(), _threadPool(), _shutdownRequested(false),
    _acceptor( _ioService, tcp::endpoint( tcp::v4(), portNumber ) )
{
    // Switch the acceptor to listening state
    LOG(DEBUG) << "Accepting connections on port " << portNumber;
    _acceptor.listen();
    
    shared_ptr<tcp::socket> socket( new tcp::socket(_ioService) );
    _acceptor.async_accept( *socket,
        [this, socket] (const asio::error_code &ec) { AsyncAcceptHandler(socket, ec); } );
    
    // Start the specified number of job processor threads
    for (size_t idx = 0; idx < ThreadPoolSize; ++idx)
    {
        _threadPool.push_back( thread( [this]
        {
            while (! _shutdownRequested)
                { _ioService.run(); }
        } ) );
    }
}


TcpServer::~TcpServer()
{
    Shutdown();
    
    _ioService.stop();
    for (auto &thr : _threadPool)
        { thr.join(); }
    _threadPool.clear();
}


void TcpServer::Shutdown()
{
    _shutdownRequested = true;
}



ProtoBufDispatchingTcpServer::ProtoBufDispatchingTcpServer( TcpPort portNumber,
        shared_ptr<IProtoBufRequestDispatcherFactory> dispatcherFactory ) :
    TcpServer(portNumber), _dispatcherFactory(dispatcherFactory)
{
    if (_dispatcherFactory == nullptr) {
        throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "No dispatcher factory instantiated");
    }
}



void ProtoBufDispatchingTcpServer::AsyncAcceptHandler(
    std::shared_ptr<asio::ip::tcp::socket> socket, const asio::error_code &ec)
{
    if (ec)
    {
        LOG(ERROR) << "Failed to accept connection: " << ec;
        return;
    }
    LOG(DEBUG) << "Connection accepted from "
        << socket->remote_endpoint().address().to_string() << ":" << socket->remote_endpoint().port() << " to "
        << socket->local_endpoint().address().to_string()  << ":" << socket->local_endpoint().port();
    
    // Keep accepting connections on the socket
    shared_ptr<tcp::socket> nextSocket( new tcp::socket(_ioService) );
    _acceptor.async_accept( *nextSocket,
        [this, nextSocket] (const asio::error_code &ec) { AsyncAcceptHandler(nextSocket, ec); } );
    
    // Serve connected client on separate thread
    thread serveSessionThread( [this, socket] // copy by value to keep socket alive
    {
        shared_ptr<INetworkConnection> connection( new SyncTcpStreamNetworkConnection(socket) );
        shared_ptr<ProtoBufNetworkSession> session( new ProtoBufNetworkSession(connection) );
        try
        {
            shared_ptr<IProtoBufRequestDispatcher> dispatcher(
                _dispatcherFactory->Create(session) );

            while (! _shutdownRequested)
            {
                uint32_t messageId = 0;
                unique_ptr<iop::locnet::Message> incomingMsg;
                unique_ptr<iop::locnet::Response> response;
                
                try
                {
                    LOG(TRACE) << "Reading message";
                    incomingMsg = connection->ReceiveMessage();
                    if (! incomingMsg)
                        { throw LocationNetworkError(ErrorCode::ERROR_BAD_REQUEST, "Missing message body"); }
                    
                    // If incoming response, connect it with the sent out request and skip further processing
                    if ( incomingMsg->has_response() )
                    {
                        session->ResponseArrived(*incomingMsg);
                        continue;
                    }
                    
                    if ( ! incomingMsg->has_request() )
                        { throw LocationNetworkError(ErrorCode::ERROR_BAD_REQUEST, "Missing request"); }
                    
                    LOG(TRACE) << "Serving request";
                    messageId = incomingMsg->id();
                    auto request = incomingMsg->mutable_request();
                    
                    // TODO the ip detection and keepalive features are violating the current layers of
                    //      business logic: Node / messaging: Dispatcher / network abstraction: Session.
                    //      This is not a nice implementation, abstractions should be better prepared for these features
                    if ( request->has_remotenode() )
                    {
                        if ( request->remotenode().has_acceptcolleague() ) {
                            request->mutable_remotenode()->mutable_acceptcolleague()->mutable_requestornodeinfo()->mutable_contact()->set_ipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                        else if ( request->remotenode().has_renewcolleague() ) {
                            request->mutable_remotenode()->mutable_renewcolleague()->mutable_requestornodeinfo()->mutable_contact()->set_ipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                        else if ( request->remotenode().has_acceptneighbour() ) {
                            request->mutable_remotenode()->mutable_acceptneighbour()->mutable_requestornodeinfo()->mutable_contact()->set_ipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                        else if ( request->remotenode().has_renewneighbour() ) {
                            request->mutable_remotenode()->mutable_renewneighbour()->mutable_requestornodeinfo()->mutable_contact()->set_ipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                    }
                    
                    response = dispatcher->Dispatch(*request);
                    response->set_status(iop::locnet::Status::STATUS_OK);
                    
                    if ( response->has_remotenode() )
                    {
                        if ( response->remotenode().has_acceptcolleague() ) {
                            response->mutable_remotenode()->mutable_acceptcolleague()->set_remoteipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                        else if ( response->remotenode().has_renewcolleague() ) {
                            response->mutable_remotenode()->mutable_renewcolleague()->set_remoteipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                        else if ( response->remotenode().has_acceptneighbour() ) {
                            response->mutable_remotenode()->mutable_acceptneighbour()->set_remoteipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                        else if ( response->remotenode().has_renewneighbour() ) {
                            response->mutable_remotenode()->mutable_renewneighbour()->set_remoteipaddress(
                                NodeContact::AddressToBytes( connection->remoteAddress() ) );
                        }
                    }
                }
                catch (LocationNetworkError &lnex)
                {
                    // TODO This warning is also given when the connection was simply closed by the remote peer
                    // thus no request can be read. This case should be distinguished and logged with a lower level.
                    LOG(WARNING) << "Failed to serve request with code "
                        << static_cast<uint32_t>( lnex.code() ) << ": " << lnex.what();
                    response.reset( new iop::locnet::Response() );
                    response->set_status( Converter::ToProtoBuf( lnex.code() ) );
                    response->set_details( lnex.what() );
                }
                catch (exception &ex)
                {
                    LOG(WARNING) << "Failed to serve request: " << ex.what();
                    response.reset( new iop::locnet::Response() );
                    response->set_status(iop::locnet::Status::ERROR_INTERNAL);
                    response->set_details( ex.what() );
                }
                
                LOG(TRACE) << "Sending response";
                iop::locnet::Message responseMsg;
                responseMsg.set_allocated_response( response.release() );
                responseMsg.set_id(messageId);
                
                connection->SendMessage(responseMsg);
            }
        }
        catch (exception &ex)
        {
            LOG(WARNING) << "Message dispatch loop failed: " << ex.what();
        }
        
        LOG(INFO) << "Message dispatch loop for session " << session->id() << " finished";
    } );
    
    // Keep thread running independently, don't block io_service here by joining it
    serveSessionThread.detach();
}




SyncTcpStreamNetworkConnection::SyncTcpStreamNetworkConnection(shared_ptr<tcp::socket> socket) :
    _id(), _remoteAddress(), _stream(), _socket(socket)
{
    if (! _socket)
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "No socket instantiated"); }
    
    _remoteAddress = socket->remote_endpoint().address().to_string();
    _id = _remoteAddress + ":" + to_string( socket->remote_endpoint().port() );
    // TODO this is a hack to transform a socket object into a blocking TCP stream,
    //      we should consider possible ownership and other problems of this construct.
    //      We have experienced very rare and undeterministic "Socket operation via non-socket"
    //      system errors, probably this might cause it.
    _stream.rdbuf()->assign( socket->local_endpoint().protocol(), socket->native_handle() );
    // TODO handle session expiration for clients with no keepalive
    //_stream.expires_after(NormalStreamExpirationPeriod);
}


SyncTcpStreamNetworkConnection::SyncTcpStreamNetworkConnection(const NetworkEndpoint &endpoint) :
    _id( endpoint.address() + ":" + to_string( endpoint.port() ) ),
    _remoteAddress( endpoint.address() ), _stream(), _socket()
{
    _stream.expires_after( GetNormalStreamExpirationPeriod() );
    _stream.connect( endpoint.address(), to_string( endpoint.port() ) );
    if (! _stream)
        { throw LocationNetworkError(ErrorCode::ERROR_CONNECTION, "Session failed to connect: " + _stream.error().message() ); }
    LOG(DEBUG) << "Connected to " << endpoint;
}

SyncTcpStreamNetworkConnection::~SyncTcpStreamNetworkConnection()
{
    LOG(DEBUG) << "Connection to " << id() << " closed";
}


const SessionId& SyncTcpStreamNetworkConnection::id() const
    { return _id; }

const Address& SyncTcpStreamNetworkConnection::remoteAddress() const
    { return _remoteAddress; }


uint32_t GetMessageSizeFromHeader(const char *bytes)
{
    // Adapt big endian value from network to local format
    const uint8_t *data = reinterpret_cast<const uint8_t*>(bytes);
    return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}



unique_ptr<iop::locnet::Message> SyncTcpStreamNetworkConnection::ReceiveMessage()
{
    if ( _stream.eof() )
        { throw LocationNetworkError(ErrorCode::ERROR_INVALID_STATE,
            "Connection " + id() + " is already closed, cannot read message"); }
        
    // Allocate a buffer for the message header and read it
    string messageBytes(MessageHeaderSize, 0);
    _stream.read( &messageBytes[0], MessageHeaderSize );
    if ( _stream.fail() )
        { throw LocationNetworkError(ErrorCode::ERROR_PROTOCOL_VIOLATION,
            "Connection " + id() + " failed to read message header, connection may have been closed by remote peer"); }
    
    // Extract message size from the header to know how many bytes to read
    uint32_t bodySize = GetMessageSizeFromHeader( &messageBytes[MessageSizeOffset] );
    
    if (bodySize > MaxMessageSize)
        { throw LocationNetworkError(ErrorCode::ERROR_BAD_REQUEST,
            "Connection " + id() + " message size is over limit: " + to_string(bodySize) ); }
    
    // Extend buffer to fit remaining message size and read it
    messageBytes.resize(MessageHeaderSize + bodySize, 0);
    _stream.read( &messageBytes[0] + MessageHeaderSize, bodySize );
    if ( _stream.fail() )
        { throw LocationNetworkError(ErrorCode::ERROR_PROTOCOL_VIOLATION,
            "Connection " + id() + " failed to read full message body"); }

    // Deserialize message from receive buffer, avoid leaks for failing cases with RAII-based unique_ptr
    unique_ptr<iop::locnet::MessageWithHeader> message( new iop::locnet::MessageWithHeader() );
    message->ParseFromString(messageBytes);
    
    string buffer;
    google::protobuf::TextFormat::PrintToString(*message, &buffer);
    LOG(TRACE) << "Connection " << id() << " received message " << buffer;
    
    return unique_ptr<iop::locnet::Message>( message->release_body() );;
}



void SyncTcpStreamNetworkConnection::SendMessage(iop::locnet::Message& message)
{
    iop::locnet::MessageWithHeader messageWithHeader;
    messageWithHeader.set_allocated_body( new iop::locnet::Message(message) );
    
    messageWithHeader.set_header(1);
    messageWithHeader.set_header( messageWithHeader.ByteSize() - MessageHeaderSize );
    _stream << messageWithHeader.SerializeAsString();
    
    string buffer;
    google::protobuf::TextFormat::PrintToString(message, &buffer);
    LOG(TRACE) << "Connection " << id() << " sent message " << buffer;
}


// void ProtoBufTcpStreamSession::KeepAlive()
// {
//     _stream.expires_after(KeepAliveStreamExpirationPeriod);
// }
// // TODO CHECK This doesn't really seem to work as on "normal" std::streamss
// bool ProtoBufTcpStreamSession::IsAlive() const
//     { return _stream.good(); }
// 
// void ProtoBufTcpStreamSession::Close()
//     { _stream.close(); }



ProtoBufNetworkSession::ProtoBufNetworkSession(shared_ptr<INetworkConnection> connection) :
    _connection(connection), _nextMessageId(1)
{
    if (_connection == nullptr)
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "No connection instantiated"); }
}


const SessionId& ProtoBufNetworkSession::id() const
    { return _connection->id(); }


future<iop::locnet::Response> ProtoBufNetworkSession::SendRequest(iop::locnet::Message& requestMessage)
{
    if (! requestMessage.has_request() )
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "Attempt to send non-request message"); }

    lock_guard<mutex> pendingRequestGuard(_pendingRequestsMutex);
    uint32_t messageId = _nextMessageId++;
    auto emplaceResult = _pendingRequests.emplace(messageId, promise<iop::locnet::Response>());
    if (! emplaceResult.second)
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "Failed to store pending request"); }
    
    _connection->SendMessage(requestMessage);
    
    return emplaceResult.first->second.get_future();
}


void ProtoBufNetworkSession::ResponseArrived(const iop::locnet::Message& responseMessage)
{
    uint32_t messageId = responseMessage.id();
    if (! responseMessage.has_response() )
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "Attempt to receive non-response message"); }
    
    lock_guard<mutex> pendingRequestGuard(_pendingRequestsMutex);
    auto requestIter = _pendingRequests.find(messageId);
    if ( requestIter == _pendingRequests.end() )
        { throw LocationNetworkError(ErrorCode::ERROR_PROTOCOL_VIOLATION, "No request found for message id " + to_string(messageId)); }
    
    requestIter->second.set_value( responseMessage.response() );
    _pendingRequests.erase(requestIter);
}




ProtoBufRequestNetworkDispatcher::ProtoBufRequestNetworkDispatcher(
    shared_ptr<ProtoBufNetworkSession> session) : _session(session) {}

    

unique_ptr<iop::locnet::Response> ProtoBufRequestNetworkDispatcher::Dispatch(
    const iop::locnet::Request& request)
{
    iop::locnet::Request *clonedReq = new iop::locnet::Request(request);
    clonedReq->set_version({1,0,0});
    
    iop::locnet::Message reqMsg;
    reqMsg.set_allocated_request(clonedReq);
    
    std::future<iop::locnet::Response> futureResponse = _session->SendRequest(reqMsg);
    if ( futureResponse.wait_for( GetNormalStreamExpirationPeriod() ) != future_status::ready )
    {
        LOG(WARNING) << "Session " << _session->id() << " received no response, timed out";
        throw LocationNetworkError( ErrorCode::ERROR_BAD_RESPONSE, "Timeout waiting for response of dispatched request" );
    }
    unique_ptr<iop::locnet::Response> result( new iop::locnet::Response( futureResponse.get() ) );
    if ( result && result->status() != iop::locnet::Status::STATUS_OK )
    {
        LOG(WARNING) << "Session " << _session->id() << " received response code " << result->status()
                     << ", error details: " << result->details();
        throw LocationNetworkError( ErrorCode::ERROR_BAD_RESPONSE, result->details() );
    }
    return result;
}



void TcpStreamNodeConnectionFactory::detectedIpCallback(function<void(const Address&)> detectedIpCallback)
{
    _detectedIpCallback = detectedIpCallback;
    LOG(DEBUG) << "Callback for detecting external IP address is set " << static_cast<bool>(_detectedIpCallback); 
}


shared_ptr<INodeMethods> TcpStreamNodeConnectionFactory::ConnectTo(const NetworkEndpoint& endpoint)
{
    LOG(DEBUG) << "Connecting to " << endpoint;
    shared_ptr<INetworkConnection> connection( new SyncTcpStreamNetworkConnection(endpoint) );
    shared_ptr<ProtoBufNetworkSession> session( new ProtoBufNetworkSession(connection) );
    shared_ptr<IProtoBufRequestDispatcher> dispatcher( new ProtoBufRequestNetworkDispatcher(session) );
    shared_ptr<INodeMethods> result( new NodeMethodsProtoBufClient(dispatcher, _detectedIpCallback) );
    return result;
}



LocalServiceRequestDispatcherFactory::LocalServiceRequestDispatcherFactory(
    shared_ptr<ILocalServiceMethods> iLocal) : _iLocal(iLocal) {}


shared_ptr<IProtoBufRequestDispatcher> LocalServiceRequestDispatcherFactory::Create(
    shared_ptr<ProtoBufNetworkSession> session )
{
    shared_ptr<IChangeListenerFactory> listenerFactory(
        new ProtoBufTcpStreamChangeListenerFactory(session) );
    return shared_ptr<IProtoBufRequestDispatcher>(
        new IncomingLocalServiceRequestDispatcher(_iLocal, listenerFactory) );
}



StaticDispatcherFactory::StaticDispatcherFactory(shared_ptr<IProtoBufRequestDispatcher> dispatcher) :
    _dispatcher(dispatcher) {}

shared_ptr<IProtoBufRequestDispatcher> StaticDispatcherFactory::Create(shared_ptr<ProtoBufNetworkSession>)
    { return _dispatcher; }



CombinedRequestDispatcherFactory::CombinedRequestDispatcherFactory(shared_ptr<Node> node) :
    _node(node) {}

shared_ptr<IProtoBufRequestDispatcher> CombinedRequestDispatcherFactory::Create(
    shared_ptr<ProtoBufNetworkSession> session)
{
    shared_ptr<IChangeListenerFactory> listenerFactory(
        new ProtoBufTcpStreamChangeListenerFactory(session) );
    return shared_ptr<IProtoBufRequestDispatcher>(
        new IncomingRequestDispatcher(_node, listenerFactory) );
}




ProtoBufTcpStreamChangeListenerFactory::ProtoBufTcpStreamChangeListenerFactory(
        shared_ptr<ProtoBufNetworkSession> session) :
    _session(session) {}



shared_ptr<IChangeListener> ProtoBufTcpStreamChangeListenerFactory::Create(
    shared_ptr<ILocalServiceMethods> localService)
{
    shared_ptr<IProtoBufRequestDispatcher> dispatcher(
        new ProtoBufRequestNetworkDispatcher(_session) );
    return shared_ptr<IChangeListener>(
        new ProtoBufTcpStreamChangeListener(_session, localService, dispatcher) );
}



ProtoBufTcpStreamChangeListener::ProtoBufTcpStreamChangeListener(
        shared_ptr<ProtoBufNetworkSession> session,
        shared_ptr<ILocalServiceMethods> localService,
        shared_ptr<IProtoBufRequestDispatcher> dispatcher ) :
    _sessionId( session->id() ), _localService(localService), _dispatcher(dispatcher)
{
    //session->KeepAlive();
}


ProtoBufTcpStreamChangeListener::~ProtoBufTcpStreamChangeListener()
{
    Deregister();
    LOG(DEBUG) << "ChangeListener destroyed";
}

void ProtoBufTcpStreamChangeListener::Deregister()
{
    if ( ! _sessionId.empty() )
    {
        LOG(DEBUG) << "ChangeListener deregistering for session " << _sessionId;
        _localService->RemoveListener(_sessionId);
        _sessionId.clear();
    }
}



const SessionId& ProtoBufTcpStreamChangeListener::sessionId() const
    { return _sessionId; }



void ProtoBufTcpStreamChangeListener::AddedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            iop::locnet::Request req;
            iop::locnet::NeighbourhoodChange *change =
                req.mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
            iop::locnet::NodeInfo *info = change->mutable_addednodeinfo();
            Converter::FillProtoBuf(info, node);
            
            unique_ptr<iop::locnet::Response> response( _dispatcher->Dispatch(req) );
            // TODO what to do with response status codes here?
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification";
            Deregister();
        }
    }
}


void ProtoBufTcpStreamChangeListener::UpdatedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            iop::locnet::Request req;
            iop::locnet::NeighbourhoodChange *change =
                req.mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
            iop::locnet::NodeInfo *info = change->mutable_updatednodeinfo();
            Converter::FillProtoBuf(info, node);
            
            unique_ptr<iop::locnet::Response> response( _dispatcher->Dispatch(req) );
            // TODO what to do with response status codes here?
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification";
            Deregister();
        }
    }
}


void ProtoBufTcpStreamChangeListener::RemovedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            iop::locnet::Request req;
            iop::locnet::NeighbourhoodChange *change =
                req.mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
            change->set_removednodeid( node.id() );
            
            unique_ptr<iop::locnet::Response> response( _dispatcher->Dispatch(req) );
            // TODO what to do with response status codes here?
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification";
            Deregister();
        }
    }
}



} // namespace LocNet