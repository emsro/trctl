#pragma once

#include "ecor/ecor.hpp"
#include "util.hpp"

#include <span>
#include <spdlog/spdlog.h>
#include <uv.h>

namespace trctl
{

struct client : uv_tcp_t, uv_connect_t
{

        struct promise
        {
                client*                    c;
                circular_buffer_memory&    mem;
                std::span< uint8_t const > data;

                send_status fullfill( std::span< uint8_t const > data )
                {
                        return cobs_send( mem, c, data );
                }
        };

        template < typename R >
        struct _recv : R
        {
                client* _client;

                _recv( R r, client* c )
                  : R( std::move( r ) )
                  , _client( c )
                {
                }

                void set_value( cobs_receiver::reply r )
                {
                        R::set_value(
                            promise{
                                .c    = _client,
                                .mem  = _client->mem,
                                .data = r.data,
                            } );
                }
        };

        struct _sender
        {
                client* _client;

                using value_type = promise;
                using error_type = cobs_receiver::err;

                template < typename R >
                auto connect( R rec ) noexcept
                {
                        return _client->recv.recv_src.schedule().connect(
                            _recv< R >{ std::move( rec ), _client } );
                }
        };

        client()                           = default;
        client( client const& )            = delete;
        client& operator=( client const& ) = delete;
        client( client&& )                 = delete;
        client& operator=( client&& )      = delete;

        _sender incoming()
        {
                return { this };
        }

        uint8_t       rx_buffer[1024];
        cobs_receiver recv{ rx_buffer };

        uint8_t                buffer[1024 * 1024];
        circular_buffer_memory mem{ std::span{ buffer } };
};

int client_init( client& c, uv_loop_t* loop, std::string_view addr, int port );

}  // namespace trctl