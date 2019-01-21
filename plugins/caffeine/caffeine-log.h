#pragma once

#include <util/dstr.h>

#ifndef CAFFEINE_LOG_TITLE
#error "Define CAFFEINE_LOG_TITLE before including caffeine-log.h"
#endif

#define do_log(level, format, ...) \
	blog(level, "[" CAFFEINE_LOG_TITLE "] " format, ##__VA_ARGS__)

#define log_error(format, ...)  do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define log_warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define log_info(format, ...)  do_log(LOG_INFO, format, ##__VA_ARGS__)
#define log_debug(format, ...)  do_log(LOG_DEBUG, format, ##__VA_ARGS__)

#define trace() log_debug("%s", __func__)

#define set_error(output, fmt, ...) \
	do { \
		struct dstr message; \
		dstr_init(&message); \
		dstr_printf(&message, (fmt), ##__VA_ARGS__); \
		log_error("%s", message.array); \
		obs_output_set_last_error((output), message.array); \
		dstr_free(&message); \
	} while(false)
