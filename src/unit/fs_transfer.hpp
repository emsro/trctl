#pragma once

#include "../fs.hpp"
#include "../util/async_sender_fifo.hpp"

#include <cstdint>
#include <filesystem>

namespace trctl
{


struct file_transfer_slot
{
        async_sender_fifo workers;
        uv_file           fh;
        uint64_t          filesize;
        std::string       path;

        static task< void > start( auto& tctx, file_transfer_slot& s, std::string_view path )
        {
                spdlog::info( "Opening file for transfer: {}", path );
                s.path = std::string{ path };
                s.fh   = co_await fs_open{ tctx.loop, path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR };
                spdlog::info( "Opened file (fh={})", s.fh );
        }

        static task< void >
        write( auto& tctx, file_transfer_slot& s, uint64_t offset, std::span< uint8_t const > data )
        {
                spdlog::info(
                    "Writing {} bytes at offset {} to file (fh={})", data.size(), offset, s.fh );
                co_await fs_write{ tctx.loop, s.fh, offset, data };
        }
        uint8_t buffer[4 * 1024];

        static task< void > end( auto& tctx, file_transfer_slot& s, uint32_t expected_hash )
        {
                spdlog::info( "Finalizing transfer for file (fh={})", s.fh );
                fnv1a hasher;
                for ( uint32_t offset = 0; offset < s.filesize; offset += std::size( s.buffer ) ) {
                        std::span data = co_await fs_read{ tctx.loop, s.fh, offset, s.buffer };
                        hasher( data );
                }
                if ( hasher.hash != expected_hash ) {
                        spdlog::error(
                            "Hash mismatch: expected {:08x}, got {:08x}",
                            expected_hash,
                            hasher.hash );
                        co_await error::input_error;
                }

                spdlog::info( "Closing file (fh={})", s.fh );
                co_await fs_close{ tctx.loop, s.fh };
        }

        static task< void > abort( auto& tctx, file_transfer_slot& s )
        {
                spdlog::info( "Aborting transfer for file (fh={})", s.fh );
                if ( s.fh != 0 ) {
                        co_await fs_close{ tctx.loop, s.fh };
                        co_await fs_unlink{ tctx.loop, s.path.c_str() };
                }
        }
};


struct file_transfer_ctx : comp_buff, component
{
        std::filesystem::path& workdir;

        file_transfer_ctx( uv_loop_t* l, task_core& c, std::filesystem::path& wd )
          : component( l, c, comp_buff::buffer )
          , workdir( wd )
        {
        }

        void tick() override
        {
        }

        task< void > shutdown() override
        {
                spdlog::info( "Shutting down file transfers: {} transfers", transfers.size() );
                for ( auto& [id, t] : transfers )
                        co_await file_transfer_slot::abort( *this, t );
                co_return;
        }

        uint8_t                buffer[1024 * 1024];
        circular_buffer_memory mem{ buffer };

        using m     = map< uint32_t, file_transfer_slot >;
        using alloc = typename m::allocator_type;
        m transfers{ alloc{ mem } };
};

task< void > start_transfer(
    auto&              tctx,
    file_transfer_ctx& ctx,
    uint32_t           id,
    std::string_view   filename,
    uint64_t           filesize )
{
        auto iter = ctx.transfers.find( id );
        if ( iter != ctx.transfers.end() ) {
                spdlog::error( "Transfer with ID {} already exists", id );
                co_await error::input_error;
        }

        iter          = ctx.transfers.emplace_hint( iter, id, file_transfer_slot{} );
        auto& slot    = iter->second;
        slot.filesize = filesize;
        co_await slot.workers.enque( file_transfer_slot::start( tctx, slot, filename ) );
}

task< void > transfer_data(
    auto&                      tctx,
    file_transfer_ctx&         ctx,
    uint32_t                   id,
    uint64_t                   offset,
    std::span< uint8_t const > data )
{
        auto it = ctx.transfers.find( id );
        if ( it == ctx.transfers.end() ) {
                spdlog::error( "No active transfer with ID {}", id );
                co_await error::input_error;
        }
        file_transfer_slot& t = it->second;
        co_await t.workers.enque( file_transfer_slot::write( tctx, t, offset, data ) );
}


task< error >
end_transfer( auto& tctx, file_transfer_ctx& ctx, uint32_t id, uint32_t expected_hash )
{
        // XXX: this should wait for active reads/writes to finish
        auto it = ctx.transfers.find( id );
        if ( it == ctx.transfers.end() ) {
                spdlog::error( "No active transfer with ID {}", id );
                co_await error::input_error;
        }
        file_transfer_slot& t = it->second;

        auto opt_err = co_await (
            t.workers.enque( file_transfer_slot::end( tctx, t, expected_hash ) ) | ecor::sink_err );
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