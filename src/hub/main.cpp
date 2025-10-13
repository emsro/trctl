#include "../server.hpp"

#include <CLI/CLI.hpp>
#include <pb_decode.h>
#include <pb_encode.h>

namespace trctl
{

struct unit_repr
{
};

struct task_ctx
{
        ecor::task_core         core;
        circular_buffer_memory& mem;
        ecor::task_allocator    alloc{ mem };
};

ecor::task_allocator& get_task_alloc( task_ctx& ctx )
{
        return ctx.alloc;
}

ecor::task_core& get_task_core( task_ctx& ctx )
{
        return ctx.core;
}


ecor::task< unit_to_hub > transact( task_ctx& ctx, server_client& c, hub_to_unit data )
{
        auto buff = make_unique_buffer( ctx.mem, 512 );

        pb_ostream_t stream = pb_ostream_from_buffer( buff.ptr(), buff.size() );
        if ( !pb_encode( &stream, hub_to_unit_fields, &data ) ) {
                spdlog::error( "Encoding error: {}", PB_GET_ERROR( &stream ) );
                // XXX: signal error
                co_return {};
        }

        auto res = co_await c.transact( { buff.ptr(), stream.bytes_written } );
        if ( auto* e = std::get_if< cobs_receiver::err >( &res ) ) {
                spdlog::error( "Transaction error" );
                // XXX: signal error
                co_return {};
        }
        auto&        repl    = *std::get_if< cobs_receiver::reply >( &res );
        pb_istream_t rstream = pb_istream_from_buffer( repl.data.data(), repl.data.size() );
        unit_to_hub  msg;
        if ( !pb_decode( &rstream, unit_to_hub_fields, &msg ) ) {
                spdlog::error( "Decoding error: {}", PB_GET_ERROR( &rstream ) );
                // XXX: signal error
                co_return {};
        }
        co_return msg;
}

ecor::task< unique_ptr< unit_repr > > unit_handle_init( task_ctx& ctx, server_client& c )
{
        unique_ptr< unit_repr > res{
            ctx.mem.construct< unit_repr >(), deleter< unit_repr >{ &ctx.mem } };

        co_return res;
}
}  // namespace trctl

int main( int argc, char** argv )
{
        int      port;
        CLI::App app{ "trctl" };

        app.add_option( "-p,--port", port, "Port to listen on" )->default_val( "7000" );


        uv_loop_t* loop = uv_default_loop();

        trctl::server server;

        if ( int e = trctl::server_init( server, loop, port ); e ) {
                std::cerr << "Server init failed: " << uv_strerror( e ) << std::endl;
                return 1;
        }

        return uv_run( loop, UV_RUN_DEFAULT );
}