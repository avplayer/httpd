//Copyright (c) 2006-2009 Emil Dotchevski and Reverge Studios, Inc.

//Distributed under the Boost Software License, Version 1.0. (See accompanying
//file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#define BOOST_EXCEPTION_DISABLE

#include <boost/config.hpp>

#if defined( BOOST_NO_EXCEPTIONS )
#   error This program requires exception handling.
#endif

#include <boost/throw_exception.hpp>
#include <boost/detail/lightweight_test.hpp>

class my_exception: public std::exception { };

int
main()
    {
    try
        {
        boost::throw_exception(my_exception());
        BOOST_ERROR("boost::throw_exception failed to throw.");
        }
    catch(
    my_exception & )
        {
        }
    catch(
    ... )
        {
        BOOST_ERROR("boost::throw_exception malfunction.");
        }
    return boost::report_errors();
    }
