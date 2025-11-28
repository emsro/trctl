#include "iface.hpp"

namespace trctl
{


extern "C" bool
init_msg_callback( pb_istream_t* istream, pb_ostream_t* ostream, pb_field_t const* field )
{
        if ( field->tag == init_msg_mac_addr_tag || field->tag == init_msg_version_tag )
                return npb_handle_string_field( istream, ostream, field );
        else
                return pb_default_field_callback( istream, ostream, field );
}

extern "C" bool file_transfer_start_callback(
    pb_istream_t*     istream,
    pb_ostream_t*     ostream,
    pb_field_t const* field )
{
        if ( field->tag == file_transfer_start_filename_tag )
                return npb_handle_string_field( istream, ostream, field );
        else
                return pb_default_field_callback( istream, ostream, field );
}

extern "C" bool
file_transfer_data_callback( pb_istream_t* istream, pb_ostream_t* ostream, pb_field_t const* field )
{
        if ( field->tag == file_transfer_data_data_tag )
                return npb_handle_data_field( istream, ostream, field );
        else
                return pb_default_field_callback( istream, ostream, field );
}

extern "C" bool
list_folders_resp_callback( pb_istream_t* istream, pb_ostream_t* ostream, pb_field_t const* field )
{
        if ( field->tag == list_folders_resp_entries_tag )
                return npb_handle_repeated_string_field( istream, ostream, field );
        else
                return pb_default_field_callback( istream, ostream, field );
}

extern "C" bool
task_start_req_callback( pb_istream_t* istream, pb_ostream_t* ostream, pb_field_t const* field )
{
        if ( field->tag == task_start_req_args_tag )
                return npb_handle_repeated_string_field( istream, ostream, field );
        else
                return pb_default_field_callback( istream, ostream, field );
}

extern "C" bool
task_progress_resp_callback( pb_istream_t* istream, pb_ostream_t* ostream, pb_field_t const* field )
{
        if ( field->tag == task_progress_resp_sout_tag ||
             field->tag == task_progress_resp_serr_tag ) {
                auto res = npb_handle_data_field( istream, ostream, field );
                if ( istream )
                        *(pb_size_t*) field->pSize = field->tag;
                return res;
        } else
                return pb_default_field_callback( istream, ostream, field );
}


}  // namespace trctl