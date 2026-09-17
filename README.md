# duckdb-jev

A DuckDB extension that asks [Jev](https://typesafe.ai) a typed question about every
row, and returns the answer as a **real SQL type**.

```sql
SELECT jev_choice(body, MAP{
    'refund': 'The customer wants money back',
    'bug':    'The customer reports something broken',
    'praise': 'The customer is complimenting the product'
}) AS intent
FROM tickets;
```

`intent` is an `ENUM('refund', 'bug', 'praise')`. Not a `VARCHAR` you cast and hope.

## Why this is not another LLM-in-SQL extension

Every existing extension in this space returns text. You cast it, and the cast
fails partway through a query you have already paid for:

```
Conversion Error: Could not convert string 'Sure! Based on the ticket,
I'd say: praise.' to UINT8
```

Three things stack here to make that impossible:

1. **The model cannot produce an off-list answer.** Jev takes the option set as a
   request parameter, not as a wish expressed in a prompt. Chat-completions APIs
   have no equivalent, which is why no existing extension can reach this endpoint
   by pointing a base URL at it.
2. **The return type is built from the same map.** The extension reads the criteria
   map during *bind* and constructs the `ENUM` from its keys, so the column type and
   the model's option set cannot drift apart.
3. **Mistakes fail at bind time.** An empty map, a duplicate option, more than 255
   options, or a non-constant map are rejected before a single API call is made.

What this does **not** claim: that the answer is correct. Strict typing guarantees
the value is in the set and the column has the right type. A wrong answer is still
wrong.

## API contract

Verified directly against `https://api.typesafe.ai/openapi.json` and a live call,
because the published docs differ from the served schema.

| Question | `criteria` shape | Answer fields |
|---|---|---|
| `choice` | object, `option -> description` | `choice`, `confidence`, `probabilities` |
| `score`  | array, an ordered rubric | `score` (continuous), `confidence`, `legend`, `probabilities` |
| `noul`   | object with `true` / `false` | `noul` only, a probability in [0,1] |

One request carries exactly **one state** and **many questions**, so a scalar
function costs one HTTP call per row. Batching questions is the only lever.

A real response, for the ticket "I want a refund for last month, the charge was wrong":

```json
{"model":"jev-1.13.0",
 "answers":{
   "intent":{"type":"choice","choice":"refund","confidence":1.0,
             "probabilities":{"refund":1.0,"bug":0.0,"praise":0.0}},
   "severity":{"type":"score","score":2.1,"confidence":0.9, "...": "..."},
   "urgent":{"type":"noul","noul":0.6}},
 "usage":{"input_tokens":412,"output_tokens":69}}
```

## Status

Working, four functions, each checked end to end against the live API:

| Function | Criteria | Returns |
|---|---|---|
| `jev_choice(state, MAP{option: description})` | the option set | `ENUM(options...)`, built at bind |
| `jev_score(state, [level, ...])` | an ordered rubric | `DOUBLE` on that scale |
| `jev_noul(state, MAP{'true': ..., 'false': ...})` | what each answer means | `DOUBLE`, the probability of true |
| `jev_ask(state, {name: criteria, ...})` | any mix of the above | `STRUCT`, one typed field per question |

`jev_ask` is the one to use for more than one question. Jev bills per state, so three
questions about a row cost one request through `jev_ask` and three through the
scalar functions. The field type follows the criteria shape: a `MAP` is a choice and
becomes an `ENUM`, a `LIST` is a rubric and becomes a `DOUBLE`, and a `MAP` whose keys
are only `true` and `false` is a yes/no question. Choice and score fields carry a
`<name>_confidence` beside them.

```sql
WITH asked AS (
    SELECT id, jev_ask(body, {
        intent:   MAP{'refund': 'wants money back', 'bug': 'something broken'},
        severity: ['trivial', 'minor', 'normal', 'serious', 'critical'],
        urgent:   MAP{'true': 'needs a reply today', 'false': 'can wait'}
    }) AS a FROM tickets)
SELECT id, a.intent, a.severity, a.urgent
FROM asked WHERE a.intent_confidence > 0.8;
```

Shared by all three:

- `CREATE SECRET (TYPE jev, API_KEY '...')`, with optional `ENDPOINT` and `MODEL`.
  No secret is a bind error, not a row-one failure.
- One POST per row, the API's floor. Rows within a chunk go out concurrently:
  16 rows at 200 ms each take 0.36 s, not 3.3 s.
- An answer cache keyed on the request. DuckDB evaluates a volatile function once
  per occurrence, so the same call in `WHERE` and `SELECT` was two bills per row.
  Now one.

- Retry with backoff on 429 and 5xx, up to four attempts. Any other 4xx fails once.

- `SET jev_on_error = 'null'` turns a row whose request failed after its retries into
  `NULL` instead of failing the query. The default is `fail`.
- `SELECT * FROM jev_usage()` reports what the process has spent so far: requests,
  cache hits, and the input and output tokens the API charged. A careless query over
  a large table is a large bill; this is the warning.

Not yet: a build in the community extensions registry, a wasm target, a per-call
`on_error`, and the answer probabilities as a `MAP` column.

## Tests

Two suites, both at the SQL seam:

```sh
make test                          # bind-time behaviour, no network
python3 test/python/test_http.py   # a mock endpoint replaying a recorded response
```

## Building

```sh
GEN=ninja make release
./build/release/duckdb -unsigned
```

## Licence

MIT
