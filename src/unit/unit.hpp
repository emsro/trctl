#pragma once

#include "../client.hpp"
#include "../fs.hpp"
#include "folder.hpp"
#include "fs_transfer.hpp"
#include "iface.hpp"
#include "process.hpp"

#include <filesystem>
#include <list>

namespace trctl
{


struct unit_ctx : comp_buff, task_ctx
{
        uv_loop_t*                loop;
        std::filesystem::path&    workdir;
        zll::ll_list< component > comps;

        unit_ctx( uv_loop_t* l, std::filesystem::path& wd, task_core& c )
          : task_ctx( l, c, comp_buff::buffer )
          , loop( l )
          , workdir( wd )
        {
                comps.link_back( pctx );
                comps.link_back( slots );
                comps.link_back( fctx );
        }

        task< void > shutdown()
        {
                uv_close( (uv_handle_t*) &cl.tcp, nullptr );
                co_await slots.shutdown();
                co_await pctx.shutdown();
                co_await fctx.shutdown();
                co_return;
        }

        client            cl;
        file_transfer_ctx fctx{ loop, core, workdir };
        folders_ctx       folctx{ .workdir = workdir };
        uint32_t          pctx_buffer[1024 * 8];
        proc_ctx          pctx{ loop, core };

        task_slots< 1024 * 8, 1024 * 16 > slots{ loop, core };
};


inline unit_to_hub prepare_reply( uv_loop_t* loop, uint32_t req_id )
{
        unit_to_hub msg = unit_to_hub_init_default;
        msg.req_id      = req_id;
        auto now_ms     = uv_now( loop );
        msg.has_ts      = true;
        msg.ts.sec      = now_ms / 1000;
        msg.ts.nsec     = ( now_ms % 1000 ) * 1000000;
        return msg;
}

inline task< unit_to_hub > on_msg(
    task_ctx&               ctx,
    circular_buffer_memory& mem,
    file_transfer_ctx&      fctx,
    folders_ctx&            folctx,
    proc_ctx&               pctx,
    hub_to_unit             msg )
{
        unit_to_hub reply;
        switch ( msg.which_sub ) {
        case hub_to_unit_init_tag: {
                spdlog::info( "Received get_init message" );
                init_msg resp;
                resp.mac_addr = "DE:AD:BE:EF:00:01";  // XXX: fill
                resp.version  = "0.0.0";              // XXX: fill

                reply           = prepare_reply( ctx.loop, msg.req_id );
                reply.which_sub = unit_to_hub_init_tag;
                reply.sub.init  = resp;
                break;
        }
        case hub_to_unit_file_transfer_tag: {
                auto& ftr = msg.sub.file_transfer;
                switch ( ftr.which_sub ) {
                case file_transfer_req_start_tag: {
                        spdlog::info( "Received file_transfer_start message" );

                        auto& sub = ftr.sub.start;

                        static constexpr std::size_t n = 128;

                        auto sp = mem.make_span< char >( n );
                        std::snprintf(
                            sp.data(),
                            n,
                            "%s/%s/%s",
                            fctx.workdir.string().c_str(),
                            sub.folder,
                            sub.filename );

                        auto opt_err = co_await (
                            start_transfer( ctx, fctx, ftr.seq, sp.data(), sub.filesize ) |
                            ecor::sink_err );
                        if ( opt_err )
                                spdlog::error( "Error during start transfer" );
                        reply           = prepare_reply( ctx.loop, msg.req_id );
                        reply.which_sub = unit_to_hub_file_tag;
                        reply.sub.file  = file_resp{ .success = !opt_err };
                        break;
                }
                case file_transfer_req_data_tag: {
                        spdlog::info( "Received file_transfer_data message" );

                        auto& sub = ftr.sub.data;

                        auto opt_err = co_await (
                            transfer_data(
                                ctx, fctx, ftr.seq, sub.offset, { sub.data.data, sub.data.size } ) |
                            ecor::sink_err );
                        if ( opt_err )
                                spdlog::error( "Error during data transfer" );

                        reply           = prepare_reply( ctx.loop, msg.req_id );
                        reply.which_sub = unit_to_hub_file_tag;
                        reply.sub.file  = file_resp{ .success = !opt_err };
                        break;
                }
                case file_transfer_req_end_tag: {
                        spdlog::info( "Received file_transfer_end message" );

                        auto& sub = ftr.sub.end;

                        error e = co_await end_transfer( ctx, fctx, ftr.seq, sub.fnv1a );

                        reply           = prepare_reply( ctx.loop, msg.req_id );
                        reply.which_sub = unit_to_hub_file_tag;
                        reply.sub.file  = file_resp{ .success = e == error::none };
                        break;
                }
                default:
                        spdlog::warn( "Unknown file_transfer_req sub type: {}", ftr.which_sub );
                        break;
                }
                break;
        }
        case hub_to_unit_task_tag: {
                auto& treq = msg.sub.task;
                switch ( treq.which_sub ) {
                case task_req_start_tag: {

                        auto& sub = treq.sub.start;

                        spdlog::info( "Run task ID {}:  folder='{}'", treq.task_id, sub.folder );

                        task_resp res;
                        res.task_id   = treq.task_id;
                        res.which_sub = task_resp_started_tag;

                        std::array< char*, 32 > args;
                        std::size_t             i = 0;
                        args[i++]                 = "--login";
                        args[i++]                 = "-c";
                        args[i++]                 = "exec \"$@\"";
                        args[i++]                 = "--";
                        for ( npb_str* p = sub.args; p != nullptr && i < args.size() - 1;
                              p          = p->next ) {
                                // XXX: dropping const qualifier, fix in near future
                                args[i++] = (char*) p->str;
                        }
                        args[i] = nullptr;
                        if ( i == args.size() - 1 ) {
                                spdlog::error( "Too many args for task execution" );
                                res.sub.started = false;
                        }

                        static constexpr std::size_t n = 128;

                        auto sp = mem.make_span< char >( n );
                        std::snprintf(
                            sp.data(), n, "%s/%s", fctx.workdir.string().c_str(), sub.folder );

                        co_await task_start(
                            ctx, pctx, treq.task_id, "/bin/bash", sp.data(), args );


                        reply           = prepare_reply( ctx.loop, msg.req_id );
                        reply.which_sub = unit_to_hub_task_tag;
                        reply.sub.task  = res;
                        break;
                }
                case task_req_progress_tag: {
                        spdlog::info( "Progress request for task ID {}", treq.task_id );

                        task_resp res;
                        res.task_id      = treq.task_id;
                        res.which_sub    = task_resp_progress_tag;
                        res.sub.progress = task_progress_resp{};

                        auto  progress = co_await task_progress( ctx, pctx, treq.task_id );
                        auto& evt      = progress.event;
                        if ( auto* x = std::get_if< proc_stream::exit_evt >( &evt ) ) {
                                res.sub.progress.which_sub = task_progress_resp_exit_status_tag;
                                res.sub.progress.sub.exit_status = x->exit_status;
                        } else if ( auto* x = std::get_if< proc_stream::stdout_evt >( &evt ) ) {
                                res.sub.progress.which_sub = task_progress_resp_sout_tag;
                                auto& s                    = res.sub.progress.sub.sout;
                                s                          = copy( mem, x->mem );
                        } else if ( auto* x = std::get_if< proc_stream::stderr_evt >( &evt ) ) {
                                res.sub.progress.which_sub = task_progress_resp_serr_tag;
                                auto& s                    = res.sub.progress.sub.serr;
                                s                          = copy( mem, x->mem );
                        }
                        res.sub.progress.events_left = progress.events_n;

                        spdlog::info(
                            "Reporting {} events left for task ID {} with subkind {}",
                            res.sub.progress.events_left,
                            treq.task_id,
                            res.sub.progress.which_sub );
                        reply           = prepare_reply( ctx.loop, msg.req_id );
                        reply.which_sub = unit_to_hub_task_tag;
                        reply.sub.task  = res;
                        break;
                }
                case task_req_cancel_tag: {
                        spdlog::info( "Cancel request for task ID {}", treq.task_id );

                        task_resp res;
                        res.task_id = treq.task_id;

                        reply           = prepare_reply( ctx.loop, msg.req_id );
                        reply.which_sub = unit_to_hub_task_tag;
                        reply.sub.task  = res;
                        break;
                }
                default:
                        spdlog::warn( "Unknown task_req sub type: {}", treq.which_sub );
                        break;
                }
                break;
        }
        case hub_to_unit_list_folder_tag: {
                spdlog::info( "Received list_folder message" );

                auto& sub = msg.sub.list_folder;

                spdlog::info( "List folder request: offset={}, limit={}", sub.offset, sub.limit );

                list_folders_resp res = {};

                auto it = folctx.flds.rbegin();
                std::advance( it, std::min( (size_t) sub.offset, folctx.flds.size() ) );

                size_t count = 0;
                for ( ; it != folctx.flds.rend() && count < sub.limit; ++it, ++count ) {
                        auto* p = (char*) mem.allocate( sizeof( npb_str ), alignof( npb_str ) );
                        if ( !p ) {
                                spdlog::error( "Memory allocation failed for folder entry" );
                                break;
                        }
                        spdlog::debug( "Adding folder entry: {}", it->name );
                        res.entries = new ( p ) npb_str{
                            .next = res.entries,
                            .str  = it->name,
                        };
                }

                reply                 = prepare_reply( ctx.loop, msg.req_id );
                reply.which_sub       = unit_to_hub_list_folder_tag;
                reply.sub.list_folder = res;
                break;
        }
        case hub_to_unit_folder_ctl_tag: {
                spdlog::info( "Received folder_ctl message" );

                auto&           sub = msg.sub.folder_ctl;
                folder_ctl_resp res = {};
                std::strncpy( res.folder, sub.folder, sizeof( res.folder ) );


                switch ( sub.which_sub ) {
                case folder_ctl_req_create_tag: {
                        spdlog::info(
                            "Folder control command 'create' for folder '{}'", sub.folder );
                        auto err =
                            co_await ( folder_create( ctx, folctx, sub.folder ) | ecor::sink_err );
                        res.success = !err;
                        break;
                }
                case folder_ctl_req_del_tag: {
                        spdlog::info( "Folder control command 'del' for folder '{}'", sub.folder );
                        auto err =
                            co_await ( folder_delete( ctx, folctx, sub.folder ) | ecor::sink_err );
                        res.success = !err;
                        break;
                }
                case folder_ctl_req_clear_tag: {
                        spdlog::info( "Folder control command 'clear'" );
                        auto err =
                            co_await ( folder_clear( ctx, folctx, sub.folder ) | ecor::sink_err );
                        res.success = !err;
                        break;
                }
                }

                reply                = prepare_reply( ctx.loop, msg.req_id );
                reply.which_sub      = unit_to_hub_folder_ctl_tag;
                reply.sub.folder_ctl = res;
                break;
        }
        case hub_to_unit_list_tasks_tag: {
                spdlog::info( "Received list_tasks message" );

                auto& sub = msg.sub.list_tasks;

                spdlog::info( "List tasks request: offset={}", sub.offset );

                list_tasks_resp res = {};

                reply                = prepare_reply( ctx.loop, msg.req_id );
                reply.which_sub      = unit_to_hub_list_tasks_tag;
                reply.sub.list_tasks = res;
                break;
        }
        }
        co_return reply;
}

inline task< void >
on_raw_msg( task_ctx& ctx, client::promise p, std::span< uint8_t > buffer, auto f )
{
        circular_buffer_memory mem{ buffer };
        npb_istream_ctx        octx{ .buff = p.data, .mem = mem };
        pb_istream_t           stream = npb_istream_from( octx );
        hub_to_unit            hu_msg = {};

        spdlog::debug( "Decoding message: {}", std::vector< int >{ p.data.begin(), p.data.end() } );

        if ( !pb_decode( &stream, hub_to_unit_fields, &hu_msg ) ) {
                spdlog::error( "Decoding error: {}", PB_GET_ERROR( &stream ) );
                co_await error::decoding_failed;
        }

        unit_to_hub reply = co_await f( ctx, mem, hu_msg );

        static constexpr std::size_t repl_size = 1024;

        auto* pp = (uint8_t*) mem.allocate( repl_size, 1 );
        if ( pp == nullptr ) {
                spdlog::error(
                    "Memory allocation failed, {}/{}", mem.used_bytes(), mem.capacity() );
                co_await error::memory_allocation_failed;
        }
        npb_ostream_ctx ictx{ .buff = { pp, repl_size } };
        pb_ostream_t    ostream = npb_ostream_from( ictx );
        if ( !pb_encode( &ostream, unit_to_hub_fields, &reply ) ) {
                spdlog::error( "Encoding error: {}", PB_GET_ERROR( &ostream ) );
                co_await error::encoding_failed;
        }
        spdlog::debug( "Sending: {}", std::vector< int >{ pp, pp + ostream.bytes_written } );
        // XXX: no ignore
        std::ignore = p.fullfill( { pp, ostream.bytes_written } );
        mem.deallocate( pp, repl_size, 1 );
}

template < typename R >
struct unit_transaction_cb : R
{
        unit_transaction_cb( R&& r, unit_ctx& uctx )
          : R( std::move( r ) )
          , uctx( uctx )
        {
        }

