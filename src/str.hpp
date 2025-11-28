#pragma once

#include <span>
#include <string_view>

namespace trctl
{

struct fixed_str
{
        fixed_str( std::span< char > buff )
          : p( buff.data() )
          , e( buff.data() + buff.size() )
        {
        }

        char* p;
        char* e;

        struct node
        {
                fixed_str* path;
                char*      p;  // points to end of last string

                node operator()( std::string_view sv )
                {
                        auto pp = p + sv.size() + 1;
                        if ( pp > path->e )
                                pp = path->e;
                        std::memcpy( p, sv.data(), pp - p - 1 );
                        *( pp - 1 ) = '\0';
                        return { path, pp - 1 };
                }

                void set_end()
                {
                        *p = '\0';
                }

                char* str() const
                {
                        return path->p;
                }
        };


        node operator()( std::string_view sv )
        {
                auto pp = p + sv.size() + 1;
                if ( pp > e )
                        pp = e;
                std::memcpy( p, sv.data(), pp - p - 1 );
                *( pp - 1 ) = '\0';
                return { this, pp - 1 };
        }
};


}  // namespace trctl