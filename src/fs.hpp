#pragma once

#include "str.hpp"
#include "task.hpp"
#include "util.hpp"

#include <ecor/ecor.hpp>
#include <spdlog/spdlog.h>
#include <uv.h>

namespace trctl
{

struct fs_guard
{
        fs_guard( uv_fs_t* fs )
          : fs( fs )
        {
        }
        fs_guard( fs_guard const& )            = delete;
        fs_guard& operator=( fs_guard const& ) = delete;
        fs_guard( fs_guard&& other ) noexcept
          : fs( other.fs )
        {
                other.fs = nullptr;
        }
        fs_guard& operator=( fs_guard&& other ) noexcept
        {
                if ( this != &other ) {
                        fs       = other.fs;
                        other.fs = nullptr;
                }
                return *this;
        }
        ~fs_guard()
        {
                if ( fs )
                        uv_fs_req_cleanup( fs );
        }

        uv_fs_t* fs;
};

struct _fs_open
{
        using value_sig = ecor::set_value_t( uv_file );

        uv_loop_t*       loop;
        std::string_view filename;
        int              flags;
        int              mode;
        uv_fs_t          fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_open(
                    loop,
                    (uv_fs_t*) &fs,
                    filename.data(),  // gets copied
                    flags,
                    mode,
                    +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to open file {}: {}",
                                        fs->path,
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Opened file {}, fh={}", fs->path, fs->result );
                            op.recv.set_value( (uv_file) fs->result );
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_open = _sender< _fs_open >;

struct _fs_access
{
        using value_sig = ecor::set_value_t( int );

        uv_loop_t*       loop;
        std::string_view filename;
        int              mode = 0;
        uv_fs_t          fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_access(
                    loop,
                    &fs,
                    filename.data(),  // gets copied
                    mode,
                    +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            spdlog::info( "Access check succeeded for file {}", fs->path );
                            op.recv.set_value( fs->result );
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_access = _sender< _fs_access >;

struct _fs_close
{
        using value_sig = ecor::set_value_t();

        uv_loop_t* loop;
        uv_file    fh;
        uv_fs_t    fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_close(
                    loop, &fs, fh, +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to close file: {}", uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Closed file, fh={}", op.ctx.fh );
                            op.recv.set_value();
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_close = _sender< _fs_close >;

struct _fs_write
{
        using value_sig = ecor::set_value_t();

        uv_loop_t*                 loop;
        uv_file                    fh;
        uint64_t                   offset;
        std::span< uint8_t const > data;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data      = &op;
                uv_buf_t buf = uv_buf_init( (char*) data.data(), data.size() );
                uv_fs_write(
                    loop, &fs, fh, &buf, 1, offset, +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to write file {}: {}",
                                        fs->path ? fs->path : "<unknown>",
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info(
                                "Wrote {} bytes at offset {}", fs->result, op.ctx.offset );
                            op.recv.set_value();
                            uv_fs_req_cleanup( fs );
                    } );
        }
};
using fs_write = _sender< _fs_write >;

struct _fs_read
{
        using value_sig = ecor::set_value_t( std::span< uint8_t > );

        uv_loop_t*           loop;
        uv_file              fh;
        uint64_t             offset;
        std::span< uint8_t > buffer;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data      = &op;
                uv_buf_t buf = uv_buf_init( (char*) buffer.data(), buffer.size() );
                uv_fs_read(
                    loop, &fs, fh, &buf, 1, offset, +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to read file: {}", uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info(
                                "Read from {} bytes at offset {}", fs->result, op.ctx.offset );
                            op.recv.set_value(
                                std::span< uint8_t >( op.ctx.buffer.data(), fs->result ) );
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_read = _sender< _fs_read >;

struct _fs_mkdtemp
{
        using value_sig = ecor::set_value_t( std::string_view );

        uv_loop_t*       loop;
        std::string_view template_path;
        std::string_view buffer;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_mkdtemp(
                    loop,
                    &fs,
                    template_path.data(),  // gets copied
                    +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to create temp dir: {}",
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Created temp dir: {}", fs->path );
                            auto n = std::min( op.ctx.buffer.size(), std::strlen( fs->path ) + 1 );
                            std::memcpy( (void*) op.ctx.buffer.data(), (void*) fs->path, n );
                            op.recv.set_value( op.ctx.buffer.substr( 0, n - 1 ) );
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_mkdtemp = _sender< _fs_mkdtemp >;

struct _fs_opendir
{
        using value_sig = ecor::set_value_t( uv_dir_t* );

        uv_loop_t*  loop;
        char const* path;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_opendir(
                    loop, &fs, path, +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to open dir {}: {}",
                                        fs->path,
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Opened dir: {}", fs->path );
                            op.recv.set_value( (uv_dir_t*) fs->ptr );
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_opendir = _sender< _fs_opendir >;

struct _fs_readdir
{

        using value_sig = ecor::set_value_t( std::span< uv_dirent_t >, fs_guard );

        uv_loop_t*               loop;
        uv_fs_t*                 fs;
        uv_dir_t*                dir;
        std::span< uv_dirent_t > dirs;

        _fs_readdir( uv_loop_t* loop, uv_fs_t* fs, uv_dir_t* dir, std::span< uv_dirent_t > dirs )
          : loop( loop )
          , fs( fs )
          , dir( dir )
          , dirs( dirs )
        {
        }

        template < typename OP >
        void start( OP& op )
        {
                fs->data      = &op;
                dir->dirents  = dirs.data();
                dir->nentries = dirs.size();
                uv_fs_readdir(
                    loop, fs, dir, +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to read dir {}: {}",
                                        fs->path,
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            op.recv.set_value(
                                op.ctx.dirs.subspan( 0, fs->result ), fs_guard{ fs } );
                    } );
        }
};

using fs_readdir = _sender< _fs_readdir >;

struct _fs_closedir
{
        using value_sig = ecor::set_value_t();

        uv_loop_t* loop;
        uv_dir_t*  dir;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_closedir(
                    loop, &fs, dir, +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to close dir {}: {}",
                                        fs->path,
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Closed dir" );
                            op.recv.set_value();
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_closedir = _sender< _fs_closedir >;

struct _fs_mkdir
{
        using value_sig = ecor::set_value_t();

        uv_loop_t*  loop;
        char const* path;
        int         mode = 0755;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_mkdir(
                    loop,
                    &fs,
                    path,  // gets copied
                    mode,
                    +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to create dir {}: {}",
                                        fs->path,
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Created dir: {}", fs->path );
                            op.recv.set_value();
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_mkdir = _sender< _fs_mkdir >;

struct _fs_unlink
{
        using value_sig = ecor::set_value_t();

        uv_loop_t*  loop;
        char const* path;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_unlink(
                    loop,
                    &fs,
                    path,  // gets copied
                    +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to unlink file {}: {}",
                                        fs->path,
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Unlinked file: {}", fs->path );
                            op.recv.set_value();
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_unlink = _sender< _fs_unlink >;

struct _fs_rmdir
{
        using value_sig = ecor::set_value_t();

        uv_loop_t*  loop;
        char const* path;

        uv_fs_t fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;
                uv_fs_rmdir(
                    loop,
                    &fs,
                    path,  // gets copied
                    +[]( uv_fs_t* fs ) -> void {
                            auto& op = *( (OP*) fs->data );
                            if ( fs->result < 0 ) {
                                    spdlog::error(
                                        "Failed to remove dir {}: {}",
                                        fs->path,
                                        uv_strerror( fs->result ) );
                                    op.recv.set_error( error::libuv_error );
                                    return;
                            }
                            spdlog::info( "Removed dir: {}", fs->path );
                            op.recv.set_value();
                            uv_fs_req_cleanup( fs );
                    } );
        }
};

using fs_rmdir = _sender< _fs_rmdir >;

task< void > recursive_dir_iter( auto& ctx, fixed_str::node path, auto&& f )
{
        uv_dir_t* dir = co_await fs_opendir{ ctx.loop, path.str() };

        while ( true ) {
                uv_fs_t     fs2;
                uv_dirent_t dirents[16];
                auto&& [entries, g] =
                    co_await fs_readdir{ ctx.loop, &fs2, dir, std::span{ dirents } };
                if ( entries.size() == 0 )
                        break;

                for ( auto& ent : entries ) {
                        co_await f( ctx, path, ent );
                        if ( ent.type == UV_DIRENT_DIR )
                                co_await recursive_dir_iter( ctx, path( "/" )( ent.name ), f );
                }
        }

        co_await fs_closedir{ ctx.loop, dir };
}

task< void > dir_iter( auto& ctx, fixed_str::node path, auto&& f )
{
        uv_dir_t* dir = co_await fs_opendir{ ctx.loop, path.str() };

        while ( true ) {
                uv_fs_t     fs2;
                uv_dirent_t dirents[16];
                auto&& [entries, g] =
                    co_await fs_readdir{ ctx.loop, &fs2, dir, std::span{ dirents } };
                if ( entries.size() == 0 )
                        break;

                for ( auto& ent : entries )
                        co_await f( ctx, path, ent );
        }

        co_await fs_closedir{ ctx.loop, dir };
}

struct fs_rm_rf_buff_entry
{
        uv_dir_t*       dir;
        fixed_str::node path;
};

struct _fs_rm_rf
{
        using value_sig = ecor::set_value_t();


        uv_loop_t*                       loop;
        fixed_str::node                  path;
        std::span< fs_rm_rf_buff_entry > dirs;

        _fs_rm_rf( uv_loop_t* loop, fixed_str::node path, std::span< fs_rm_rf_buff_entry > dirs )
          : loop( loop )
          , path( path )
          , dirs( dirs )
        {
                if ( path.end() != '/' )
                        this->path = this->path( "/" );
        }

        fs_rm_rf_buff_entry* p = nullptr;
        uv_dirent_t          entry;
        uv_fs_t              fs;

        template < typename OP >
        void start( OP& op )
        {
                fs.data = &op;

                p = dirs.data();
                if ( p == nullptr ) {
                        spdlog::error( "Can't reserve entry: {}", path.str() );
                        op.recv.set_error( error::libuv_error );
                        return;
                }
                p->path     = path;
                std::ignore = uv_fs_opendir( loop, &fs, path.str(), on_opendir< OP > );
        }

        fs_rm_rf_buff_entry* incr_entry()
        {
                if ( ( p + 1 ) == ( dirs.data() + dirs.size() ) )
                        return nullptr;
                p = p + 1;
                return p;
        }

        fs_rm_rf_buff_entry* decr_entry()
        {
                if ( p == dirs.data() )
                        return nullptr;
                p = p - 1;
                return p;
        }

        template < typename OP >
        static void on_opendir( uv_fs_t* fs )
        {
                auto& op = *( (OP*) fs->data );
                if ( fs->result < 0 ) {
                        spdlog::error(
                            "Failed to open dir {} for rm_rf: {}",
                            fs->path,
                            uv_strerror( fs->result ) );
                        op.recv.set_error( error::libuv_error );
                        return;
                }
                spdlog::info( "Opened dir for rm_rf: {}", fs->path );
                op.ctx.p->dir = (uv_dir_t*) fs->ptr;

                uv_fs_req_cleanup( fs );

                do_readdir< OP >( op );
        }

        template < typename OP >
        static void do_readdir( OP& op )
        {
                op.ctx.p->dir->dirents  = &op.ctx.entry;
                op.ctx.p->dir->nentries = 1;
                op.ctx.fs.data          = &op;
                std::ignore =
                    uv_fs_readdir( op.ctx.loop, &op.ctx.fs, op.ctx.p->dir, on_readdir< OP > );
        }

        template < typename OP >
        static void on_readdir( uv_fs_t* fs )
        {
                auto& op = *( (OP*) fs->data );
                if ( fs->result < 0 ) {
                        spdlog::error(
                            "Failed to read dir for rm_rf: {}", uv_strerror( fs->result ) );
                        op.recv.set_error( error::libuv_error );
                        return;
                }
                spdlog::info( "Read dir for rm_rf: {}", op.ctx.p->path.str() );

                if ( fs->result > 0 ) {

                        uv_dirent_t ent = op.ctx.entry;
                        if ( ent.type == UV_DIRENT_DIR ) {
                                auto  fullpath = op.ctx.p->path( ent.name )( "/" );
                                auto* e        = op.ctx.incr_entry();
                                e->path        = fullpath;
                                uv_fs_req_cleanup( fs );
                                op.ctx.fs.data = &op;
                                std::ignore    = uv_fs_opendir(
                                    op.ctx.loop, &op.ctx.fs, fullpath.str(), on_opendir< OP > );
                                return;
                        } else if ( ent.type == UV_DIRENT_FILE ) {
                                auto fullpath = op.ctx.p->path( ent.name );
                                uv_fs_req_cleanup( fs );
                                op.ctx.fs.data = &op;
                                std::ignore    = uv_fs_unlink(
                                    op.ctx.loop, &op.ctx.fs, fullpath.str(), on_unlink< OP > );
                                return;
                        } else {
                                spdlog::warn(
                                    "Unknown dirent type {} for entry {} in rm_rf",
                                    (int) ent.type,
                                    ent.name );
                                uv_fs_req_cleanup( fs );
                                op.recv.set_error( error::libuv_error );
                        }

                } else {
                        uv_fs_req_cleanup( fs );
                        // XXX: do we really want to block?
                        op.ctx.fs.data = &op;
                        std::ignore    = uv_fs_closedir(
                            op.ctx.loop, &op.ctx.fs, op.ctx.p->dir, on_closedir< OP > );
                }
        }
        template < typename OP >
        static void on_unlink( uv_fs_t* fs )
        {
                auto& op = *( (OP*) fs->data );
                if ( fs->result < 0 ) {
                        spdlog::error(
                            "Failed to unlink file for rm_rf {}: {}",
                            op.ctx.p->path.str(),
                            uv_strerror( fs->result ) );
                        op.recv.set_error( error::libuv_error );
                        return;
                }
                spdlog::info( "Unlinked file for rm_rf: {}", op.ctx.path.str() );
                op.ctx.p->path.set_end();

                uv_fs_req_cleanup( fs );

                do_readdir< OP >( op );
        }

        template < typename OP >
        static void on_closedir( uv_fs_t* fs )
        {
                auto& op = *( (OP*) fs->data );
                if ( fs->result < 0 ) {
                        spdlog::error(
                            "Failed to close dir for rm_rf: {}", uv_strerror( fs->result ) );
                        op.recv.set_error( error::libuv_error );
                        return;
                }
                spdlog::info( "Closed dir for rm_rf: {}", op.ctx.path.str() );
                uv_fs_req_cleanup( fs );

                op.ctx.fs.data = &op;
                std::ignore =
                    uv_fs_rmdir( op.ctx.loop, &op.ctx.fs, op.ctx.path.str(), on_rmdir< OP > );
        }

        template < typename OP >
        static void on_rmdir( uv_fs_t* fs )
        {
                auto& op = *( (OP*) fs->data );
                if ( fs->result < 0 ) {
                        spdlog::error(
                            "Failed to remove dir for rm_rf: {}", uv_strerror( fs->result ) );
                        op.recv.set_error( error::libuv_error );
                        return;
                }
                spdlog::info( "Removed dir for rm_rf: {}", op.ctx.path.str() );
                uv_fs_req_cleanup( fs );

                auto* e = op.ctx.decr_entry();
                if ( !e ) {
                        spdlog::info( "Completed rm_rf: {}", op.ctx.path.str() );
                        op.recv.set_value();
                        return;
                }
                e->path.set_end();

                do_readdir< OP >( op );
        }
};

using fs_rm_rf = _sender< _fs_rm_rf >;

}  // namespace trctl