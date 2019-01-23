#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("caffeine", "en-US")

MODULE_EXPORT char const * obs_module_description(void)
{
	return "Caffeine.tv output";
}

extern struct obs_output_info caffeine_output_info;

bool obs_module_load(void)
{
	obs_register_output(&caffeine_output_info);

	return true;
}

void obs_module_unload(void)
{
}
