# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

# special case skip regeneration
cmake_minimum_required(VERSION 3.16)
project(pixelgadget LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(pixelgadget
    main.cpp
)

target_link_libraries(pixelgadget PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Widgets
)

