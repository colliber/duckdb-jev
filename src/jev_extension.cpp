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
struct JevBindData : public FunctionData {
	JevQuestion question;
	JevSettings settings;
	JevBindData(JevQuestion q, JevSettings s) : question(std::move(q)), settings(std::move(s)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JevBindData>(question, settings);
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<JevBindData>();
		return question == o.question && settings.endpoint == o.settings.endpoint && settings.model == o.settings.model;
	}
	//! Enum index of an option name, or -1 when the model answered off-list.
	int64_t IndexOf(const string &option) const {
		for (idx_t i = 0; i < question.criteria_map.size(); i++) {
			if (question.criteria_map[i].first == option) {
				return (int64_t)i;
			}
		}
		return -1;
	}
};

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

//===--------------------------------------------------------------------===//
// jev_choice(state, MAP{option: description}) -> ENUM(options...)
//
// The keys of the criteria map become the SQL ENUM, so the column type and the
// option set the model is constrained to come from one literal and cannot drift.
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> JevChoiceBind(ClientContext &context, ScalarFunction &bound_function,
                                              vector<unique_ptr<Expression>> &args) {
	auto criteria = ConstantCriteria(context, "jev_choice", args);
	JevQuestion question;
	question.type = JevQuestionType::CHOICE;
	question.criteria_map = ReadCriteriaMap("jev_choice", criteria);
	if (question.criteria_map.empty()) {
		throw BinderException("jev_choice: the criteria map must not be empty");
	}
	if (question.criteria_map.size() > 255) {
		throw BinderException("jev_choice: at most 255 options, got %llu",
		                      (unsigned long long)question.criteria_map.size());
	}
	Vector ordered(LogicalType::VARCHAR, question.criteria_map.size());
	auto data = FlatVector::GetData<string_t>(ordered);
	for (idx_t i = 0; i < question.criteria_map.size(); i++) {
		data[i] = StringVector::AddString(ordered, question.criteria_map[i].first);
	}
	// Throws on duplicate option names; that is the behaviour we want.
	bound_function.return_type = LogicalType::ENUM(ordered, question.criteria_map.size());
	// Fail now, not at row one, when there is no secret to call the API with.
	auto settings = ResolveJevSettings(context);
	return make_uniq<JevBindData>(std::move(question), std::move(settings));
}

//===--------------------------------------------------------------------===//
// jev_score(state, [level, level, ...]) -> DOUBLE on the rubric scale
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> JevScoreBind(ClientContext &context, ScalarFunction &bound_function,
                                             vector<unique_ptr<Expression>> &args) {
	auto criteria = ConstantCriteria(context, "jev_score", args);
	JevQuestion question;
	question.type = JevQuestionType::SCORE;
	for (auto &level : ListValue::GetChildren(criteria)) {
		if (level.IsNull()) {
			throw BinderException("jev_score: a rubric level is NULL");
		}
		question.criteria_list.push_back(level.ToString());
	}
	if (question.criteria_list.size() < 2) {
		throw BinderException("jev_score: the rubric needs at least two levels to be a scale");
	}
	auto settings = ResolveJevSettings(context);
	return make_uniq<JevBindData>(std::move(question), std::move(settings));
}

//===--------------------------------------------------------------------===//
// jev_noul(state, MAP{'true': meaning, 'false': meaning}) -> DOUBLE, P(true)
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> JevNoulBind(ClientContext &context, ScalarFunction &bound_function,
                                            vector<unique_ptr<Expression>> &args) {
	auto criteria = ConstantCriteria(context, "jev_noul", args);
	JevQuestion question;
	question.type = JevQuestionType::NOUL;
	question.criteria_map = ReadCriteriaMap("jev_noul", criteria);
	if (question.criteria_map.empty()) {
		throw BinderException("jev_noul: the criteria map must not be empty");
	}
	for (auto &kv : question.criteria_map) {
		if (kv.first != "true" && kv.first != "false") {
			throw BinderException("jev_noul: criteria keys must be 'true' or 'false', got '%s'", kv.first);
		}
	}
	auto settings = ResolveJevSettings(context);
	return make_uniq<JevBindData>(std::move(question), std::move(settings));
}

//===--------------------------------------------------------------------===//
// Execution. One POST per row is the API's floor: a request carries exactly one
// state. But the rows of a chunk are independent, so they are requested
// concurrently rather than waited for in turn. The pool is per chunk and bounded;
// DuckDB may already be running several chunks on several threads.
//===--------------------------------------------------------------------===//
static constexpr idx_t MAX_CONCURRENT_REQUESTS = 16;

template <class T, class WRITE>
static void JevExecConcurrent(DataChunk &args, ExpressionState &state, Vector &result, WRITE write) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind = func_expr.bind_info->Cast<JevBindData>();
	auto count = args.size();

	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(count, input);
	auto input_data = UnifiedVectorFormat::GetData<string_t>(input);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<T>(result);
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
	std::mutex error_lock;
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
				auto answer = client.Ask(input_data[idx].GetString(), bind.question);
				out[row] = write(bind, answer);
			} catch (std::exception &ex) {
				std::lock_guard<std::mutex> guard(error_lock);
				if (first_error.empty()) {
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

static void JevChoiceExec(DataChunk &args, ExpressionState &state, Vector &result) {
	JevExecConcurrent<uint8_t>(args, state, result, [](const JevBindData &bind, const JevAnswer &answer) {
		auto option = bind.IndexOf(answer.choice);
		if (option < 0) {
			// The model is constrained to the option set; reaching this means the
			// service broke its contract. Surface it, do not coerce it.
			throw IOException("jev_choice: the model answered '%s', which is not one of the options", answer.choice);
		}
		return (uint8_t)option;
	});
}

static void JevNumberExec(DataChunk &args, ExpressionState &state, Vector &result) {
	JevExecConcurrent<double>(args, state, result,
	                          [](const JevBindData &, const JevAnswer &answer) { return answer.number; });
}

static void LoadInternal(ExtensionLoader &loader) {
	RegisterJevSecret(loader);

	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);

	// Registered with a placeholder return type; bind overwrites it per call site.
	ScalarFunction jev_choice("jev_choice", {LogicalType::VARCHAR, map_type}, LogicalType::ANY, JevChoiceExec,
	                          JevChoiceBind);
	ScalarFunction jev_score("jev_score", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)},
	                         LogicalType::DOUBLE, JevNumberExec, JevScoreBind);
	ScalarFunction jev_noul("jev_noul", {LogicalType::VARCHAR, map_type}, LogicalType::DOUBLE, JevNumberExec,
	                        JevNoulBind);
	// A network call is not a pure function. This also stops DuckDB folding it.
	for (auto fn : {&jev_choice, &jev_score, &jev_noul}) {
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
