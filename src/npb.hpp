#pragma once

#include "npb_extra.h"
#include "util.hpp"

#include <pb_decode.h>
#include <pb_encode.h>

namespace trctl
{

// ---------------------------------------------------------------------------

struct npb_istream_ctx
{
        std::span< uint8_t const > buff;
        circular_buffer_memory&    mem;
        size_t                     pos = 0;
};

inline bool npb_istream_cb( pb_istream_t* istream, pb_byte_t* buf, size_t count )
{
        auto* ctx = (npb_istream_ctx*) istream->state;
        if ( ctx->pos + count > ctx->buff.size() )
                return false;
        std::copy_n( ctx->buff.data() + ctx->pos, count, buf );
        ctx->pos += count;
        return true;
}

inline pb_istream_t npb_istream_from( npb_istream_ctx& ctx )
{
        pb_istream_t stream{
            .callback   = &npb_istream_cb,
            .state      = &ctx,
            .bytes_left = ctx.buff.size(),
            .errmsg     = nullptr,
        };
        return stream;
}

inline npb_istream_ctx* ctx_of( pb_istream_t* istream )
{
        return (npb_istream_ctx*) istream->state;
}

// ---------------------------------------------------------------------------

struct npb_ostream_ctx
{
        std::span< uint8_t > buff;
        size_t               pos = 0;
};

inline bool npb_ostream_cb( pb_ostream_t* ostream, pb_byte_t const* buf, size_t count )
{
        auto* ctx = (npb_ostream_ctx*) ostream->state;
        if ( ctx->pos + count > ctx->buff.size() )
                return false;
        std::copy_n( buf, count, ctx->buff.data() + ctx->pos );
        ctx->pos += count;
        return true;
}

inline pb_ostream_t npb_ostream_from( npb_ostream_ctx& ctx )
{
        pb_ostream_t stream{
            .callback      = &npb_ostream_cb,
            .state         = &ctx,
            .max_size      = ctx.buff.size(),
            .bytes_written = 0,
            .errmsg        = nullptr,
        };
        return stream;
}

inline npb_ostream_ctx* ctx_of( pb_ostream_t* ostream )
{
        return (npb_ostream_ctx*) ostream->state;
}

// ---------------------------------------------------------------------------

inline bool
npb_handle_string_field( pb_istream_t* istream, pb_ostream_t* ostream, pb_field_t const* field )
{
        if ( ostream ) {
                char const* str = *(char const**) field->pData;

                if ( !str )
                        return true;

                if ( !pb_encode_tag_for_field( ostream, field ) )
                        return false;

                return pb_encode_string( ostream, (uint8_t const*) str, strlen( str ) );
        }
        if ( istream ) {
                npb_istream_ctx* ctx = ctx_of( istream );
                auto* buffer         = (pb_byte_t*) ctx->mem.allocate( istream->bytes_left + 1, 1 );
                if ( !buffer )
                        return false;

                *(char const**) field->pData = (char const*) buffer;
                memset( buffer, 0, istream->bytes_left + 1 );
                if ( !pb_read( istream, buffer, istream->bytes_left ) )
                        return false;

                return true;
        }
        return false;
}

inline bool npb_handle_repeated_string_field(
    pb_istream_t*     istream,
    pb_ostream_t*     ostream,
    pb_field_t const* field )
{
        if ( ostream ) {
                npb_str* str = *(npb_str**) field->pData;
                for ( ; str != nullptr; str = str->next ) {
                        if ( !pb_encode_tag_for_field( ostream, field ) )
                                return false;

                        if ( !pb_encode_string(
                                 ostream, (uint8_t const*) str->str, strlen( str->str ) ) )
                                return false;
                }
                return true;
        }
        if ( istream ) {
                npb_istream_ctx* ctx = ctx_of( istream );
                npb_str**        trg = (npb_str**) field->pData;
                while ( ( *trg ) )
                        trg = &( ( *trg )->next );

                auto* pp = ctx->mem.allocate( sizeof( npb_str ), alignof( npb_str ) );
                if ( !pp )
                        return false;
                *trg = new ( pp ) npb_str{
                    .str  = nullptr,
                    .next = nullptr,
                };

                auto* p = (char*) ctx->mem.allocate( istream->bytes_left + 1, 1 );
                if ( !p )
                        return false;

                memset( p, 0, istream->bytes_left + 1 );
                ( *trg )->str = p;
                if ( !pb_read( istream, (pb_byte_t*) ( *trg )->str, istream->bytes_left ) )
                        return false;

                return true;
        }
        return false;
}

inline bool
npb_handle_data_field( pb_istream_t* istream, pb_ostream_t* ostream, pb_field_t const* field )
{
        if ( ostream ) {
                struct npb_data* data = (struct npb_data*) field->pData;
                if ( !data )
                        return false;

                if ( !pb_encode_tag_for_field( ostream, field ) )
                        return false;
                if ( !pb_encode_string( ostream, (uint8_t const*) data->data, data->size ) )
                        return false;
                return true;
        }
        if ( istream ) {
                npb_istream_ctx* ctx    = ctx_of( istream );
                auto*            buffer = (pb_byte_t*) ctx->mem.allocate( istream->bytes_left, 1 );
                if ( !buffer )
                        return false;


                struct npb_data* data = (struct npb_data*) field->pData;
                data->data            = buffer;
                data->size            = istream->bytes_left;
                if ( !pb_read( istream, buffer, istream->bytes_left ) )
                        return false;

                return true;
        }
        return false;
}


}  // namespace trctl