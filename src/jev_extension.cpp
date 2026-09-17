#define DUCKDB_EXTENSION_MAIN

#include "jev_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "jev_secret.hpp"
#include "jev_client.hpp"

#include <atomic>
#include <thread>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind data shared by every function: the question, and where to send it.
//===--------------------------------------------------------------------===//
//! What to do with a row whose request failed after its retries.
enum class JevOnError : uint8_t { FAIL, NULL_ROW };

static constexpr const char *ON_ERROR_SETTING = "jev_on_error";

static JevOnError ParseOnError(const string &mode) {
	auto lower = StringUtil::Lower(mode);
	if (lower == "fail") {
		return JevOnError::FAIL;
	}
	if (lower == "null") {
		return JevOnError::NULL_ROW;
	}
	throw InvalidInputException("%s must be 'fail' or 'null', got '%s'", ON_ERROR_SETTING, mode);
}

static void ValidateOnError(ClientContext &context, SetScope scope, Value &parameter) {
	ParseOnError(parameter.ToString());
}

static JevOnError CurrentOnError(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting(ON_ERROR_SETTING, v) && !v.IsNull()) {
		return ParseOnError(v.ToString());
	}
	return JevOnError::FAIL;
}

//! Enum index of an option name, or -1 when the model answered off-list.
static int64_t IndexOf(const JevQuestion &question, const string &option) {
	for (idx_t i = 0; i < question.criteria_map.size(); i++) {
		if (question.criteria_map[i].first == option) {
			return (int64_t)i;
		}
	}
	return -1;
}

struct JevBindData : public FunctionData {
	JevQuestions questions;
	JevSettings settings;
	JevOnError on_error;
	JevBindData(JevQuestions q, JevSettings s, JevOnError e)
	    : questions(std::move(q)), settings(std::move(s)), on_error(e) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JevBindData>(questions, settings, on_error);
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<JevBindData>();
		return questions == o.questions && settings.endpoint == o.settings.endpoint &&
		       settings.model == o.settings.model && on_error == o.on_error;
	}
	//! The single question of jev_choice, jev_score and jev_noul.
	const JevQuestion &Only() const {
		return questions[0].second;
	}
};

static unique_ptr<JevBindData> SingleQuestionBindData(ClientContext &context, JevQuestion question) {
	JevQuestions questions;
	questions.emplace_back("q", std::move(question));
	// Fail now, not at row one, when there is no secret to call the API with.
	return make_uniq<JevBindData>(std::move(questions), ResolveJevSettings(context), CurrentOnError(context));
}

//! The criteria argument decides the return type, and a type must be known at plan
//! time. So it has to be a constant. Evaluate it, or explain why not.
static Value ConstantCriteria(ClientContext &context, const string &fn, vector<unique_ptr<Expression>> &args) {
	if (args.size() != 2) {
		throw BinderException("%s(state, criteria) takes exactly two arguments", fn);
	}
	if (!args[1]->IsFoldable()) {
		throw BinderException("%s: the criteria argument must be a constant", fn);
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, *args[1]);
	if (value.IsNull()) {
		throw BinderException("%s: the criteria argument must not be NULL", fn);
	}
	return value;
}

//! Reads a MAP(VARCHAR, VARCHAR) constant into ordered key/description pairs.
static vector<std::pair<string, string>> ReadCriteriaMap(const string &fn, const Value &map) {
	auto &entries = MapValue::GetChildren(map);
	vector<std::pair<string, string>> out;
	for (idx_t i = 0; i < entries.size(); i++) {
		auto &kv = StructValue::GetChildren(entries[i]);
		if (kv[0].IsNull()) {
			throw BinderException("%s: criteria key %llu is NULL", fn, (unsigned long long)i);
		}
		out.emplace_back(kv[0].ToString(), kv[1].IsNull() ? string() : kv[1].ToString());
	}
	return out;
}

//! The ENUM of a choice question's options. Throws on duplicate names.
static LogicalType EnumOf(const JevQuestion &question) {
	Vector ordered(LogicalType::VARCHAR, question.criteria_map.size());
	auto data = FlatVector::GetData<string_t>(ordered);
	for (idx_t i = 0; i < question.criteria_map.size(); i++) {
		data[i] = StringVector::AddString(ordered, question.criteria_map[i].first);
	}
	return LogicalType::ENUM(ordered, question.criteria_map.size());
}

