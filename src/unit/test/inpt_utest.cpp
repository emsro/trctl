
#include "../../test/tutil.hpp"
#include "../src/npb.hpp"
#include "../src/util.hpp"
#include "../unit.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <iface.pb.h>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace trctl
{

enum class message_type
{
        init,
        file_transfer_start,
        file_transfer_data,
        file_transfer_end,
        folder_ctl,
        task_start,
        task_progress,
        task_cancel,
        list_folder,
        list_tasks,
        file,
        task
};

struct fields_map
{
        using map_type = std::map< std::string, std::string, std::less<> >;

        std::string take( std::string_view name )
        {
                if ( auto x = try_take( name ) )
                        return std::move( *x );
                spdlog::error( "Missing arg: {}", name );
                throw std::runtime_error{ "Missing arg " };
        }

        std::optional< std::string > try_take( std::string_view name )
        {
                auto it = _fields.find( name );
                if ( it == _fields.end() )
                        return {};
                auto res = std::move( it->second );
                _fields.erase( it );
                return res;
        }

        auto& operator=( map_type&& m )
        {
                _fields = std::move( m );
                return *this;
        }

        void finalize()
        {
                if ( _fields.empty() )
                        return;
                for ( auto& [k, v] : _fields )
                        spdlog::error( "{}: {}", k, v );
                throw std::runtime_error{ "Got leftover fields" };
        }

private:
        std::map< std::string, std::string, std::less<> > _fields;
};

struct send_command
{
        uint64_t     req_id;
        message_type msg_type;
        fields_map   fields;
};

struct recv_command
{
        uint64_t     req_id;
        message_type msg_type;
        fields_map   fields;
};

struct executor_command
{
        enum class kind
        {
                checksum,
                exists,
                not_exists,
                folder_empty,
                active_transfers,
                active_tasks,
                skip
        };

        kind       cmd;
        fields_map fields;
};

using test_command = std::variant< send_command, recv_command, executor_command >;

struct test_case
{
        std::string                 title;
        std::vector< test_command > commands;
};


namespace parser
{

struct parser
{
        char const* p;
        char const* e;
        int         line_num = 0;

        std::filesystem::path const&         data_folder;
        std::map< std::string, std::string > file_cache;

        [[noreturn]] void error( std::string const& msg ) const
        {
                throw std::runtime_error{ "Line " + std::to_string( line_num ) + ": " + msg };
        }

        std::string& get_file_contents( std::string const& path )
        {
                auto it = file_cache.find( path );
                if ( it != file_cache.end() )
                        return it->second;

                std::ifstream file{ data_folder / path };
                if ( !file.is_open() )
                        throw std::runtime_error{ "Failed to open file: " + path };
                std::string contents{
                    std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >() };
                return file_cache[path] = std::move( contents );
        }

        parser( std::string_view sv, std::filesystem::path const& df )
          : p( sv.data() )
          , e( sv.data() + sv.size() )
          , data_folder( df )
        {
        }

        char take_char()
        {
                if ( p < e ) {
                        char c = *p++;
                        if ( c == '\n' )
                                line_num++;
                        return c;
                } else
                        return '\n';
        }

        std::string_view take_line()
        {
                char const* start = p;
                while ( p < e ) {
                        if ( *p == '\n' ) {
                                std::string_view sv{ start, (std::size_t) ( p - start ) };
                                p++;
                                line_num++;
                                return sv;
                        }
                        p++;
                }
                std::string_view sv{ start, (std::size_t) ( p - start ) };
                return sv;
        }

        operator bool() const
        {
                return p < e;
        }
};

std::string_view trim( std::string_view sv )
{
        while ( !sv.empty() && std::isspace( sv.front() ) )
                sv.remove_prefix( 1 );
        while ( !sv.empty() && std::isspace( sv.back() ) )
                sv.remove_suffix( 1 );
        return sv;
}

std::vector< std::string > split( std::string_view sv, char delim = ' ' )
{
        std::vector< std::string > tokens;
        std::string                cur;
        cur.reserve( sv.size() );

        bool escape = false;
        for ( size_t i = 0; i < sv.size(); ++i ) {
                char c = sv[i];
                if ( escape ) {
                        // Append escaped character verbatim
                        cur.push_back( c );
                        escape = false;
                        continue;
                }

                if ( c == '\\' ) {
                        escape = true;
                        continue;
                }

                if ( c == delim ) {
                        auto tv = trim( std::string_view{ cur } );
                        if ( !tv.empty() )
                                tokens.emplace_back( tv );
                        cur.clear();
                        continue;
                }

                cur.push_back( c );
        }

        if ( escape )
                cur.push_back( '\\' );

        auto tv = trim( std::string_view{ cur } );
        if ( !tv.empty() )
                tokens.emplace_back( tv );

        return tokens;
}

std::optional< message_type > parse_message_type( std::string_view s )
{
        if ( s == "init" )
                return message_type::init;
        if ( s == "file_transfer_start" )
                return message_type::file_transfer_start;
        if ( s == "file_transfer_data" )
                return message_type::file_transfer_data;
        if ( s == "file_transfer_end" )
                return message_type::file_transfer_end;
        if ( s == "file" )
                return message_type::file;
        if ( s == "task" )
                return message_type::task;
        if ( s == "folder_ctl" )
                return message_type::folder_ctl;
        if ( s == "task_start" )
                return message_type::task_start;
        if ( s == "task_progress" )
                return message_type::task_progress;
        if ( s == "task_cancel" )
                return message_type::task_cancel;
        if ( s == "list_folder" )
                return message_type::list_folder;
        if ( s == "list_tasks" )
                return message_type::list_tasks;
        return std::nullopt;
}

std::optional< executor_command::kind > parse_executor_kind( std::string_view s )
{
        if ( s == "checksum" )
                return executor_command::kind::checksum;
        if ( s == "skip" )
                return executor_command::kind::skip;
        if ( s == "exists" )
                return executor_command::kind::exists;
        if ( s == "not_exists" )
                return executor_command::kind::not_exists;
        if ( s == "folder_empty" )
                return executor_command::kind::folder_empty;
        if ( s == "active_transfers" )
                return executor_command::kind::active_transfers;
        if ( s == "active_tasks" )
                return executor_command::kind::active_tasks;
        return std::nullopt;
}

std::map< std::string, std::string, std::less<> >
parse_fields( std::vector< std::string > const& tokens, size_t start_idx, parser& p )
{
        std::map< std::string, std::string, std::less<> > fields;
        for ( size_t i = start_idx; i < tokens.size(); ++i ) {
                auto colon = tokens[i].find( ':' );
                if ( colon == std::string_view::npos )
                        p.error( "Invalid field format: " + std::string( tokens[i] ) );
                auto key   = tokens[i].substr( 0, colon );
                auto value = tokens[i].substr( colon + 1 );

                fields[std::string( key )] = std::string( value );
        }
        return fields;
}

void autoload_from_file( auto& fields, parser& p )
{
        if ( auto iter = fields.find( "data" );
             iter != fields.end() && iter->second.starts_with( "@" ) ) {
                std::string contents = p.get_file_contents( iter->second.substr( 1 ) );
                if ( auto ii = fields.find( "offset" ); ii != fields.end() ) {
                        size_t offset = std::stoull( ii->second );
                        contents      = contents.substr( std::min( offset, contents.size() ) );
                }
                iter->second = std::move( contents );
        }
}

test_command parse_command( parser& p )
{
        char c    = p.take_char();
        auto line = p.take_line();
        switch ( c ) {
        case '>': {
                auto tokens = split( line );
                if ( tokens.size() < 2 )
                        p.error( "Invalid send command: expected req_id and message_type" );

                send_command cmd;
                cmd.req_id = std::stoull( std::string( tokens[0] ) );

                auto msg_opt = parse_message_type( tokens[1] );
                if ( !msg_opt )
                        p.error( "Invalid message type: " + std::string( tokens[1] ) );
                cmd.msg_type = *msg_opt;

                auto fields = parse_fields( tokens, 2, p );
                autoload_from_file( fields, p );
                cmd.fields = std::move( fields );
                return std::move( cmd );
        } break;
        case '<': {
                auto tokens = split( line );
                if ( tokens.size() < 2 )
                        p.error( "Invalid recv command: expected req_id and message_type" );

                recv_command cmd;
                cmd.req_id = std::stoull( std::string( tokens[0] ) );

                auto msg_opt = parse_message_type( tokens[1] );
                if ( !msg_opt )
                        p.error( "Invalid message type: " + std::string( tokens[1] ) );
                cmd.msg_type = *msg_opt;

                auto fields = parse_fields( tokens, 2, p );
                autoload_from_file( fields, p );
                cmd.fields = std::move( fields );

                return cmd;
        } break;
        case '|': {
                auto tokens = split( line );
                if ( tokens.empty() )
                        p.error( "Invalid executor command: expected command kind" );

                auto kind_opt = parse_executor_kind( tokens[0] );
                if ( !kind_opt )
                        p.error( "Invalid executor command kind: " + std::string( tokens[0] ) );

                executor_command cmd;
                cmd.cmd     = *kind_opt;
                auto fields = parse_fields( tokens, 1, p );
                autoload_from_file( fields, p );
                cmd.fields = std::move( fields );

                return cmd;
        } break;
        default:
                p.error( "Invalid command prefix: expected '>', '<', or '|'" );
        }
        return {};
}

struct comment_line
{
        std::string_view content;
};

struct empty_line
{
};

std::variant< empty_line, comment_line, test_command > parse_line( parser& p )
{
        char c = p.take_char();
        switch ( c ) {
        case '#': {
                auto line = p.take_line();
                return comment_line{ .content = line.substr( 1 ) };
        } break;
        case '>':
        case '<':
        case '|': {
                p.p--;  // put back
                return parse_command( p );
        } break;
        case '\n':
                return empty_line{};
                break;
        default:
                p.error( "Invalid line prefix: expected '#', '>', '<', '|', or empty line" );
        }
        return {};
}

test_case parse_test_case( parser& p )
{
        test_case tc;
        bool      seen_command = false;

        while ( p ) {
                struct handler
                {
                        test_case& tc;
                        bool&      seen_command;
                        bool&      should_break;

                        void operator()( empty_line ) const
                        {
                                if ( seen_command )
                                        should_break = true;  // End of test case
                        }

                        void operator()( comment_line const& cl ) const
                        {
                                if ( tc.title.empty() )
                                        tc.title = cl.content;
                        }

                        void operator()( test_command const& cmd ) const
                        {
                                tc.commands.push_back( cmd );
                                seen_command = true;
                        }
                };

                bool should_break = false;
                std::visit( handler{ tc, seen_command, should_break }, parse_line( p ) );
                if ( should_break )
                        break;
        }
        return tc;
}

std::vector< test_case > parse( parser& p )
{
        std::vector< test_case > test_cases;
        while ( p )
                test_cases.push_back( parse_test_case( p ) );
        return test_cases;
}

}  // namespace parser


struct inpt_test : public ::testing::Test
{
        inpt_test( test_case const* test_case_ptr )
          : tc( test_case_ptr )
        {
        }

        virtual ~inpt_test() noexcept = default;

        test_case const*      tc;
        test_ctx              ctx;
        task_core             core{ ctx.loop };
        std::filesystem::path workfolder{ "./_work" };

        unit_ctx uctx{ ctx.loop, workfolder, core };
        std::optional< ecor::connect_type< task< void >, ecor::_dummy_receiver > > trans_loop;

        uv_tcp_t server;
        uv_tcp_t server_client;

        uint8_t                              recv_buffer[1024 * 8];
        cobs_receiver                        recv{ recv_buffer };
        std::deque< std::vector< uint8_t > > received_messages;

        uint8_t client_buffer[4096 + 128];

        static void client_alloc( uv_handle_t*, size_t suggested_size, uv_buf_t* buf )
        {
                buf->base = (char*) malloc( suggested_size );
                buf->len  = suggested_size;
        }

        static void client_read( uv_stream_t* cl, ssize_t nread, uv_buf_t const* buf )
        {
                if ( nread < 0 ) {
                        if ( nread != UV_EOF )
                                spdlog::error( "Read error {}", uv_err_name( nread ) );
                        uv_close( (uv_handle_t*) cl, nullptr );
                }

                auto& test = *(inpt_test*) cl->data;
                if ( nread > 0 ) {
                        std::span data = { (uint8_t*) buf->base, (std::size_t) nread };
                        test.recv._handle_rx( data, [&]( std::span< uint8_t > data ) {
                                test.received_messages.emplace_back( data.begin(), data.end() );
                        } );
                }

                free( buf->base );
        }


        void SetUp() override
        {
                SCOPED_TRACE( "case " + tc->title );
                sockaddr_in addr;
                uv_tcp_init( ctx.loop, &server );
                uv_ip4_addr( "0.0.0.0", 0, &addr );
                uv_tcp_bind( &server, (const struct sockaddr*) &addr, 0 );
                server.data        = this;
                server_client.data = this;
                uv_listen(
                    (uv_stream_t*) &server, SOMAXCONN, []( uv_stream_t* server, int status ) {
                            if ( status < 0 ) {
                                    spdlog::error(
                                        "Server connection error {}", uv_strerror( status ) );
                                    return;
                            }
                            inpt_test& test = *(inpt_test*) server->data;
                            uv_tcp_init( test.ctx.loop, &test.server_client );
                            if ( uv_accept( server, (uv_stream_t*) &test.server_client ) == 0 ) {
                                    spdlog::info( "Accepted new connection" );
                                    uv_read_start(
                                        (uv_stream_t*) &test.server_client,
                                        client_alloc,
                                        client_read );
                            } else {
                                    spdlog::error(
                                        "Accepting new connection error {}",
                                        uv_strerror( status ) );
                                    uv_close( (uv_handle_t*) &test.server_client, nullptr );
                                    FAIL();
                            }
                    } );


                auto [ip, port] = get_connection_info( &server, sock_kind::SOCK );

                trans_loop.emplace( unit_ctx_loop( ctx, uctx, "0.0.0.0", port )
                                        .connect( ecor::_dummy_receiver{} ) );
                trans_loop->start();

                while ( uv_handle_get_type( (uv_handle_t*) &server_client ) == UV_UNKNOWN_HANDLE )
                        run_loop( ctx.loop, 10 );
        }

        void TearDown() override
        {
                ctx.stop.request_stop();
                run_loop( ctx.loop, 128 );

                uv_run( ctx.loop, UV_RUN_ONCE );
        }


        void TestBody() override
        {
                SCOPED_TRACE( "case " + tc->title );
                for ( auto const& cmd : tc->commands ) {
                        bool cont = std::visit(
                            [&]( auto const& c ) -> bool {
                                    return execute_command( c );
                            },
                            cmd );
                        if ( !cont )
                                break;
                }
        }

private:
        uint8_t                buffer1[1024 * 8], buffer2[1024 * 8];
        char                   filename_buffer[256];
        circular_buffer_memory mem2{ std::span{ buffer2 } };

        hub_to_unit build_message( send_command cmd, circular_buffer_memory& mem )
        {
                hub_to_unit msg = hub_to_unit_init_default;
                msg.req_id      = cmd.req_id;
                msg.has_ts      = false;

                switch ( cmd.msg_type ) {
                case message_type::init:
                        msg.which_sub = hub_to_unit_init_tag;
                        msg.sub.init  = unit_init_default;
                        break;

                case message_type::file_transfer_start: {
                        file_transfer_req   ftr = file_transfer_req_init_default;
                        file_transfer_start fts = file_transfer_start_init_default;

                        auto flnm = cmd.fields.take( "filename" );
                        strcpy( filename_buffer, flnm.c_str() );
                        fts.filename = filename_buffer;

                        auto fldr = cmd.fields.take( "folder" );
                        strcpy( fts.folder, fldr.c_str() );

                        fts.filesize = std::stoull( cmd.fields.take( "filesize" ) );
                        ftr.seq      = std::stoul( cmd.fields.take( "seq" ) );

                        ftr.which_sub         = file_transfer_req_start_tag;
                        ftr.sub.start         = fts;
                        msg.which_sub         = hub_to_unit_file_transfer_tag;
                        msg.sub.file_transfer = ftr;
                        break;
                }

                case message_type::file_transfer_data: {
                        file_transfer_req  ftr = file_transfer_req_init_default;
                        file_transfer_data ftd = {};
                        ftd.data.data          = nullptr;
                        ftd.data.size          = 0;
                        ftd.offset             = 0;


                        auto  data = cmd.fields.take( "data" );
                        auto* p    = (char*) mem.allocate( data.size(), 1 );
                        if ( !p )
                                throw std::runtime_error{ "not enough memory" };
                        std::memcpy( p, data.data(), data.size() );
                        ftd.data.data = (uint8_t*) p;
                        ftd.data.size = data.size();

                        auto off   = cmd.fields.take( "offset" );
                        ftd.offset = std::stoull( off );

                        auto seq = cmd.fields.take( "seq" );
                        ftr.seq  = std::stoul( seq );

                        ftr.which_sub         = file_transfer_req_data_tag;
                        ftr.sub.data          = ftd;
                        msg.which_sub         = hub_to_unit_file_transfer_tag;
                        msg.sub.file_transfer = ftr;
                        break;
                }

                case message_type::file_transfer_end: {
                        file_transfer_req ftr = file_transfer_req_init_default;
                        file_transfer_end fte = file_transfer_end_init_default;

                        auto seq = cmd.fields.take( "seq" );
                        ftr.seq  = std::stoul( seq );

                        auto fnv1a = cmd.fields.take( "fnv1a" );
                        fte.fnv1a  = std::stoul( "0x" + fnv1a, nullptr, 16 );

                        ftr.which_sub         = file_transfer_req_end_tag;
                        ftr.sub.end           = fte;
                        msg.which_sub         = hub_to_unit_file_transfer_tag;
                        msg.sub.file_transfer = ftr;
                        break;
                }

                case message_type::folder_ctl: {
                        folder_ctl_req fcr = folder_ctl_req_init_default;

                        auto fld = cmd.fields.take( "folder" );
                        strcpy( fcr.folder, fld.c_str() );

                        // Check which oneof field is present
                        if ( cmd.fields.try_take( "create" ) ) {
                                fcr.which_sub  = folder_ctl_req_create_tag;
                                fcr.sub.create = unit_init_default;
                        } else if ( cmd.fields.try_take( "delete" ) ) {
                                fcr.which_sub = folder_ctl_req_del_tag;
                                fcr.sub.del   = unit_init_default;
                        } else if ( cmd.fields.try_take( "clear" ) ) {
                                fcr.which_sub = folder_ctl_req_clear_tag;
                                fcr.sub.clear = unit_init_default;
                        } else {
                                throw std::runtime_error{ "Missing folder op" };
                        }

                        msg.which_sub      = hub_to_unit_folder_ctl_tag;
                        msg.sub.folder_ctl = fcr;
                        break;
                }
                case message_type::list_folder: {
                        list_folders_req lfr = list_folders_req_init_default;
                        lfr.offset           = std::stoi( cmd.fields.take( "offset" ) );
                        lfr.limit            = std::stoi( cmd.fields.take( "limit" ) );
                        msg.which_sub        = hub_to_unit_list_folder_tag;
                        msg.sub.list_folder  = lfr;
                        break;
                }

                case message_type::task_start: {
                        task_req       tr  = task_req_init_default;
                        task_start_req tsr = task_start_req_init_default;

                        tr.task_id = std::stoul( cmd.fields.take( "task_id" ) );
                        auto fldr  = cmd.fields.take( "folder" );
                        strcpy( tsr.folder, fldr.c_str() );

                        {
                                auto      args = cmd.fields.take( "args" );
                                npb_str** last = &tsr.args;
                                *last          = nullptr;

                                std::istringstream argss( args );
                                std::string        arg;
                                while ( std::getline( argss, arg, ',' ) ) {
                                        *last = mem.make< npb_str >( npb_str{
                                                                         .next = nullptr,
                                                                     } )
                                                    .release();
                                        auto* s =
                                            (char*) mem.allocate( arg.size() + 1, alignof( char ) );
                                        strcpy( s, arg.c_str() );
                                        ( *last )->str = s;

                                        last = &( *last )->next;
                                }
                        }

                        tr.which_sub  = task_req_start_tag;
                        tr.sub.start  = tsr;
                        msg.which_sub = hub_to_unit_task_tag;
                        msg.sub.task  = tr;
                        break;
                }

                case message_type::task_progress: {
                        task_req tr     = task_req_init_default;
                        tr.task_id      = std::stoul( cmd.fields.take( "task_id" ) );
                        tr.which_sub    = task_req_progress_tag;
                        tr.sub.progress = unit_init_default;
                        msg.which_sub   = hub_to_unit_task_tag;
                        msg.sub.task    = tr;
                        break;
                }

                case message_type::task_cancel: {
                        task_req tr   = task_req_init_default;
                        tr.task_id    = std::stoul( cmd.fields.take( "task_id" ) );
                        tr.which_sub  = task_req_cancel_tag;
                        tr.sub.cancel = unit_init_default;
                        msg.which_sub = hub_to_unit_task_tag;
                        msg.sub.task  = tr;
                        break;
                }

                case message_type::list_tasks: {
                        list_tasks_req ltr = list_tasks_req_init_default;
                        ltr.offset         = std::stoi( cmd.fields.take( "offset" ) );
                        msg.which_sub      = hub_to_unit_list_tasks_tag;
                        msg.sub.list_tasks = ltr;
                        break;
                }
                default: {
                        break;
                }
                }

                cmd.fields.finalize();

                return msg;
        }

        bool execute_command( send_command const& cmd )
        {
                uint8_t                buffer[1024 * 8];
                circular_buffer_memory mem{ buffer };
                hub_to_unit            msg = build_message( cmd, mem );

                npb_ostream_ctx octx{ .buff = std::span{ buffer1 } };
                pb_ostream_t    stream = npb_ostream_from( octx );

                bool result = pb_encode( &stream, hub_to_unit_fields, &msg );
                if ( !result )
                        EXPECT_TRUE( result )
                            << "Failed to encode message: " << PB_GET_ERROR( &stream );

                spdlog::info(
                    "Sending message {} of size {}: {}",
                    cmd.req_id,
                    stream.bytes_written,
                    std::vector< int >{ buffer1, buffer1 + stream.bytes_written } );
                auto status = cobs_send(
                    mem2, &server_client, { std::data( buffer1 ), stream.bytes_written } );
                EXPECT_EQ( send_status::SUCCESS, status ) << "Failed to send message";
                return true;
        }


        unit_to_hub receive_message()
        {
                for ( std::size_t i = 0; i < 1024 * 2; ++i ) {
                        uv_run( ctx.loop, UV_RUN_NOWAIT );
                        if ( !received_messages.empty() )
                                break;
                }
                if ( received_messages.empty() ) {
                        ADD_FAILURE() << "Timeout waiting for message";
                        return {};
                }
                auto rawmsg = received_messages.front();
                received_messages.pop_front();

                npb_istream_ctx ictx{ .buff = rawmsg, .mem = mem2 };
                pb_istream_t    istream = npb_istream_from( ictx );
                unit_to_hub     msg     = {};

                if ( !pb_decode( &istream, unit_to_hub_fields, &msg ) ) {
                        spdlog::debug(
                            "Error decoding message: {} bytes {}",
                            rawmsg.size(),
                            std::vector< int >{ rawmsg.begin(), rawmsg.end() } );
                        ADD_FAILURE() << "Error receiving message: " << PB_GET_ERROR( &istream );
                        return {};
                }

                return msg;
        }

        void verify_field( fields_map& expected, std::string const& key, std::string_view actual )
        {
                if ( auto x = expected.try_take( key ) )
                        EXPECT_EQ( *x, actual ) << "Field mismatch: " << key;
        }

        void verify_field( fields_map& expected, std::string const& key, char const* actual )
        {
                if ( auto x = expected.try_take( key ) )
                        EXPECT_EQ( *x, actual ) << "Field mismatch: " << key;
        }

        template < typename T >
                requires(
                    std::same_as< T, uint32_t > || std::same_as< T, uint64_t > ||
                    std::same_as< T, int32_t > || std::same_as< T, int64_t > )
        void verify_field( fields_map& expected, std::string const& key, T actual )
        {
                if ( auto x = expected.try_take( key ) )
                        EXPECT_EQ( std::stoull( *x ), actual ) << "Field mismatch: " << key;
        }

        void verify_field( fields_map& expected, std::string const& key, bool actual )
        {
                if ( auto x = expected.try_take( key ) ) {
                        bool expected_val = ( *x == "true" );
                        EXPECT_EQ( expected_val, actual ) << "Field mismatch: " << key;
                }
        }

        void
        verify_field( fields_map& expected, std::string const& key, struct npb_data const& actual )
        {
                auto f = expected.try_take( key );
                if ( f ) {
                        std::string_view sv{ (char const*) actual.data, actual.size };
                        EXPECT_EQ( ( *f ), sv )
                            << "Field mismatch: " << key << " expected: \"" << *f << "\"";
                }
        }

        void verify_field_contains(
            fields_map&            expected,
            std::string const&     key,
            struct npb_data const& actual )
        {
                auto f = expected.try_take( key );
                if ( f ) {
                        std::string_view sv{ (char const*) actual.data, actual.size };
                        bool             match = sv.contains( *f );
                        EXPECT_TRUE( match ) << "Field mismatch: " << key
                                             << " expected to contain: \"" << *f << "\"";
                }
        }

        bool execute_command( recv_command cmd )
        {
                unit_to_hub msg = receive_message();

                SCOPED_TRACE(
                    "Verifying received message for req_id " + std::to_string( cmd.req_id ) );

                // Verify req_id
                EXPECT_EQ( cmd.req_id, msg.req_id )
                    << "Request ID mismatch, expected: " << cmd.req_id;

                // Verify message type and fields
                switch ( cmd.msg_type ) {
                case message_type::init:
                        EXPECT_EQ( unit_to_hub_init_tag, msg.which_sub );
                        verify_field( cmd.fields, "mac_addr", msg.sub.init.mac_addr );
                        verify_field( cmd.fields, "version", msg.sub.init.version );
                        break;

                case message_type::file:
                case message_type::file_transfer_start:
                case message_type::file_transfer_data:
                case message_type::file_transfer_end:
                        EXPECT_EQ( unit_to_hub_file_tag, msg.which_sub );
                        verify_field( cmd.fields, "success", msg.sub.file.success );
                        break;

                case message_type::folder_ctl:
                        EXPECT_EQ( unit_to_hub_folder_ctl_tag, msg.which_sub );
                        verify_field( cmd.fields, "success", msg.sub.folder_ctl.success );
                        verify_field(
                            cmd.fields, "folder", std::string_view{ msg.sub.folder_ctl.folder } );
                        break;

                case message_type::list_folder: {
                        EXPECT_EQ( unit_to_hub_list_folder_tag, msg.which_sub );
                        // Build comma-separated list of entries
                        std::string entries_str;
                        npb_str*    entry = msg.sub.list_folder.entries;
                        while ( entry != nullptr ) {
                                if ( !entries_str.empty() )
                                        entries_str += ",";
                                entries_str += entry->str;
                                entry = entry->next;
                        }
                        verify_field( cmd.fields, "entries", entries_str );
                        break;
                }

                case message_type::task:
                case message_type::task_start:
                case message_type::task_cancel:
                        EXPECT_EQ( unit_to_hub_task_tag, msg.which_sub );
                        verify_field( cmd.fields, "task_id", msg.sub.task.task_id );
                        if ( msg.sub.task.which_sub == task_resp_success_tag )
                                verify_field( cmd.fields, "success", msg.sub.task.sub.success );
                        break;

                case message_type::task_progress: {
                        EXPECT_EQ( unit_to_hub_task_tag, msg.which_sub );
                        verify_field( cmd.fields, "task_id", msg.sub.task.task_id );
                        EXPECT_EQ( task_resp_progress_tag, msg.sub.task.which_sub );
                        auto which_sub = msg.sub.task.sub.progress.which_sub;
                        if ( which_sub == task_progress_resp_sout_tag ) {
                                spdlog::debug( "Trying sout" );
                                verify_field_contains(
                                    cmd.fields, "sout", msg.sub.task.sub.progress.sub.sout );
                        } else if ( which_sub == task_progress_resp_serr_tag ) {
                                spdlog::debug( "Trying serr" );
                                verify_field_contains(
                                    cmd.fields, "serr", msg.sub.task.sub.progress.sub.serr );
                        } else if ( which_sub == task_progress_resp_exit_status_tag ) {
                                spdlog::debug( "Trying exit status" );
                                verify_field(
                                    cmd.fields,
                                    "exit_status",
                                    msg.sub.task.sub.progress.sub.exit_status );
                        } else {
                                EXPECT_TRUE( false );
                                return false;
                        }
                        break;
                }
                case message_type::list_tasks: {
                        EXPECT_EQ( unit_to_hub_list_tasks_tag, msg.which_sub );
                        // Build comma-separated list of task IDs
                        std::string tasks_str;
                        for ( size_t i = 0; i < msg.sub.list_tasks.tasks_count; ++i ) {
                                if ( !tasks_str.empty() )
                                        tasks_str += ",";
                                tasks_str += std::to_string( msg.sub.list_tasks.tasks[i] );
                        }
                        verify_field( cmd.fields, "tasks", tasks_str );
                        break;
                }
                }
                cmd.fields.finalize();
                return true;
        }


        static std::filesystem::path get_path( executor_command& cmd )
        {
                if ( auto x = cmd.fields.try_take( "path" ) )
                        return *x;
                ADD_FAILURE() << "get_path: missing 'path' field";
                throw std::runtime_error( "missing 'path' field" );
        }

        static uint64_t get_fnv1a( executor_command& cmd )
        {
                if ( auto x = cmd.fields.try_take( "fnv1a" ) )
                        return std::stoull( *x, nullptr, 16 );
                ADD_FAILURE() << "get_fnv1a: missing 'fnv1a' field";
                throw std::runtime_error( "missing 'fnv1a' field" );
        }

        static uint64_t get_count( executor_command& cmd )
        {
                if ( auto x = cmd.fields.try_take( "count" ) )
                        return std::stoull( *x );
                ADD_FAILURE() << "get_count: missing 'count' field";
                throw std::runtime_error( "missing 'count' field" );
        }

        bool execute_command( executor_command cmd )
        {
                for ( std::size_t i = 0; i < 100; ++i )
                        uv_run( ctx.loop, UV_RUN_NOWAIT );
                switch ( cmd.cmd ) {
                case executor_command::kind::skip: {
                        return false;
                }
                case executor_command::kind::checksum: {
                        auto path  = get_path( cmd );
                        auto fnv1a = get_fnv1a( cmd );

                        std::filesystem::path file_path = workfolder / path;
                        if ( !std::filesystem::exists( file_path ) ) {
                                ADD_FAILURE() << "checksum: file does not exist: " << file_path;
                                break;
                        }

                        // Read file and compute FNV1a hash
                        std::ifstream file( file_path, std::ios::binary );
                        if ( !file.is_open() ) {
                                ADD_FAILURE() << "checksum: failed to open file: " << file_path;
                                break;
                        }

                        struct fnv1a hasher;
                        char         buffer[4096];
                        while ( file.read( buffer, sizeof( buffer ) ) || file.gcount() > 0 )
                                hasher(
                                    std::span{
                                        (uint8_t const*) buffer,
                                        static_cast< std::size_t >( file.gcount() ) } );

                        EXPECT_EQ( fnv1a, hasher.hash )
                            << "Checksum mismatch for file: " << file_path << " "
                            << " hex: " << std::hex << hasher.hash;
                        break;
                }
                case executor_command::kind::exists: {
                        auto path = workfolder / get_path( cmd );
                        EXPECT_TRUE( exists( path ) ) << "Path should exist: " << path;
                        break;
                }
                case executor_command::kind::not_exists: {
                        auto path = workfolder / get_path( cmd );
                        EXPECT_FALSE( exists( path ) ) << "Path should not exist: " << path;
                        break;
                }
                case executor_command::kind::folder_empty: {

                        auto path = get_path( cmd );

                        std::filesystem::path full_path = workfolder / path;
                        if ( !std::filesystem::exists( full_path ) ) {
                                ADD_FAILURE() << "folder_empty: path does not exist: " << full_path;
                                break;
                        }
                        if ( !std::filesystem::is_directory( full_path ) ) {
                                ADD_FAILURE()
                                    << "folder_empty: path is not a directory: " << full_path;
                                break;
                        }
                        EXPECT_TRUE( std::filesystem::is_empty( full_path ) )
                            << "Folder should be empty: " << full_path;
                        break;
                }
                case executor_command::kind::active_transfers: {

                        size_t expected_count = get_count( cmd );
                        size_t actual_count   = uctx.fctx.transfers.size();
                        EXPECT_EQ( expected_count, actual_count )
                            << "Active transfers count mismatch";
                        break;
                }
                case executor_command::kind::active_tasks: {
                        size_t expected_count = get_count( cmd );
                        size_t actual_count   = uctx.pctx.procs.size();
                        EXPECT_EQ( expected_count, actual_count ) << "Active tasks count mismatch";
                        break;
                }
                }
                cmd.fields.finalize();
                return true;
        }
};

}  // namespace trctl

