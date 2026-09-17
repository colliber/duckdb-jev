PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=jev
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
# ---------------------------------------------------------------------------
# Behaviour tests. `make test` runs the sqllogictests in test/sql, which is the
# DuckDB standard and what CI runs on every platform. Two more targets:
#
#   make test_http   the mock-endpoint suite: one POST per row, cache, retry,
#                    concurrency, on_error. Needs a server that can fail on
#                    demand; Python's stdlib http.server is that server, and
#                    Python is already required by the format and tidy targets.
#   make test_live   test/live/*.test against api.typesafe.ai. Pure SQL,
#                    skipped unless TYPESAFE_API_KEY is set.
#   make test_all    all three.
# ---------------------------------------------------------------------------
DUCKDB_BIN ?= ./build/release/duckdb
JEV_EXTENSION ?= ./build/release/extension/jev/jev.duckdb_extension

test_http:
	DUCKDB_BIN=$(DUCKDB_BIN) JEV_EXTENSION=$(JEV_EXTENSION) python3 test/python/test_http.py

# The runner registers tests by their path relative to the project, tagged with the
# directory name, so test/live/jev.test is `test/live/jev.test [live]`. Filter the
# same way the standard target filters "test/*".
test_live:
	./build/release/$(TEST_PATH) "test/live/*"

test_all: test test_http test_live