static JevQuestion ChoiceQuestion(const string &fn, const Value &criteria) {
	JevQuestion question;
	question.type = JevQuestionType::CHOICE;
	question.criteria_map = ReadCriteriaMap(fn, criteria);
	if (question.criteria_map.empty()) {
		throw BinderException("%s: the criteria map must not be empty", fn);
	}
	if (question.criteria_map.size() > 255) {
		throw BinderException("%s: at most 255 options, got %llu", fn,
		                      (unsigned long long)question.criteria_map.size());
	}
	return question;
}

static JevQuestion ScoreQuestion(const string &fn, const Value &criteria) {
	JevQuestion question;
	question.type = JevQuestionType::SCORE;
	for (auto &level : ListValue::GetChildren(criteria)) {
		if (level.IsNull()) {
			throw BinderException("%s: a rubric level is NULL", fn);
		}
		question.criteria_list.push_back(level.ToString());
	}
	if (question.criteria_list.size() < 2) {
		throw BinderException("%s: the rubric needs at least two levels to be a scale", fn);
	}
	return question;
}

static JevQuestion NoulQuestion(const string &fn, const Value &criteria) {
	JevQuestion question;
	question.type = JevQuestionType::NOUL;
	question.criteria_map = ReadCriteriaMap(fn, criteria);
	if (question.criteria_map.empty()) {
		throw BinderException("%s: the criteria map must not be empty", fn);
	}
	for (auto &kv : question.criteria_map) {
		if (kv.first != "true" && kv.first != "false") {
			throw BinderException("%s: criteria keys must be 'true' or 'false', got '%s'", fn, kv.first);
		}
	}
	return question;
}

//===--------------------------------------------------------------------===//
// jev_choice(state, MAP{option: description}) -> ENUM(options...)
//
// The keys of the criteria map become the SQL ENUM, so the column type and the
// option set the model is constrained to come from one literal and cannot drift.
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> JevChoiceBind(ClientContext &context, ScalarFunction &bound_function,
                                              vector<unique_ptr<Expression>> &args) {
	auto question = ChoiceQuestion("jev_choice", ConstantCriteria(context, "jev_choice", args));
	bound_function.return_type = EnumOf(question);
	return SingleQuestionBindData(context, std::move(question));
}

//===--------------------------------------------------------------------===//
// jev_score(state, [level, level, ...]) -> DOUBLE on the rubric scale
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> JevScoreBind(ClientContext &context, ScalarFunction &bound_function,
                                             vector<unique_ptr<Expression>> &args) {
	return SingleQuestionBindData(context, ScoreQuestion("jev_score", ConstantCriteria(context, "jev_score", args)));
}

//===--------------------------------------------------------------------===//
// jev_noul(state, MAP{'true': meaning, 'false': meaning}) -> DOUBLE, P(true)
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> JevNoulBind(ClientContext &context, ScalarFunction &bound_function,
                                            vector<unique_ptr<Expression>> &args) {
	return SingleQuestionBindData(context, NoulQuestion("jev_noul", ConstantCriteria(context, "jev_noul", args)));
}

//===--------------------------------------------------------------------===//
// jev_ask(state, {name: criteria, ...}) -> STRUCT(name TYPE, name_confidence DOUBLE, ...)
//
// Several questions about one row in one request, which is the only lever the
// API offers on cost. The field type follows the criteria shape:
//   MAP                          -> choice, ENUM of the keys, plus name_confidence
//   LIST                         -> score, DOUBLE on the rubric, plus name_confidence
//   MAP with only true/false     -> noul, DOUBLE probability of true
//===--------------------------------------------------------------------===//
static bool LooksLikeNoul(const Value &map) {
	auto &entries = MapValue::GetChildren(map);
	if (entries.empty()) {
		return false;
	}
	for (auto &entry : entries) {
		auto &kv = StructValue::GetChildren(entry);
		auto key = kv[0].IsNull() ? string() : kv[0].ToString();
		if (key != "true" && key != "false") {
			return false;
		}
	}
	return true;
}

