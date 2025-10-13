#pragma once

#include "npb.hpp"

#include <iface.pb.h>
#include <utility>

namespace trctl
{

inline void set_get_init( hub_to_unit& msg, unit&& val )
{
        msg.which_sub    = hub_to_unit_get_init_tag;
        msg.sub.get_init = std::move( val );
}

inline void set_sub( hub_to_unit& msg, file_transfer_start&& val )
{
        msg.which_sub               = hub_to_unit_file_transfer_start_tag;
        msg.sub.file_transfer_start = std::move( val );
}

inline void set_sub( hub_to_unit& msg, file_transfer_data&& val )
{
        msg.which_sub              = hub_to_unit_file_transfer_data_tag;
        msg.sub.file_transfer_data = std::move( val );
}

inline void set_sub( hub_to_unit& msg, file_transfer_end&& val )
{
        msg.which_sub             = hub_to_unit_file_transfer_end_tag;
        msg.sub.file_transfer_end = std::move( val );
}

}  // namespace trctl