#include "jev_secret.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void CopyOption(const string &key, const CreateSecretInput &input, KeyValueSecret &result) {
	auto it = input.options.find(key);
	if (it != input.options.end()) {
		result.secret_map[key] = it->second;
	}
}

static unique_ptr<BaseSecret> CreateJevSecret(ClientContext &context, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	CopyOption("api_key", input, *result);
	CopyOption("endpoint", input, *result);
	CopyOption("model", input, *result);
	result->redact_keys.insert("api_key");
	return std::move(result);
}

void RegisterJevSecret(ExtensionLoader &loader) {
	SecretType type;
	type.name = "jev";
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = "config";
	loader.RegisterSecretType(type);

	CreateSecretFunction config = {"jev", "config", CreateJevSecret};
	config.named_parameters["api_key"] = LogicalType::VARCHAR;
	config.named_parameters["endpoint"] = LogicalType::VARCHAR;
	config.named_parameters["model"] = LogicalType::VARCHAR;
	loader.RegisterFunction(config);
}

JevSettings ResolveJevSettings(ClientContext &context) {
	auto &manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = manager.LookupSecret(transaction, "jev", "jev");
	if (!match.HasMatch()) {
		throw BinderException("jev: no secret found. Run CREATE SECRET (TYPE jev, API_KEY '...') first");
	}
	auto &kv = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
	JevSettings s;
	Value v;
	if (kv.TryGetValue("api_key", v) && !v.IsNull()) {
		s.api_key = v.ToString();
	}
	if (s.api_key.empty()) {
		throw BinderException("jev: the secret has no API_KEY");
	}
	if (kv.TryGetValue("endpoint", v) && !v.IsNull()) {
		s.endpoint = v.ToString();
	}
	if (kv.TryGetValue("model", v) && !v.IsNull()) {
		s.model = v.ToString();
	}
	return s;
}

} // namespace duckdb
