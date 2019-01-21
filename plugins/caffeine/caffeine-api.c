#include "caffeine-api.h"

#include <obs.h>
#include <obs-config.h>

#include <curl/curl.h>
#include <jansson.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <util/platform.h>

#include "caffeine-config.h"

#define CAFFEINE_LOG_TITLE "caffeine api"
#include "caffeine-log.h"

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)
#define API_VERSION STR(LIBOBS_API_MAJOR_VER) "." STR(LIBOBS_API_MINOR_VER)

#define API_ENDPOINT       "https://api." CAFFEINE_DOMAIN
#define REALTIME_ENDPOINT  "https://realtime." CAFFEINE_DOMAIN

/* TODO: some of these are deprecated */
#define VERSION_CHECK_URL  API_ENDPOINT  "/v1/version-check"
#define SIGNIN_URL         API_ENDPOINT  "/v1/account/signin"
#define REFRESH_TOKEN_URL  API_ENDPOINT  "/v1/account/token"
#define GETGAMES_URL       API_ENDPOINT  "/v1/games"
#define GETUSER_URL_F      API_ENDPOINT  "/v1/users/%s"
#define BROADCAST_URL_F    API_ENDPOINT  "/v1/broadcasts/%s"

#define STAGE_UPDATE_URL_F REALTIME_ENDPOINT "/v4/stage/%s"
#define STREAM_HEARTBEAT_URL_F "%s/heartbeat"

#define CONTENT_TYPE_JSON  "Content-Type: application/json"
#define CONTENT_TYPE_FORM  "Content-Type: multipart/form-data"

/*
 * Request type: GET, PUT, POST, PATCH
 *
 * Request body format: Form data, JSON
 *
 * Request data itself
 *
 * URL, URL_F
 *
 * Basic, Authenticated
 *
 * Result type: new object/NULL, success/failure
 *
 * Error type: (various json formats, occasionally HTML e.g. for 502)
 */

struct caffeine_credentials {
	char * access_token;
	char * refresh_token;
	char * caid;
	char * credential;

	pthread_mutex_t mutex;
};

struct caffeine_stage_response {
	char * cursor;
	double retry_in;
	struct caffeine_stage * stage;
};

struct caffeine_display_message {
	char * title;
	char * body;
};

struct caffeine_failure_response {
	char * type;
	char * reason;
	struct caffeine_display_message display_message;
};

struct caffeine_stage_response_result {
	struct caffeine_stage_response * response;
	struct caffeine_failure_response * failure;
};

char * caffeine_generate_unique_id()
{
    const int id_length = 12;
    const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";

    struct dstr id;
    dstr_init(&id);
    dstr_reserve(&id, id_length + 1);

    for (int i = 0; i < id_length; ++i) {
        int random_index = rand() % (sizeof(charset) - 1);
        char character = charset[random_index];
        dstr_cat_ch(&id, character);
    }

    return id.array;
}

void caffeine_set_string(char ** source, char const * new_value) {
    bfree(*source);
    *source = bstrdup(new_value);
}

static size_t caffeine_curl_write_callback(char * ptr, size_t size,
	size_t nmemb, void * user_data)
{
	UNUSED_PARAMETER(size);
	if (nmemb == 0)
		return 0;

	struct dstr * result_str = user_data;
	dstr_ncat(result_str, ptr, nmemb);
	return nmemb;
}

static struct curl_slist * caffeine_basic_headers(char const * content_type) {
	struct curl_slist * headers = NULL;
	headers = curl_slist_append(headers, content_type);
	headers = curl_slist_append(headers, "X-Client-Type: obs");
	headers = curl_slist_append(headers, "X-Client-Version: " API_VERSION);
	return headers;
}

static struct curl_slist * caffeine_authenticated_headers(
	char const * content_type,
	struct caffeine_credentials * creds)
{
	pthread_mutex_lock(&creds->mutex);
	struct dstr header;
	dstr_init(&header);
	struct curl_slist * headers = caffeine_basic_headers(content_type);

	dstr_printf(&header, "Authorization: Bearer %s", creds->access_token);
	headers = curl_slist_append(headers, header.array);

	dstr_printf(&header, "X-Credential: %s", creds->credential);
	headers = curl_slist_append(headers, header.array);

	dstr_free(&header);
	pthread_mutex_unlock(&creds->mutex);
	return headers;
}

static struct caffeine_credentials * make_credentials(
	char const * access_token,
	char const * caid,
	char const * refresh_token,
	char const * credential)
{
	struct caffeine_credentials * creds =
		bzalloc(sizeof(struct caffeine_credentials));

	creds->access_token = bstrdup(access_token);
	creds->caid = bstrdup(caid);
	creds->refresh_token = bstrdup(refresh_token);
	creds->credential = bstrdup(credential);
	pthread_mutex_init(&creds->mutex, NULL);

	return creds;
}

char const * caffeine_refresh_token(struct caffeine_credentials * creds)
{
	return creds->refresh_token;
}

#define RETRY_MAX 3

/* TODO: this is not very robust or intelligent. Some kinds of failure will
 * never be recoverable and should not be retried
 */
#define retry_request(result_t, request) \
	for (int try_num = 0; try_num < RETRY_MAX; ++try_num) { \
		result_t result = request; \
		if (result) \
			return result; \
		os_sleep_ms(1000 + 1000 * try_num); \
	} \
	return (result_t)0

