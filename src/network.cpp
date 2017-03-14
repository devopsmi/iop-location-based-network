#include <chrono>
#include <functional>

#include "config.hpp"
#include "network.hpp"

// NOTE on Windows this includes <winsock(2).h> so must be after asio includes in "network.hpp"
#include <easylogging++.h>

using namespace std;
using namespace asio::ip;



namespace LocNet
{


static const size_t ThreadPoolSize = 1;
static const size_t MaxMessageSize = 1024 * 1024;
static const size_t MessageHeaderSize = 5;
static const size_t MessageSizeOffset = 1;


static chrono::duration<uint32_t> GetClientExpirationPeriod()
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
    else { throw LocationNetworkError(ErrorCode::ERROR_INVALID_VALUE, "Invalid ip address bytearray size: " + bytes.size()); }
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
    else { throw LocationNetworkError(ErrorCode::ERROR_INVALID_VALUE, "Unknown type of address: " + addr); }
    return result;
}

string  NodeContact::AddressBytes() const
    { return AddressToBytes(_address); }



IoService IoService::_instance;

IoService::IoService(): _serverIoService() {} // , _clientIoService() {}

IoService& IoService::Instance() { return _instance; }

void IoService::Shutdown()
{
    _serverIoService.stop();
//    _clientIoService.stop();
}

asio::io_service& IoService::Server() { return _serverIoService; }
//asio::io_service& IoService::Client() { return _clientIoService; }



TcpServer::TcpServer(TcpPort portNumber) :
    _acceptor( IoService::Instance().Server(), tcp::endpoint( tcp::v4(), portNumber ) )
{
    // Switch the acceptor to listening state
    LOG(DEBUG) << "Accepting connections on port " << portNumber;
    _acceptor.listen();
    
    shared_ptr<tcp::socket> socket( new tcp::socket( IoService::Instance().Server() ) );
    _acceptor.async_accept( *socket,
        [this, socket] (const asio::error_code &ec) { AsyncAcceptHandler(socket, ec); } );
}


TcpServer::~TcpServer() {}



DispatchingTcpServer::DispatchingTcpServer( TcpPort portNumber,
        shared_ptr<IBlockingRequestDispatcherFactory> dispatcherFactory ) :
    TcpServer(portNumber), _dispatcherFactory(dispatcherFactory)
{
    if (_dispatcherFactory == nullptr) {
        throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "No dispatcher factory instantiated");
    }
}



