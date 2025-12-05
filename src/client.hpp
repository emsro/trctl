#pragma once

#include "ecor/ecor.hpp"
#include "util.hpp"

#include <span>
#include <spdlog/spdlog.h>
#include <uv.h>

namespace trctl
{

struct client
{
        struct promise
        {
                client&                 c;
                circular_buffer_memory& mem;
                uspan< uint8_t >        data;

                send_status fullfill( std::span< uint8_t const > data )
                {
                        return cobs_send( mem, &c.tcp, data );
                }
        };

        template < typename R >
        struct _recv : R
        {
                client& _client;

                _recv( R r, client& c )
                  : R( std::move( r ) )
                  , _client( c )
                {
                }

                void set_value( cobs_receiver::reply r )
                {
                        auto data = _client.mem.make_span< uint8_t >( r.data.size() );
                        if ( !data.data() ) {
                                R::set_error( cobs_receiver::err{} );
                                return;
                        }

                        std::copy_n( r.data.data(), r.data.size(), data.data() );
                        R::set_value(
                            promise{
                                .c    = _client,
                                .mem  = _client.mem,
                                .data = std::move( data ),
                            } );
                }
        };

        struct _sender
        {
                using sender_concept = ecor::sender_t;
                client& _client;

                template < typename Env >
                using completion_signatures = ecor::completion_signatures<
                    ecor::set_value_t( promise ),
                    ecor::set_error_t( cobs_receiver::err ) >;

                template < typename Env >
                completion_signatures< Env > get_completion_signatures( Env&& )
                {
                        return {};
                }

                template < typename R >
                auto connect( R rec ) noexcept
                {
                        return _client.recv.recv_src.schedule().connect(
                            _recv< R >{ std::move( rec ), _client } );
                }
        };

        client()
        {
                tcp.data     = this;
                connect.data = this;
        }
        client( client const& )            = delete;
        client& operator=( client const& ) = delete;
        client( client&& )                 = delete;
        client& operator=( client&& )      = delete;

        _sender incoming()
        {
                return { *this };
        }
        uv_tcp_t     tcp;
        uv_connect_t connect;

        uint8_t       rx_buffer[1024 * 8];
        cobs_receiver recv{ rx_buffer };

        uint8_t                buffer[1024 * 1024];
        circular_buffer_memory mem{ std::span{ buffer } };
};

int client_init( client& c, uv_loop_t* loop, std::string_view addr, int port );

}  // namespace trctl