#pragma once

#include "../fs.hpp"
#include "../util/async_sender_fifo.hpp"
#include "../util/async_storage.hpp"
#include "./folder.hpp"

#include <cstdint>
#include <filesystem>

namespace trctl
{


struct file_transfer_slot : folder_dep, comp_buff, task_ctx
{
        async_ptr_source< file_transfer_slot > src;
        async_sender_fifo                      workers;
        uv_file                                fh;
        uint64_t                               filesize;
        uint64_t                               written_bytes = 0;
        std::string                            path;

        file_transfer_slot(
            async_ptr_source< file_transfer_slot > src,
            uv_loop_t*                             loop,
            task_core&                             core,
            uint64_t                               filesize,
            std::string_view                       path,
            zll::ll_list< folder_dep >&            deps )
          : task_ctx( loop, core, comp_buff::buffer )
          , src( src )
          , fh( 0 )
          , filesize( filesize )
          , path( path )
        {
                deps.link_back( *this );
        }

        task< void > start()
        {
                spdlog::info( "Opening file for transfer: {}", path );
                this->fh =
                    co_await fs_open{ loop, this->path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR };
                spdlog::info( "Opened file (fh={})", this->fh );
        }

        task< void > write( uint64_t offset, std::span< uint8_t const > data )
        {
                spdlog::info(
                    "Writing {} bytes at offset {} to file (fh={})", data.size(), offset, fh );
                co_await fs_write{ loop, fh, offset, data };
                written_bytes += data.size();
        }

        uint8_t buffer[4 * 1024];

        task< void > end( uint32_t expected_hash )
        {
                spdlog::info( "Finalizing transfer for file (fh={})", fh );
                if ( written_bytes != filesize ) {
                        spdlog::error( "Got invalid written size: {}/{}", written_bytes, filesize );
                        co_yield ecor::with_error{ error::input_error };
                }

                fnv1a hasher;
                for ( uint32_t offset = 0; offset < filesize; offset += std::size( buffer ) ) {
                        std::span data = co_await fs_read{ loop, fh, offset, buffer };
                        hasher( data );
                }
                if ( hasher.hash != expected_hash ) {
                        spdlog::error(
                            "Hash mismatch: expected {:08x}, got {:08x}",
                            expected_hash,
                            hasher.hash );
                        co_yield ecor::with_error{ error::input_error };
                }

                spdlog::info( "Closing file (fh={})", fh );
                co_await fs_close{ loop, fh };
                fh = 0;
        }

        task< void > shutdown() override
        {
                auto p = src.get();
                src.clear();
                if ( fh != 0 ) {
                        co_await fs_close{ loop, fh };
                        co_await fs_unlink{ loop, path.c_str() };
                        fh = 0;
                }
                co_return;
        }

        task< void > destroy()
        {
                spdlog::info( "Shutting down file transfer slot for file: {}", path );
                if ( fh != 0 ) {
                        co_await fs_close{ loop, fh };
                        co_await fs_unlink{ loop, path.c_str() };
                        fh = 0;
                }
                co_return;
        };
};


struct file_transfer_ctx : comp_buff, component
{
        std::filesystem::path& workdir;

        file_transfer_ctx( uv_loop_t* l, task_core& c, std::filesystem::path& wd )
          : component( l, c, comp_buff::buffer )
          , workdir( wd )
          , transfers( l, c, buffer )
        {
        }

        void tick() override
        {
        }

        task< void > shutdown() override
        {
                spdlog::info( "Shutting down file transfers: {} transfers", transfers.size() );
                co_await transfers.shutdown();
                co_return;
        }

        uint8_t buffer[1024 * 1024];

        async_map< uint32_t, file_transfer_slot > transfers;
};

task< void > start_transfer(
    auto&                       tctx,
    file_transfer_ctx&          ctx,
    uint32_t                    id,
    std::string_view            filename,
    uint64_t                    filesize,
    zll::ll_list< folder_dep >& deps )
{
        auto iter = ctx.transfers.find( id );
        if ( iter != ctx.transfers.end() ) {
                spdlog::error( "Transfer with ID {} already exists", id );
                co_yield ecor::with_error{ error::input_error };
        }

        auto slot =
            ctx.transfers.emplace( iter, id, tctx.loop, tctx.core, filesize, filename, deps );
        co_await ( slot->start() | slot->workers.wrap() );
}

task< void > transfer_data(
    auto&,
    file_transfer_ctx&         ctx,
    uint32_t                   id,
    uint64_t                   offset,
    std::span< uint8_t const > data )
{
        auto it = ctx.transfers.find( id );
        if ( it == ctx.transfers.end() ) {
                spdlog::error( "No active transfer with ID {}", id );
                co_yield ecor::with_error{ error::input_error };
        }
        auto& t = it->second;
        if ( offset + data.size() > t->filesize ) {
                spdlog::error(
                    "Transfer data exceeds declared filesize: offset {} + size {} > filesize {}",
                    offset,
                    data.size(),
                    t->filesize );
                co_yield ecor::with_error{ error::input_error };
        }
        co_await ( t->write( offset, data ) | t->workers.wrap() );
}


task< error > end_transfer( auto&, file_transfer_ctx& ctx, uint32_t id, uint32_t expected_hash )
{
        // XXX: this should wait for active reads/writes to finish
        auto it = ctx.transfers.find( id );
        if ( it == ctx.transfers.end() ) {
                spdlog::error( "No active transfer with ID {}", id );
                co_yield ecor::with_error{ error::input_error };
        }
        auto& t = it->second;

        auto opt_err = co_await ( t->end( expected_hash ) | t->workers.wrap() | ecor::sink_err );
        ctx.transfers.erase( it );
        if ( opt_err ) {
                auto e = unify( *opt_err );
                spdlog::error( "Error during finalizing transfer ID {}", id, str( e ) );
                co_return e;
        } else {
                spdlog::info( "Transfer ID {} completed successfully", id );
                co_return error::none;
        }
}

}  // namespace trctl