void DispatchingTcpServer::AsyncAcceptHandler(
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
    shared_ptr<tcp::socket> nextSocket( new tcp::socket( IoService::Instance().Server() ) );
    _acceptor.async_accept( *nextSocket,
        [this, nextSocket] (const asio::error_code &ec) { AsyncAcceptHandler(nextSocket, ec); } );
    
    // Serve connected client on separate thread
    thread serveSessionThread( [this, socket] // copy by value to keep socket alive
    {
        shared_ptr<INetworkConnection> connection( new SyncTcpNetworkConnection(socket) );
        shared_ptr<ProtoBufNetworkSession> session( new ProtoBufNetworkSession(connection) );
        try
        {
            shared_ptr<IBlockingRequestDispatcher> dispatcher( _dispatcherFactory->Create(session) );

            bool endMessageLoop = false;
            while ( ! endMessageLoop && ! IoService::Instance().Server().stopped() )
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
                    unique_ptr<iop::locnet::Request> request( incomingMsg->release_request() );
                    
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
                    
                    response = dispatcher->Dispatch( move(request) );
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
                    endMessageLoop = true;
                }
                catch (exception &ex)
                {
                    LOG(WARNING) << "Failed to serve request: " << ex.what();
                    response.reset( new iop::locnet::Response() );
                    response->set_status(iop::locnet::Status::ERROR_INTERNAL);
                    response->set_details( ex.what() );
                    endMessageLoop = true;
                }
                
                LOG(TRACE) << "Sending response";
                unique_ptr<iop::locnet::Message> responseMsg( new iop::locnet::Message() );
                responseMsg->set_allocated_response( response.release() );
                responseMsg->set_id(messageId);
                
                connection->SendMessage( move(responseMsg) );
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




SyncTcpNetworkConnection::SyncTcpNetworkConnection(shared_ptr<tcp::socket> socket) :
    _socket(socket), _id(), _remoteAddress(), _socketWriteMutex(), _nextRequestId(1) // , _socketReadMutex()
{
    if (! _socket)
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "No socket instantiated"); }
    
    _remoteAddress = socket->remote_endpoint().address().to_string();
    _id = _remoteAddress + ":" + to_string( socket->remote_endpoint().port() );
    // TODO handle session expiration for clients with no keepalive
    //_stream.expires_after(NormalStreamExpirationPeriod);
}


SyncTcpNetworkConnection::SyncTcpNetworkConnection(const NetworkEndpoint &endpoint) :
    _socket( new tcp::socket( IoService::Instance().Server() ) ),
    _id( endpoint.address() + ":" + to_string( endpoint.port() ) ),
    _remoteAddress( endpoint.address() ), _socketWriteMutex(), _nextRequestId(1) // , _socketReadMutex()
{
    tcp::resolver resolver( IoService::Instance().Server() );
    tcp::resolver::query query( endpoint.address(), to_string( endpoint.port() ) );
    tcp::resolver::iterator addressIter = resolver.resolve(query);
    try { asio::connect(*_socket, addressIter); }
    catch (exception &ex) { throw LocationNetworkError(ErrorCode::ERROR_CONNECTION, "Failed connecting to " +
        endpoint.address() + ":" + to_string( endpoint.port() ) + " with error: " + ex.what() ); }
    LOG(DEBUG) << "Connected to " << endpoint;
    // TODO handle session expiration
    //_stream.expires_after( GetNormalStreamExpirationPeriod() );
}

SyncTcpNetworkConnection::~SyncTcpNetworkConnection()
{
    LOG(DEBUG) << "Connection to " << id() << " closed";
}


const SessionId& SyncTcpNetworkConnection::id() const
    { return _id; }

const Address& SyncTcpNetworkConnection::remoteAddress() const
    { return _remoteAddress; }


uint32_t GetMessageSizeFromHeader(const char *bytes)
{
    // Adapt big endian value from network to local format
    const uint8_t *data = reinterpret_cast<const uint8_t*>(bytes);
    return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}



unique_ptr<iop::locnet::Message> SyncTcpNetworkConnection::ReceiveMessage()
{
    //lock_guard<mutex> readGuard(_socketReadMutex);
    
    if ( ! _socket->is_open() )
        { throw LocationNetworkError(ErrorCode::ERROR_BAD_STATE,
            "Session " + id() + " connection is already closed, cannot read message"); }
        
    // Allocate a buffer for the message header and read it
    string messageBytes(MessageHeaderSize, 0);
    asio::read( *_socket, asio::buffer(messageBytes) );

    // Extract message size from the header to know how many bytes to read
    uint32_t bodySize = GetMessageSizeFromHeader( &messageBytes[MessageSizeOffset] );
    if (bodySize > MaxMessageSize)
        { throw LocationNetworkError(ErrorCode::ERROR_BAD_REQUEST,
            "Connection " + id() + " message size is over limit: " + to_string(bodySize) ); }
    
    // Extend buffer to fit remaining message size and read it
    messageBytes.resize(MessageHeaderSize + bodySize, 0);
    asio::read( *_socket, asio::buffer(&messageBytes[0] + MessageHeaderSize, bodySize) );

    // Deserialize message from receive buffer, avoid leaks for failing cases with RAII-based unique_ptr
    unique_ptr<iop::locnet::MessageWithHeader> message( new iop::locnet::MessageWithHeader() );
    message->ParseFromString(messageBytes);
    
    string msgDebugStr;
    google::protobuf::TextFormat::PrintToString(*message, &msgDebugStr);
    LOG(TRACE) << "Connection " << id() << " received message " << msgDebugStr;
    
    return unique_ptr<iop::locnet::Message>( message->release_body() );;
}



void SyncTcpNetworkConnection::SendMessage(unique_ptr<iop::locnet::Message> &&messagePtr)
{
    lock_guard<mutex> writeGuard(_socketWriteMutex);

    if (! messagePtr)
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "Got empty message argument to send"); }
    if ( messagePtr->has_request() )
    {
        messagePtr->set_id(_nextRequestId);
        ++_nextRequestId;
    }
    
    iop::locnet::MessageWithHeader message;
    message.set_allocated_body( messagePtr.release() );
    
    message.set_header(1);
    message.set_header( message.ByteSize() - MessageHeaderSize );
    
    asio::write( *_socket, asio::buffer( message.SerializeAsString() ) );
    
    string msgDebugStr;
    google::protobuf::TextFormat::PrintToString(message, &msgDebugStr);
    LOG(TRACE) << "Connection " << id() << " sent message " << msgDebugStr;
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


