#pragma once

#include "npb.hpp"

#include <iface.pb.h>
#include <utility>

namespace trctl
{

inline void set_get_init( hub_to_unit& msg )
{
        msg.which_sub = hub_to_unit_init_tag;
        msg.sub.init  = unit_init_default;
}

inline void set_sub( hub_to_unit& msg, file_transfer_start&& val, uint32_t seq )
{
        msg.which_sub                   = hub_to_unit_file_transfer_tag;
        msg.sub.file_transfer.seq       = seq;
        msg.sub.file_transfer.which_sub = file_transfer_req_start_tag;
        msg.sub.file_transfer.sub.start = std::move( val );
}

inline void set_sub( hub_to_unit& msg, file_transfer_data&& val, uint32_t seq )
{
        msg.which_sub                   = hub_to_unit_file_transfer_tag;
        msg.sub.file_transfer.seq       = seq;
        msg.sub.file_transfer.which_sub = file_transfer_req_data_tag;
        msg.sub.file_transfer.sub.data  = std::move( val );
}

inline void set_sub( hub_to_unit& msg, file_transfer_end&& val, uint32_t seq )
{
        msg.which_sub                   = hub_to_unit_file_transfer_tag;
        msg.sub.file_transfer.seq       = seq;
        msg.sub.file_transfer.which_sub = file_transfer_req_end_tag;
        msg.sub.file_transfer.sub.end   = std::move( val );
}

}  // namespace trctl