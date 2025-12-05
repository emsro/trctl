
#include "../../test/tutil.hpp"
#include "../async_storage.hpp"

#include <gtest/gtest.h>

namespace trctl
{


struct test_obj : test_ctx
{
        async_ptr_source< test_obj > src;
        int                          value = 0;

        test_obj( async_ptr_source< test_obj > s, int v )
          : src( s )
          , value( v )
        {
        }

        task< void > destroy()
        {
                co_return;
        }
};

TEST( async, emplace_find_erase_lifetime )
{
        test_ctx ctx{};
        uint8_t  membuf[1024];

        async_map< int, test_obj > m( ctx.loop, ctx, std::span< uint8_t >( membuf ) );

        EXPECT_EQ( m.size(), 0u );

        // emplace with hint (end) should insert
        auto p = m.emplace( m.end(), 10, 123 );
        ASSERT_TRUE( p.get() != nullptr );
        EXPECT_EQ( m.size(), 1u );

        auto it = m.find( 10 );
        EXPECT_NE( it, m.end() );
        EXPECT_EQ( it->second.get()->value, 123 );

        // Erase the mapping while keeping the async_ptr alive
        m.erase( it );
        EXPECT_EQ( m.size(), 0u );
        // the pointer should still keep the object alive
        EXPECT_EQ( p->value, 123 );

        // When p goes out of scope, destructor will queue destruction.
}

TEST( async, multiple_emplace_and_iteration )
{
        test_ctx ctx;
        uint8_t  membuf[1024];

        async_map< int, test_obj > m( ctx.loop, ctx, std::span< uint8_t >( membuf ) );

        auto p1 = m.emplace( m.end(), 1, 11 );
        auto p2 = m.emplace( m.end(), 2, 22 );
        auto p3 = m.emplace( m.end(), 3, 33 );

        EXPECT_EQ( m.size(), 3u );

        // iterate
        size_t count = 0;
        for ( auto it = m.begin(); it != m.end(); ++it )
                ++count;
        EXPECT_EQ( count, 3u );

        // erase one
        auto it2 = m.find( 2 );
        ASSERT_NE( it2, m.end() );
        m.erase( it2 );
        EXPECT_EQ( m.size(), 2u );

        // remaining pointers still valid
        EXPECT_EQ( p1->value, 11 );
        EXPECT_EQ( p3->value, 33 );
}

TEST( async, shutdown_with_pending_destruction )
{
        test_ctx ctx;
        uint8_t  membuf[1024];

        async_map< int, test_obj > m( ctx.loop, ctx, std::span< uint8_t >( membuf ) );

        // Create a scope so the async_ptr goes out of scope and schedules destruction
        {
                auto p = m.emplace( m.end(), 42, 4242 );
                ASSERT_TRUE( p.get() != nullptr );
                // erase mapping but keep p alive until scope exit
                auto it = m.find( 42 );
                ASSERT_NE( it, m.end() );
                m.erase( it );
        }

        // Now the async_ptr was destroyed and the item should be queued for async destruction.
        // Call shutdown and ensure it completes without hanging/crashing.
        auto op = m.shutdown().connect( ecor::_dummy_receiver{} );
        op.start();

        // Drive the loop a bit to allow destruction tasks to run
        for ( size_t i = 0; i < 100; ++i )
                uv_run( ctx.loop, UV_RUN_NOWAIT );

        SUCCEED();
}

}  // namespace trctl