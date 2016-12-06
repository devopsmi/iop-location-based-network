#include <thread>

#include <asio.hpp>
#include <catch.hpp>
#include <easylogging++.h>

#include "network.hpp"
#include "testimpls.hpp"
#include "testdata.hpp"

using namespace std;
using namespace LocNet;
using namespace asio::ip;



unique_ptr<std::thread> StartServerThread()
{
    const NetworkInterface &BudapestNodeContact(
        TestData::NodeBudapest.profile().contact() );
    TcpNetwork network(BudapestNodeContact);
    
    bool initialized = false;
    thread *serverThread = new thread( [&network, &initialized]()
    {
        try {
            shared_ptr<ISpatialDatabase> geodb( new InMemorySpatialDatabase(TestData::Budapest) );
            geodb->Store(TestData::EntryKecskemet);
            geodb->Store(TestData::EntryLondon);
            geodb->Store(TestData::EntryNewYork);
            geodb->Store(TestData::EntryWien);
            geodb->Store(TestData::EntryCapeTown);

            shared_ptr<INodeConnectionFactory> connectionFactory(
                new DummyNodeConnectionFactory() );
            Node node( TestData::NodeBudapest, geodb, connectionFactory );
            IncomingRequestDispatcher dispatcher(node);
            
            initialized = true;
            ProtoBufSyncTcpSession serverSession(network);
            while (true)
            {
                shared_ptr<iop::locnet::MessageWithHeader> requestMsg( serverSession.ReceiveMessage() );

                unique_ptr<iop::locnet::Response> response( dispatcher.Dispatch( requestMsg->body().request() ) );
                
                iop::locnet::MessageWithHeader responseMsg;
                responseMsg.mutable_body()->set_allocated_response( response.release() );
                serverSession.SendMessage(responseMsg);
            }
        }
        catch (exception &e) {
            // LOG(ERROR) << "Server thread failed: " << e.what();
        }
    } );
    
    while (! initialized)
        { this_thread::sleep_for( chrono::milliseconds(1) ); }
    
    return unique_ptr<thread>(serverThread);
}



SCENARIO("TCP networking", "[network]")
{
    GIVEN("A configured Node and Tcp networking")
    {
        unique_ptr<thread> serverThread( StartServerThread() );
        
        THEN("It serves clients via sync TCP")
        {
            const NetworkInterface &BudapestNodeContact(
                TestData::NodeBudapest.profile().contact() );
            shared_ptr<IProtoBufNetworkSession> clientSession(
                new ProtoBufSyncTcpSession(BudapestNodeContact) );
            
            {
                iop::locnet::MessageWithHeader requestMsg;
                requestMsg.mutable_body()->mutable_request()->mutable_localservice()->mutable_getneighbournodes();
                requestMsg.mutable_body()->mutable_request()->set_version("1");
                clientSession->SendMessage(requestMsg);
                
                unique_ptr<iop::locnet::MessageWithHeader> msgReceived( clientSession->ReceiveMessage() );
                
                const iop::locnet::GetNeighbourNodesByDistanceResponse &response =
                    msgReceived->body().response().localservice().getneighbournodes();
                REQUIRE( response.nodes_size() == 2 );
                REQUIRE( Converter::FromProtoBuf( response.nodes(0) ) == TestData::NodeKecskemet );
                REQUIRE( Converter::FromProtoBuf( response.nodes(1) ) == TestData::NodeWien );
            }
            {
                iop::locnet::MessageWithHeader requestMsg;
                requestMsg.mutable_body()->mutable_request()->mutable_remotenode()->mutable_getnodecount();
                requestMsg.mutable_body()->mutable_request()->set_version("1");
                clientSession->SendMessage(requestMsg);
                
                unique_ptr<iop::locnet::MessageWithHeader> msgReceived( clientSession->ReceiveMessage() );
                
                const iop::locnet::GetNodeCountResponse &response =
                    msgReceived->body().response().remotenode().getnodecount();
                REQUIRE( response.nodecount() == 5 );
            }
            
            clientSession->Close();
        }
        
        serverThread->join();
    }
}



SCENARIO("Transparent remote node client", "[network]") {
    GIVEN("A configured Node and Tcp networking")
    {
        unique_ptr<thread> serverThread( StartServerThread() );
        
        THEN("It serves transparent clients using ProtoBuf/TCP protocol")
        {
            const NetworkInterface &BudapestNodeContact(
                TestData::NodeBudapest.profile().contact() );
            shared_ptr<IProtoBufNetworkSession> clientSession(
                new ProtoBufSyncTcpSession(BudapestNodeContact) );
            
            shared_ptr<IProtoBufRequestDispatcher> netDispatcher(
                new ProtoBufRequestNetworkDispatcher(clientSession) );
            NodeMethodsProtoBufClient client(netDispatcher);
            
            size_t nodeCount = client.GetNodeCount();
            REQUIRE( nodeCount == 5 );
        }
        
        serverThread->join();
    }
}