bool do_caffeine_is_supported_version()
{
	trace();

	CURL * curl = curl_easy_init();
	if (!curl)
	{
		log_error("Failed to initialize cURL");
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, VERSION_CHECK_URL);

	struct curl_slist *headers = caffeine_basic_headers(CONTENT_TYPE_JSON);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	struct dstr response_str;
	dstr_init(&response_str);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
		caffeine_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_str);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	bool result = false;

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure checking version: [%d] %s", res, curl_error);
		goto request_error;
	}

	json_error_t json_error;
	json_t * response_json = json_loads(response_str.array, 0, &json_error);
	if (!response_json) {
		log_error("Failed to parse version check response: %s",
			json_error.text);
		goto json_failed_error;
	}
	char const *error_text = NULL;
	int error_result = json_unpack(response_json, "{s:{s:[s!]}}",
		"errors", "_expired", &error_text);
	if (error_result == 0) {
		log_error("", error_text);
		goto json_parsed_error;
	}
	else {
		result = true;
	}

json_parsed_error:
	json_decref(response_json);
json_failed_error:
request_error:
	dstr_free(&response_str);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return result;
}

bool caffeine_is_supported_version() {
	retry_request(bool, do_caffeine_is_supported_version());
}

/* TODO: refactor this - lots of dupe code between request types
 * TODO: reuse curl handle across requests
 */
static struct caffeine_auth_response * do_caffeine_signin(
	char const * username,
	char const * password,
	char const * otp)
{
	trace();
	struct caffeine_auth_response * response = NULL;

	json_t * request_json = NULL;
	if (otp)
		request_json = json_pack("{s:{s:s,s:s},s:{s:s}}",
			"account", "username", username, "password", password,
			"mfa", "otp", otp);
	else
		request_json = json_pack("{s:{s:s,s:s}}",
			"account", "username", username, "password", password);

	if (!request_json)
	{
		log_error("Failed to create request JSON");
		goto request_json_error;
	}

	char * request_body = json_dumps(request_json, 0);
	if (!request_body)
	{
		log_error("Failed to serialize request JSON");
		goto request_serialize_error;
	}

	CURL * curl = curl_easy_init();

	if (!curl)
	{
		log_error("Failed to initialize cURL");
		goto curl_init_error;
	}

	curl_easy_setopt(curl, CURLOPT_URL, SIGNIN_URL);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);

	struct curl_slist *headers = caffeine_basic_headers(CONTENT_TYPE_JSON);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	struct dstr response_str;
	dstr_init(&response_str);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
		caffeine_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_str);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure signing in: [%d] %s", res, curl_error);
		goto request_error;
	}

	json_error_t json_error;
	json_t * response_json = json_loads(response_str.array, 0, &json_error);
	if (!response_json) {
		log_error("Failed to parse signin response: %s",
			json_error.text);
		goto json_failed_error;
	}
	char const *error_text = NULL;
	int error_result = json_unpack(response_json, "{s:{s:[s!]}}",
		"errors", "_error", &error_text);
	if (error_result == 0) {
		log_error("Error logging in: %s", error_text);
		goto json_parsed_error;
	}

	char const *access_token = NULL;
	char const *refresh_token = NULL;
	char const *caid = NULL;
	char const *credential = NULL;
	char const *next = NULL;
	char const *mfa_otp_method = NULL;
	int success_result = json_unpack_ex(response_json, &json_error, 0,
		"{s?:{s:s,s:s,s:s,s:s}, s?:s, s?:s}",
		"credentials",
		"access_token", &access_token,
		"refresh_token", &refresh_token,
		"caid", &caid,
		"credential", &credential,

		"next", &next,

		"mfa_otp_method", &mfa_otp_method);

	if (success_result != 0) {
		log_error("Failed to extract response info: [%s]",
			json_error.text);
		goto json_parsed_error;
	}

	error_result = json_unpack(response_json, "{s:{s:[s!]}}",
		"errors", "otp", &error_text);
	if (error_result == 0) {
		log_error("One time password error: %s", error_text);
		next = "mfa_otp_required";
	}

	struct caffeine_credentials * creds = NULL;

	if (access_token) {
		log_debug("Sign-in complete");
		creds = make_credentials(access_token, caid, refresh_token,
					credential);
	}
	else if (mfa_otp_method) {
		log_debug("MFA required");
	}
	else
		log_error("Sign-in response missing");

	response = bzalloc(sizeof(struct caffeine_auth_response));

	response->credentials = creds;
	response->next = bstrdup(next);
	response->mfa_otp_method = bstrdup(mfa_otp_method);

json_parsed_error:
	json_decref(response_json);
json_failed_error:
request_error:
	dstr_free(&response_str);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
curl_init_error:
	free(request_body);
request_serialize_error:
	json_decref(request_json);
request_json_error:

	return response;
}

struct caffeine_auth_response * caffeine_signin(
	char const * username,
	char const * password,
	char const * otp)
{
	retry_request(void*, do_caffeine_signin(username, password, otp));
}

static struct caffeine_credentials * do_caffeine_refresh_auth(
	char const * refresh_token)
{
	trace();

	json_t * request_json = json_pack("{s:s}",
		"refresh_token", refresh_token);

	struct caffeine_credentials * result = NULL;

	if (!request_json)
	{
		log_error("Failed to create request JSON");
		goto request_json_error;
	}

	char * request_body = json_dumps(request_json, 0);
	if (!request_body)
	{
		log_error("Failed to serialize request JSON");
		goto request_serialize_error;
	}

	CURL * curl = curl_easy_init();

	if (!curl)
	{
		log_error("Failed to initialize cURL");
		goto curl_init_error;
	}

	curl_easy_setopt(curl, CURLOPT_URL, REFRESH_TOKEN_URL);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);

	struct curl_slist *headers = caffeine_basic_headers(CONTENT_TYPE_JSON);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	struct dstr response_str;
	dstr_init(&response_str);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
		caffeine_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_str);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure refreshing tokens: [%d] %s", res,
			curl_error);
		goto request_error;
	}

	long response_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	log_debug("Http response [%ld]", response_code);

	json_error_t json_error;
	json_t * response_json = json_loads(response_str.array, 0, &json_error);
	if (!response_json) {
		log_error("Failed to parse refresh response: %s",
			json_error.text);
		goto json_failed_error;
	}
	char const *error_text = NULL;
	int error_result = json_unpack(response_json, "{s:{s:[s!]}}",
		"errors", "_error", &error_text);
	if (error_result == 0) {
		log_error("Error refreshing tokens: %s", error_text);
		goto json_parsed_error;
	}

	char const *access_token = NULL;
	char const *new_refresh_token = NULL;
	char const *caid = NULL;
	char const *credential = NULL;
	int success_result = json_unpack_ex(response_json, &json_error, 0,
		"{s:{s:s,s:s,s:s,s:s}}",
		"credentials",
		"access_token", &access_token,
		"refresh_token", &new_refresh_token,
		"caid", &caid,
		"credential", &credential);

	if (success_result != 0) {
		log_error("Failed to extract response info: [%s]",
			json_error.text);
		goto json_parsed_error;
	}

	result = make_credentials(access_token, caid, new_refresh_token,
		credential);

	log_debug("Auth tokens refreshed");

