#include "server.hpp"

namespace trctl
{
namespace
{

        void server_client_close( uv_handle_t* handle )
        {
                auto& c = *(server_client*) ( handle->data );
                spdlog::info( "Client disconnected: {}:{}", c.ip, c.port );
                c.server._remove_client( c );
        }


        void server_client_read( uv_stream_t* client, ssize_t nread, uv_buf_t const* buf )
        {
                auto& c = *(server_client*) ( client->data );
                if ( nread > 0 )
                        c._handle_rx( { (uint8_t*) buf->base, (std::size_t) nread } );
                if ( nread < 0 ) {
                        if ( nread != UV_EOF )
                                spdlog::error( "Read error {}", uv_err_name( nread ) );
                        uv_close( (uv_handle_t*) client, server_client_close );
                }

                free( buf->base );
        }


        void server_client_alloc( uv_handle_t*, size_t suggested_size, uv_buf_t* buf )
        {
                // auto& c = *(server_client*) ( handle->data );
                // XXX: should use memory pool from client instead
                buf->base = (char*) malloc( suggested_size );
                buf->len  = suggested_size;
        }
        void server_new_conn( uv_stream_t* srv, int status )
        {
                if ( status < 0 ) {
                        spdlog::error( "New connection error {}", uv_strerror( status ) );
                        return;
                }
                server& s = *(server*) srv->data;

                server_client& c = s._new_client();
                uv_tcp_init( s.loop, &c.tcp );
                if ( uv_accept( srv, (uv_stream_t*) &c.tcp ) == 0 ) {
                        spdlog::info( "Accepted new connection" );
                        std::tie( c.ip, c.port ) = get_connection_info( &c.tcp, sock_kind::PEER );
                        spdlog::info( "Client address {}:{}", c.ip, c.port );
                        s._commit_client( c );
                        uv_read_start(
                            (uv_stream_t*) &c.tcp, server_client_alloc, server_client_read );
                } else {
                        spdlog::error( "Accepting new connection error {}", uv_strerror( status ) );
                        uv_close( (uv_handle_t*) &c.tcp, []( uv_handle_t* handle ) {
                                auto& c = *(server_client*) ( handle->data );
                                c.server._drop_client( c );
                        } );
                }
        }
}  // namespace

int server_init( server& s, uv_loop_t* loop, int port )
{
        s.loop = loop;
        uv_tcp_init( loop, &s.tcp );

        uv_ip4_addr( "0.0.0.0", port, &s.addr );

        uv_tcp_bind( &s.tcp, (const struct sockaddr*) &s.addr, 0 );
        spdlog::info( "Starting new server on port {}", port );
        return uv_listen( (uv_stream_t*) &s.tcp, server::backlog, trctl::server_new_conn );
}

}  // namespace trctl