# Once done these will be defined:
#
#  CAFFEINE_FOUND
#  CAFFEINE_INCLUDE_DIRS
#  CAFFEINE_LIBRARIES
#
# For use in OBS: 
#
#  CAFFEINE_INCLUDE_DIR

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
	pkg_check_modules(_CAFFEINE QUIET caffeine-rtc)
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
find_path(CAFFEINE_INCLUDE_DIR
	NAMES caffeine.h
	HINTS
		ENV CaffeinePath{_lib_suffix}
		ENV CaffeinePath
		ENV DepsPath${_lib_suffix}
		ENV DepsPath
		${CaffeinePath${_lib_suffix}}
		${CaffeinePath}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_CAFFEINE_INCLUDE_DIRS}
	PATHS
		/usr/include /usr/local/include /opt/local/include /sw/include
	PATH_SUFFIXES
		src/caffeine
		caffeine include/caffeine include)

set(_build_dir_base "${_os}_x${_lib_suffix}")

find_library(CAFFEINE_LIB
	NAMES caffeine-rtc caffeine-rtc.dll libcaffeine-rtc
	HINTS
		${CAFFEINE_INCLUDE_DIR}
		ENV CaffeinePath{_lib_suffix}
		ENV CaffeinePath
		ENV DepsPath${_lib_suffix}
		ENV DepsPath
		${CaffeinePath${_lib_suffix}}
		${CaffeinePath}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_CAFFEINE_LIBRARY_DIRS}
	PATHS
		/usr/lib /usr/local/lib /opt/local/lib /sw/lib
	PATH_SUFFIXES
		../out/${_build_dir_base}_debug
		../out/${_build_dir_base}_release
		src/out/${_build_dir_base}_debug
		src/out/${_build_dir_base}_release
		lib${_lib_suffix} lib
		libs${_lib_suffix} libs
		bin${_lib_suffix} bin
		../lib${_lib_suffix} ../lib
		../libs${_lib_suffix} ../libs
		../bin${_lib_suffix} ../bin)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(Caffeine
	FOUND_VAR CAFFEINE_FOUND
	REQUIRED_VARS CAFFEINE_LIB CAFFEINE_INCLUDE_DIR)

mark_as_advanced(CAFFEINE_INCLUDE_DIR CAFFEINE_LIB)

if (CAFFEINE_FOUND)
	set (CAFFEINE_INCLUDE_DIRS ${CAFFEINE_INCLUDE_DIR})
	set (CAFFEINE_LIBRARIES ${CAFFEINE_LIB})
	if(WIN32)
		string(REGEX REPLACE ".lib$" "" CAFFEINE_SHARED ${CAFFEINE_LIB})
	elseif(APPLE)
		set(CAFFEINE_SHARED ${CAFFEINE_LIB})
	else()
		string(REGEX REPLACE ".a$" ".so" CAFFEINE_SHARED ${CAFFEINE_LIB})
	endif()

	set (HAVE_CAFFEINE "1")
else()
	set (HAVE_CAFFEINE "0")
endif()
