
#include "../client.hpp"

#include <CLI/CLI.hpp>


int main( int argc, char** argv )
{
        int         port;
        std::string address;
        CLI::App    app{ "trctl" };

        app.add_option( "-p,--port", port, "Port to listen on" )->default_val( "7000" );
        app.add_option( "-a,--address", address, "Address to connect to" )
            ->default_val( "127.0.0.1" );


        uv_loop_t* loop = uv_default_loop();

        trctl::client client;

        if ( int e = trctl::client_init( client, loop, address, port ); e ) {
                std::cerr << "Client init failed: " << uv_strerror( e ) << std::endl;
                return 1;
        }

        return uv_run( loop, UV_RUN_DEFAULT );
}