#pragma once

#include "cobs.hpp"

#include <ecor/ecor.hpp>
#include <map>
#include <set>
#include <span>
#include <spdlog/spdlog.h>
#include <string>
#include <uv.h>

namespace trctl
{

template < typename T >
using opt = std::optional< T >;

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


using circular_buffer_memory = ecor::circular_buffer_memory< uint64_t, dealloc_iface >;

template < typename T >
using uspan = circular_buffer_memory::uspan< T >;

template < typename K, typename T >
using map = std::map<
    K,
    T,
    std::less< void >,
    ecor::circular_buffer_allocator< std::pair< K const, T >, uint64_t, dealloc_iface > >;

template < typename T >
using set =
    std::set< T, std::less< void >, ecor::circular_buffer_allocator< T, uint64_t, dealloc_iface > >;


struct tcp_send_req : uv_write_t
{
        uv_buf_t                                 buf;
        circular_buffer_memory::uspan< uint8_t > buff;
        circular_buffer_memory&                  mem;

        tcp_send_req( circular_buffer_memory::uspan< uint8_t > b, circular_buffer_memory& m )
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


        ecor::broadcast_source< ecor::set_value_t( reply ), ecor::set_error_t( err ) > recv_src;
};


struct mem_usage_guard
{
        mem_usage_guard();
        ~mem_usage_guard();
};

enum class error
{
        none,
        decoding_failed,
        encoding_failed,
        input_error,
        libuv_error,
        memory_allocation_failed,
        task_error,
        internal_error
};

constexpr error unify( std::variant< ecor::task_error, error > e )
{
        if ( std::holds_alternative< error >( e ) ) {
                return std::get< error >( e );
        } else {
                switch ( *std::get_if< ecor::task_error >( &e ) ) {
                case ecor::task_error::task_allocation_failure:
                        return error::memory_allocation_failed;
                default:
                        return error::task_error;
                }
        }
}

constexpr char const* str( error e )
{
        switch ( e ) {
        case error::none:
                return "no error";
        case error::decoding_failed:
                return "decoding failed";
        case error::encoding_failed:
                return "encoding failed";
        case error::input_error:
                return "input error";
        case error::libuv_error:
                return "libuv error";
        case error::memory_allocation_failed:
                return "memory allocation failed";
        case error::task_error:
                return "task error";
        default:
                return "unknown";
        }
}

template < typename T >
struct _sender
{
        using sender_concept = ecor::sender_t;
        using context_type   = T;
        using value_sig      = typename T::value_sig;

        T ctx;

        template < typename... Args >
        _sender( Args&&... args )
          : ctx( (Args&&) args... )
        {
        }

        template < typename Env >
        using completion_signatures =
            ecor::completion_signatures< value_sig, ecor::set_error_t( error ) >;

        template < typename Env >
        completion_signatures< Env > get_completion_signatures( Env&& ) const noexcept
        {
                return {};
        }

        template < typename R >
        struct _op
        {
                R            recv;
                context_type ctx;

                void start()
                {
                        ctx.start( *this );
                }
        };

        template < typename R >
        _op< R > connect( R&& receiver ) && noexcept
        {
                return { std::move( receiver ), std::move( ctx ) };
        }
};

struct fnv1a
{
        uint32_t hash  = 0x811c9dc5;
        uint32_t prime = 0x1000193;

        constexpr void operator()( std::span< uint8_t const > data )
        {
                for ( auto x : data ) {
                        hash = hash ^ x;
                        hash *= prime;
                }
        }
};

inline std::string joined( char** args )
{
        std::string res;
        for ( char** x = args; *x; x++ ) {
                res += " ";
                res += *x;
        }
        return res;
}

}  // namespace trctl