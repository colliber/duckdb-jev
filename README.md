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

**Spike.** `jev_choice` proves the bind-time `ENUM` return type. Execution is
stubbed and makes no network call yet.

Next, in order: the HTTP client and a secret type, a per-chunk thread pool, a
response cache (DuckDB evaluates the same call twice when it appears in both
`WHERE` and `SELECT`, and that is not going to be fixed), and `jev_ask` returning a
struct so many questions cost one call.

## Building

```sh
GEN=ninja make release
./build/release/duckdb -unsigned
```

## Licence

MIT
