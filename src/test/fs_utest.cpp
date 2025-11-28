
#include "../fs.hpp"
#include "./str.hpp"
#include "./tutil.hpp"

#include <gtest/gtest.h>

namespace trctl
{

task< void > fs_touch( test_ctx& ctx, uv_loop_t* loop, fixed_str::node path )
{
        uv_file fh = co_await fs_open{
            loop, path.str(), UV_FS_O_CREAT | UV_FS_O_WRONLY | UV_FS_O_TRUNC, 0644 };
        co_await fs_close{ loop, fh };
}


struct err_receiver
{
        using receiver_concept = ecor::receiver_t;

        void set_value() noexcept
        {
        }
        template < typename... Args >
        void set_error( Args&&... ) noexcept
        {
                FAIL() << "Unexpected error in fs basic test";
        }

        void set_stopped() noexcept
        {
                FAIL() << "Unexpected stop in fs basic test";
        }
};

ecor::empty_env get_env( err_receiver const& ) noexcept
{
        return {};
}

TEST( fs, basic )
{
        test_ctx ctx;

        auto f = [&]( test_ctx& ctx ) -> task< void > {
                char             buff[128];
                std::string_view dir = co_await fs_mkdtemp{ ctx.loop, "./tmp-XXXXXX", buff };

                char      buff2[128];
                fixed_str dir_str{ std::span{ buff2 } };
                auto      n = dir_str( dir )( "/" );

                co_await fs_touch( ctx, ctx.loop, n( "file.txt" ) );

                co_await fs_mkdir{ ctx.loop, n( "subdir" ).str() };

                co_await fs_touch( ctx, ctx.loop, n( "subdir/file2.txt" ) );

                fs_rm_rf_buff_entry dir_entries[16] = {};
                co_await fs_rm_rf{ ctx.loop, dir_str( dir )( "/" ), dir_entries };

                uv_stop( ctx.loop );
                co_return;
        };

        auto h = f( ctx ).connect( err_receiver{} );
        h.start();

        run_loop( ctx.loop, 5000 );

        uv_loop_close( ctx.loop );
}

}  // namespace trctl