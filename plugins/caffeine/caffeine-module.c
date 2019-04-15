#include <obs-module.h>
#include "caffeine.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("caffeine", "en-US")

MODULE_EXPORT char const * obs_module_description(void)
{
	return "Caffeine.tv output";
}

extern struct obs_output_info caffeine_output_info;

/* Converts libcaffeine log levels to OBS levels. NONE or unrecognized values
 * return 0 to indicate the message shouldn't be logged
 */
static int caffeine_to_obs_log_level(caff_Severity level)
{
	switch (level)
	{
	case caff_SeverityAll:
	case caff_SeverityDebug:
		return LOG_DEBUG;
	case caff_SeverityWarning:
		return LOG_WARNING;
	case caff_SeverityError:
		return LOG_ERROR;
	case caff_SeverityNone:
	default:
		return 0;
	}
}

static void caffeine_log(caff_Severity level, char const * message)
{
	int log_level = caffeine_to_obs_log_level(level);
	if (log_level)
		blog(log_level, "[libcaffeine] %s", message);
}

bool obs_module_load(void)
{
	obs_register_output(&caffeine_output_info);
#ifdef NDEBUG
	caff_initialize(caff_SeverityWarning, caffeine_log);
#else
	caff_initialize(caff_SeverityDebug, caffeine_log);
#endif
	return true;
}

void obs_module_unload(void)
{
}
