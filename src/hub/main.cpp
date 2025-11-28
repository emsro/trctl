#include "../server.hpp"
#include "../task.hpp"
#include "iface.hpp"

#include <CLI/CLI.hpp>


namespace trctl
{

struct unit_repr
{
};


ecor::task< unit_to_hub > transact( task_ctx& ctx, server_client& c, hub_to_unit data )
{
        static constexpr std::size_t mem_size = 1024 * 8;
        uint8_t                      buffer[mem_size];
        circular_buffer_memory       mem{ std::span{ buffer } };

        std::size_t     n = 128;
        uint8_t*        p = (uint8_t*) mem.allocate( n, 1 );
        npb_ostream_ctx octx{ .buff = std::span{ p, n } };
        pb_ostream_t    stream = npb_ostream_from( octx );
        if ( !pb_encode( &stream, hub_to_unit_fields, &data ) ) {
                spdlog::error( "Encoding error: {}", PB_GET_ERROR( &stream ) );
                // XXX: signal error
                co_return {};
        }

        auto res = co_await (
            c.transact( { p, stream.bytes_written } ) | ecor::err_to_val | ecor::as_variant );
        if ( std::get_if< cobs_receiver::err >( &res ) ) {
                spdlog::error( "Transaction error" );
                // XXX: signal error
                co_return {};
        }
        auto&           repl = *std::get_if< cobs_receiver::reply >( &res );
        npb_istream_ctx ictx{ .buff = repl.data, .mem = mem };
        pb_istream_t    istream = npb_istream_from( ictx );
        unit_to_hub     msg;
        if ( !pb_decode( &istream, unit_to_hub_fields, &msg ) ) {
                spdlog::error( "Decoding error: {}", PB_GET_ERROR( &istream ) );
                // XXX: signal error
                co_return {};
        }
        if ( !msg.has_ts ) {
                spdlog::error( "No timestamp in response" );
                // XXX: signal error
                co_return {};
        }
        co_return msg;
}

ecor::task< init_msg > transact_init( task_ctx& ctx, server_client& c )
{
        hub_to_unit msg = hub_to_unit_init_default;
        set_get_init( msg );
        unit_to_hub resp = co_await transact( ctx, c, msg );
        if ( resp.which_sub != unit_to_hub_init_tag ) {
                spdlog::error( "Unexpected response to init" );
                // XXX: signal error
                co_return {};
        }
        co_return resp.sub.init;
}


ecor::task< unit_repr > unit_handle_init( task_ctx& ctx, server_client& c )
{

        unit_repr res{};
        co_return res;
}
}  // namespace trctl

int main( int argc, char** argv )
{
        uv_disable_stdio_inheritance();
        int      port;
        CLI::App app{ "trctl" };

        app.add_option( "-p,--port", port, "Port to listen on" )->default_val( "7000" );

        CLI11_PARSE( app, argc, argv );

        uv_loop_t* loop = uv_default_loop();

        trctl::server server;

        if ( int e = trctl::server_init( server, loop, port ); e ) {
                std::cerr << "Server init failed: " << uv_strerror( e ) << std::endl;
                return 1;
        }

        return uv_run( loop, UV_RUN_DEFAULT );
}