json_parsed_error:
	json_decref(response_json);
json_failed_error:
request_error:
	dstr_free(&response_str);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
curl_init_error:
	free(request_body);
request_serialize_error:
	json_decref(request_json);
request_json_error:

	return result;
}

struct caffeine_credentials * caffeine_refresh_auth(
	char const * refresh_token)
{
	retry_request(void*, do_caffeine_refresh_auth(refresh_token));
}

void caffeine_free_credentials(struct caffeine_credentials ** credentials)
{
	trace();
	if (!credentials || !*credentials)
		return;

	pthread_mutex_lock(&(*credentials)->mutex);
	bfree((*credentials)->access_token);
	bfree((*credentials)->caid);
	bfree((*credentials)->refresh_token);
	bfree((*credentials)->credential);

	pthread_mutex_unlock(&(*credentials)->mutex);
	pthread_mutex_destroy(&(*credentials)->mutex);
	bfree(*credentials);
	*credentials = NULL;
}

void caffeine_free_auth_response(struct caffeine_auth_response ** auth_response)
{
	trace();
	if (!auth_response || !*auth_response)
		return;

	caffeine_free_credentials(&(*auth_response)->credentials);

	bfree((*auth_response)->next);
	bfree((*auth_response)->mfa_otp_method);

	bfree(*auth_response);
	*auth_response = NULL;
}

static bool do_refresh_credentials(struct caffeine_credentials * creds)
{
	trace();
	char * refresh_token = NULL;
	pthread_mutex_lock(&creds->mutex);
	refresh_token = bstrdup(creds->refresh_token);
	pthread_mutex_unlock(&creds->mutex);

	struct caffeine_credentials * new_creds =
	caffeine_refresh_auth(refresh_token);

	if (!new_creds)
		return false;

#define swap(field) do { \
	void * temp = creds->field; \
	creds->field = new_creds->field; \
	new_creds->field = temp; } while (false)

	pthread_mutex_lock(&creds->mutex);
	swap(access_token);
	swap(caid);
	swap(refresh_token);
	swap(credential);
	pthread_mutex_unlock(&creds->mutex);

#undef swap

	caffeine_free_credentials(&new_creds);
	return true;
}

bool refresh_credentials(struct caffeine_credentials * creds)
{
	retry_request(bool, do_refresh_credentials(creds));
}

static struct caffeine_user_info * do_caffeine_getuser(
	struct caffeine_credentials * creds)
{
	trace();
	if (creds == NULL) {
		log_error("Missing credentials");
		return NULL;
	}

	CURL * curl = curl_easy_init();

	if (!curl)
	{
		log_error("Failed to initialize cURL");
		return NULL;
	}

	struct caffeine_user_info * user_info = NULL;

	struct dstr url_str;
	dstr_init(&url_str);
	dstr_printf(&url_str, GETUSER_URL_F, creds->caid);

	curl_easy_setopt(curl, CURLOPT_URL, url_str.array);

	struct curl_slist *headers =
		caffeine_authenticated_headers(CONTENT_TYPE_JSON, creds);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	struct dstr response_str;
	dstr_init(&response_str);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
		caffeine_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_str);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure fetching user: [%d] %s", res, curl_error);
		goto request_error;
	}

	json_error_t json_error;
	json_t * response_json = json_loads(response_str.array, 0, &json_error);
	if (!response_json) {
		log_error("Failed to parse user response: %s",
			json_error.text);
		goto json_failed_error;
	}
	char const *error_text = NULL;
	int error_result = json_unpack(response_json, "{s:{s:[s!]}}",
		"errors", "_error", &error_text);
	if (error_result == 0) {
		log_error("Error fetching user: %s", error_text);
		goto json_parsed_error;
	}

	char const *fetched_caid = NULL;
	char const *username = NULL;
	char const *stage_id = NULL;
	int can_broadcast = false; /* Jansson uses int for bool */
	int success_result = json_unpack_ex(response_json, &json_error, 0,
		"{s:{s:s,s:s,s:s,s:b}}",
		"user",
		"caid", &fetched_caid,
		"username", &username,
		"stage_id", &stage_id,
		"can_broadcast", &can_broadcast);

	if (success_result != 0) {
		log_error("Failed to extract response info: [%s]",
			json_error.text);
		goto json_parsed_error;
	}

	if (strcmp(fetched_caid, creds->caid) != 0) {
		log_warn("Somehow got a different user. Original caid: %s - "
			 "Fetched caid: %s",
			 creds->caid, fetched_caid);
	}

	user_info = bzalloc(sizeof(struct caffeine_user_info));
	user_info->caid = bstrdup(fetched_caid);
	user_info->username = bstrdup(username);
	user_info->stage_id = bstrdup(stage_id);
	user_info->can_broadcast = can_broadcast;

	log_debug("Got user details");

