#pragma once

#include "../fs.hpp"
#include "../task.hpp"
#include "../util.hpp"

#include <filesystem>

namespace trctl
{

static constexpr std::size_t folder_max_path_l = 256;
static constexpr std::size_t folder_max_name_l = 32;

struct folder_ctx
{
        char name[folder_max_name_l];
        char path[folder_max_path_l];
};
constexpr auto operator<=>( folder_ctx const& a, folder_ctx const& b )
{
        return std::strcmp( a.name, b.name ) <=> 0;
}
constexpr bool operator==( folder_ctx const& a, folder_ctx const& b )
{
        return std::strcmp( a.name, b.name ) == 0;
}
constexpr bool operator<( folder_ctx const& a, char const* b )
{
        return std::strcmp( a.name, b ) < 0;
}
constexpr bool operator<( char const* a, folder_ctx const& b )
{
        return std::strcmp( a, b.name ) < 0;
}

struct folders_ctx
{
        std::filesystem::path& workdir;

        uint8_t                buffer[1024 * 1024] = {};
        circular_buffer_memory mem{ std::span{ buffer } };

        using m     = set< folder_ctx >;
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
                co_await error::libuv_error;
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

                    folder_ctx  fc          = {};
                    std::string folder_name = entr.name;
                    if ( folder_name.size() >= sizeof( fc.name ) ) {
                            spdlog::error( "Folder name '{}' is too long", folder_name );
                            co_await error::input_error;
                    }
                    std::strncpy( fc.name, folder_name.c_str(), sizeof( fc.name ) - 1 );
                    std::strncpy( fc.path, path.str(), sizeof( fc.path ) - 1 );

                    auto [iter, inserted] = ctx.flds.insert( fc );
                    if ( !inserted ) {
                            spdlog::error( "Duplicate folder name '{}'", fc.name );
                            co_await error::input_error;
                    }
            } );
}

task< void > folder_create( auto& tctx, folders_ctx& ctx, char const* name )
{
        auto iter = ctx.flds.find( name );
        if ( iter != ctx.flds.end() ) {
                spdlog::error( "Folder '{}' already exists", name );
                co_await error::input_error;
        }

        folder_ctx fc = {};
        if ( strlen( name ) >= sizeof( fc.name ) ) {
                spdlog::error( "Folder name '{}' is too long", name );
                co_await error::input_error;
        }
        std::strncpy( fc.name, name, sizeof( fc.name ) - 1 );
        auto folder_path = ( ctx.workdir / name ).string();
        if ( folder_path.size() >= sizeof( fc.path ) ) {
                spdlog::error( "Folder path '{}' is too long", folder_path );
                co_await error::input_error;
        }
        std::strncpy( fc.path, folder_path.c_str(), sizeof( fc.path ) - 1 );

        co_await fs_mkdir{ tctx.loop, folder_path.c_str(), 0700 };

        ctx.flds.insert( fc );
        spdlog::info( "Created folder '{}'", folder_path );
}

task< void > folder_delete( auto& tctx, folders_ctx& ctx, char const* name )
{
        auto iter = ctx.flds.find( name );
        if ( iter == ctx.flds.end() ) {
                spdlog::error( "Folder '{}' does not exist", name );
                co_await error::input_error;
        }


        fs_rm_rf_buff_entry dir_buf[32] = {};
        char                dir_path[folder_max_path_l];
        fixed_str           dir_path_str{ dir_path };
        co_await fs_rm_rf{ tctx.loop, dir_path_str( iter->path ), dir_buf };

        ctx.flds.erase( iter );
}

task< void > folder_clear( auto& tctx, folders_ctx& ctx, char const* name )
{
        auto iter = ctx.flds.find( name );
        if ( iter == ctx.flds.end() ) {
                spdlog::error( "Folder '{}' does not exist", name );
                co_await error::input_error;
        }

        fs_rm_rf_buff_entry dir_buf[32] = {};
        char                dir_path[folder_max_path_l];
        fixed_str           dir_path_str{ dir_path };
        co_await fs_rm_rf{ tctx.loop, dir_path_str( iter->path ), dir_buf };
        co_await fs_mkdir{ tctx.loop, iter->path, 0700 };
}

}  // namespace trctl