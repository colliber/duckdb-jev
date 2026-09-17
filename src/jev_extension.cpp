#define DUCKDB_EXTENSION_MAIN

#include "jev_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <thread>
#include <atomic>
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "jev_secret.hpp"
#include "jev_client.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// jev_choice(state, criteria) -> ENUM built from the criteria keys
//
// The Jev API takes a Choice question as a map of option -> description
// (verified against https://api.typesafe.ai/openapi.json, ChoiceQuestion.criteria
// is an object, not a list). The keys of that same map become the SQL ENUM, so
// the column type and the model's option set cannot drift apart.
//===--------------------------------------------------------------------===//

struct JevChoiceBindData : public FunctionData {
	JevChoiceQuestion question;
	JevSettings settings;
	JevChoiceBindData(JevChoiceQuestion q, JevSettings s) : question(std::move(q)), settings(std::move(s)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JevChoiceBindData>(question, settings);
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<JevChoiceBindData>();
		return question.criteria == o.question.criteria && settings.endpoint == o.settings.endpoint &&
		       settings.model == o.settings.model;
	}
	//! Enum index of an option name, or -1 when the model answered off-list.
	int64_t IndexOf(const string &option) const {
		for (idx_t i = 0; i < question.criteria.size(); i++) {
			if (question.criteria[i].first == option) {
				return (int64_t)i;
			}
		}
		return -1;
	}
};

static unique_ptr<FunctionData> JevChoiceBind(ClientContext &context, ScalarFunction &bound_function,
                                              vector<unique_ptr<Expression>> &args) {
	if (args.size() != 2) {
		throw BinderException("jev_choice(state, criteria) takes exactly two arguments");
	}
	// The return TYPE depends on this argument, and a type must be known at plan
	// time. So the criteria map has to be a constant. This is the whole design.
	if (!args[1]->IsFoldable()) {
		throw BinderException("jev_choice: the criteria map must be a constant");
	}
	auto criteria = ExpressionExecutor::EvaluateScalar(context, *args[1]);
	if (criteria.IsNull()) {
		throw BinderException("jev_choice: the criteria map must not be NULL");
	}

	auto &entries = MapValue::GetChildren(criteria);
	if (entries.empty()) {
		throw BinderException("jev_choice: the criteria map must not be empty");
	}
	if (entries.size() > 255) {
		throw BinderException("jev_choice: at most 255 options, got %llu", (unsigned long long)entries.size());
	}

	JevChoiceQuestion question;
	Vector ordered(LogicalType::VARCHAR, entries.size());
	auto data = FlatVector::GetData<string_t>(ordered);
	for (idx_t i = 0; i < entries.size(); i++) {
		auto &kv = StructValue::GetChildren(entries[i]);
		if (kv[0].IsNull()) {
			throw BinderException("jev_choice: option name %llu is NULL", (unsigned long long)i);
		}
		auto name = kv[0].ToString();
		question.criteria.emplace_back(name, kv[1].IsNull() ? string() : kv[1].ToString());
		data[i] = StringVector::AddString(ordered, name);
	}
	// Throws on duplicate option names; that is the behaviour we want.
	bound_function.return_type = LogicalType::ENUM(ordered, entries.size());
	// Fail now, not at row one, when there is no secret to call the API with.
	auto settings = ResolveJevSettings(context);
	return make_uniq<JevChoiceBindData>(std::move(question), std::move(settings));
}

// One POST per row is the API's floor: a request carries exactly one state. But the
// rows of a chunk are independent, so they are requested concurrently rather than
// waited for in turn. The pool is per chunk and bounded; DuckDB may already be
// running several chunks on several threads.
static constexpr idx_t MAX_CONCURRENT_REQUESTS = 16;

static void JevChoiceExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind = func_expr.bind_info->Cast<JevChoiceBindData>();
	auto count = args.size();

	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(count, input);
	auto input_data = UnifiedVectorFormat::GetData<string_t>(input);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<uint8_t>(result);
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
				auto answer = client.AskChoice(input_data[idx].GetString(), bind.question);
				auto option = bind.IndexOf(answer.choice);
				if (option < 0) {
					// The model is constrained to the option set; reaching this means the
					// service broke its contract. Surface it, do not coerce it.
					throw IOException("jev_choice: the model answered '%s', which is not one of the options",
					                  answer.choice);
				}
				out[row] = (uint8_t)option;
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

static void LoadInternal(ExtensionLoader &loader) {
	RegisterJevSecret(loader);
	ScalarFunction jev_choice("jev_choice", {LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	                          LogicalType::ANY, JevChoiceExec, JevChoiceBind);
	// A network call is not a pure function. This also stops DuckDB folding it.
	jev_choice.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(jev_choice);
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
