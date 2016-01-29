# GNU Readline library finder

# - Find GNU Readline header and library
#
# This module defines
#  READLINE_FOUND, if false, do not try to use GNU Readline.
#  READLINE_INCLUDE_DIR, where to find readline/readline.h.
#  READLINE_LIBRARY, the libraries to link against to use GNU Readline.
#  READLINE_VERSION, the library version
#
# As a hint allows READLINE_ROOT_DIR


find_path(READLINE_INCLUDE_DIRS
    NAMES readline/readline.h
    HINTS ${READLINE_ROOT_DIR}/include
)
find_library(READLINE_LIBRARY
    NAMES readline
    HINTS ${READLINE_ROOT_DIR}/lib
)
find_library(NCURSES_LIBRARY       # readline depends on libncurses, or similar
    NAMES ncurses ncursesw curses termcap
    HINTS ${READLINE_ROOT_DIR}/lib
)
set(READLINE_LIBRARIES ${READLINE_LIBRARY} ${NCURSES_LIBRARY})

if( EXISTS "${READLINE_INCLUDE_DIRS}/readline/readline.h" )
  file( STRINGS "${READLINE_INCLUDE_DIRS}/readline/readline.h" readline_h_content REGEX "#define RL_READLINE_VERSION" )
  string( REGEX REPLACE ".*0x([0-9][0-9])([0-9][0-9]).*" "\\2.\\1" READLINE_VERSION ${readline_h_content} )
  string( REGEX REPLACE "^0" "" READLINE_VERSION ${READLINE_VERSION} )
  string( REPLACE ".0" "." READLINE_VERSION ${READLINE_VERSION} )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Readline
  FOUND_VAR
    READLINE_FOUND
  REQUIRED_VARS
    READLINE_LIBRARY
    NCURSES_LIBRARY
    READLINE_INCLUDE_DIRS
  VERSION_VAR
    READLINE_VERSION
)

mark_as_advanced(READLINE_ROOT_DIR READLINE_INCLUDE_DIRS READLINE_LIBRARIES)
