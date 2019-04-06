project(caffeine)

find_package(Libcaffeine REQUIRED)
find_package(Libcurl REQUIRED)
find_package(Threads REQUIRED)
find_package(FFmpeg REQUIRED
	COMPONENTS avcodec avfilter avdevice avutil swscale avformat swresample)

if(MSVC)
	set(caffeine_PLATFORM_DEPS
		shlwapi.lib
		w32-pthreads)
endif()

add_definitions(${LIBCURL_DEFINITIONS})

include_directories(
	../obs-ffmpeg
	${FFMPEG_INCLUDE_DIRS}
	${LIBCURL_INCLUDE_DIRS}
	${OBS_JANSSON_INCLUDE_DIRS}
	${LIBCAFFEINE_INCLUDE_DIR})

set(caffeine_HEADERS
	caffeine-foreground-process.h
	caffeine-log.h
	caffeine-settings.h)

set(caffeine_SOURCES
	caffeine-foreground-process.c
	caffeine-module.c
	caffeine-output.c)

add_library(caffeine MODULE
    ${caffeine_HEADERS}
	${caffeine_SOURCES})

target_link_libraries(caffeine
	${LIBCAFFEINE_LIBRARY}
	${LIBCURL_LIBRARIES}
	${FFMPEG_LIBRARIES}
	${OBS_JANSSON_IMPORT}
	${caffeine_PLATFORM_DEPS}
	${THREADS_LIBRARIES}
	libobs)

install_obs_plugin_with_data(caffeine data)

#install_obs_core(libcaffeine EXPORT LibCaffeine)
#install_obs_data(libcaffeine data libcaffeine)
#install_obs_headers(${caffeine_HEADERS})
#obs_install_additional(libcaffeine)
# TODO: this doesn't do the right thing on linux
#install_obs_bin(caffeine "" ${CAFFEINE_SHARED})
