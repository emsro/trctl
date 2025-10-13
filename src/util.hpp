#pragma once

#include "cobs.hpp"

#include <ecor/ecor.hpp>
#include <span>
#include <spdlog/spdlog.h>
#include <string>
#include <uv.h>

namespace trctl
{

struct addr_info
{
        std::string ip;
        int         port = 0;

        operator std::tuple< std::string&, int& >()
        {
                return { ip, port };
        }
};

enum class sock_kind
{
        PEER,
        SOCK
};

addr_info get_connection_info( uv_tcp_t const* c, sock_kind kind );

struct dealloc_iface
{
        virtual void deallocate( void* ptr, std::size_t, std::size_t ) = 0;
};

template < typename T >
struct deleter
{
        dealloc_iface* iface;
        void           operator()( void* p ) noexcept
        {
                iface->deallocate( p, sizeof( T ), alignof( T ) );
        }
};

template < typename T >
using unique_ptr = std::unique_ptr< T, deleter< T > >;

using circular_buffer_memory = ecor::circular_buffer_memory< uint64_t, dealloc_iface >;

struct unique_buffer
{
        unique_buffer( std::span< uint8_t > d, dealloc_iface* p )
          : data( d )
          , pool( p )
        {
        }

        unique_buffer( unique_buffer const& )            = delete;
        unique_buffer& operator=( unique_buffer const& ) = delete;

        unique_buffer( unique_buffer&& other ) noexcept
          : data( other.data )
          , pool( other.pool )
        {
                other.data = {};
                other.pool = nullptr;
        }

        operator std::span< uint8_t >() const
        {
                return data;
        }

        uint8_t* ptr() const
        {
                return data.data();
        }

        std::size_t size() const
        {
                return data.size();
        }

        ~unique_buffer()
        {
                if ( pool )
                        pool->deallocate( data.data(), data.size(), 1 );
        }

private:
        std::span< uint8_t > data;
        dealloc_iface*       pool;
};

inline unique_buffer make_unique_buffer( circular_buffer_memory& mem, std::size_t size )
{
        void* p = mem.allocate( size, 1 );
        if ( !p )
                return { {}, nullptr };
        memset( p, 0x00, size );
        return { { (uint8_t*) p, size }, &mem };
}

struct tcp_send_req : uv_write_t
{
        uv_buf_t                buf;
        unique_buffer           buff;
        circular_buffer_memory& mem;

        tcp_send_req( unique_buffer b, circular_buffer_memory& m )
          : buff( std::move( b ) )
          , mem( m )
        {
        }
};

enum class [[nodiscard]] send_status
{
        ENCODING_ERROR,
        WRITE_ERROR,
        SUCCESS
};


send_status cobs_send( circular_buffer_memory& mem, uv_tcp_t* c, std::span< uint8_t const > data );

struct cobs_receiver
{
        void _handle_rx( std::span< uint8_t const > data );

        struct reply
        {
                std::span< uint8_t const > data;
        };

        struct err
        {
        };

        cobs_receiver( std::span< uint8_t > buffer )
          : rx_buffer( buffer )
          , rx_used( 0 )
        {
        }

        std::span< uint8_t > rx_buffer;
        std::size_t          rx_used;

        ecor::event_source< reply, err > recv_src;
};


struct mem_usage_guard
{
        mem_usage_guard();
        ~mem_usage_guard();
};

}  // namespace trctl