future<iop::locnet::Response> ProtoBufNetworkSession::SendRequest(
    unique_ptr<iop::locnet::Message> &&requestMessage)
{
    if (! requestMessage->has_request() )
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "Attempt to send non-request message"); }

    lock_guard<mutex> pendingRequestGuard(_pendingRequestsMutex);
    uint32_t messageId = _nextMessageId++;
    auto emplaceResult = _pendingRequests.emplace(messageId, promise<iop::locnet::Response>());
    if (! emplaceResult.second)
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "Failed to store pending request"); }
    
    requestMessage->set_id(messageId);
    _connection->SendMessage( move(requestMessage) );
    
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




NetworkDispatcher::NetworkDispatcher(
    shared_ptr<ProtoBufNetworkSession> session) : _session(session) {}

    

unique_ptr<iop::locnet::Message> RequestToMessage(unique_ptr<iop::locnet::Request> &&request)
{
    request->set_version({1,0,0});
    unique_ptr<iop::locnet::Message> message( new iop::locnet::Message() );
    message->set_allocated_request( request.release() );
    return message;
}


unique_ptr<iop::locnet::Response> NetworkDispatcher::Dispatch(unique_ptr<iop::locnet::Request> &&request)
{
    unique_ptr<iop::locnet::Message> requestMessage( RequestToMessage( move(request) ) );
    std::future<iop::locnet::Response> futureResponse = _session->SendRequest( move(requestMessage) );
    if ( futureResponse.wait_for( GetClientExpirationPeriod() ) != future_status::ready )
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



void TcpNodeConnectionFactory::detectedIpCallback(function<void(const Address&)> detectedIpCallback)
{
    _detectedIpCallback = detectedIpCallback;
    LOG(DEBUG) << "Callback for detecting external IP address is set " << static_cast<bool>(_detectedIpCallback); 
}


shared_ptr<INodeMethods> TcpNodeConnectionFactory::ConnectTo(const NetworkEndpoint& endpoint)
{
    LOG(DEBUG) << "Connecting to " << endpoint;
    shared_ptr<INetworkConnection> connection( new SyncTcpNetworkConnection(endpoint) );
    shared_ptr<ProtoBufNetworkSession> session( new ProtoBufNetworkSession(connection) );
    shared_ptr<IBlockingRequestDispatcher> dispatcher( new NetworkDispatcher(session) );
    shared_ptr<INodeMethods> result( new NodeMethodsProtoBufClient(dispatcher, _detectedIpCallback) );
    return result;
}



LocalServiceRequestDispatcherFactory::LocalServiceRequestDispatcherFactory(
    shared_ptr<ILocalServiceMethods> iLocal) : _iLocal(iLocal) {}


shared_ptr<IBlockingRequestDispatcher> LocalServiceRequestDispatcherFactory::Create(
    shared_ptr<ProtoBufNetworkSession> session )
{
    shared_ptr<IChangeListenerFactory> listenerFactory(
        new TcpChangeListenerFactory(session) );
    return shared_ptr<IBlockingRequestDispatcher>(
        new IncomingLocalServiceRequestDispatcher(_iLocal, listenerFactory) );
}



StaticBlockingDispatcherFactory::StaticBlockingDispatcherFactory(shared_ptr<IBlockingRequestDispatcher> dispatcher) :
    _dispatcher(dispatcher) {}

shared_ptr<IBlockingRequestDispatcher> StaticBlockingDispatcherFactory::Create(shared_ptr<ProtoBufNetworkSession>)
    { return _dispatcher; }



CombinedBlockingRequestDispatcherFactory::CombinedBlockingRequestDispatcherFactory(shared_ptr<Node> node) :
    _node(node) {}

shared_ptr<IBlockingRequestDispatcher> CombinedBlockingRequestDispatcherFactory::Create(
    shared_ptr<ProtoBufNetworkSession> session)
{
    shared_ptr<IChangeListenerFactory> listenerFactory(
        new TcpChangeListenerFactory(session) );
    return shared_ptr<IBlockingRequestDispatcher>(
        new IncomingRequestDispatcher(_node, listenerFactory) );
}




TcpChangeListenerFactory::TcpChangeListenerFactory(shared_ptr<ProtoBufNetworkSession> session) :
    _session(session) {}



shared_ptr<IChangeListener> TcpChangeListenerFactory::Create(
    shared_ptr<ILocalServiceMethods> localService)
{
//     shared_ptr<IProtoBufRequestDispatcher> dispatcher(
//         new ProtoBufRequestNetworkDispatcher(_session) );
//     return shared_ptr<IChangeListener>(
//         new ProtoBufTcpStreamChangeListener(_session, localService, dispatcher) );
    return shared_ptr<IChangeListener>(
        new TcpChangeListener(_session, localService) );
}



TcpChangeListener::TcpChangeListener(
        shared_ptr<ProtoBufNetworkSession> session,
        shared_ptr<ILocalServiceMethods> localService ) :
        // shared_ptr<IProtoBufRequestDispatcher> dispatcher ) :
    _sessionId(), _localService(localService), _session(session) //, _dispatcher(dispatcher)
{
    //session->KeepAlive();
}


TcpChangeListener::~TcpChangeListener()
{
    Deregister();
    LOG(DEBUG) << "ChangeListener for session " << _sessionId << " destroyed";
}

void TcpChangeListener::OnRegistered()
    { _sessionId = _session->id(); }


void TcpChangeListener::Deregister()
{
    // Remove only after successful registration of this instance, needed to protect
    // from deregistering another instance after failed (e.g. repeated) addlistener request for same session
    if ( ! _sessionId.empty() )
    {
        LOG(DEBUG) << "ChangeListener deregistering for session " << _sessionId;
        _localService->RemoveListener(_sessionId);
        _sessionId.clear();
    }
}



const SessionId& TcpChangeListener::sessionId() const
    { return _sessionId; }



void TcpChangeListener::AddedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            // TODO this is a potentially long lasting operation that should not block the same queue
            //      as socket accepts and other fast operations. Thus notifications should be done in
            //      a separate client strand.
            shared_ptr<ProtoBufNetworkSession> session(_session);
            shared_ptr<NodeDbEntry> nodeData( new NodeDbEntry(node) );
            IoService::Instance().Server().post( [session, nodeData]
            {
                unique_ptr<iop::locnet::Request> req( new iop::locnet::Request() );
                iop::locnet::NeighbourhoodChange *change =
                    req->mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
                iop::locnet::NodeInfo *info = change->mutable_addednodeinfo();
                Converter::FillProtoBuf(info, *nodeData);
                
                unique_ptr<iop::locnet::Message> msgToSend( RequestToMessage( move(req) ) );
                session->SendRequest( move(msgToSend) );
            } );
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification: " << ex.what();
            Deregister();
        }
    }
}


