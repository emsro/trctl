
#include "../src/client.hpp"
#include "../src/server.hpp"
#include "../src/util.hpp"
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


void run_loop( uv_loop_t* loop, std::size_t max_iters )
{
        for ( std::size_t i = 0; i < max_iters; ++i ) {
                auto r = uv_run( loop, UV_RUN_NOWAIT );
                EXPECT_GT( r, 0 ) << "Error: " << uv_strerror( r );
        }
}

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

struct nd_mem
{
        void* allocate( std::size_t bytes, std::size_t align )
        {
                return ::operator new( bytes, std::align_val_t( align ) );
        }

        void deallocate( void* p, std::size_t bytes, std::size_t align )
        {
                ::operator delete( p, std::align_val_t( align ) );
        }
};

struct test_ctx
{
        nd_mem               mem;
        ecor::task_allocator alloc{ mem };
        ecor::task_core      core;
};

auto& get_task_alloc( test_ctx& ctx )
{
        return ctx.alloc;
}
auto& get_task_core( test_ctx& ctx )
{
        return ctx.core;
}

void init_both( uv_loop_t* loop, server& s, client& c )
{
        auto ret = server_init( s, loop, 0 );
        EXPECT_EQ( 0, ret ) << "Error: " << uv_strerror( ret );
        ret = uv_run( loop, UV_RUN_NOWAIT );

        auto [ip, port] = get_connection_info( &s, sock_kind::SOCK );
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
                auto e = co_await ( srv.new_event() || srv.disc_event() );
                spdlog::info( "Event received" );
                std::visit( h, e );
        }
}

TEST( server, new_conn )
{
        mem_usage_guard g;
        uv_loop_t*      loop = uv_default_loop();

        server server;
        client client;

        int      new_counter  = 0;
        int      disc_counter = 0;
        test_ctx ctx;
        auto     t1 = count_connections( ctx, server, new_counter, disc_counter );

        init_both( loop, server, client );
        run_loop( loop, 20 );

        uv_close( (uv_handle_t*) &client, nullptr );
        run_loop( loop, 20 );

        uv_stop( loop );
        uv_run( loop, UV_RUN_ONCE );
        uv_loop_close( loop );

        EXPECT_EQ( new_counter, 1 );
        EXPECT_EQ( disc_counter, 1 );
}

ecor::task< void > server_client_coro( test_ctx& ctx, uv_loop_t* loop, server& s, auto&& f )
{
        for ( ;; ) {
                auto evt = co_await ( s.new_event() || s.disc_event() );
                if ( auto* e = std::get_if< server::new_client >( &evt ) )
                        co_await f( ctx, e->client );
                else
                        std::abort();
        }
}

TEST( server, one_interaction )
{
        mem_usage_guard g;
        uv_loop_t*      loop = uv_default_loop();

        server   server;
        client   client;
        test_ctx ctx;
        int      fired_counter = 0;

        auto h1 = server_client_coro(
            ctx,
            loop,
            server,
            [&fired_counter]( test_ctx&, server_client& client ) -> ecor::task< void > {
                    std::array< uint8_t, 4 > buff = { 1, 2, 3, 4 };
                    spdlog::info( "Starting test transaction" );
                    auto res = co_await client.transact( buff );
                    if ( auto* e = std::get_if< cobs_receiver::err >( &res ) )
                            std::abort();
                    auto& prom = std::get< cobs_receiver::reply >( res );

                    std::array< uint8_t, 4 > expected = { 5, 6, 7, 8 };
                    EXPECT_EQ( prom.data, std::span< uint8_t const >{ expected } );
                    ++fired_counter;
            } );

        auto client_coro = [&fired_counter, &client]( test_ctx& ctx ) -> ecor::task< void > {
                std::array< uint8_t, 4 > buff = { 5, 6, 7, 8 };
                auto                     res  = co_await client.incoming();
                if ( auto* e = std::get_if< cobs_receiver::err >( &res ) )
                        std::abort();
                auto&                    prom     = std::get< client::promise >( res );
                std::array< uint8_t, 4 > expected = { 1, 2, 3, 4 };
                EXPECT_EQ( prom.data, std::span< uint8_t const >{ expected } );
                std::ignore = prom.fullfill( buff );
                ++fired_counter;
        };
        auto h2 = client_coro( ctx );

        init_both( loop, server, client );

        run_loop( loop, 50 );

        EXPECT_EQ( fired_counter, 2 );
}


}  // namespace trctl