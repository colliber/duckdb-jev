#pragma once
#include "duckdb.hpp"
#include "jev_secret.hpp"

namespace duckdb {

//! One Choice question: option -> description, exactly as the API's
//! ChoiceQuestion.criteria expects it.
struct JevChoiceQuestion {
	vector<std::pair<string, string>> criteria;
};

//! Answer to a Choice question.
struct JevChoiceAnswer {
	string choice;
	double confidence = 0;
};

//! Talks to POST {endpoint}/v1/systemone. One state per request.
class JevClient {
public:
	explicit JevClient(JevSettings settings) : settings(std::move(settings)) {
	}
	//! Ask one Choice question about one state. Throws on transport or protocol error.
	JevChoiceAnswer AskChoice(const string &state, const JevChoiceQuestion &question);

private:
	JevSettings settings;
	//! Process-wide answer cache keyed on endpoint + request body. DuckDB evaluates a
	//! volatile function once per occurrence, so the same row asked in WHERE and again
	//! in SELECT would otherwise be two paid requests.
	static bool CacheGet(const string &key, string &body);
	static void CachePut(const string &key, const string &body);
	string BuildChoiceRequest(const string &state, const JevChoiceQuestion &question);
	string Post(const string &body);
	JevChoiceAnswer ParseChoiceResponse(const string &body);
};

} // namespace duckdb