json_parsed_error:
	json_decref(response_json);
json_failed_error:
request_error:
	dstr_free(&url_str);
	dstr_free(&response_str);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return user_info;
}

struct caffeine_user_info * caffeine_getuser(
	struct caffeine_credentials * creds)
{
	retry_request(void*, do_caffeine_getuser(creds));
}

void caffeine_free_user_info(struct caffeine_user_info ** user_info)
{
	trace();
	if (!user_info || !*user_info)
		return;

	bfree((*user_info)->caid);
	bfree((*user_info)->username);
	bfree((*user_info)->stage_id);
	bfree(*user_info);
	*user_info = NULL;
}

static struct caffeine_games * do_caffeine_get_supported_games()
{
	struct caffeine_games * response = NULL;

	CURL * curl = curl_easy_init();

	if (!curl)
	{
		log_error("Failed to initialize cURL");
		goto curl_init_error;
	}

	curl_easy_setopt(curl, CURLOPT_URL, GETGAMES_URL);

	struct curl_slist * headers = caffeine_basic_headers(CONTENT_TYPE_JSON);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	struct dstr response_str;
	dstr_init(&response_str);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
		caffeine_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_str);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure fetching supported games: [%d] %s",
			res, curl_error);
		goto request_error;
	}

	json_error_t json_error;
	json_t * response_json = json_loads(response_str.array, 0, &json_error);
	if (!response_json) {
		log_error("Failed to parse game list response: %s",
			json_error.text);
		goto json_failed_error;
	}
	size_t num_games = json_array_size(response_json); /* returns 0 if not array*/
	if (num_games == 0)
	{
		log_error("Unable to retrieve games list");
		goto json_parsed_error;
	}

	response = bzalloc(sizeof(struct caffeine_games));
	response->num_games = num_games;
	response->game_infos =
		bzalloc(num_games * sizeof(struct caffeine_game_info *));

	size_t index;
	json_t * value;
	json_array_foreach(response_json, index, value) {
		int id_num = 0;
		char const * name = NULL;
		json_t * process_names = NULL;
		int success_result = json_unpack_ex(value, &json_error, 0,
			"{s:i,s:s,s:o}",
			"id", &id_num,
			"name", &name,
			"process_names", &process_names);
		if (success_result != 0) {
			log_warn("Unable to parse game list entry; ignoring");
			continue;
		}

		size_t num_processes = json_array_size(process_names);
		if (num_processes == 0) {
			log_warn("No process names found for %s; ignoring",
				name);
			continue;
		}

		// Ownership will be stolen by the info below; don't free this
		struct dstr id_str;
		dstr_init(&id_str);
		dstr_printf(&id_str, "%d", id_num);

		struct caffeine_game_info * info =
			bzalloc(sizeof(struct caffeine_game_info));
		info->id = id_str.array;
		info->name = bstrdup(name);
		info->num_process_names = num_processes;
		info->process_names = bzalloc(num_processes * sizeof(char *));

		size_t process_index;
		json_t * process_value;
		json_array_foreach(process_names, process_index, process_value) {
			char const * process_name = NULL;
			success_result = json_unpack(process_value, "s",
				&process_name);
			if (success_result != 0) {
				log_warn("Unable to read process name; ignoring");
				continue;
			}
			info->process_names[process_index] =
				bstrdup(process_name);
		}

		response->game_infos[index] = info;
	}

json_parsed_error:
	json_decref(response_json);
json_failed_error:
request_error:
	dstr_free(&response_str);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
curl_init_error:

	return response;
}

struct caffeine_games * caffeine_get_supported_games()
{
	retry_request(struct caffeine_games *,
		do_caffeine_get_supported_games());
}

void caffeine_free_game_info(struct caffeine_game_info ** info)
{
	if (!info || !*info)
		return;

	for (size_t i = 0; i < (*info)->num_process_names; ++i) {
		bfree((*info)->process_names[i]);
		(*info)->process_names[i] = NULL;
	}
	bfree((*info)->id);
	bfree((*info)->name);
	bfree((*info)->process_names);
	bfree(*info);
	*info = NULL;
}

void caffeine_free_game_list(struct caffeine_games ** games)
{
	trace();
	if (!games || !*games)
		return;

	for (size_t i = 0; i < (*games)->num_games; ++i) {
		caffeine_free_game_info(&(*games)->game_infos[i]);
	}

	bfree((*games)->game_infos);
	bfree(*games);
	*games = NULL;
}

static bool do_caffeine_trickle_candidates(
	caff_ice_candidates candidates,
	size_t num_candidates,
	char const * stream_url,
	struct caffeine_credentials * creds)
{
	trace();
	json_t * ice_candidates = json_array();
	for (size_t i = 0; i < num_candidates; ++i)
	{
		json_array_append_new(ice_candidates, json_pack(
			"{s:s,s:s,s:i}",
			"candidate", candidates[i].sdp,
			"sdpMid", candidates[i].sdp_mid,
			"sdpMLineIndex", candidates[i].sdp_mline_index));
	}

	json_t * request_json = json_pack(
		"{s:o}",
		"ice_candidates", ice_candidates);

	bool response = false;

	if (!request_json)
	{
		log_error("Failed to create request JSON");
		goto request_json_error;
	}

	char * request_body = json_dumps(request_json, 0);
	if (!request_body)
	{
		log_error("Failed to serialize request JSON");
		goto request_serialize_error;
	}

	CURL * curl = curl_easy_init();

	if (!curl)
	{
		log_error("Failed to initialize cURL");
		goto curl_init_error;
	}

