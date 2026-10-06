cmake_minimum_required(VERSION 3.20)
project(load_balancer)

set(CMAKE_CXX_STANDARD 20)

add_executable(load_balancer main.cpp
        listener.cpp
        listener.h
        load_balancer.cpp
        load_balancer.h
        backend_pool.cpp
        backend_pool.h)