void TcpChangeListener::UpdatedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            shared_ptr<ProtoBufNetworkSession> session(_session);
            shared_ptr<NodeDbEntry> nodeData( new NodeDbEntry(node) );
            IoService::Instance().Server().post( [session, nodeData]
            {
                unique_ptr<iop::locnet::Request> req( new iop::locnet::Request() );
                iop::locnet::NeighbourhoodChange *change =
                    req->mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
                iop::locnet::NodeInfo *info = change->mutable_updatednodeinfo();
                Converter::FillProtoBuf(info, *nodeData);
                
                unique_ptr<iop::locnet::Message> msgToSend( RequestToMessage( move(req) ) );
                session->SendRequest( move(msgToSend) );
            } );
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification: " << ex.what();
            Deregister();
        }
    }
}


void TcpChangeListener::RemovedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            shared_ptr<ProtoBufNetworkSession> session(_session);
            NodeId nodeId = node.id();
            IoService::Instance().Server().post( [session, nodeId]
            {
                unique_ptr<iop::locnet::Request> req( new iop::locnet::Request() );
                iop::locnet::NeighbourhoodChange *change =
                    req->mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
                change->set_removednodeid(nodeId);
            
                unique_ptr<iop::locnet::Message> msgToSend( RequestToMessage( move(req) ) );
                session->SendRequest( move(msgToSend) );
            } );
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification: " << ex.what();
            Deregister();
        }
    }
}



} // namespace LocNet