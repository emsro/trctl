#include "client.hpp"

namespace trctl
{
namespace
{
        void client_close( uv_handle_t* handle )
        {
                spdlog::info( "Disconnecting from server" );
                std::abort();
        }

        void client_read( uv_stream_t* cl, ssize_t nread, uv_buf_t const* buf )
        {
                auto& c = (client&) ( *cl );
                if ( nread > 0 )
                        c.recv._handle_rx( { (uint8_t*) buf->base, (std::size_t) nread } );
                if ( nread < 0 ) {
                        if ( nread != UV_EOF )
                                spdlog::error( "Read error {}", uv_err_name( nread ) );
                        uv_close( (uv_handle_t*) cl, client_close );
                }

                free( buf->base );
        }

        void client_alloc( uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf )
        {
                auto& c = (client&) ( *handle );
                // XXX: should use memory pool from client instead
                buf->base = (char*) malloc( suggested_size );
                buf->len  = suggested_size;
        }

        void client_on_connect( uv_connect_t* req, int status )
        {
                if ( status < 0 ) {
                        spdlog::error( "Client connection error {}", uv_strerror( status ) );
                        std::abort();
                }
                spdlog::info( "Client connected" );
                client& c = *(client*) req;

                uv_read_start( (uv_stream_t*) &c, client_alloc, client_read );
        }
}  // namespace

int client_init( client& c, uv_loop_t* loop, std::string_view addr, int port )
{
        uv_tcp_init( loop, &c );

        struct sockaddr_in dest;
        uv_ip4_addr( addr.data(), port, &dest );

        spdlog::info( "Connecting to server on port {}", port );
        return uv_tcp_connect(
            (uv_connect_t*) &c, (uv_tcp_t*) &c, (const struct sockaddr*) &dest, client_on_connect );
}
}  // namespace trctl