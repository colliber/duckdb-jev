#pragma once
#include "duckdb.hpp"
#include "jev_secret.hpp"

namespace duckdb {

enum class JevQuestionType : uint8_t { CHOICE, SCORE, NOUL };

//! One question, in the shape the API's Question schema expects:
//!  choice: criteria is an object, option -> description
//!  score:  criteria is an ordered array, the rubric
//!  noul:   criteria is an object with the keys true and false
struct JevQuestion {
	JevQuestionType type = JevQuestionType::CHOICE;
	vector<std::pair<string, string>> criteria_map; // choice, noul
	vector<string> criteria_list;                   // score

	bool operator==(const JevQuestion &o) const {
		return type == o.type && criteria_map == o.criteria_map && criteria_list == o.criteria_list;
	}
};

//! The typed answer. Which field is set follows the question type.
struct JevAnswer {
	string choice;         // choice
	double number = 0;     // score: the value on the rubric scale; noul: P(true)
	double confidence = 0; // choice, score
};

//! Questions by name, in the order they were given.
using JevQuestions = vector<std::pair<string, JevQuestion>>;
//! Answers by question name.
using JevAnswers = unordered_map<string, JevAnswer>;

//! Talks to POST {endpoint}/v1/systemone. One state per request, any number of
//! questions: that is the only lever the API offers on cost and latency.
class JevClient {
public:
	explicit JevClient(JevSettings settings) : settings(std::move(settings)) {
	}
	//! Ask one question about one state. Throws on transport or protocol error.
	JevAnswer Ask(const string &state, const JevQuestion &question);
	//! Ask several named questions about one state in a single request.
	JevAnswers Ask(const string &state, const JevQuestions &questions);

private:
	JevSettings settings;
	//! Process-wide answer cache keyed on endpoint + request body. DuckDB evaluates a
	//! volatile function once per occurrence, so the same row asked in WHERE and again
	//! in SELECT would otherwise be two paid requests.
	static bool CacheGet(const string &key, string &body);
	static void CachePut(const string &key, const string &body);
	string BuildRequest(const string &state, const JevQuestions &questions);
	string Post(const string &body);
	JevAnswers ParseResponse(const string &body, const JevQuestions &questions);
};

} // namespace duckdb
