#pragma once

#include "../task.hpp"

#include <map>
#include <zll.hpp>

namespace trctl
{

// Asynchronous operation storage.
//
// async_map<K,T> provides map-like storage for instances of T. Lifetime of T is controlled by
// async_ptr<T> which provide reference counting for allocated storage for T. async_map<K,T> is just
// one holder of the pointer. The pointer can't be copied, only moved. Only central control object
// is allowed to create new. T is kept alive as long as there is enough existing pointers to it.
//
// After count goes to 0, T is scheduled to removal by the map, which asynchronously executes
// destroy() task on the type, after that finishes the T is destructed and memory is returned.
//

template < typename T >
struct async_ptr_core;

template < typename T >
struct async_ptr;

template < typename T >
struct async_map_core_base
{
        zll::ll_list< async_ptr_core< T > > to_del;
        async_ptr_core< T >*                to_erase = nullptr;

        virtual void clear( async_ptr_core< T >& ) = 0;
};

template < typename K, typename T >
struct async_map_core : async_map_core_base< T >
{
        std::map< K, async_ptr< T >, std::less<> > m;

        void clear( async_ptr_core< T >& c )
        {
                auto iter = m.begin();
                while ( iter != m.end() ) {
                        if ( iter->second.get() == &c.item ) {
                                m.erase( iter );
                                break;
                        }
                }
        }
};

template < typename T >
struct async_ptr_source
{
        async_ptr_source( async_map_core_base< T >& raii, async_ptr_core< T >& core )
          : raii( &raii )
          , core( &core )
        {
        }

        void clear()
        {
                raii->clear( *this->core );
        }

        async_ptr< T > get()
        {
                return { *core };
        }

private:
        async_map_core_base< T >* raii = nullptr;
        async_ptr_core< T >*      core = nullptr;
};

template < typename T >
struct async_ptr_core : zll::ll_base< async_ptr_core< T > >
{
        async_map_core_base< T >& raii_core;
        uint32_t                  cnt = 0;
        T                         item;

        template < typename... Args >
        async_ptr_core( async_map_core_base< T >& core, Args&&... args )
          : raii_core( core )
          , item( async_ptr_source< T >{ core, *this }, (Args&&) args... )
        {
        }
};

template < typename T >
struct async_ptr
{
        using core = async_ptr_core< T >;

        async_ptr() noexcept = default;

        async_ptr( core& c )
          : c( &c )
        {
                c.cnt++;
        }

        async_ptr( async_ptr const& )       = delete;
        auto& operator=( async_ptr const& ) = delete;

        async_ptr( async_ptr&& other )
          : c( other.c )
        {
                other.c = nullptr;
        }

        auto& operator=( async_ptr&& other )
        {
                auto cpy{ std::move( other ) };
                std::swap( c, cpy.c );
                return *this;
        }

        T& operator*()
        {
                return c->item;
        }

        T* operator->()
        {
                return &c->item;
        }

        [[nodiscard]] operator bool() const
        {
                return !!c;
        }

        T* get()
        {
                return &c->item;
        }

        ~async_ptr()
        {
                if ( !c )
                        return;
                c->cnt--;
                if ( c->cnt == 0 )
                        c->raii_core.to_del.link_back( *c );
        }

private:
        core* c = nullptr;

        template < typename U >
        friend struct async_ptr;
};

template < typename T >
concept has_destroy_member_function = requires( T x ) {
        { x.destroy() } -> std::same_as< task< void > >;
};

struct do_destroy_t
{

        template < typename T >
        task< void > operator()( auto& ctx, T& x )
        {
                if constexpr ( has_destroy_member_function< T > )
                        return x.destroy();
                else
                        return destroy( ctx, x );
        }
};

inline static do_destroy_t do_destroy;


template < typename K, typename T >
struct async_map : component
{
        using key_type   = K;
        using value_type = T;

        async_map( uv_loop_t* l, task_core& c, std::span< uint8_t > mem_buffer )
          : component( l, c, mem_buffer )
        {
        }

        template < typename... Args >
        async_ptr< T > emplace( K key, Args&&... args )
        {
                auto iter = _core.m.find( key );
                if ( iter != _core.m.end() )
                        return {};
                return emplace( iter, key, (Args&&) args... );
        }

        using iterator = typename std::map< K, async_ptr< T > >::iterator;

        template < typename... Args >
        std::pair< iterator, bool > try_emplace( K key, Args&&... args )
        {
                auto iter = _core.m.find( key );
                if ( iter != _core.m.end() )
                        return { iter, false };
                auto* p = new async_ptr_core< T >{ _core, (Args&&) args... };
                iter    = _core.m.emplace_hint( iter, std::move( key ), async_ptr< T >{ *p } );
                return { iter, true };
        }

        template < typename... Args >
        async_ptr< T > emplace( iterator iter, K key, Args&&... args )
        {
                auto* p = new async_ptr_core< T >{ _core, (Args&&) args... };
                _core.m.emplace_hint( iter, std::move( key ), async_ptr< T >{ *p } );
                return { *p };
        }

        [[nodiscard]] std::size_t size() const
        {
                return _core.m.size();
        }

        auto find( this auto& self, auto&& k )
        {
                return self._core.m.find( k );
        }

        auto begin( this auto& self )
        {
                return self._core.m.begin();
        }

        auto rbegin( this auto& self )
        {
                return self._core.m.rbegin();
        }

        auto end( this auto& self )
        {
                return self._core.m.end();
        }

        auto rend( this auto& self )
        {
                return self._core.m.rend();
        }

        void erase( auto iter )
        {
                _core.m.erase( iter );
        }

        task< void > shutdown() override
        {
                _core.m.clear();
                if ( !_core.to_del.empty() )
                        co_await _on_all_destroyed.schedule();
                co_return;
        }

        ~async_map()
        {
                uv_idle_stop( &idle );
                while ( !_core.to_del.empty() ) {
                        auto& x = _core.to_del.take_front();
                        delete &x;
                }
        }

private:
        void tick() override
        {
                if ( auto* p = std::exchange( _core.to_erase, nullptr ) ) {
                        _destroy_task.clear();
                        delete p;
                }
                if ( !_destroy_task && !_core.to_del.empty() ) {
                        auto& x       = _core.to_del.take_front();
                        _destroy_task = do_destroy( (task_ctx&) *this, x.item )
                                            .connect( _destroy_recv{ this, &x } );
                        _destroy_task.start();
                } else if ( !!_destroy_task && _core.to_del.empty() ) {
                        _on_all_destroyed.set_value();
                }
        }

        struct _destroy_recv
        {
                using receiver_concept = ecor::receiver_t;

                async_map< K, T >*   map;
                async_ptr_core< T >* x;

                void set_value()
                {
                        map->_core.to_erase = x;
                }

                void set_error( auto&& )
                {
                        map->_core.to_erase = x;
                }

                void set_stopped()
                {
                        map->_core.to_erase = x;
                }
        };

        uv_loop_t*                                        _loop;
        async_map_core< K, T >                            _core;
        ecor::broadcast_source< ecor::set_value_t() >     _on_all_destroyed;
        ecor::connect_type< task< void >, _destroy_recv > _destroy_task;
};

}  // namespace trctl