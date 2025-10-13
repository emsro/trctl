#pragma once

#include "./util.hpp"
#include "cobs.hpp"
#include "ecor/ecor.hpp"
#include "iface.pb.h"

#include <cstdint>
#include <iostream>
#include <list>
#include <memory_resource>
#include <sys/types.h>
#include <uv.h>

namespace trctl
{

struct server;


struct server_client : private uv_tcp_t
{
        server&     server;
        std::string ip;
        int         port = 0;


        server_client( struct server& s, std::span< uint8_t > rx_buffer )
          : server( s )
          , _recv( rx_buffer )
        {
        }

        server_client( server_client const& )            = delete;
        server_client& operator=( server_client const& ) = delete;
        server_client( server_client&& )                 = delete;
        server_client& operator=( server_client&& )      = delete;

        struct _transact_sender;

        _transact_sender transact( std::span< uint8_t const > data )
        {
                return { this, data };
        }

        uv_tcp_t* h()
        {
                return (uv_tcp_t*) this;
        }

        template < typename ChildOp >
        struct _transact_op
        {
                server_client*             _client;
                std::span< uint8_t const > _data;
                ChildOp                    _child_op;

                void start()
                {
                        _child_op.start();
                        _client->_send( _data );
                }
        };

        struct _transact_sender
        {
                server_client*             _client;
                std::span< uint8_t const > _data;

                using value_type = cobs_receiver::reply;
                using error_type = cobs_receiver::err;

                template < typename R >
                auto connect( R rec ) noexcept
                {
                        return _transact_op{
                            _client,
                            _data,
                            _client->_recv.recv_src.schedule().connect( std::move( rec ) ) };
                }
        };

        void _send( std::span< uint8_t const > data )
        {
                auto status = cobs_send( _mem, this, data );
                switch ( status ) {
                case send_status::ENCODING_ERROR:
                        _recv.recv_src.set_error( cobs_receiver::err{} );
                        return;
                case send_status::WRITE_ERROR:
                        _recv.recv_src.set_error( cobs_receiver::err{} );
                        return;
                case send_status::SUCCESS:
                        break;
                }
        }

        void _handle_rx( std::span< uint8_t const > data )
        {
                _recv._handle_rx( data );
        }

private:
        uint8_t                _buffer[2048];
        circular_buffer_memory _mem{ std::span{ _buffer } };
        cobs_receiver          _recv;
};


struct server : uv_tcp_t
{
        // how many pending connections the queue will hold
        static constexpr std::size_t backlog = 128;

        sockaddr_in addr;
        uv_loop_t*  loop;

        struct client_disconnected
        {
                server_client& client;
        };

        struct new_client
        {
                server_client& client;
        };

        auto new_event()
        {
                return _new_event.schedule();
        }

        auto disc_event()
        {
                return _disc_event.schedule();
        }

        void _remove_client( server_client& client )
        {
                _disc_event.set_value( client_disconnected{ .client = client } );
                _clients.remove_if( [&client]( auto& other ) {
                        return &other == &client;
                } );
        }

        server_client& _new_client()
        {
                static constexpr std::size_t rx_size = 1024;
                auto*                        mem     = _mem.allocate( rx_size, 1 );
                return _clients.emplace_back(
                    *this, std::span< uint8_t >{ (uint8_t*) mem, rx_size } );
        }

        void _commit_client( server_client& client )
        {
                _new_event.set_value( server::new_client{ .client = client } );
        }

        void _drop_client( server_client& client )
        {
                _clients.remove_if( [&client]( auto& other ) {
                        return &other == &client;
                } );
        }

private:
        uint8_t                _buffer[64 * sizeof( server_client )];
        circular_buffer_memory _mem{ std::span{ _buffer } };

        using allocator = ecor::circular_buffer_allocator< server_client, uint64_t, dealloc_iface >;
        using list      = std::list< server_client, allocator >;
        list _clients{ allocator{ _mem } };

        ecor::event_source< new_client, void >          _new_event;
        ecor::event_source< client_disconnected, void > _disc_event;
};


int server_init( server& s, uv_loop_t* loop, int port );

}  // namespace trctl