# duckdb-jev

A DuckDB extension that asks [Jev](https://typesafe.ai) a typed question about every
row, and returns the answer as a **real SQL type**.

```console
D CREATE SECRET (TYPE jev, API_KEY 'sk-...');
D CREATE TABLE tickets AS SELECT * FROM (VALUES
      (1, 'I want a refund for last month, the charge was wrong'),
      (2, 'The export button crashes on files over 100MB'),
      (3, 'Great product, the new dashboard is lovely')) t(id, body);

D SELECT id, jev_choice(body, MAP{
      'refund': 'The customer wants money back',
      'bug':    'The customer reports something broken',
      'praise': 'The customer is complimenting the product'
  }) AS intent
  FROM tickets;
┌───────┬─────────────────────────────────┐
│  id   │             intent              │
│ int32 │ enum('refund', 'bug', 'praise') │
├───────┼─────────────────────────────────┤
│     1 │ refund                          │
│     2 │ bug                             │
│     3 │ praise                          │
└───────┴─────────────────────────────────┘
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

## Asking a question

Every function takes the row's text as its first argument and a **criteria**
literal as its second. The criteria does two jobs at once: it is sent to the model
as the set of permitted answers, and it decides the SQL type that comes back. That
is why the two can never disagree. It must be a constant, because a type has to be
known before the query runs.

```console
D CREATE SECRET (TYPE jev, API_KEY 'sk-...');
```

`ENDPOINT` and `MODEL` are optional. Without a secret, a query fails when it is
planned rather than on its first row.

| Call | Criteria you write | Column you get |
|---|---|---|
| `jev_choice(text, MAP{option: meaning})` | what each option means | `ENUM` of those options |
| `jev_score(text, [worst, ..., best])` | an ordered rubric | `DOUBLE` on that scale |
| `jev_noul(text, MAP{'true': …, 'false': …})` | what yes and no mean | `DOUBLE`, the probability of yes |
| `jev_ask(text, {name: criteria, …})` | any mix of the three | `STRUCT`, one field per question |

The descriptions are not decoration. They are how the model is told what each
option means, so a useful one reads like an instruction to a colleague.

### Ask everything at once

The API charges per piece of text, not per question, and carries only one piece of
text per request. So three questions asked separately cost three requests, and
`jev_ask` asks all three in one. Prefer it whenever you want more than one answer
about a row.

Each field's type follows the shape you wrote: a map becomes an `ENUM`, a list
becomes a `DOUBLE` on the rubric, and a map keyed `true`/`false` becomes a
probability. Choice and score fields get a `<name>_confidence` beside them, which
the model derives from its own answer distribution.

```console
D WITH asked AS (
      SELECT id, jev_ask(body, {
          intent:   MAP{'refund': 'wants money back', 'bug': 'something broken',
                        'praise': 'a compliment'},
          severity: ['trivial', 'minor', 'normal', 'serious', 'critical'],
          urgent:   MAP{'true': 'needs a reply today', 'false': 'can wait'}
      }) AS a FROM tickets)
  SELECT id, a.intent, round(a.severity, 1) AS severity, round(a.urgent, 2) AS urgent
  FROM asked ORDER BY id;
┌───────┬─────────────────────────────────┬──────────┬────────┐
│  id   │             intent              │ severity │ urgent │
│ int32 │ enum('refund', 'bug', 'praise') │  double  │ double │
├───────┼─────────────────────────────────┼──────────┼────────┤
│     1 │ refund                          │      1.7 │   0.52 │
│     2 │ bug                             │      3.0 │   0.49 │
│     3 │ praise                          │      0.6 │   0.46 │
└───────┴─────────────────────────────────┴──────────┴────────┘
```

### What a query costs

One request per row is the floor, so treat these functions like a join against a
paid service rather than like `upper()`. Three things soften it, and none of them
need configuring:

- Rows in a chunk are requested **concurrently**, sixteen at a time.
- Identical requests are **cached** for the life of the process. This matters more
  than it sounds: DuckDB evaluates a function once per place it appears, so the same
  call in `WHERE` and in `SELECT` would otherwise be billed twice per row.
- Rate limits, server errors and dropped connections are **retried** with backoff,
  four attempts. Any other error fails the query immediately, because retrying a
  malformed request cannot help.

Watch the bill as you go:

```console
D SELECT * FROM jev_usage();
┌──────────┬────────────┬──────────────┬───────────────┐
│ requests │ cache_hits │ input_tokens │ output_tokens │
│  int64   │   int64    │    int64     │     int64     │
├──────────┼────────────┼──────────────┼───────────────┤
│        3 │          0 │         1163 │           208 │
└──────────┴────────────┴──────────────┴───────────────┘
```

A row that still fails after its retries fails the whole query. If you would rather
lose the row than the query, `SET jev_on_error = 'null'` puts `NULL` in that cell
and carries on.

Not yet: a published build in the DuckDB community registry, a wasm target, a
per-call error mode, and the full answer distribution as a `MAP` column.

## Licence

MIT