	curl_easy_setopt(curl, CURLOPT_URL, stream_url);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

	struct curl_slist *headers =
		caffeine_authenticated_headers(CONTENT_TYPE_JSON, creds);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure negotiating ICE: [%d] %s",
			res, curl_error);
		goto request_error;
	}

	long response_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

	switch (response_code) {
		case 200:
			response = true;
			break;
		case 401:
			log_info("Unauthorized - refreshing credentials");
			if (refresh_credentials(creds)) {
				response = do_caffeine_trickle_candidates(
					candidates, num_candidates, stream_url, creds);
			}
			break;
		default:
			break;
	}

	if (response) {
		log_debug("ICE candidates trickled");
	} else {
		log_error("Error negotiating ICE candidates");
	}

request_error:
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
curl_init_error:
	free(request_body);
request_serialize_error:
	json_decref(request_json);
request_json_error:

	return response;
}

bool caffeine_trickle_candidates(
	caff_ice_candidates candidates,
	size_t num_candidates,
	char const * stream_url,
	struct caffeine_credentials * creds)
{
	retry_request(bool,
		do_caffeine_trickle_candidates(candidates, num_candidates,
		stream_url, creds));
}

static struct caffeine_heartbeat_response * do_caffeine_heartbeat_stream(
	  char const * stream_url,
	  struct caffeine_credentials * creds)
{
	trace();

	json_t * request_json = json_object();
	char * request_body = json_dumps(request_json, 0);
	struct caffeine_heartbeat_response * response = NULL;

	CURL * curl = curl_easy_init();

	if (!curl)
	{
		log_error("Failed to initialize cURL");
		goto curl_init_error;
	}

	struct dstr url;
	dstr_init(&url);
	dstr_printf(&url, STREAM_HEARTBEAT_URL_F, stream_url);

	curl_easy_setopt(curl, CURLOPT_URL, url.array);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");

	struct curl_slist *headers =
		caffeine_authenticated_headers(CONTENT_TYPE_JSON, creds);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	struct dstr response_str;
	dstr_init(&response_str);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
		caffeine_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_str);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure hearbeating stream: [%d] %s",
				  res, curl_error);
		goto request_error;
	}

	long response_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

	if (response_code == 401) {
		log_info("Unauthorized - refreshing credentials");
		if (refresh_credentials(creds)) {
			response = do_caffeine_heartbeat_stream(stream_url, creds);
		}
		goto auth_refresh;
	}

	if (response_code != 200) {
		log_error("Error heartbeating stream: %ld", response_code);
		goto request_error;
	}

	json_error_t json_error;
	json_t * response_json = json_loads(response_str.array, 0, &json_error);
	if (!response_json) {
		log_error("Failed to parse refresh response: %s",
				  json_error.text);
		goto json_parsed_error;
	}


	char * connection_quality = NULL;
	json_unpack(
		response_json, "{s?s}",
		"connection_quality", &connection_quality);

	response = bzalloc(sizeof(struct caffeine_heartbeat_response));
	response->connection_quality = bstrdup(connection_quality);

	log_debug("Stream heartbeat succeeded");

json_parsed_error:
	json_decref(response_json);
request_error:
auth_refresh:
	dstr_free(&response_str);
	dstr_free(&url);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
curl_init_error:
	free(request_body);
	json_decref(request_json);

	return response;
}

struct caffeine_heartbeat_response * caffeine_heartbeat_stream(
	char const * stream_url,
	struct caffeine_credentials * creds)
{
	retry_request(
		struct caffeine_heartbeat_response *,
		do_caffeine_heartbeat_stream(stream_url, creds));
}

void caffeine_free_heartbeat_response(
	struct caffeine_heartbeat_response ** response)
{
	if (!response || !*response) {
		return;
	}

	bfree((*response)->connection_quality);
	bfree(*response);
	*response = NULL;
}

char * caffeine_annotate_title(char const * title, enum caffeine_rating rating)
{
	static const size_t MAX_TITLE_LENGTH = 60;
	static char const * rating_strings[] = { "", "[17+] " };

	if (rating < CAFF_RATING_NONE || rating >= CAFF_RATING_MAX)
		rating = CAFF_RATING_NONE;

	struct dstr final_title;
	dstr_init(&final_title);
	dstr_printf(&final_title, "%s%s", rating_strings[rating], title);
	dstr_resize(&final_title, MAX_TITLE_LENGTH);

	return final_title.array;
}

void add_text_part(
	struct curl_httppost ** post,
	struct curl_httppost ** last,
	char const * name,
	char const * text)
{
	curl_formadd(post, last,
		CURLFORM_COPYNAME, name,
		CURLFORM_COPYCONTENTS, text,
		CURLFORM_END);
}

static bool do_update_broadcast_screenshot(
	char const * broadcast_id,
	uint8_t const * screenshot_data,
	size_t screenshot_size,
	struct caffeine_credentials * creds)
{
	trace();
	CURL * curl = curl_easy_init();

	if (!curl)
	{
		log_error("Failed to initialize cURL");
		return false;
	}

	struct curl_httppost * post = NULL;
	struct curl_httppost * last = NULL;

	if (screenshot_data) {
		curl_formadd(&post, &last,
			CURLFORM_COPYNAME, "broadcast[game_image]",
			CURLFORM_BUFFER, "game_image.jpg",
			CURLFORM_BUFFERPTR, screenshot_data,
			CURLFORM_BUFFERLENGTH, screenshot_size,
			CURLFORM_CONTENTTYPE, "image/jpeg",
			CURLFORM_END);
	}

	curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

	struct dstr url;
	dstr_init(&url);
	dstr_printf(&url, BROADCAST_URL_F, broadcast_id);
	curl_easy_setopt(curl, CURLOPT_URL, url.array);

