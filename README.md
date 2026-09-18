# duckdb-jev

Ask a question about every row of a table, in SQL, and get a real SQL type back.

```console
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

`intent` is an `ENUM`, not a `VARCHAR` you cast and hope. A DuckDB extension over
[Jev](https://typesafe.ai), TypeSafe's model for typed answers instead of text.

## Why

The data you want to ask about is already in a table or a Parquet file. The usual
route pulls it out, wraps an API in a script, parses the reply and writes it back.
SQL is the query language everyone already has and DuckDB reads the formats the data
already lives in, so ask the question where the data is.

Typed output alone would not be worth an extension. There is a network call either
way, so retries exist regardless, and models are getting better at schemas, not
worse. The interesting part is what the constraint bought. Jev "outputs all
probabilities in parallel instead of autoregressively generating by token". A model
writing a sentence emits one token at a time because it does not know where it is
going. A model choosing among five known options does not have that problem, so it
does not need that machinery.

| | Server-side time |
|---|---|
| One question about a row | 49 to 104 ms |
| Three questions about the same row | 53 to 91 ms |

Under a tenth of a second, and three questions cost what one costs. Work is not
proportional to how much you ask, which is not how generating text behaves. Being
typed is what made it fast enough to run per row, and a classification you can
afford per row changes what you would attempt in SQL at all.

*Measured from Amsterdam against `api.typesafe.ai`, model `jev-1.13.0`, reading the
service's own processing-time header. End to end I see ~700 ms, nearly all network.
TypeSafe [claim](https://typesafe.ai/blog/introducing-system-one-models-and-jev) 40
to 200 times faster than frontier models, from their own evaluation against
non-reasoning baselines they chose, which their post calls "the higher end of real
world gains". Nobody has published an independent benchmark.*

## Using it

```console
D CREATE SECRET (TYPE jev, API_KEY 'sk-...');
```

`ENDPOINT` and `MODEL` are optional. With no secret, queries fail when planned
rather than part-way through.

Each function takes the row's text, then a **criteria** literal. The criteria tells
the model which answers are permitted and decides the column's type, which is why
they cannot drift apart, and why it must be constant.

| Call | Criteria | Column |
|---|---|---|
| `jev_choice(text, MAP{option: meaning})` | what each option means | `ENUM` of those options |
| `jev_score(text, [worst, ..., best])` | an ordered rubric | `DOUBLE` on that scale |
| `jev_noul(text, MAP{'true': …, 'false': …})` | what yes and no mean | `DOUBLE`, probability of yes |
| `jev_ask(text, {name: criteria, …})` | any mix | `STRUCT`, one field per question |

The descriptions are how the model is told what an option means. Write them like an
instruction to a colleague.

Since three questions cost what one costs, ask them together. Fields take their type
from the criteria shape; choice and score get a `<name>_confidence` beside them.

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

An empty option map, a duplicate option, a one-level rubric, over 255 options or a
non-constant criteria all fail when the query is planned, not on row 400,000.

### Cost

One request per row, so treat these like a join against a paid service. Rows in a
chunk go out sixteen at a time. Identical requests are cached for the life of the
process, which matters because DuckDB evaluates a function once per place it
appears, so the same call in `WHERE` and `SELECT` would bill twice per row. Rate
limits, server errors and dropped connections retry with backoff; anything else
fails at once.

```console
D SELECT * FROM jev_usage();
┌──────────┬────────────┬──────────────┬───────────────┐
│ requests │ cache_hits │ input_tokens │ output_tokens │
│  int64   │   int64    │    int64     │     int64     │
├──────────┼────────────┼──────────────┼───────────────┤
│        3 │          0 │         1163 │           208 │
└──────────┴────────────┴──────────────┴───────────────┘
```

`SET jev_on_error = 'null'` loses the row instead of the query.

## Next

- **Batching.** One request can carry many rows: 100 tickets classified correctly in
  a single call, 129 ms, a third of the tokens per row. Not wired up yet.
- **A playground**, so the idea can be tried without an install or a key.
- **Autocomplete** that suggests options from the table's own schema.
- Publishing to the DuckDB community registry.

## Licence

MIT
