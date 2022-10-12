cmake_minimum_required(VERSION 3.16)
project(httpd)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DENABLE_LOGGER")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DENABLE_LOGGER -DDISABLE_LOGGER_TO_CONSOLE -DLOGGER_DBG_VIEW")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -DENABLE_LOGGER -DDISABLE_LOGGER_TO_CONSOLE -DLOGGER_DBG_VIEW")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -DENABLE_LOGGER -DDISABLE_LOGGER_TO_CONSOLE -DLOGGER_DBG_VIEW")

file(GLOB httpd_HEADERS
	${CMAKE_CURRENT_SOURCE_DIR}/include/httpd/logging.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/httpd/scoped_exit.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/httpd/use_awaitable.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/httpd/misc.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/httpd/publish_subscribe.hpp
)

file(GLOB httpd_SRCS
	${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
)

add_executable(httpd
	${httpd_HEADERS}
	${httpd_SRCS}
	)

target_link_libraries(httpd
	Boost::asio
	Boost::beast
)


if (MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.29.30129 AND CMAKE_VERSION VERSION_GREATER 3.20.3)
	set_property(TARGET httpd PROPERTY CXX_STANDARD 23)
	message(STATUS "httpd set cxx standard: C++23")
else()
	set_property(TARGET httpd PROPERTY CXX_STANDARD 20)
	message(STATUS "httpd set cxx standard: C++20")
endif()

set(BASE_SOURCE_DIR
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(httpd
	PUBLIC

	${BASE_SOURCE_DIR}
	${Boost_INCLUDE_DIRS}
)
