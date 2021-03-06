#.rst:
# FindIntl
# --------
#
# Find the Gettext libintl headers and libraries.
#
# This module reports information about the Gettext libintl
# installation in several variables.  General variables::
#
#   Intl_FOUND - true if the libintl headers and libraries were found
#   Intl_INCLUDE_DIRS - the directory containing the libintl headers
#   Intl_LIBRARIES - libintl libraries to be linked
#
# The following cache variables may also be set::
#
#   Intl_INCLUDE_DIR - the directory containing the libintl headers
#   Intl_LIBRARY - the libintl library (if any)
#
# .. note::
#   On some platforms, such as Linux with GNU libc, the gettext
#   functions are present in the C standard library and libintl
#   is not required.  ``Intl_LIBRARIES`` will be empty in this
#   case.
#
# .. note::
#   If you wish to use the Gettext tools (``msgmerge``,
#   ``msgfmt``, etc.), use :module:`FindGettext`.


# Written by Roger Leigh <rleigh@codelibre.net>

#=============================================================================
# Copyright 2014 Roger Leigh <rleigh@codelibre.net>
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================
# (To distribute this file outside of CMake, substitute the full
#  License text for the above reference.)

# Find include directory
find_path(Intl_INCLUDE_DIR
          NAMES "libintl.h"
          DOC "libintl include directory")
mark_as_advanced(Intl_INCLUDE_DIR)
# message("Intl_INCLUDE_DIR = ${Intl_INCLUDE_DIR}")

# Find all Intl libraries
# ln -s /usr/lib/preloadable_libintl.so /usr/lib/libgnuintl.so.8
# message("CMAKE_FIND_LIBRARY_PREFIXES = ${CMAKE_FIND_LIBRARY_PREFIXES}")
# set(OLD_CMAKE_FIND_LIBRARY_PREFIXES ${CMAKE_FIND_LIBRARY_PREFIXES})
# set(CMAKE_FIND_LIBRARY_PREFIXES ";lib")
find_library(Intl_LIBRARY
    NAMES intl #preloadable_libintl
    DOC "libintl libraries (if not in the C library)")
mark_as_advanced(Intl_LIBRARY)
# set(CMAKE_FIND_LIBRARY_PREFIXES ${OLD_CMAKE_FIND_LIBRARY_PREFIXES})
# message("Intl_LIBRARY = ${Intl_LIBRARY}")

# include(${CMAKE_CURRENT_LIST_DIR}/FindPackageHandleStandardArgs.cmake)
include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Intl
#                                   FOUND_VAR Intl_FOUND # deprecated / cmake create it automatically in uppercase
                                  REQUIRED_VARS Intl_INCLUDE_DIR
                                  FAIL_MESSAGE "Failed to find Gettext libintl")
# message("INTL_FOUND = ${INTL_FOUND}")

if(INTL_FOUND)
  set(Intl_INCLUDE_DIRS "${Intl_INCLUDE_DIR}")
  if(Intl_LIBRARY)
    set(Intl_LIBRARIES "${Intl_LIBRARY}")
  else()
    unset(Intl_LIBRARIES)
  endif()
endif()