	struct curl_slist *headers =
		caffeine_authenticated_headers(CONTENT_TYPE_FORM, creds);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	bool result = false;

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure updating broadcast screenshot: [%d] %s",
			res, curl_error);
		goto request_error;
	}

	long response_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

	log_debug("Http response code [%ld]", response_code);

	result = response_code / 100 == 2;

	if (!result) {
		log_error("Failed to update broadcast screenshot");
	}

request_error:
	curl_slist_free_all(headers);
	dstr_free(&url);
	curl_formfree(post);
	curl_easy_cleanup(curl);

	return result;
}

bool caffeine_update_broadcast_screenshot(
	char const * broadcast_id,
	uint8_t const * screenshot_data,
	size_t screenshot_size,
	struct caffeine_credentials * creds)
{
	if (!broadcast_id) {
		log_error("Passed in NULL broadcast_id");
		return false;
	}

	retry_request(bool,
		do_update_broadcast_screenshot(
				broadcast_id,
				screenshot_data,
				screenshot_size,
				creds));
}

static void caffeine_free_feed_contents(struct caffeine_feed * feed)
{
    if (!feed) {
        return;
    }

    bfree(feed->id);
    bfree(feed->client_id);
    bfree(feed->role);
    bfree(feed->description);
    bfree(feed->source_connection_quality);
    bfree(feed->content.id);
    bfree(feed->content.type);
    bfree(feed->stream.id);
    bfree(feed->stream.source_id);
    bfree(feed->stream.url);
    bfree(feed->stream.sdp_offer);
    bfree(feed->stream.sdp_answer);
}

struct caffeine_feed * caffeine_get_stage_feed(struct caffeine_stage * stage, char const * id)
{
	if (!stage || !id) {
		return NULL;
	}

	for (size_t i = 0; i < stage->num_feeds; ++i) {
		struct caffeine_feed * feed = &stage->feeds[i];
		if (strcmp(id, feed->id) == 0) {
			return feed;
		}
	}

	return NULL;
}

static void caffeine_copy_feed_contents(
	struct caffeine_feed const * from, struct caffeine_feed * to)
{
	to->id = bstrdup(from->id);
	to->client_id = bstrdup(from->client_id);
	to->role = bstrdup(from->role);
	to->description = bstrdup(from->description);
	to->source_connection_quality = bstrdup(from->source_connection_quality);
	to->volume = from->volume;
	to->capabilities = from->capabilities;
	to->content.id = bstrdup(from->content.id);
	to->content.type = bstrdup(from->content.type);
	to->stream.id = bstrdup(from->stream.id);
	to->stream.source_id = bstrdup(from->stream.source_id);
	to->stream.url = bstrdup(from->stream.url);
	to->stream.sdp_offer = bstrdup(from->stream.sdp_offer);
	to->stream.sdp_answer = bstrdup(from->stream.sdp_answer);
}


void caffeine_set_stage_feed(struct caffeine_stage * stage, struct caffeine_feed const * feed)
{
	if (!stage || !feed) {
		return;
	}

	caffeine_clear_stage_feeds(stage);

	stage->feeds = bmalloc(sizeof(struct caffeine_feed));
	caffeine_copy_feed_contents(feed, stage->feeds);
	stage->num_feeds = 1;
}

void caffeine_clear_stage_feeds(struct caffeine_stage * stage)
{
	if (!stage || !stage->feeds) {
		return;
	}

	for (size_t i = 0; i < stage->num_feeds; ++i) {
		caffeine_free_feed_contents(&stage->feeds[i]);
	}

	bfree(stage->feeds);
	stage->feeds = NULL;
	stage->num_feeds = 0;
}

void caffeine_free_stage(struct caffeine_stage ** stage)
{
	if (!stage || !*stage) {
		return;
	}

	bfree((*stage)->id);
    	bfree((*stage)->username);
    	bfree((*stage)->title);
    	bfree((*stage)->broadcast_id);
	caffeine_clear_stage_feeds(*stage);
    	bfree(*stage);
    	*stage = NULL;
}

struct caffeine_stage_request * caffeine_copy_stage_request(
    struct caffeine_stage_request const * request)
{
	if (!request) {
		return NULL;
	}

	struct caffeine_stage_request * copy =
		bzalloc(sizeof(struct caffeine_stage_request));
	copy->username = bstrdup(request->username);
	copy->client_id = bstrdup(request->client_id);
	copy->cursor = bstrdup(request->cursor);

	struct caffeine_stage * stage = request->stage;
	if (!stage) {
		return copy;
	}

	struct caffeine_stage * stage_copy = bzalloc(sizeof(struct caffeine_stage));
	stage_copy->id = bstrdup(stage->id);
	stage_copy->username = bstrdup(stage->username);
	stage_copy->title = bstrdup(stage->title);
	stage_copy->broadcast_id = bstrdup(stage->broadcast_id);
	stage_copy->upsert_broadcast = stage->upsert_broadcast;
	stage_copy->live = stage->live;
	stage_copy->num_feeds = stage->num_feeds;

	if (stage_copy->num_feeds > 0) {
		stage_copy->feeds
	    		= bmalloc(sizeof(struct caffeine_feed) * stage_copy->num_feeds);
	}

	for (size_t i = 0; i < stage_copy->num_feeds; ++i) {
		caffeine_copy_feed_contents(&stage->feeds[i], &stage_copy->feeds[i]);
	}

	copy->stage = stage_copy;

	return copy;
}

void caffeine_free_stage_request(struct caffeine_stage_request ** request)
{
	if (!request || !*request) {
		return;
	}

	bfree((*request)->username);
	bfree((*request)->client_id);
	bfree((*request)->cursor);
	caffeine_free_stage(&(*request)->stage);
	bfree(*request);
	*request = NULL;
}

