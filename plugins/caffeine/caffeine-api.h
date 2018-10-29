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

struct caffeine_stream_info {
	char * stream_id;
	char * sdp_answer;
	char * signed_payload;
};

struct caffeine_auth_response * caffeine_signin(
	char const * username,
	char const * password,
	char const * otp);

void caffeine_free_credentials(struct caffeine_credentials * creds);

void caffeine_free_auth_response(struct caffeine_auth_response * auth_response);

struct caffeine_user_info * caffeine_getuser(
	struct caffeine_credentials * creds);

void caffeine_free_user_info(struct caffeine_user_info * user_info);

struct caffeine_stream_info * caffeine_start_stream(
	char const * stage_id,
	char const * sdp_offer,
	struct caffeine_credentials * creds);

void caffeine_free_stream_info(struct caffeine_stream_info * stream_info);

bool caffeine_trickle_candidates(
	caff_ice_candidates candidates,
	size_t num_candidates,
	struct caffeine_credentials * creds,
	struct caffeine_stream_info const * stream_info);

bool create_broadcast(
	char const * title,
	struct caffeine_credentials * creds);

char * set_stage_live(
	bool isLive,
	char const * session_id,
	char const * stage_id,
	char const * stream_id,
	char const * title,
	struct caffeine_credentials * creds);

bool send_heartbeat(
	char const * stage_id,
	char const * signed_payload,
	struct caffeine_credentials * creds);
