#pragma once

#include "../util.hpp"

namespace trctl
{

// FIFO sender that ensures that only one operation is active at a time
struct async_sender_fifo
{
        struct _start_iface : zll::ll_base< _start_iface >
        {
                virtual void do_start() = 0;
        };
        struct _core
        {
                zll::ll_list< _start_iface > waiters;
                _start_iface*                current = nullptr;

                void on_start( _start_iface& op )
                {
                        if ( !current ) {
                                current = &op;
                                op.do_start();
                                return;
                        } else
                                waiters.link_back( op );
                }

                void on_end()
                {
                        current = nullptr;
                        if ( !waiters.empty() ) {
                                auto& op = waiters.front();
                                waiters.detach_front();
                                current = &op;
                                op.do_start();
                        }
                }
        };

        template < typename S >
        struct _sender
        {
                using sender_concept = ecor::sender_t;
                async_sender_fifo* fifo;
                S                  t;

                template < typename Env >
                auto get_completion_signatures( Env&& e ) const noexcept
                {
                        return t.get_completion_signatures( (Env&&) e );
                }

                template < typename R >
                struct _op : _start_iface
                {
                        struct _recv
                        {
                                _core* core;
                                R      recv;

                                using receiver_concept = ecor::receiver_t;

                                void set_value() noexcept
                                {
                                        recv.set_value();
                                        core->on_end();
                                }

                                template < typename E >
                                void set_error( E&& e ) noexcept
                                {
                                        recv.set_error( (E&&) e );
                                        core->on_end();
                                }

                                void set_stopped() noexcept
                                {
                                        recv.set_stopped();
                                        core->on_end();
                                }
                        };

                        _core*                         core;
                        ecor::connect_type< S, _recv > op;
                        R                              recv;

                        _op( _core* c, S t, R r )
                          : core( c )
                          , op( std::move( t ).connect( _recv{ core, std::move( r ) } ) )
                          , recv( std::move( r ) )
                        {
                        }

                        void start()
                        {
                                core->on_start( *this );
                        }

                        void do_start() override
                        {
                                op.start();
                        }
                };

                template < typename R >
                auto connect( R receiver )
                {
                        return _op< R >{ &fifo->core, std::move( t ), std::move( receiver ) };
                }
        };

        template < ecor::sender S >
        _sender< S > wrap( S x )
        {
                return _sender< S >{ this, std::move( x ) };
        }

        struct _wrap
        {
                async_sender_fifo* fifo;

                template < ecor::sender S >
                friend auto operator|( S&& s, _wrap&& self ) noexcept
                {
                        return self.fifo->wrap( (S&&) s );
                }
        };

        _wrap wrap()
        {
                return { this };
        }

private:
        _core core;
};

}  // namespace trctl