static unique_ptr<FunctionData> JevAskBind(ClientContext &context, ScalarFunction &bound_function,
                                           vector<unique_ptr<Expression>> &args) {
	auto spec = ConstantCriteria(context, "jev_ask", args);
	if (spec.type().id() != LogicalTypeId::STRUCT) {
		throw BinderException("jev_ask: the questions argument must be a STRUCT of name: criteria");
	}
	auto &names = StructType::GetChildTypes(spec.type());
	auto &values = StructValue::GetChildren(spec);
	if (names.empty()) {
		throw BinderException("jev_ask: give at least one question");
	}
	JevQuestions questions;
	child_list_t<LogicalType> fields;
	for (idx_t i = 0; i < names.size(); i++) {
		auto &name = names[i].first;
		auto &criteria = values[i];
		auto fn = "jev_ask." + name;
		if (criteria.IsNull()) {
			throw BinderException("%s: criteria must not be NULL", fn);
		}
		switch (criteria.type().id()) {
		case LogicalTypeId::LIST: {
			questions.emplace_back(name, ScoreQuestion(fn, criteria));
			fields.emplace_back(name, LogicalType::DOUBLE);
			fields.emplace_back(name + "_confidence", LogicalType::DOUBLE);
			break;
		}
		case LogicalTypeId::MAP: {
			if (LooksLikeNoul(criteria)) {
				questions.emplace_back(name, NoulQuestion(fn, criteria));
				fields.emplace_back(name, LogicalType::DOUBLE);
			} else {
				auto question = ChoiceQuestion(fn, criteria);
				fields.emplace_back(name, EnumOf(question));
				fields.emplace_back(name + "_confidence", LogicalType::DOUBLE);
				questions.emplace_back(name, std::move(question));
			}
			break;
		}
		default:
			throw BinderException("%s: criteria must be a MAP (choice or noul) or a LIST (score), got %s", fn,
			                      criteria.type().ToString());
		}
	}
	bound_function.return_type = LogicalType::STRUCT(std::move(fields));
	return make_uniq<JevBindData>(std::move(questions), ResolveJevSettings(context), CurrentOnError(context));
}

//===--------------------------------------------------------------------===//
// Execution. One POST per row is the API's floor: a request carries exactly one
// state. But the rows of a chunk are independent, so they are requested
// concurrently rather than waited for in turn. The pool is per chunk and bounded;
// DuckDB may already be running several chunks on several threads.
//===--------------------------------------------------------------------===//
static constexpr idx_t MAX_CONCURRENT_REQUESTS = 16;

//! Runs `work(row, state_text)` for every non-NULL row of the chunk, concurrently.
//! `work` writes its own outputs at `row`; distinct rows are distinct memory, so
//! only validity masks need the lock. A failed row either fails the query or, with
//! jev_on_error = 'null', nulls the result at that row.
template <class WORK>
static void ForEachRowConcurrently(DataChunk &args, ExpressionState &state, Vector &result, WORK work) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind = func_expr.bind_info->Cast<JevBindData>();
	auto count = args.size();

	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(count, input);
	auto input_data = UnifiedVectorFormat::GetData<string_t>(input);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &out_validity = FlatVector::Validity(result);

	// Gather the rows that need a request. NULL in, NULL out, no request.
	vector<idx_t> pending;
	pending.reserve(count);
	for (idx_t row = 0; row < count; row++) {
		auto idx = input.sel->get_index(row);
		if (input.validity.RowIsValid(idx)) {
			pending.push_back(row);
		} else {
			out_validity.SetInvalid(row);
		}
	}

	JevClient client(bind.settings);
	std::atomic<idx_t> next(0);
	std::mutex lock;
	string first_error;

	auto worker = [&]() {
		while (true) {
			auto i = next.fetch_add(1);
			if (i >= pending.size()) {
				return;
			}
			auto row = pending[i];
			auto idx = input.sel->get_index(row);
			try {
				work(bind, client, row, input_data[idx].GetString());
			} catch (std::exception &ex) {
				std::lock_guard<std::mutex> guard(lock);
				if (bind.on_error == JevOnError::NULL_ROW) {
					out_validity.SetInvalid(row); // the row is lost, the query is not
				} else if (first_error.empty()) {
					first_error = ex.what();
				}
			}
		}
	};

	auto workers = MinValue<idx_t>(MAX_CONCURRENT_REQUESTS, pending.size());
	vector<std::thread> pool;
	for (idx_t i = 1; i < workers; i++) {
		pool.emplace_back(worker);
	}
	if (workers > 0) {
		worker(); // this thread pulls its share too
	}
	for (auto &t : pool) {
		t.join();
	}
	if (!first_error.empty()) {
		throw IOException("%s", first_error);
	}
}

