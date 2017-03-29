#include <thread>

#include <asio.hpp>
#include <catch.hpp>

#include "network.hpp"
#include "testimpls.hpp"
#include "testdata.hpp"

#include <easylogging++.h>

using namespace std;
using namespace LocNet;
using namespace asio::ip;



SCENARIO("TCP networking", "[network]")
{
    GIVEN("A configured Node and Tcp networking")
    {
        const NodeContact &BudapestNodeContact( TestData::NodeBudapest.contact() );
        
        shared_ptr<ISpatialDatabase> geodb( new SpatiaLiteDatabase( TestData::NodeBudapest,
            SpatiaLiteDatabase::IN_MEMORY_DB, chrono::hours(1) ) );
        geodb->Store(TestData::EntryKecskemet);
        geodb->Store(TestData::EntryLondon);
        geodb->Store(TestData::EntryNewYork);
        geodb->Store(TestData::EntryWien);
        geodb->Store(TestData::EntryCapeTown);

        shared_ptr<INodeConnectionFactory> connectionFactory( new DummyNodeConnectionFactory() );
        shared_ptr<Node> node( new Node(geodb, connectionFactory) );
        
        shared_ptr<IBlockingRequestDispatcherFactory> dispatcherFactory(
            new CombinedBlockingRequestDispatcherFactory(node) );
        DispatchingTcpServer tcpServer( BudapestNodeContact.nodePort(), dispatcherFactory );
        
        THEN("It serves clients via sync TCP")
        {
            const NodeContact &BudapestNodeContact( TestData::NodeBudapest.contact() );
            shared_ptr<INetworkConnection> clientConnection( new SyncTcpNetworkConnection(
                BudapestNodeContact.nodeEndpoint() ) );
            {
                unique_ptr<iop::locnet::Message> requestMsg( new iop::locnet::Message() );
                requestMsg->mutable_request()->mutable_localservice()->mutable_getneighbournodes();
                requestMsg->mutable_request()->set_version({1,0,0});
                clientConnection->SendMessage( move(requestMsg) );
                
                unique_ptr<iop::locnet::Message> msgReceived( clientConnection->ReceiveMessage() );
                
                const iop::locnet::GetNeighbourNodesByDistanceResponse &response =
                    msgReceived->response().localservice().getneighbournodes();
                REQUIRE( response.nodes_size() == 2 );
                REQUIRE( Converter::FromProtoBuf( response.nodes(0) ) == TestData::NodeKecskemet );
                REQUIRE( Converter::FromProtoBuf( response.nodes(1) ) == TestData::NodeWien );
            }
            {
                unique_ptr<iop::locnet::Message> requestMsg( new iop::locnet::Message() );
                requestMsg->mutable_request()->mutable_remotenode()->mutable_getnodecount();
                requestMsg->mutable_request()->set_version({1,0,0});
                clientConnection->SendMessage( move(requestMsg) );
                
                unique_ptr<iop::locnet::Message> msgReceived( clientConnection->ReceiveMessage() );
                
                const iop::locnet::GetNodeCountResponse &response =
                    msgReceived->response().remotenode().getnodecount();
                REQUIRE( response.nodecount() == 6 );
            }
        }
        
        THEN("It serves transparent clients using ProtoBuf/TCP protocol")
        {
            const NodeContact &BudapestNodeContact( TestData::NodeBudapest.contact() );
            shared_ptr<INetworkConnection> clientConnection( new SyncTcpNetworkConnection(
                BudapestNodeContact.nodeEndpoint() ) );
            shared_ptr<ProtoBufNetworkSession> clientSession(
                new ProtoBufNetworkSession(clientConnection) );
            
            shared_ptr<IBlockingRequestDispatcher> netDispatcher(
                new NetworkDispatcher(clientSession) );
            NodeMethodsProtoBufClient client(netDispatcher, {});
            
            size_t nodeCount = client.GetNodeCount();
            REQUIRE( nodeCount == 6 );
        }
    }
}
