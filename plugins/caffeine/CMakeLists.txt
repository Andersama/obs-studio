project(caffeine)

find_package(Caffeine REQUIRED)


find_package(Libcurl REQUIRED)
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
	${CAFFEINE_INCLUDE_DIR})

set(caffeine_SOURCES
	caffeine-api.c
	caffeine-api.h
	caffeine-config.h
	caffeine-log.h
	caffeine-foreground-process.c
	caffeine-foreground-process.h
	caffeine-module.c
	caffeine-output.c
	caffeine-service.c
	caffeine-service.h)

add_library(caffeine MODULE
	${caffeine_SOURCES}
	${caffeine_HEADERS})

target_link_libraries(caffeine
	${CAFFEINE_LIBRARIES}
	${LIBCURL_LIBRARIES}
	${FFMPEG_LIBRARIES}
	${OBS_JANSSON_IMPORT}
	${caffeine_PLATFORM_DEPS}
	libobs)

install_obs_plugin_with_data(caffeine data)

# TODO: this doesn't do the right thing on linux
install_obs_bin(caffeine "" ${CAFFEINE_SHARED})
