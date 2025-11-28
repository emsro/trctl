#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <tuple>

namespace trctl
{


class cobs_encoder
{
public:
        cobs_encoder( std::span< uint8_t > target )
          : target( target )
        {
        }

        bool insert( uint8_t b )
        {
                if ( b != uint8_t{ 0 } ) {
                        count += 1;
                        *p = b;
                } else {
                        *last_p = uint8_t{ count };
                        count   = 1;
                        last_p  = p;
                }

                ++p;

                if ( p == target.data() + target.size() )
                        return false;

                if ( count == 255 ) {
                        *last_p = uint8_t{ 255 };
                        count   = 1;
                        last_p  = p++;
                }

                return p != target.data() + target.size();
        }

        std::span< uint8_t > commit() &&
        {
                *last_p = uint8_t{ count };
                return { target.data(), p };
        }

private:
        std::span< uint8_t > target;
        uint8_t*             last_p = target.data();
        uint8_t*             p      = std::next( last_p );
        uint8_t              count  = 1;
};

/// Encodes data from source range into target buffer with Consistent Overhead Byte Stuffing (COBS)
/// encoding, returns bool indicating whenever conversion succeeded and subview used for conversion
/// from target buffer. Note that this does not store 0 at the end.
inline std::tuple< bool, std::span< uint8_t > >
encode_cobs( std::span< uint8_t const > source, std::span< uint8_t > target )
{
        cobs_encoder e( target );
        for ( uint8_t b : source )
                if ( !e.insert( b ) )
                        return { false, {} };
        return { true, std::move( e ).commit() };
}

struct cobs_decoder
{
        [[nodiscard]] std::optional< uint8_t > get( uint8_t inpt ) const
        {
                if ( offset == 1 ) {
                        if ( nonzero )
                                return std::nullopt;
                        else
                                return uint8_t{ 0 };
                }
                return inpt;
        }

        bool non_value_byte()
        {
                return offset == 1 && nonzero;
        }

        void advance( uint8_t inpt )
        {
                if ( offset == 1 ) {
                        nonzero = inpt == uint8_t{ 255 };
                        offset  = static_cast< uint8_t >( inpt );
                } else {
                        offset--;
                }
        }

        [[nodiscard]] std::optional< uint8_t > iter( uint8_t inpt )
        {
                std::optional< uint8_t > const b = get( inpt );
                advance( inpt );
                return b;
        }

        bool    nonzero = false;
        uint8_t offset  = 1;

        cobs_decoder() = default;

        cobs_decoder( uint8_t b )
          : nonzero( b == uint8_t{ 255 } )
          , offset( static_cast< uint8_t >( b ) )
        {
        }
};

/// Decodes data from source range into target buffer with Consistent Overhead Byte Stuffing (COBS)
/// encoding, returns bool indicating whenever conversion succeeded and subview used for conversion
/// from target buffer. Note that this does not expect 0 at the end.
inline std::tuple< bool, std::span< uint8_t > >
decode_cobs( std::span< uint8_t const > source, std::span< uint8_t > target )
{

        uint8_t*     target_current = target.data();
        cobs_decoder dec( source.front() );

        for ( std::size_t i = 1; i < source.size(); ++i ) {
                auto&                    b   = source[i];
                std::optional< uint8_t > val = dec.iter( b );

                if ( !val.has_value() )
                        continue;
                *target_current = *val;
                target_current += 1;

                if ( target_current == target.data() + target.size() )
                        return { false, target };
        }
        return { true, { target.data(), target_current } };
}


}  // namespace trctl