void caffeine_free_stage_response(struct caffeine_stage_response ** response)
{
	if (!response || !*response) {
		return;
	}

	bfree((*response)->cursor);
    	caffeine_free_stage(&(*response)->stage);
	bfree(*response);

	*response = NULL;
}

void caffeine_free_failure(struct caffeine_failure_response ** failure)
{
	if (!failure || !*failure) {
		return;
	}

	bfree((*failure)->type);
	bfree((*failure)->reason);
	bfree((*failure)->display_message.title);
	bfree((*failure)->display_message.body);
	bfree(*failure);

	*failure = NULL;
}

void caffeine_free_stage_response_result(struct caffeine_stage_response_result ** result)
{
	if (!result || !*result) {
		return;
	}

	caffeine_free_stage_response(&(*result)->response);
	caffeine_free_failure(&(*result)->failure);
	bfree(*result);

	*result = NULL;
}

static json_t * caffeine_serialize_stage_request(struct caffeine_stage_request request)
{
	json_t * request_json = json_object();
	json_object_set_new(request_json, "client",
		json_pack("{s:s,s:o}",
			"id", request.client_id,
			"headless", json_true()));

	if (request.cursor) {
		json_object_set_new(request_json, "cursor", json_string(request.cursor));
	}

	if (request.stage) {
	json_t * stage = json_pack(
		"{s:s,s:s,s:s?,s:s?,s:o,s:o}",
		"id", request.stage->id,
		"username", request.stage->username,
		"title", request.stage->title,
		"broadcast_id", request.stage->broadcast_id,
		"upsert_broadcast", json_boolean(request.stage->upsert_broadcast),
		"live", json_boolean(request.stage->live));

	json_t * feeds = json_object();

	for (size_t i = 0; i < request.stage->num_feeds; ++i) {
	    	struct caffeine_feed * feed = &request.stage->feeds[i];
	    	json_t * json_feed = json_pack(
			"{s:s,s:s?,s:s?,s:s?,s:s?,s:f,s:{s:o,s:o}}",
			"id", feed->id,
			"client_id", feed->client_id,
			"role", feed->role,
			"description", feed->description,
			"source_connection_quality", feed->source_connection_quality,
			"volume", feed->volume,
			"capabilities",
				"video", json_boolean(feed->capabilities.video),
				"audio", json_boolean(feed->capabilities.audio));

		if (feed->content.id && feed->content.type) {
			json_object_set_new(json_feed, "content", json_pack(
				"{s:s,s:s}", "id", feed->content.id, "type", feed->content.type));
		}

		if (feed->stream.sdp_offer || feed->stream.id) {
			json_object_set_new(json_feed, "stream", json_pack(
				"{s:s?,s:s?,s:s?,s:s?,s:s?}",
				"id", feed->stream.id,
				"source_id", feed->stream.source_id,
				"url", feed->stream.url,
				"sdp_offer", feed->stream.sdp_offer,
				"sdp_answer", feed->stream.sdp_answer));
		}

		json_object_set_new(feeds, feed->id, json_feed);
	}

		json_object_set_new(stage, "feeds", feeds);
		json_object_set_new(request_json, "payload", stage);
	}

	return request_json;
}

static struct caffeine_stage_response * caffeine_deserialize_stage_response(
	json_t * json)
{
	struct caffeine_stage_response * response = NULL;
	struct caffeine_stage * stage = NULL;
    	char * cursor = NULL;
    	double retry_in = 0.0;
    	if (json_unpack(
        	json,
        	"{s:s, s:F}",
        	"cursor",
        	&cursor,
        	"retry_in",
        	&retry_in) != 0)
	{
        	log_error("Failed to parse stage response cursor and retry_in");
        	goto json_parsed_error;
    	}

    	json_t * payload = json_object_get(json, "payload");
    	if (!payload || payload->type != JSON_OBJECT) {
        	log_error("Response did not contain a payload");
        	goto json_parsed_error;
    	}

	stage = bzalloc(sizeof(struct caffeine_stage));

	char * id = NULL;
	char * username = NULL;
	char * title = NULL;
	char * broadcast_id = NULL;
	int upsert_broadcast = 0;
	int live = 0;

	if (json_unpack(payload,
		"{s:s,s:s,s:s,s?s,s?b,s:b}",
		"id", &id,
		"username", &username,
		"title", &title,
		"broadcast_id", &broadcast_id,
		"upsert_broadcast", &upsert_broadcast,
		"live", &live) != 0)
	{
		log_error("Failed to parse stage");
		goto json_parsed_error;
    	}

	stage->id = bstrdup(id);
	stage->username = bstrdup(username);
	stage->title = bstrdup(title);
	stage->broadcast_id = bstrdup(broadcast_id);
	stage->upsert_broadcast = upsert_broadcast;
	stage->live = live;

	json_t * json_feeds = json_object_get(payload, "feeds");

	if (json_is_object(json_feeds)) {
		stage->num_feeds = json_object_size(json_feeds);
		if (stage->num_feeds > 0) {
			stage->feeds =
				bzalloc(sizeof(struct caffeine_feed) * stage->num_feeds);
			struct caffeine_feed * feed_pointer = stage->feeds;
            		const char *feed_key;
            		json_t *feed_value;

			json_object_foreach(json_feeds, feed_key, feed_value) {
                		struct caffeine_feed feed = { NULL };
				int video = 1;
				int audio = 1;

				int result = json_unpack(
					feed_value,
					"{s:s,s?s,s?s,s?s,s?s,s?F,s?{s:b,s:b},s?{s:s,s:s},s?{s?s,s?s,s?s,s?s,s?s}}",
					"id", &feed.id,
					"client_id", &feed.client_id,
					"role", &feed.role,
					"description", &feed.description,
					"source_connection_quality",
						&feed.source_connection_quality,
					"volume", &feed.volume,
					"capabilities",
						"video", &video,
						"audio", &audio,
					"content",
						"id", &feed.content.id,
						"type", &feed.content.type,
					"stream",
						"id", &feed.stream.id,
						"source_id", &feed.stream.source_id,
						"url", &feed.stream.url,
						"sdp_offer", &feed.stream.sdp_offer,
						"sdp_answer", &feed.stream.sdp_answer);

				if (result != 0) {
					log_error("Failed to parse feed");
					goto json_parsed_error;
				}

                		feed.capabilities.video = video;
                		feed.capabilities.audio = audio;

				caffeine_copy_feed_contents(&feed, feed_pointer);
				++feed_pointer;
			}
		}
	}

	response = bzalloc(sizeof(struct caffeine_stage_response));
	response->cursor = bstrdup(cursor);
	response->retry_in = retry_in;
	response->stage = stage;
	stage = NULL;

json_parsed_error:
	caffeine_free_stage(&stage);

    	return response;
}

