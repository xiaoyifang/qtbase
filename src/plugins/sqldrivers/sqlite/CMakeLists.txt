# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

# Generated from sqlite.pro.

#####################################################################
## QSQLiteDriverPlugin Plugin:
#####################################################################

qt_internal_add_plugin(QSQLiteDriverPlugin
    OUTPUT_NAME qsqlite
    PLUGIN_TYPE sqldrivers
    SOURCES
        qsql_sqlite.cpp qsql_sqlite_p.h
        smain.cpp
    DEFINES
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
    LIBRARIES
        Qt::Core
        Qt::CorePrivate
        Qt::SqlPrivate
)

#### Keys ignored in scope 1:.:.:sqlite.pro:<TRUE>:
# OTHER_FILES = "sqlite.json"
# QT_FOR_CONFIG = "sqldrivers-private"

## Scopes:
#####################################################################

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION QT_FEATURE_system_sqlite
    LIBRARIES
        SQLite::SQLite3
)

# special case begin
if (NOT QT_FEATURE_system_sqlite)
    # On newer compilers compiling sqlite.c produces warnings
    qt_disable_warnings(QSQLiteDriverPlugin)
endif()

if(QT_FEATURE_system_sqlite)
    qt_internal_force_macos_intel_arch(QSQLiteDriverPlugin)
endif()
# special case end

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION NOT QT_FEATURE_system_sqlite
    SOURCES
        ../../../3rdparty/sqlite/sqlite3.c
    DEFINES
        SQLITE_ENABLE_COLUMN_METADATA
        SQLITE_ENABLE_FTS3
        SQLITE_ENABLE_FTS3_PARENTHESIS
        SQLITE_ENABLE_FTS5
        SQLITE_ENABLE_JSON1
        SQLITE_ENABLE_RTREE
        SQLITE_OMIT_COMPLETE
    INCLUDE_DIRECTORIES
        ../../../3rdparty/sqlite
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION CMAKE_BUILD_TYPE STREQUAL Release AND NOT QT_FEATURE_system_sqlite
    DEFINES
        NDEBUG
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION NOT QT_FEATURE_largefile AND NOT QT_FEATURE_system_sqlite
    DEFINES
        SQLITE_DISABLE_LFS
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION QT_FEATURE_posix_fallocate AND NOT QT_FEATURE_system_sqlite
    DEFINES
        HAVE_POSIX_FALLOCATE=1
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION QNX AND NOT QT_FEATURE_system_sqlite
    DEFINES
        _QNX_SOURCE
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION UNIX AND NOT QT_FEATURE_system_sqlite
    DEFINES
        HAVE_USLEEP=1
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION INTEGRITY
    COMPILE_OPTIONS -include qplatformdefs.h
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION QT_FEATURE_dlopen AND NOT QT_FEATURE_system_sqlite
    LIBRARIES
        ${CMAKE_DL_LIBS}
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION NOT QT_FEATURE_dlopen AND NOT QT_FEATURE_system_sqlite
    DEFINES
        SQLITE_OMIT_LOAD_EXTENSION
)

qt_internal_extend_target(QSQLiteDriverPlugin CONDITION NOT QT_FEATURE_thread AND NOT QT_FEATURE_system_sqlite
    DEFINES
        SQLITE_THREADSAFE=0
)
