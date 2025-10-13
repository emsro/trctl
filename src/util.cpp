#include "util.hpp"

namespace trctl
{

addr_info get_connection_info( uv_tcp_t const* c, sock_kind kind )
{
        addr_info info;

        sockaddr_storage addr;
        int              addr_size = sizeof addr;
        if ( kind == sock_kind::PEER )
                uv_tcp_getpeername( c, (struct sockaddr*) &addr, &addr_size );
        else
                uv_tcp_getsockname( c, (struct sockaddr*) &addr, &addr_size );
        if ( addr.ss_family == AF_INET ) {
                auto* s = (struct sockaddr_in*) &addr;
                char  ip[INET_ADDRSTRLEN];
                uv_ip4_name( s, ip, sizeof ip );
                info.port = ntohs( s->sin_port );
                info.ip   = ip;
        } else if ( addr.ss_family == AF_INET6 ) {
                auto* s = (struct sockaddr_in6*) &addr;
                char  ip[INET6_ADDRSTRLEN];
                uv_ip6_name( s, ip, sizeof ip );
                info.port = ntohs( s->sin6_port );
                info.ip   = ip;
        } else {
                spdlog::error( "Unknown address family {}", addr.ss_family );
        }

        return info;
}

static inline void cobs_send_write_cb( uv_write_t* req, int status )
{
        if ( status < 0 )
                spdlog::error( "Write error {}\n", uv_strerror( status ) );
        auto* wr = (tcp_send_req*) req;
        auto& m  = wr->mem;
        m.destroy< tcp_send_req >( *wr );
}

send_status cobs_send( circular_buffer_memory& mem, uv_tcp_t* c, std::span< uint8_t const > data )
{
        tcp_send_req* wr = mem.construct< tcp_send_req >(
            make_unique_buffer( mem, 3 + data.size() * 258 / 255 ), mem );
        auto [succ, used] =
            encode_cobs( data, std::span< uint8_t >{ wr->buff }.subspan( 0, wr->buff.size() - 1 ) );
        if ( !succ ) {
                spdlog::error( "COBS encoding failed, message too large" );
                return send_status::ENCODING_ERROR;
        }
        wr->buf = uv_buf_init( (char*) used.data(), used.size() + 1 );
        int r   = uv_write( (uv_write_t*) wr, (uv_stream_t*) c, &wr->buf, 1, cobs_send_write_cb );
        if ( r ) {
                spdlog::error( "uv_write failed: {}", uv_strerror( r ) );
                delete wr;
                return send_status::WRITE_ERROR;
        }

        return send_status::SUCCESS;
}

void cobs_receiver::_handle_rx( std::span< uint8_t const > data )
{
        auto iter = rx_buffer.begin() + rx_used;
        if ( std::distance( iter, rx_buffer.end() ) < data.size() ) {
                spdlog::error(
                    "Failed to handle RX, message too large: size: {} capacity: {}",
                    data.size(),
                    std::distance( iter, rx_buffer.end() ) );
                std::abort();
                // XXX: improve
        }
        std::copy_n( data.begin(), data.size(), iter );
        rx_used += data.size();
        for ( ;; ) {
                auto buff = std::span{ rx_buffer }.subspan( 0, rx_used );
                auto iter = std::ranges::find( buff, 0x00u );
                if ( iter == buff.end() )
                        break;

                auto n = std::distance( buff.begin(), iter );

                auto msg = buff.subspan( 0, n );

                auto [succ, used] = decode_cobs( msg, msg );
                std::ignore       = succ;  // assert?

                recv_src.set_value( reply{ .data = used } );

                std::copy( iter + 1, buff.end(), rx_buffer.begin() );
                rx_used = std::distance( iter + 1, buff.end() );
        }
}

auto& get_guard_memory()
{
        static bool guard_memory = false;
        return guard_memory;
}

mem_usage_guard::mem_usage_guard()
{
        get_guard_memory() = true;
}

mem_usage_guard::~mem_usage_guard()
{
        get_guard_memory() = false;
}

}  // namespace trctl

void* operator new( std::size_t sz )
{
        if ( !trctl::get_guard_memory() )
                return std::malloc( sz );

        std::abort();
}

void operator delete( void* ptr ) noexcept
{
        if ( !trctl::get_guard_memory() ) {
                std::free( ptr );
                return;
        }
        std::abort();
}
