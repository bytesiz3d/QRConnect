cmake_minimum_required(VERSION 3.20)
project(QRConnect CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(BUILD_SHARED_LIBS OFF)

set(FETCHCONTENT_QUIET OFF)
include(FetchContent)
FetchContent_Declare(zxing-cpp
	GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
	GIT_SHALLOW TRUE
	GIT_TAG v2.1.0)
FetchContent_MakeAvailable(zxing-cpp)

add_compile_definitions(UNICODE _UNICODE)

add_executable(QRConnect QRConnect.cpp)
target_link_libraries(QRConnect ZXing::ZXing 
	ole32 uuid mf mfplat mfuuid mfreadwrite Strmiids wmcodecdspuuid evr d3d9 wlanapi)