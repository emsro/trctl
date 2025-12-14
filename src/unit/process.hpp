#pragma once

#include "../npb_extra.h"
#include "../task.hpp"
#include "../util.hpp"
#include "../util/async_optional.hpp"
#include "../util/async_queue.hpp"
#include "../util/async_storage.hpp"

#include <deque>
#include <ranges>

namespace trctl
{
struct mem_buff
{
        uint8_t* mem;
        size_t   size;

        mem_buff( uint8_t* m, size_t s )
          : mem( m )
          , size( s )
        {
        }

        ~mem_buff()
        {
                if ( mem )
                        free( mem );
        }

        mem_buff( mem_buff const& )            = delete;
        mem_buff& operator=( mem_buff const& ) = delete;
        mem_buff( mem_buff&& other ) noexcept
          : mem( other.mem )
          , size( other.size )
        {
                other.mem  = nullptr;
                other.size = 0;
        }
};

inline npb_data copy( circular_buffer_memory& mem, mem_buff& b )
{
        npb_data d;
        d.data = (uint8_t*) mem.allocate( b.size, 1 );
        std::memcpy( d.data, b.mem, b.size );
        d.size = b.size;
        return d;
}

struct proc_stream
{

        struct stdout_evt
        {
                mem_buff mem;
        };

        struct stderr_evt
        {
                mem_buff mem;
        };

        struct exit_evt
        {
                int64_t exit_status;
        };

        using evt_var     = std::variant< stdout_evt, stderr_evt, exit_evt >;
        using data_stream = async_queue< evt_var >;

        void enque( stdout_evt item )
        {
                _stream.enque( std::move( item ) );
        }
        void enque( stderr_evt item )
        {
                _stream.enque( std::move( item ) );
        }
        void enque( exit_evt item )
        {
                _exit_status.emplace( item.exit_status );
                _stream.enque_all( std::move( item ) );
        }

        auto& exit_status()
        {
                return _exit_status;
        }

        auto deque()
        {
                return _stream.deque();
        }

private:
        data_stream               _stream;
        async_optional< int64_t > _exit_status;
};

struct proc : zll::ll_base< proc >
{
        uv_loop_t*            loop;
        zll::ll_list< proc >& finished_procs;

        uv_process_t         process;
        uv_process_options_t options = {};
        uv_stdio_container_t stdio[3];
        uv_pipe_t            stdin_pipe, stdout_pipe, stderr_pipe;
        proc_stream          stream;

        proc( async_ptr_source< proc >, uv_loop_t* loop, zll::ll_list< proc >& finished_procs )
          : loop( loop )
          , finished_procs( finished_procs )
        {
                uv_pipe_init( loop, &stdin_pipe, 0 );
                uv_pipe_init( loop, &stdout_pipe, 0 );
                uv_pipe_init( loop, &stderr_pipe, 0 );

                options.stdio   = stdio;
                options.exit_cb = +[]( uv_process_t* process,
                                       int64_t       exit_status,
                                       int           term_signal ) {
                        spdlog::info(
                            "Process exited with status {}, signal {}", exit_status, term_signal );
                        auto& self = *static_cast< proc* >( process->data );
                        self.on_exit( exit_status, term_signal );
                        uv_close( (uv_handle_t*) process, NULL );
                };
                options.stdio_count  = std::size( stdio );
                stdio[0].flags       = (uv_stdio_flags) ( UV_CREATE_PIPE | UV_READABLE_PIPE );
                stdio[0].data.stream = (uv_stream_t*) &stdin_pipe;
                stdio[1].flags       = (uv_stdio_flags) ( UV_CREATE_PIPE | UV_WRITABLE_PIPE );
                stdio[1].data.stream = (uv_stream_t*) &stdout_pipe;
                stdio[2].flags       = (uv_stdio_flags) ( UV_CREATE_PIPE | UV_WRITABLE_PIPE );
                stdio[2].data.stream = (uv_stream_t*) &stderr_pipe;
        }

        static void alloc_read_stream( uv_handle_t*, size_t suggested_size, uv_buf_t* buf )
        {
                buf->base = (char*) malloc( suggested_size );
                buf->len  = suggested_size;
        }

        template < bool is_err >
        static void on_msg( uv_stream_t* stream, ssize_t nread, uv_buf_t const* buf )
        {
                spdlog::debug( "Received message on stream: {} bytes", nread );
                if ( nread < 0 ) {
                        if ( nread != UV_EOF )
                                spdlog::error( "Read error {}", uv_err_name( nread ) );
                        free( buf->base );
                        uv_close( (uv_handle_t*) stream, nullptr );
                        return;
                }
                auto& s = *static_cast< proc* >( stream->data );
                if ( is_err )
                        s.stream.enque(
                            proc_stream::stderr_evt{
                                mem_buff{ (uint8_t*) buf->base, (size_t) nread } } );
                else
                        s.stream.enque(
                            proc_stream::stdout_evt{
                                mem_buff{ (uint8_t*) buf->base, (size_t) nread } } );
        }

        void on_exit( int exit_status, int )
        {
                stream.enque( proc_stream::exit_evt{ exit_status } );
                finished_procs.link_back( *this );
        }

