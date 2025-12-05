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
        virtual task< async_ptr< folder_dep > > shutdown() = 0;
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
        char                       path[folder_max_path_l];
        zll::ll_list< folder_dep > deps;
};

struct folders_ctx
{
        std::filesystem::path& workdir;

        uint8_t                buffer[1024 * 1024] = {};
        circular_buffer_memory mem{ std::span{ buffer } };

        using m     = map< folder_name, folder_ctx >;
        using alloc = typename m::allocator_type;
        m flds{ alloc{ mem } };
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
                    folder_ctx  fc          = {};
                    std::string folder_name = entr.name;
                    if ( folder_name.size() >= sizeof( name.name ) ) {
                            spdlog::error( "Folder name '{}' is too long", folder_name );
                            co_yield ecor::with_error{ error::input_error };
                    }
                    std::strncpy( name.name, folder_name.c_str(), sizeof( name.name ) - 1 );
                    std::strncpy(
                        fc.path, path( "/" )( folder_name ).str(), sizeof( fc.path ) - 1 );

                    auto [iter, inserted] = ctx.flds.try_emplace( name, std::move( fc ) );
                    if ( !inserted ) {
                            spdlog::error( "Duplicate folder name '{}'", name.name );
                            co_yield ecor::with_error{ error::input_error };
                    }
            } );
}

task< void > folder_create( auto& tctx, folders_ctx& ctx, char const* n )
{
        auto iter = ctx.flds.find( n );
        if ( iter != ctx.flds.end() ) {
                spdlog::error( "Folder '{}' already exists", n );
                co_yield ecor::with_error{ error::input_error };
        }

        folder_name name;
        folder_ctx  fc = {};
        if ( strlen( n ) >= sizeof( name.name ) ) {
                spdlog::error( "Folder name '{}' is too long", name.name );
                co_yield ecor::with_error{ error::input_error };
        }
        std::strncpy( name.name, n, sizeof( name.name ) - 1 );
        auto folder_path = ( ctx.workdir / name.name ).string();
        if ( folder_path.size() >= sizeof( fc.path ) ) {
                spdlog::error( "Folder path '{}' is too long", folder_path );
                co_yield ecor::with_error{ error::input_error };
        }
        std::strncpy( fc.path, folder_path.c_str(), sizeof( fc.path ) - 1 );

        co_await fs_mkdir{ tctx.loop, folder_path.c_str(), 0700 };

        ctx.flds.try_emplace( name, std::move( fc ) );
        spdlog::info( "Created folder '{}'", folder_path );
}

task< void > folder_delete( auto& tctx, folders_ctx& ctx, char const* name )
{
        auto iter = ctx.flds.find( name );
        if ( iter == ctx.flds.end() ) {
                spdlog::error( "Folder '{}' does not exist", name );
                co_yield ecor::with_error{ error::input_error };
        }

        for ( auto& d : iter->second.deps )
                co_await d.shutdown();

        fs_rm_rf_buff_entry dir_buf[32] = {};
        char                dir_path[folder_max_path_l];
        fixed_str           dir_path_str{ dir_path };
        co_await fs_rm_rf{ tctx.loop, dir_path_str( iter->second.path ), dir_buf };

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
        spdlog::info( "Clearing folder {} path '{}'", iter->first.name, iter->second.path );
        auto n = dir_path_str( iter->second.path );
        co_await fs_rm_rf{ tctx.loop, n, dir_buf };
        co_await fs_mkdir{ tctx.loop, iter->second.path, 0700 };
}

}  // namespace trctl