int main( int argc, char** argv )
{
        using namespace trctl;

        uv_disable_stdio_inheritance();
        testing::InitGoogleTest( &argc, argv );
        spdlog::set_level( spdlog::level::debug );


        if ( argc < 2 ) {
                std::cerr << "Usage: " << argv[0] << " <test_file_path>\n";
                return 1;
        }

        std::filesystem::path test_file_path = argv[1];

        // Read file contents
        std::ifstream file( test_file_path );
        if ( !file.is_open() ) {
                std::cerr << "Failed to open test file: " << test_file_path << "\n";
                return 1;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string input = buffer.str();

        parser::parser           p{ input, test_file_path.parent_path() / "data" };
        std::vector< test_case > test_cases = parser::parse( p );

        for ( auto& tc : test_cases ) {
                std::string test_name = "";
                if ( !tc.title.empty() ) {
                        // Sanitize title for test name
                        std::string sanitized = std::string( tc.title );
                        for ( char& c : sanitized )
                                if ( !std::isalnum( c ) && c != '_' )
                                        c = '_';
                        test_name += sanitized;
                }

                ::testing::RegisterTest(
                    "inpt",
                    test_name.c_str(),
                    nullptr,
                    nullptr,
                    __FILE__,
                    __LINE__,
                    [&tc]() -> ::testing::Test* {
                            return new inpt_test( &tc );
                    } );
        }

        auto*       loop           = uv_default_loop();
        std::string workfolder_str = "./_work/";
        // XXX: the setup for fs_rm_rf gets repeated a lot
        fs_rm_rf_buff_entry dir_buf[32] = {};
        char                dir_path[folder_max_path_l];
        fixed_str           dir_path_str{ dir_path };
        auto                op = fs_rm_rf{ loop, dir_path_str( workfolder_str ), dir_buf }.connect(
            ecor::_dummy_receiver{} );
        op.start();
        run_loop( loop, 500 );
        auto op2 =
            fs_mkdir{ loop, workfolder_str.c_str(), 0700 }.connect( ecor::_dummy_receiver{} );
        op2.start();
        run_loop( loop, 10 );

        return RUN_ALL_TESTS();
}