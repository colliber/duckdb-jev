#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"
#include "yyjson.hpp"

#include "jev_client.hpp"
#include "duckdb/common/exception.hpp"
#include <mutex>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

static constexpr const char *SYSTEMONE_PATH = "/v1/systemone";
//! The question key inside the request. One question per request in this slice.
static constexpr const char *QUESTION_KEY = "q";
//! Crude bound: when the cache reaches this many entries it is cleared. Enough for
//! any single query to dedupe itself; not a persistent store.
static constexpr size_t CACHE_MAX_ENTRIES = 100000;
//! Retry budget for 429 and 5xx. The API documents 429 with no quota numbers, so
//! back off and try again. Other 4xx mean the request is wrong; retrying cannot help.
static constexpr int MAX_ATTEMPTS = 4;
static constexpr int FIRST_BACKOFF_MS = 200;

static bool IsRetryable(int status) {
	return status == 429 || (status >= 500 && status < 600);
}

namespace {
std::mutex cache_lock;
std::unordered_map<string, string> cache;
} // namespace

bool JevClient::CacheGet(const string &key, string &body) {
	std::lock_guard<std::mutex> guard(cache_lock);
	auto it = cache.find(key);
	if (it == cache.end()) {
		return false;
	}
	body = it->second;
	return true;
}

void JevClient::CachePut(const string &key, const string &body) {
	std::lock_guard<std::mutex> guard(cache_lock);
	if (cache.size() >= CACHE_MAX_ENTRIES) {
		cache.clear();
	}
	cache[key] = body;
}

string JevClient::BuildChoiceRequest(const string &state, const JevChoiceQuestion &question) {
	unique_ptr<yyjson_mut_doc, void (*)(yyjson_mut_doc *)> doc(yyjson_mut_doc_new(nullptr), &yyjson_mut_doc_free);
	auto root = yyjson_mut_obj(doc.get());
	yyjson_mut_doc_set_root(doc.get(), root);

	yyjson_mut_obj_add_strn(doc.get(), root, "state", state.c_str(), state.size());
	yyjson_mut_obj_add_str(doc.get(), root, "model", settings.model.c_str());

	auto questions = yyjson_mut_obj(doc.get());
	auto q = yyjson_mut_obj(doc.get());
	yyjson_mut_obj_add_str(doc.get(), q, "type", "choice");
	auto criteria = yyjson_mut_obj(doc.get());
	for (auto &kv : question.criteria) {
		yyjson_mut_obj_add_strn(doc.get(), criteria, kv.first.c_str(), kv.second.c_str(), kv.second.size());
	}
	yyjson_mut_obj_add_val(doc.get(), q, "criteria", criteria);
	yyjson_mut_obj_add_val(doc.get(), questions, QUESTION_KEY, q);
	yyjson_mut_obj_add_val(doc.get(), root, "questions", questions);

	size_t len = 0;
	auto json = yyjson_mut_write(doc.get(), 0, &len);
	if (!json) {
		throw InternalException("jev: failed to serialise request");
	}
	string out(json, len);
	free(json);
	return out;
}

string JevClient::Post(const string &body) {
	duckdb_httplib_openssl::Client client(settings.endpoint);
	client.set_connection_timeout(10, 0);
	client.set_read_timeout(30, 0);
	client.set_follow_location(true);

	duckdb_httplib_openssl::Headers headers;
	headers.emplace("Authorization", "Bearer " + settings.api_key);

	int backoff_ms = FIRST_BACKOFF_MS;
	for (int attempt = 1;; attempt++) {
		auto res = client.Post(SYSTEMONE_PATH, headers, body, "application/json");
		if (!res) {
			throw IOException("jev: request to %s failed: %s", settings.endpoint,
			                  duckdb_httplib_openssl::to_string(res.error()));
		}
		if (res->status == 200) {
			return res->body;
		}
		if (!IsRetryable(res->status) || attempt >= MAX_ATTEMPTS) {
			throw IOException("jev: HTTP %d from %s after %d attempt(s): %s", res->status, settings.endpoint, attempt,
			                  res->body);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
		backoff_ms *= 2;
	}
}

JevChoiceAnswer JevClient::ParseChoiceResponse(const string &body) {
	unique_ptr<yyjson_doc, void (*)(yyjson_doc *)> doc(yyjson_read(body.c_str(), body.size(), 0), &yyjson_doc_free);
	if (!doc) {
		throw IOException("jev: response is not valid JSON");
	}
	auto root = yyjson_doc_get_root(doc.get());
	auto answers = yyjson_obj_get(root, "answers");
	auto answer = answers ? yyjson_obj_get(answers, QUESTION_KEY) : nullptr;
	auto choice = answer ? yyjson_obj_get(answer, "choice") : nullptr;
	if (!choice || !yyjson_is_str(choice)) {
		throw IOException("jev: response has no answers.%s.choice", QUESTION_KEY);
	}
	JevChoiceAnswer out;
	out.choice = yyjson_get_str(choice);
	auto confidence = yyjson_obj_get(answer, "confidence");
	if (confidence && yyjson_is_num(confidence)) {
		out.confidence = yyjson_get_num(confidence);
	}
	return out;
}

JevChoiceAnswer JevClient::AskChoice(const string &state, const JevChoiceQuestion &question) {
	auto request = BuildChoiceRequest(state, question);
	// The request body already encodes state, model and criteria; add the endpoint
	// so two services never share an answer.
	auto key = settings.endpoint + "\x1f" + request;
	string response;
	if (!CacheGet(key, response)) {
		response = Post(request);
		CachePut(key, response);
	}
	return ParseChoiceResponse(response);
}

} // namespace duckdb
