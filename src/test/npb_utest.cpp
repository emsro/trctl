
#include "iface.hpp"

#include <cstddef>
#include <gtest/gtest.h>

namespace trctl
{

TEST( npb, base )
{
        uint8_t                buffer[1024];
        circular_buffer_memory mem{ std::span{ buffer } };

        std::size_t     n = 128;
        auto*           p = (uint8_t*) mem.allocate( n, 1 );
        npb_ostream_ctx octx{ .buff = std::span{ p, n } };
        pb_ostream_t    ostream = npb_ostream_from( octx );

        hub_to_unit msg = hub_to_unit_init_default;
        msg.ts          = timestamp{
                     .sec  = 123456789,
                     .nsec = 987654321,
        };
        msg.has_ts = true;
        set_sub(
            msg,
            file_transfer_start{
                .filename = (char*) "testfile",
                .seq      = 42,
                .filesize = 12345,
            } );

        EXPECT_TRUE( pb_encode( &ostream, hub_to_unit_fields, &msg ) );
        hub_to_unit     msg2 = hub_to_unit_init_default;
        npb_istream_ctx rctx{ .buff = std::span{ p, ostream.bytes_written }, .mem = mem };
        pb_istream_t    istream = npb_istream_from( rctx );
        EXPECT_TRUE( pb_decode( &istream, hub_to_unit_fields, &msg2 ) );

        EXPECT_EQ( msg.has_ts, msg2.has_ts );
        EXPECT_EQ( msg.ts.sec, msg2.ts.sec );
        EXPECT_EQ( msg.ts.nsec, msg2.ts.nsec );
        EXPECT_EQ( msg.which_sub, msg2.which_sub );
        EXPECT_EQ( msg.sub.file_transfer_start.seq, msg2.sub.file_transfer_start.seq );
        EXPECT_EQ( msg.sub.file_transfer_start.filesize, msg2.sub.file_transfer_start.filesize );
        EXPECT_STREQ( msg.sub.file_transfer_start.filename, msg2.sub.file_transfer_start.filename );
}

}  // namespace trctl