static struct caffeine_stage_response_result * do_caffeine_stage_update(
	struct caffeine_stage_request request,
	struct caffeine_credentials * creds)
{
	trace();

    	if (!request.username) {
		log_error("Did not set request username");
        	return NULL;
    	}

    	json_t * request_json = caffeine_serialize_stage_request(request);
    	char * request_body = json_dumps(request_json, 0);
    	json_decref(request_json);
    	if (!request_body) {
        	log_error("Failed to serialize request JSON");
        	return NULL;
    	}

	struct caffeine_stage_response_result * result = NULL;
	struct caffeine_stage_response * stage_response = NULL;
	struct caffeine_failure_response * failure_response = NULL;

    	CURL * curl = curl_easy_init();
    	if (!curl) {
        	log_error("Failed to initialize cURL");
        	goto curl_init_error;
    	}

	struct dstr url_str;
	dstr_init(&url_str);
	dstr_printf(&url_str, STAGE_UPDATE_URL_F, request.username);

	curl_easy_setopt(curl, CURLOPT_URL, url_str.array);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

	struct curl_slist *headers =
	caffeine_authenticated_headers(CONTENT_TYPE_JSON, creds);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	struct dstr response_str;
	dstr_init(&response_str);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
		     caffeine_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_str);

	char curl_error[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_error("HTTP failure performing stage update: [%d] %s",
			res, curl_error);
		goto request_error;
	}

    	long response_code;
    	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    	log_debug("Http response [%ld]", response_code);

	if (response_code == 401) {
		log_info("Unauthorized - refreshing credentials");
		if (refresh_credentials(creds)) {
			result = do_caffeine_stage_update(request, creds);
		}
		goto auth_refresh;
	}

	json_error_t json_error;
	json_t * response_json = json_loads(response_str.array, 0, &json_error);
	if (!response_json) {
		log_error("Failed to deserialized stage update response to JSON: %s",
			  json_error.text);
		goto json_failed_error;
	}

	if (response_code == 200) {
		stage_response = caffeine_deserialize_stage_response(response_json);
	} else {
		char * type = NULL;
		char * reason = NULL;
		char * display_message_title = NULL;
		char * display_message_body = NULL;

		if (json_unpack(response_json, "{s?s,s?s,s?{s?s,s:s}}",
			"type", &type,
			"reason", &reason,
			"display_message",
				"title", &display_message_title,
				"body", &display_message_body) != 0)
		{
			log_error("Failed to unpack failure response");
			goto json_parsed_error;
		}

		/*
		 As of now, the only failure response we want to return and
		 not retry is `OutOfCapacity`
		*/
		if (!type || strcmp(type, "OutOfCapacity") != 0) {
			goto standard_failure_response;
		}

		struct caffeine_failure_response failure = {
			.type = bstrdup(type),
			.reason = bstrdup(reason),
			.display_message = {
				.title = bstrdup(display_message_body),
				.body = bstrdup(display_message_body)
			}
		};

		failure_response = bzalloc(sizeof(struct caffeine_failure_response));
		*failure_response = failure;
	}

json_parsed_error:
standard_failure_response:
	json_decref(response_json);
json_failed_error:
request_error:
auth_refresh:
	dstr_free(&response_str);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	dstr_free(&url_str);
curl_init_error:
    	free(request_body);

	if (stage_response || failure_response) {
		result = bzalloc(sizeof(struct caffeine_stage_response_result));

		if (stage_response) {
			result->response = stage_response;
		} else {
			result->failure = failure_response;
		}

		return result;
	}

    	return NULL;
}

struct caffeine_stage_response_result * caffeine_stage_update(
	 struct caffeine_stage_request request,
	 struct caffeine_credentials * creds)
{
	retry_request(struct caffeine_stage_response_result *,
        do_caffeine_stage_update(request, creds));
}

static void transfer_stage_data(
	struct caffeine_stage_response ** from_response,
	struct caffeine_stage_request * to_request)
{
	bfree(to_request->cursor);
	caffeine_free_stage(&to_request->stage);
	to_request->cursor = (*from_response)->cursor;
	to_request->stage = (*from_response)->stage;
	(*from_response)->cursor = NULL;
	(*from_response)->stage = NULL;
	caffeine_free_stage_response(from_response);
}

bool caffeine_request_stage_update(
	struct caffeine_stage_request * request,
	struct caffeine_credentials * creds,
	double * retry_in)
{
	struct caffeine_stage_response_result * result =
	caffeine_stage_update(*request, creds);

	bool success = result && result->response;
	if (success) {
		if (retry_in) {
			*retry_in = result->response->retry_in;
		}
		transfer_stage_data(&result->response, request);
	}

	caffeine_free_stage_response_result(&result);

	return success;
}
