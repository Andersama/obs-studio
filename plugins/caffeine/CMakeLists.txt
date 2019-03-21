project(caffeine)

find_package(Caffeine REQUIRED)
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
	${CAFFEINE_INCLUDE_DIRS})

set(caffeine_HEADERS
	${CAFFEINE_HEADERS}
#	caffeine-api.h
#	caffeine-config.h
#	caffeine-log.h
#	caffeine-foreground-process.h
	)

set(caffeine_SOURCES
	${CAFFEINE_SOURCES}
#	caffeine-api.c
#	caffeine-foreground-process.c
	)

set(caffeine_PLUGINS
	caffeine-module.c
	caffeine-output.c)

add_library(caffeine MODULE
	${caffeine_SOURCES}
	${caffeine_HEADERS}
	${caffeine_PLUGINS})

target_link_libraries(caffeine
	${CAFFEINE_LIBRARIES}
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