        void set_value( client::promise prom ) noexcept
        {
                uctx.slots.emplace_slot(
                    uctx.loop,
                    [&uctx = uctx, prom = std::move( prom )](
                        task_ctx& ctx, std::span< uint8_t > mem_buffer ) mutable -> task< void > {
                            return on_raw_msg(
                                ctx,
                                std::move( prom ),
                                mem_buffer,
                                [&]( task_ctx&               ctx,
                                     circular_buffer_memory& mem,
                                     hub_to_unit             msg ) -> task< unit_to_hub > {
                                        return on_msg(
                                            ctx, mem, uctx.fctx, uctx.folctx, uctx.pctx, msg );
                                } );
                    } );
                R::set_value();
        }

        unit_ctx& uctx;
};

inline task< void > unit_ctx_loop( auto& tctx, unit_ctx& uctx, std::string_view address, int port )
{
        co_await folder_init( tctx, uctx.folctx );

        if ( int e = trctl::client_init( uctx.cl, tctx.loop, address, port ); e ) {
                spdlog::error( "Client init failed: {}", uv_strerror( e ) );
                co_await error::input_error;
        }
        auto r = ecor::repeater(
            uctx.cl.incoming() |
            ecor::then< trctl::unit_transaction_cb, trctl::unit_ctx& >( uctx ) );

        r.start();

        spdlog::info( "Unit context started, connecting to {}:{}", address, port );

        co_await ecor::wait_until_stopped;

        spdlog::info( "Shutting down unit context" );

        co_await uctx.shutdown();

        spdlog::info( "Unit context shut down" );
}

}  // namespace trctl