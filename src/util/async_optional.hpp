#pragma once

#include <ecor/ecor.hpp>
#include <optional>
#include <zll.hpp>

namespace trctl
{

template < typename T >
struct async_optional
{
        using sender_concept = ecor::sender_t;

        async_optional()                                   = default;
        async_optional( async_optional&& )                 = default;
        async_optional& operator=( async_optional&& )      = default;
        async_optional( async_optional const& )            = default;
        async_optional& operator=( async_optional const& ) = default;

        template < typename Env >
        auto get_completion_signatures( Env&& ) const noexcept
        {
                return ecor::
                    completion_signatures< ecor::set_value_t( T ), ecor::set_stopped_t() >();
        }

        bool has_value() const
        {
                return _core.opt.has_value();
        }

        auto& emplace( T& value )
        {
                _core.opt.emplace( std::move( value ) );
                while ( !_core.waiters.empty() ) {
                        auto& w = _core.waiters.front();
                        _core.waiters.detach_front();
                        w.set_value( *_core.opt );
                }
                return *_core.opt;
        }

        struct _start_iface : zll::ll_base< _start_iface >
        {
                virtual void set_value( T& ) = 0;
        };

        struct core
        {
                std::optional< T >           opt;
                zll::ll_list< _start_iface > waiters;
        };

        template < typename R >
        struct _op : _start_iface
        {
                using operation_state_concept = ecor::operation_state_t;

                R     r;
                core& c;

                _op( R recv, core& c )
                  : r( std::move( recv ) )
                  , c( c )
                {
                }

                void start()
                {
                        if ( c.opt.has_value() )
                                r.set_value( *c.opt );
                        else
                                c.waiters.link_back( *this );
                }

                void set_value( T& val ) override
                {
                        r.set_value( val );
                }
        };

        template < typename R >
        auto connect( R&& recv )
        {
                return _op{ std::move( recv ), _core };
        }

        ~async_optional() noexcept = default;

private:
        core _core;
};

}  // namespace trctl