        bool start( char const* binary, char const* cwd, char** args )
        {
                process.data = this;
                options.file = binary;
                options.args = args;
                options.cwd  = cwd;
                spdlog::info( "Starting: {}{} in folder: {}", binary, joined( args ), cwd );
                if ( int e = uv_spawn( loop, &process, &options ); e < 0 ) {
                        spdlog::error( "uv_spawn failed: {}", uv_strerror( e ) );
                        process.data = nullptr;
                        return false;
                }
                stdout_pipe.data = this;
                if ( int e = uv_read_start(
                         (uv_stream_t*) &stdout_pipe, alloc_read_stream, on_msg< false > );
                     e < 0 ) {
                        spdlog::error( "uv_read_start stdout failed: {}", uv_strerror( e ) );
                }

                stderr_pipe.data = this;
                if ( int e = uv_read_start(
                         (uv_stream_t*) &stderr_pipe, alloc_read_stream, on_msg< true > );
                     e < 0 ) {
                        spdlog::error( "uv_read_start stderr failed: {}", uv_strerror( e ) );
                }
                return true;
        }

        proc( proc const& )            = delete;
        proc& operator=( proc const& ) = delete;
        proc( proc&& )                 = delete;
        proc& operator=( proc&& )      = delete;
};

task< void > destroy( auto&, proc& p )
{
        if ( !p.process.data )
                co_return;

        uv_read_stop( (uv_stream_t*) &p.stdout_pipe );
        uv_read_stop( (uv_stream_t*) &p.stderr_pipe );
        uv_process_kill( &p.process, SIGTERM );

        co_await p.stream.exit_status();

        p.process.data = nullptr;
}

struct proc_ctx : comp_buff, component
{
        uint8_t                     proc_mem[1024];
        async_map< uint32_t, proc > procs;

        proc_ctx( uv_loop_t* loop, task_core& core )
          : component( loop, core, comp_buff::buffer )
          , procs( loop, core, proc_mem )
        {
        }

        void tick() override
        {
                clear_procs( finished_procs );
        }

        task< void > shutdown() override
        {
                spdlog::info( "Shutting down procs: {} procs", procs.size() );
                co_await procs.shutdown();
                spdlog::info( "All procs killed" );
                co_return;
        }

        zll::ll_list< proc > finished_procs;

private:
        void clear_procs( zll::ll_list< proc >& procs )
        {
                while ( !procs.empty() ) {
                        auto& p = procs.take_front();
                        procs.remove_if( [&]( auto& x ) {
                                return &x == &p;
                        } );
                }
        }
};


task< void > task_start(
    auto&              tctx,
    proc_ctx&          ctx,
    uint32_t           task_id,
    char const*        binary,
    char const*        cwd,
    std::span< char* > args )
{
        auto [it, inserted] = ctx.procs.try_emplace( task_id, tctx.loop, ctx.finished_procs );
        if ( !inserted ) {
                spdlog::error( "Task with ID {} already exists", task_id );
                co_yield ecor::with_error{ error::input_error };
        }
        auto& p = it->second;
        if ( !p->start( binary, cwd, args.data() ) ) {
                ctx.procs.erase( it );
                co_yield ecor::with_error{ error::libuv_error };
        }
        spdlog::debug( "Task with ID {} started", task_id );
}

struct progress_report
{
        proc_stream::evt_var event;
        std::size_t          events_n = 0;
};


task< progress_report > task_progress( auto&, proc_ctx& ctx, uint32_t task_id )
{
        auto it = ctx.procs.find( task_id );
        if ( it == ctx.procs.end() ) {
                spdlog::error( "Task with ID {} not found", task_id );
                co_yield ecor::with_error{ error::input_error };
        }
        async_ptr< proc >& p = it->second;

        spdlog::debug( "Task with ID {} progress requested", task_id );

        if ( p->stream.exit_status().has_value() ) {
                co_return progress_report{
                    .event = proc_stream::exit_evt{ co_await p->stream.exit_status() },
                };
        }
        auto dq = co_await p->stream.deque();
        if ( auto* x = std::get_if< proc_stream::stdout_evt >( &dq ) ) {
                co_return progress_report{
                    .event = proc_stream::stdout_evt{ std::move( x->mem ) },
                };
        } else if ( auto* x = std::get_if< proc_stream::stderr_evt >( &dq ) ) {
                co_return progress_report{
                    .event = proc_stream::stderr_evt{ std::move( x->mem ) },
                };
        }
        auto* x = std::get_if< proc_stream::exit_evt >( &dq );
        if ( !x )
                co_yield ecor::with_error{ error::internal_error };
        co_return progress_report{
            .event = proc_stream::exit_evt{ x->exit_status },
        };
}

task< void > task_cancel( auto& tctx, proc_ctx& ctx, uint32_t task_id )
{
        auto it = ctx.procs.find( task_id );
        if ( it == ctx.procs.end() ) {
                spdlog::error( "Task with ID {} not found", task_id );
                co_yield ecor::with_error{ error::input_error };
        }
        async_ptr< proc >& p = it->second;
        spdlog::info( "Cancelling task ID {}", task_id );
        co_await destroy( tctx, *p );
        ctx.procs.erase( it );
        co_return;
}

task< std::span< uint32_t > >
task_list( auto&, proc_ctx& ctx, uint32_t offset, std::span< uint32_t > out )
{
        auto iter = ctx.procs.begin();
        std::advance( iter, std::min( (size_t) offset, ctx.procs.size() ) );
        size_t count = 0;
        for ( ; iter != ctx.procs.end() && count < out.size(); ++iter, ++count )
                out[count] = iter->first;
        co_return out.subspan( 0, count );
}

}  // namespace trctl