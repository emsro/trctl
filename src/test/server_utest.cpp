
#include "../src/client.hpp"
#include "../src/server.hpp"
#include "../src/util.hpp"
#include "./tutil.hpp"
#include "ecor/ecor.hpp"
#include "uv.h"

#include <array>
#include <cstddef>
#include <gtest/gtest.h>

namespace std
{
bool operator==( std::span< uint8_t const > lhs, std::span< uint8_t const > rhs )
{
        if ( lhs.size() != rhs.size() )
                return false;
        for ( size_t i = 0; i < lhs.size(); ++i )
                if ( lhs[i] != rhs[i] )
                        return false;
        return true;
}
}  // namespace std

namespace trctl
{


TEST( server, init )
{
        spdlog::info( "pong" );
        mem_usage_guard g;

        uv_loop_t* loop = uv_default_loop();

        server server;

        auto ret = server_init( server, loop, 0 );
        EXPECT_EQ( 0, ret ) << "Error: " << uv_strerror( ret );

        run_loop( loop, 10 );

        uv_stop( loop );
        ret = uv_run( loop, UV_RUN_ONCE );
        uv_loop_close( loop );
}


void init_both( uv_loop_t* loop, server& s, client& c )
{
        auto ret = server_init( s, loop, 0 );
        EXPECT_EQ( 0, ret ) << "Error: " << uv_strerror( ret );
        ret = uv_run( loop, UV_RUN_NOWAIT );

        auto [ip, port] = get_connection_info( &s.tcp, sock_kind::SOCK );
        spdlog::info( "Server listening on {}:{}", ip, port );
        ret = client_init( c, loop, "0.0.0.0", port );
        EXPECT_EQ( 0, ret ) << "Error: " << uv_strerror( ret );
}

ecor::task< void >
count_connections( test_ctx& ctx, server& srv, int& new_counter, int& disc_counter )
{
        struct handler
        {
                int& new_cnt;
                int& disc_cnt;
                void operator()( server::new_client const& )
                {
                        ++new_cnt;
                }
                void operator()( server::client_disconnected const& )
                {
                        ++disc_cnt;
                }
        };

        handler h{ new_counter, disc_counter };

        for ( ;; ) {
                spdlog::info( "Waiting for server event" );
                auto e = co_await ( ( srv.new_event() || srv.disc_event() ) | ecor::as_variant );
                spdlog::info( "Event received" );
                std::visit( h, e );
        }
}

TEST( server, new_conn )
{
        mem_usage_guard g;

        server server;
        client client;

        int      new_counter  = 0;
        int      disc_counter = 0;
        test_ctx ctx;
        auto     t1 = count_connections( ctx, server, new_counter, disc_counter )
                      .connect( ecor::_dummy_receiver{} );
        t1.start();

        init_both( ctx.loop, server, client );
        run_loop( ctx.loop, 20 );

        uv_close( (uv_handle_t*) &client, nullptr );
        run_loop( ctx.loop, 20 );

        // Close the server handle
        uv_close( (uv_handle_t*) &server.tcp, nullptr );
        run_loop( ctx.loop, 20 );

        EXPECT_EQ( new_counter, 1 );
        EXPECT_EQ( disc_counter, 1 );
}

TEST( server, one_interaction )
{
        mem_usage_guard g;

        server server;
        client client;

        int fired_counter = 0;


        test_ctx ctx;
        init_both( ctx.loop, server, client );

        auto server_client_coro = [&]( test_ctx& ctx ) -> ecor::task< void > {
                for ( ;; ) {
                        auto evt = co_await (
                            ( server.new_event() || server.disc_event() ) | ecor::as_variant );
                        spdlog::info( "Got event" );
                        auto* e = std::get_if< server::new_client >( &evt );
                        if ( !e )
                                std::abort();
                        std::array< uint8_t, 4 > buff = { 1, 2, 3, 4 };
                        spdlog::info( "Starting test transaction" );
                        auto res = co_await (
                            e->client.transact( buff ) | ecor::err_to_val | ecor::as_variant );
                        if ( std::get_if< cobs_receiver::err >( &res ) )
                                std::abort();
                        auto& prom = std::get< cobs_receiver::reply >( res );

                        std::array< uint8_t, 4 > expected = { 5, 6, 7, 8 };
                        EXPECT_EQ( prom.data, std::span< uint8_t const >{ expected } );
                        ++fired_counter;
                }
        };
        auto h1 = server_client_coro( ctx ).connect( ecor::_dummy_receiver{} );
        h1.start();

        auto client_coro = [&fired_counter, &client]( test_ctx& ctx ) -> ecor::task< void > {
                std::array< uint8_t, 4 > buff = { 5, 6, 7, 8 };
                auto res = co_await ( client.incoming() | ecor::err_to_val | ecor::as_variant );
                spdlog::info( "Got incoming data" );
                if ( std::get_if< cobs_receiver::err >( &res ) )
                        std::abort();
                auto&                    prom     = std::get< client::promise >( res );
                std::array< uint8_t, 4 > expected = { 1, 2, 3, 4 };
                EXPECT_EQ( prom.data, std::span< uint8_t const >{ expected } );
                std::ignore = prom.fullfill( buff );
                ++fired_counter;
        };
        auto h2 = client_coro( ctx ).connect( ecor::_dummy_receiver{} );
        h2.start();


        run_loop( ctx.loop, 50 );

        EXPECT_EQ( fired_counter, 2 );
}


}  // namespace trctl