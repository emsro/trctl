#pragma once

#include <ecor/ecor.hpp>
#include <gtest/gtest.h>
#include <uv.h>

namespace trctl
{

struct nd_mem
{
        void* allocate( std::size_t bytes, std::size_t align )
        {
                return ::operator new( bytes, std::align_val_t( align ) );
        }

        void deallocate( void* p, std::size_t, std::size_t align )
        {
                ::operator delete( p, std::align_val_t( align ) );
        }
};

struct test_ctx : ecor::task_core
{
        nd_mem                     mem;
        ecor::task_memory_resource alloc{ mem };
        uv_loop_t*                 loop = uv_default_loop();
        uv_idle_t                  idle;
        ecor::inplace_stop_source  stop;

        test_ctx()
        {
                idle.data = this;
                uv_idle_init( loop, &idle );
                uv_idle_start(
                    &idle, +[]( uv_idle_t* handle ) {
                            auto& self = *static_cast< test_ctx* >( handle->data );
                            self.run_once();
                    } );
        }

        test_ctx( test_ctx const& )            = delete;
        test_ctx& operator=( test_ctx const& ) = delete;
        test_ctx( test_ctx&& )                 = delete;
        test_ctx& operator=( test_ctx&& )      = delete;

        void reschedule( ecor::_itask_op& op )
        {
                ecor::task_core::reschedule( op );
        }

        auto& query( ecor::get_task_core_t ) noexcept
        {
                return (ecor::task_core&) *this;
        }

        auto& query( ecor::get_memory_resource_t ) noexcept
        {
                return alloc;
        }

        auto query( ecor::get_stop_token_t ) const noexcept
        {
                return stop.get_token();
        }

        ~test_ctx()
        {
                uv_idle_stop( &idle );
        }
};


inline void run_loop( uv_loop_t* loop, std::size_t max_iters )
{
        for ( std::size_t i = 0; i < max_iters; ++i ) {
                auto r = uv_run( loop, UV_RUN_NOWAIT );
                EXPECT_GE( r, 0 ) << "Error: " << uv_strerror( r );
                if ( r == 0 )
                        break;
        }
}

}  // namespace trctl