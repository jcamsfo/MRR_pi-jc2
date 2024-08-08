cmake_minimum_required(VERSION 3.10)

project(MRR_Pi)



find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT libavformat REQUIRED)
pkg_check_modules(AVCODEC libavcodec REQUIRED)
pkg_check_modules(AVUTIL libavutil REQUIRED)
pkg_check_modules(SWSCALE libswscale REQUIRED)
find_package(OpenCV REQUIRED)


set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include_directories(../)

add_executable(${PROJECT_NAME}_server test_server.cpp comms.cpp comms.h)

target_link_libraries(${PROJECT_NAME}_server ${CMAKE_THREAD_LIBS_INIT})


add_executable(${PROJECT_NAME}_client test_client.cpp comms.cpp comms.h)

target_link_libraries(${PROJECT_NAME}_client ${CMAKE_THREAD_LIBS_INIT})


add_executable(${PROJECT_NAME}_server_2 test_server_2.cpp comms.cpp comms.h)

target_link_libraries(${PROJECT_NAME}_server_2 ${CMAKE_THREAD_LIBS_INIT})


add_executable(${PROJECT_NAME}_client_2 test_client_2.cpp comms.cpp comms.h)

target_link_libraries(${PROJECT_NAME}_client_2 ${CMAKE_THREAD_LIBS_INIT})


target_link_libraries(${PROJECT_NAME}_server_2 ${AVFORMAT_LIBRARIES}
                      ${AVCODEC_LIBRARIES}
                      ${AVUTIL_LIBRARIES}
                      ${SWSCALE_LIBRARIES}
                      ${OpenCV_LIBS})



# to build xcode project
#   cd xbuild
#   cmake .. -GXcode