static uint8_t EnumIndexOrThrow(const string &fn, const JevQuestion &question, const JevAnswer &answer) {
	auto option = IndexOf(question, answer.choice);
	if (option < 0) {
		// The model is constrained to the option set; reaching this means the
		// service broke its contract. Surface it, do not coerce it.
		throw IOException("%s: the model answered '%s', which is not one of the options", fn, answer.choice);
	}
	return (uint8_t)option;
}

static void JevChoiceExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto out = FlatVector::GetData<uint8_t>(result);
	ForEachRowConcurrently(args, state, result, [&](const JevBindData &bind, JevClient &client, idx_t row, string s) {
		out[row] = EnumIndexOrThrow("jev_choice", bind.Only(), client.Ask(s, bind.Only()));
	});
}

static void JevNumberExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto out = FlatVector::GetData<double>(result);
	ForEachRowConcurrently(args, state, result, [&](const JevBindData &bind, JevClient &client, idx_t row, string s) {
		out[row] = client.Ask(s, bind.Only()).number;
	});
}

static void JevAskExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &children = StructVector::GetEntries(result);
	for (auto &child : children) {
		child->SetVectorType(VectorType::FLAT_VECTOR);
	}
	ForEachRowConcurrently(args, state, result, [&](const JevBindData &bind, JevClient &client, idx_t row, string s) {
		auto answers = client.Ask(s, bind.questions);
		idx_t field = 0;
		for (auto &named : bind.questions) {
			auto &answer = answers[named.first];
			auto &question = named.second;
			switch (question.type) {
			case JevQuestionType::CHOICE:
				FlatVector::GetData<uint8_t>(*children[field++])[row] =
				    EnumIndexOrThrow("jev_ask." + named.first, question, answer);
				FlatVector::GetData<double>(*children[field++])[row] = answer.confidence;
				break;
			case JevQuestionType::SCORE:
				FlatVector::GetData<double>(*children[field++])[row] = answer.number;
				FlatVector::GetData<double>(*children[field++])[row] = answer.confidence;
				break;
			default:
				FlatVector::GetData<double>(*children[field++])[row] = answer.number;
				break;
			}
		}
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	RegisterJevSecret(loader);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(ON_ERROR_SETTING,
	                          "What to do with a row whose Jev request failed after retries: 'fail' the query "
	                          "(default) or return 'null' for that row",
	                          LogicalType::VARCHAR, Value("fail"), ValidateOnError);

	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);

	// Registered with a placeholder return type; bind overwrites it per call site.
	ScalarFunction jev_choice("jev_choice", {LogicalType::VARCHAR, map_type}, LogicalType::ANY, JevChoiceExec,
	                          JevChoiceBind);
	ScalarFunction jev_score("jev_score", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)},
	                         LogicalType::DOUBLE, JevNumberExec, JevScoreBind);
	ScalarFunction jev_noul("jev_noul", {LogicalType::VARCHAR, map_type}, LogicalType::DOUBLE, JevNumberExec,
	                        JevNoulBind);
	ScalarFunction jev_ask("jev_ask", {LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::ANY, JevAskExec,
	                       JevAskBind);
	// A network call is not a pure function. This also stops DuckDB folding it.
	for (auto fn : {&jev_choice, &jev_score, &jev_noul, &jev_ask}) {
		fn->stability = FunctionStability::VOLATILE;
		loader.RegisterFunction(*fn);
	}
}

void JevExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string JevExtension::Name() {
	return "jev";
}
std::string JevExtension::Version() const {
#ifdef EXT_VERSION_JEV
	return EXT_VERSION_JEV;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(jev, loader) {
	duckdb::LoadInternal(loader);
}
}
