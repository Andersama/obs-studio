#pragma once

#include <caffeine.h>

struct caffeine_credentials;

struct caffeine_auth_response {
	struct caffeine_credentials * credentials;
	char * next;
	char * mfa_otp_method;
};

struct caffeine_user_info {
	char * caid;
	char * username;
	char * stage_id;
	bool can_broadcast;
};

/* TODO: game detection will be handled behind the scenes */
struct caffeine_game_info {
	char * id;
	char * name;
	char ** process_names;
	size_t num_process_names;
};

struct caffeine_games {
	struct caffeine_game_info ** game_infos;
	size_t num_games;
};

enum caffeine_rating {
	CAFF_RATING_NONE,
	CAFF_RATING_SEVENTEEN_PLUS,
	CAFF_RATING_MAX,
};

struct caffeine_feed_stream {
	char * id;
	char * source_id;
	char * url;
	char * sdp_offer;
	char * sdp_answer;
};

struct caffeine_feed_capabilities {
	bool video;
	bool audio;
};

struct caffeine_content {
	char * id;
    	char * type;
};

struct caffeine_feed {
	char * id;
	char * client_id;
	char * role;
	char * description;
	char * source_connection_quality;
	double volume;
	struct caffeine_feed_capabilities capabilities;
	struct caffeine_content content;
	struct caffeine_feed_stream stream;
};

struct caffeine_stage {
	char * id;
	char * username;
	char * title;
	char * broadcast_id;
	bool upsert_broadcast;
	bool live;
	struct caffeine_feed * feeds;
	size_t num_feeds;
};

struct caffeine_stage_request {
	char * username;
	char * client_id;
	char * cursor;
	struct caffeine_stage * stage;
};

char * caffeine_generate_unique_id();

void caffeine_set_string(char ** source, char const * new_value);

bool caffeine_is_supported_version();

struct caffeine_heartbeat_response {
	char * connection_quality;
};

struct caffeine_auth_response * caffeine_signin(
	char const * username,
	char const * password,
	char const * otp);

struct caffeine_credentials * caffeine_refresh_auth(
	char const * refresh_token);

void caffeine_free_credentials(struct caffeine_credentials ** creds);

char const * caffeine_refresh_token(struct caffeine_credentials * creds);

void caffeine_free_auth_response(struct caffeine_auth_response ** auth_response);

struct caffeine_user_info * caffeine_getuser(
	struct caffeine_credentials * creds);

void caffeine_free_user_info(struct caffeine_user_info ** user_info);

struct caffeine_games * caffeine_get_supported_games();

void caffeine_free_game_info(struct caffeine_game_info ** info);
void caffeine_free_game_list(struct caffeine_games ** games);

char * caffeine_annotate_title(char const * title, enum caffeine_rating rating);

bool caffeine_trickle_candidates(
	caff_ice_candidates candidates,
	size_t num_candidates,
	char const * stream_url,
	struct caffeine_credentials * creds);

struct caffeine_heartbeat_response * caffeine_heartbeat_stream(
	char const * stream_url,
	struct caffeine_credentials * creds);

void caffeine_free_heartbeat_response(
	struct caffeine_heartbeat_response ** response);

bool caffeine_update_broadcast_screenshot(
	char const * broadcast_id,
	uint8_t const * screenshot_data,
	size_t screenshot_size,
	struct caffeine_credentials * creds);

struct caffeine_feed * caffeine_get_stage_feed(
	struct caffeine_stage * stage, char const * id);
// Replaces any existing feeds, copies passed in data
void caffeine_set_stage_feed(
	struct caffeine_stage * stage, struct caffeine_feed const * feed);
void caffeine_clear_stage_feeds(struct caffeine_stage * stage);
void caffeine_free_stage(struct caffeine_stage ** stage);

struct caffeine_stage_request * caffeine_copy_stage_request(
	struct caffeine_stage_request const * request);
void caffeine_free_stage_request(struct caffeine_stage_request ** request);

// If successful, updates request cursor and stage with the response values
bool caffeine_request_stage_update(
	struct caffeine_stage_request * request,
	struct caffeine_credentials * creds,
	double * retry_in);
