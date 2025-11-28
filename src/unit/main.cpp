
#include "unit.hpp"

#include <CLI/CLI.hpp>
#include <list>


int main( int argc, char** argv )
{
        uv_disable_stdio_inheritance();
        int                   port;
        std::string           address;
        std::filesystem::path workdir;
        CLI::App              app{ "trctl" };

        app.add_option( "-p,--port", port, "Port to listen on" )->default_val( "7000" );
        app.add_option( "-a,--address", address, "Address to connect to" )
            ->default_val( "127.0.0.1" );
        app.add_option( "-w,--workdir", workdir, "Client working directory" )
            ->default_val( "./_work" )
            ->check( CLI::ExistingDirectory );

        CLI11_PARSE( app, argc, argv );

        uv_loop_t* loop = uv_default_loop();

        trctl::task_core tcore{ loop };
        trctl::unit_ctx  uctx{ loop, workdir, tcore };

        uint8_t         buffer[1024 * 1024] = {};
        trctl::task_ctx tctx{ loop, tcore, std::span{ buffer } };

        // XXX: search and replace dummy receiver with custom erroring impl
        auto start_op =
            trctl::unit_ctx_loop( tctx, uctx, address, port ).connect( ecor::_dummy_receiver{} );
        start_op.start();

        return uv_run( loop, UV_RUN_DEFAULT );
}