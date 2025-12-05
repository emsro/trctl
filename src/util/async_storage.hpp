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

        virtual void clear( async_ptr_core< T >& ) = 0;
};

template < typename K, typename T >
struct async_map_core : async_map_core_base< T >
{
        std::map< K, async_ptr< T > > _m;

        void clear( async_ptr_core< T >& c )
        {
                auto iter = _m.begin();
                while ( iter != _m.end() ) {
                        if ( iter->second.get() == &c ) {
                                _m.erase( iter );
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
                raii->clear( this );
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
          , item( async_ptr_source{ core, this }, (Args&&) args... )
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

        template < typename U >
                requires( std::convertible_to< U&, T& > )
        async_ptr( async_ptr< U >&& other )
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

        ~async_ptr()
        {
                c->cnt--;
                if ( c->cnt == 0 )
                        c->raii_core.to_del.link_back( *c );
        }

private:
        core* c = nullptr;

        template < typename U >
        friend struct async_ptr;
};

template < typename K, typename T >
struct async_map : component
{
        using key_type   = K;
        using value_type = T;

        async_map( uv_loop_t* l, task_core& c, std::span< uint8_t > mem_buffer )
          : component( l, c, mem_buffer )
        {
                idle.data = this;
                uv_idle_init( loop, &idle );
                uv_idle_start(
                    &idle, +[]( uv_idle_t* handle ) {
                            auto& self = *static_cast< component* >( handle->data );
                            self.tick();
                    } );
        }

        template < typename... Args >
        async_ptr< T > emplace( K key, Args&&... args )
        {
                auto iter = _core.m.find( key );
                if ( iter == _core.m.end() )
                        return {};
                auto* p = new async_ptr_core< T >{ (Args&&) args... };
                _core.m.emplace_hint( iter, std::move( key ), async_ptr< T >{ p } );
                return { p };
        }

        template < typename... Args >
        async_ptr< T > emplace( auto iter, K key, Args&&... args )
        {
                auto* p = new async_ptr_core< T >{ (Args&&) args... };
                _core.m.emplace_hint( iter, std::move( key ), async_ptr< T >{ p } );
                return { p };
        }

        [[nodiscard]] std::size_t size() const
        {
                return _core._m.size();
        }

        auto find( this auto& self, auto& k )
        {
                return self._core._m.find( k );
        }

        auto begin( this auto& self )
        {
                return self._core._m.begin();
        }

        auto end( this auto& self )
        {
                return self._core._m.end();
        }

        void erase( auto iter )
        {
                _core._m.erase( iter );
        }

        task< void > shutdown() override
        {
                _core._m.clear();
                for ( auto& x : _core.to_del )
                        co_await ( x.shutdown() | ecor::err_to_val | ecor::as_variant );
        }

        ~async_map()
        {
                uv_idle_stop( &idle );
        }

private:
        void tick() override
        {
                if ( !_destroy_task && !_core.to_del.empty() ) {
                        auto& x       = _core.to_del.take_front();
                        _destroy_task = x.shutdown().connect( _destroy_recv{ this, &x } );
                        _destroy_task.start();
                }
        }

        struct _destroy_recv
        {
                using receiver_concept = ecor::receiver_t;

                async_map< K, T >*   map;
                async_ptr_core< T >* x;

                void on_value()
                {
                        delete &x;
                }

                void on_error( auto& )
                {
                        delete &x;
                }
        };

        uv_loop_t*                                        _loop;
        uv_idle_t                                         _idle;
        async_map_core< K, T >                            _core;
        ecor::connect_type< task< void >, _destroy_recv > _destroy_task;
};

}  // namespace trctl