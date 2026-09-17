#define DUCKDB_EXTENSION_MAIN

#include "jev_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

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
	vector<string> options;
	explicit JevChoiceBindData(vector<string> o) : options(std::move(o)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JevChoiceBindData>(options);
	}
	bool Equals(const FunctionData &other) const override {
		return options == other.Cast<JevChoiceBindData>().options;
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

	vector<string> options;
	Vector ordered(LogicalType::VARCHAR, entries.size());
	auto data = FlatVector::GetData<string_t>(ordered);
	for (idx_t i = 0; i < entries.size(); i++) {
		auto &kv = StructValue::GetChildren(entries[i]);
		if (kv[0].IsNull()) {
			throw BinderException("jev_choice: option name %llu is NULL", (unsigned long long)i);
		}
		auto name = kv[0].ToString();
		options.push_back(name);
		data[i] = StringVector::AddString(ordered, name);
	}
	// Throws on duplicate option names; that is the behaviour we want.
	bound_function.return_type = LogicalType::ENUM(ordered, entries.size());
	return make_uniq<JevChoiceBindData>(std::move(options));
}

// SPIKE execution: no HTTP yet. Always answers with the first option so that the
// type plumbing can be proven on its own.
static void JevChoiceExec(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<uint8_t>(result)[0] = 0;
	ConstantVector::SetNull(result, false);
}

static void LoadInternal(ExtensionLoader &loader) {
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
