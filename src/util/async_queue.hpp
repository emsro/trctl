#pragma once

#include <ecor/ecor.hpp>
#include <zll.hpp>

namespace trctl
{

// Async queue that allows enqueuing and dequeuing items asynchronously
template < typename T >
struct async_queue
{
        struct _deque_iface : zll::ll_base< _deque_iface >
        {
                virtual void do_start( T& item ) = 0;
        };

        struct _node : zll::ll_base< _node >
        {
                T item;

                _node( T&& i )
                  : item( std::move( i ) )
                {
                }
        };

        struct _core
        {
                zll::ll_list< _node >        queue;
                zll::ll_list< _deque_iface > deque_waiters;
        };


        void enque( T&& item )
        {
                if ( __core.deque_waiters.empty() )
                        __core.queue.link_back( *( new _node{ std::move( item ) } ) );
                else {
                        auto& waiter = __core.deque_waiters.front();
                        __core.deque_waiters.detach_front();
                        waiter.do_start( item );
                }
        }

        void enque_all( T&& item )
        {
                if ( __core.deque_waiters.empty() )
                        __core.queue.link_back( *( new _node{ std::move( item ) } ) );
                else
                        while ( !__core.deque_waiters.empty() ) {
                                auto& waiter = __core.deque_waiters.front();
                                __core.deque_waiters.detach_front();
                                waiter.do_start( item );
                        }
        }

        struct _deq_sender
        {
                using sender_concept = ecor::sender_t;

                _core& __core;

                template < typename Env >
                auto get_completion_signatures( Env&& ) const noexcept
                {
                        return ecor::completion_signatures< ecor::set_value_t( T ) >();
                }

                template < typename R >
                struct _op : _deque_iface
                {
                        _core& core;
                        R      recv;

                        _op( _core& c, R r )
                          : core( c )
                          , recv( std::move( r ) )
                        {
                        }

                        void start()
                        {
                                if ( core.queue.empty() )
                                        core.deque_waiters.link_back( *this );
                                else {
                                        std::unique_ptr< _node > n{ &core.queue.front() };
                                        core.queue.detach_front();
                                        do_start( n->item );
                                }
                        }

                        void do_start( T& item ) override
                        {
                                std::move( recv ).set_value( std::move( item ) );
                        }
                };

                template < typename R >
                _op< R > connect( R receiver )
                {
                        return _op< R >{ __core, std::move( receiver ) };
                }
        };

        _deq_sender deque()
        {
                return { __core };
        }

        ~async_queue()
        {
                while ( !__core.queue.empty() ) {
                        auto& n = __core.queue.front();
                        __core.queue.detach_front();
                        delete &n;
                }
        }

private:
        _core __core;
};

}  // namespace trctl