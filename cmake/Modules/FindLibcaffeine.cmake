# Once done these will be defined:
#
#  HAVE_LIBCAFFEINE
#  LIBCAFFEINE_FOUND
#  LIBCAFFEINE_INCLUDE_DIR
#  LIBCAFFEINE_LIBRARY
#  LIBCAFFEINE_SHARED

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
	pkg_check_modules(_LIBCAFFEINE QUIET libcaffeine)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_lib_suffix 64)
else()
	set(_lib_suffix 86)
endif()

if(WIN32)
	set(_os win)
elseif(APPLE)
	set(_os mac)
else()
	message(STATUS "TODO: other os's.")
	return()
endif()

# TODO: Find debug & release versions separately for different build configs
find_path(LIBCAFFEINE_INCLUDE_DIR
	NAMES caffeine.h
	HINTS
		ENV LibcaffeinePath{_lib_suffix}
		ENV LibcaffeinePath
		ENV DepsPath${_lib_suffix}
		ENV DepsPath
		${LibcaffeinePath${_lib_suffix}}
		${LibcaffeinePath}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_LIBCAFFEINE_INCLUDE_DIRS}
	PATHS
		/usr/include /usr/local/include /opt/local/include /sw/include
	PATH_SUFFIXES
		libcaffeine/include
		include)

set(_build_dir_base "${_os}_x${_lib_suffix}")

find_library(LIBCAFFEINE_LIBRARY
	NAMES libcaffeine.lib libcaffeine.a
	HINTS
		${LIBCAFFEINE_INCLUDE_DIR}
		ENV LibcaffeinePath{_lib_suffix}
		ENV LibcaffeinePath
		ENV DepsPath${_lib_suffix}
		ENV DepsPath
		${LibcaffeinePath${_lib_suffix}}
		${LibcaffeinePath}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_LIBCAFFEINE_LIBRARY_DIRS}
	PATHS
		/usr/lib /usr/local/lib /opt/local/lib /sw/lib
	PATH_SUFFIXES
		../build/Debug
		../build/RelWithDebInfo)

if (LIBCAFFEINE_LIBRARY)
	if(WIN32)
		string(REGEX REPLACE ".lib$" ".dll" LIBCAFFEINE_SHARED ${LIBCAFFEINE_LIBRARY})
	elseif(APPLE)
		set(LIBCAFFEINE_SHARED ${LIBCAFFEINE_LIBRARY})
	else()
		string(REGEX REPLACE ".a$" ".so" LIBCAFFEINE_SHARED ${LIBCAFFEINE_LIBRARY})
	endif()

	set(LIBCAFFEINE_SHARED ${LIBCAFFEINE_SHARED} PARENT_SCOPE)

	set (HAVE_LIBCAFFEINE "1")
else()
	set (HAVE_LIBCAFFEINE "0")
endif()

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(Libcaffeine
	FOUND_VAR LIBCAFFEINE_FOUND
	REQUIRED_VARS LIBCAFFEINE_INCLUDE_DIR LIBCAFFEINE_LIBRARY LIBCAFFEINE_SHARED)


mark_as_advanced(LIBCAFFEINE_INCLUDE_DIR LIBCAFFEINE_LIBRARY LIBCAFFEINE_SHARED)
