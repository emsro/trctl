#pragma once

#include "../fs.hpp"
#include "../task.hpp"
#include "../util.hpp"
#include "../util/async_storage.hpp"

#include <filesystem>

namespace trctl
{

static constexpr std::size_t folder_max_path_l = 256;
static constexpr std::size_t folder_max_name_l = 32;

struct folder_dep : zll::ll_base< folder_dep >
{
        virtual task< void > shutdown() = 0;
};

struct folder_name
{
        char name[folder_max_name_l];

        constexpr auto operator<=>( folder_name const& ) const = default;
        constexpr auto operator<=>( char const* other ) const
        {
                return std::strcmp( name, other ) <=> 0;
        }
};

struct folder_ctx
{
        std::string                path;
        zll::ll_list< folder_dep > deps;

        folder_ctx( async_ptr_source< folder_ctx >, std::string path )
          : path( std::move( path ) )
        {
        }
};
task< void > destroy( auto&, folder_ctx& f )
{
        for ( auto& d : f.deps )
                co_await d.shutdown();
        co_return;
}

struct folders_ctx : comp_buff, task_ctx
{
        std::filesystem::path& workdir;

        folders_ctx( uv_loop_t* l, task_core& c, std::filesystem::path& wd )
          : task_ctx( l, c, comp_buff::buffer )
          , workdir( wd )
          , flds( l, c, buffer )
        {
        }

        task< void > shutdown()
        {
                co_await flds.shutdown();
        }

        uint8_t buffer[1024 * 1024];

        async_map< folder_name, folder_ctx > flds;
};

task< void > folder_init( auto& tctx, folders_ctx& ctx )
{
        int res = co_await fs_access{ tctx.loop, ctx.workdir.string().c_str() };
        if ( res == UV_ENOENT ) {
                co_await fs_mkdir{ tctx.loop, ctx.workdir.string().c_str(), 0700 };
                co_return;
        } else if ( res < 0 ) {
                spdlog::error(
                    "Failed to access workdir {}: {}", ctx.workdir.string(), uv_strerror( res ) );
                co_yield ecor::with_error{ error::libuv_error };
        }

        char      buff[256];
        fixed_str dir_str{ std::span{ buff } };

        co_await dir_iter(
            tctx,
            dir_str( ctx.workdir.string() ),
            [&]( auto&, fixed_str::node path, uv_dirent_t& entr ) -> task< void > {
                    if ( entr.type != UV_DIRENT_DIR )
                            co_return;

                    spdlog::info( "Loading folder: {}", entr.name );

                    folder_name name;
                    std::string folder_name = entr.name;
                    if ( folder_name.size() >= sizeof( name.name ) ) {
                            spdlog::error( "Folder name '{}' is too long", folder_name );
                            co_yield ecor::with_error{ error::input_error };
                    }
                    std::strncpy( name.name, folder_name.c_str(), sizeof( name.name ) - 1 );
                    std::string p = path( "/" )( folder_name ).str();

                    if ( ctx.flds.find( name ) != ctx.flds.end() ) {
                            spdlog::error( "Duplicate folder name '{}'", name.name );
                            co_yield ecor::with_error{ error::input_error };
                    }
                    ctx.flds.emplace( name, std::move( p ) );
            } );
}

task< void > folder_create( auto& tctx, folders_ctx& ctx, char const* n )
{
        auto iter = ctx.flds.find( n );
        if ( iter != ctx.flds.end() ) {
                spdlog::error( "Folder '{}' already exists", n );
                co_yield ecor::with_error{ error::input_error };
        }

        std::string_view sv{ n };
        auto             sz = sv.find_first_of( "/" );
        if ( sz != std::string_view::npos ) {
                spdlog::error( "Folder name shall not contain / char" );
                co_yield ecor::with_error{ error::input_error };
        }

        folder_name name;
        if ( sv.size() >= sizeof( name.name ) ) {
                spdlog::error( "Folder name '{}' is too long", name.name );
                co_yield ecor::with_error{ error::input_error };
        }
        std::strncpy( name.name, n, sizeof( name.name ) - 1 );
        std::string folder_path = ( ctx.workdir / name.name ).string();

        co_await fs_mkdir{ tctx.loop, folder_path.c_str(), 0700 };

        ctx.flds.emplace( name, std::move( folder_path ) );
        spdlog::info( "Created folder '{}'", folder_path );
}

task< void > folder_delete( auto& tctx, folders_ctx& ctx, char const* name )
{
        auto iter = ctx.flds.find( name );
        if ( iter == ctx.flds.end() ) {
                spdlog::error( "Folder '{}' does not exist", name );
                co_yield ecor::with_error{ error::input_error };
        }

        for ( auto& d : iter->second->deps )
                co_await d.shutdown();

        fs_rm_rf_buff_entry dir_buf[32] = {};
        char                dir_path[folder_max_path_l];
        fixed_str           dir_path_str{ dir_path };
        co_await fs_rm_rf{ tctx.loop, dir_path_str( iter->second->path ), dir_buf };

        ctx.flds.erase( iter );
}

task< void > folder_clear( auto& tctx, folders_ctx& ctx, char const* name )
{
        auto iter = ctx.flds.find( name );
        if ( iter == ctx.flds.end() ) {
                spdlog::error( "Folder '{}' does not exist", name );
                co_yield ecor::with_error{ error::input_error };
        }

        fs_rm_rf_buff_entry dir_buf[32] = {};
        char                dir_path[folder_max_path_l];
        fixed_str           dir_path_str{ dir_path };
        spdlog::info( "Clearing folder {} path '{}'", iter->first.name, iter->second->path );
        auto n = dir_path_str( iter->second->path );
        co_await fs_rm_rf{ tctx.loop, n, dir_buf };
        co_await fs_mkdir{ tctx.loop, iter->second->path.c_str(), 0700 };
}

}  // namespace trctl