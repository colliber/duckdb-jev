#pragma once
#include "duckdb.hpp"

namespace duckdb {

//! Resolved connection settings, read from the `jev` secret.
struct JevSettings {
	string api_key;
	string endpoint = "https://api.typesafe.ai";
	string model = "jev-latest";
};

//! Registers the `jev` secret type: CREATE SECRET (TYPE jev, API_KEY '...', ENDPOINT '...', MODEL '...')
void RegisterJevSecret(ExtensionLoader &loader);

//! Looks up the jev secret. Throws BinderException when none exists, so a query
//! that can never succeed fails before execution.
JevSettings ResolveJevSettings(ClientContext &context);

} // namespace duckdb
