#pragma once

#include "./util.hpp"

#include <ecor/ecor.hpp>
#include <list>

namespace trctl
{


struct task_cfg
{
        using extra_error_signatures = ecor::completion_signatures< ecor::set_error_t( error ) >;
};

template < typename T >
using task = ecor::task< T, task_cfg >;

struct task_core : public ecor::task_core
{
        uv_loop_t* loop;
        uv_idle_t  idle;

        task_core( uv_loop_t* l )
          : loop( l )
        {
                idle.data = this;
                uv_idle_init( loop, &idle );
                uv_idle_start(
                    &idle, +[]( uv_idle_t* handle ) {
                            auto& self = *static_cast< task_core* >( handle->data );
                            self.run_once();
                    } );
        }

        ~task_core()
        {
                uv_idle_stop( &idle );
        }
};

class task_ctx
{
        circular_buffer_memory mem;

public:
        uv_loop_t*                 loop;
        ecor::task_memory_resource alloc{ mem };
        task_core&                 core;
        ecor::inplace_stop_source  stop;

        task_ctx( uv_loop_t* l, task_core& c, std::span< uint8_t > mem_buffer )
          : mem( mem_buffer )
          , loop( l )
          , core( c )
        {
        }

        void reschedule( ecor::_itask_op& op )
        {
                core.reschedule( op );
        }

        auto& query( ecor::get_task_core_t ) noexcept
        {
                return core;
        }

        auto& query( ecor::get_memory_resource_t ) noexcept
        {
                return alloc;
        }
};


struct comp_buff
{
        uint8_t buffer[1024 * 16];
};

struct component : task_ctx, zll::ll_base< component >
{
        uv_idle_t idle;

        component( uv_loop_t* l, task_core& c, std::span< uint8_t > mem_buffer )
          : task_ctx( l, c, mem_buffer )
        {
                idle.data = this;
                uv_idle_init( loop, &idle );
                uv_idle_start(
                    &idle, +[]( uv_idle_t* handle ) {
                            auto& self = *static_cast< component* >( handle->data );
                            self.tick();
                    } );
        }

        virtual task< void > shutdown() = 0;
        virtual void         tick()     = 0;

        ~component()
        {
                uv_idle_stop( &idle );
        }
};

template < std::size_t N, std::size_t M >
struct task_slot;

template < std::size_t N, std::size_t M >
struct task_slots : comp_buff, component
{
        task_slots( task_slots const& )            = delete;
        task_slots& operator=( task_slots const& ) = delete;
        task_slots( task_slots&& )                 = delete;
        task_slots& operator=( task_slots&& )      = delete;

        void slot_finished( task_slot< N, M >& slot )
        {
                _slots.remove_if( [&]( auto& x ) {
                        return &x == &slot;
                } );
                _finished_slots.link_back( slot );
        }

        void tick() override
        {
                clear_slots( _finished_slots );
        }

        void emplace_slot( uv_loop_t* loop, auto&& f )
        {
                auto* slot = new task_slot< N, M >( loop, *this, f );
                _slots.link_back( *slot );
                slot->start();
        }

        task_slots( uv_loop_t* l, task_core& c )
          : component( l, c, comp_buff::buffer )
        {
        }

        task< void > shutdown() override
        {
                spdlog::info( "Shutting down task slots" );
                while ( !_slots.empty() )
                        co_await _slots.front().shutdown();
                clear_slots( _slots );
                co_return;
        }

        ~task_slots()
        {
                clear_slots( _finished_slots );
        }

private:
        zll::ll_list< task_slot< N, M > > _slots;
        zll::ll_list< task_slot< N, M > > _finished_slots;

        void clear_slots( auto& s )
        {
                while ( !s.empty() ) {
                        auto& slot = s.take_front();
                        spdlog::debug( "Deleting {}", (void*) &slot );
                        delete &slot;
                }
        }
};

template < std::size_t N, std::size_t M >
struct task_slot : task_ctx, zll::ll_base< task_slot< N, M > >
{
        uint8_t             tctx_buffer[N];
        uint8_t             mem_buffer[M];
        task_slots< N, M >* slots;

        task_slot( uv_loop_t* loop, task_slots< N, M >& slots, auto&& f )
          : task_ctx( loop, slots.core, tctx_buffer )
          , slots( &slots )
          , op( f( *this, mem_buffer ).connect( _recv{ this } ) )
        {
        }

        struct _recv
        {
                using receiver_concept = ecor::receiver_t;

                task_slot* slot;

                void set_value() noexcept
                {
                        slot->finished();
                }

                void set_error( auto&& ) noexcept
                {
                        slot->finished();
                }

                void set_stopped() noexcept
                {
                        slot->finished();
                }
        };

        void start()
        {
                op.start();
        }

        void finished()
        {
                if ( slots )
                        slots->slot_finished( *this );
        }

        task< void > shutdown()
        {
                spdlog::info( "Shutting down task slot" );
                slots = nullptr;
                stop.request_stop();
                for ( std::size_t i = 0; i < 100; ++i )
                        uv_run( loop, UV_RUN_NOWAIT );
                co_return;
        }

        ecor::connect_type< task< void >, _recv > op;
};

}  // namespace trctl