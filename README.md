# duckdb-jev

Ask a question about every row of a table, in SQL, and get back a real SQL type.

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

`intent` is an `ENUM`, not a `VARCHAR` you cast and hope. You can `GROUP BY` it,
join on it, and put it in a column that rejects anything else.

That table is three literal rows so you can paste the example, but it could just as
easily have been `SELECT * FROM 'support/*.parquet'` or a table in Postgres. The
point is that the question is asked where the data already is.

This is a DuckDB extension over [Jev](https://typesafe.ai), a model from TypeSafe
that answers typed questions instead of generating text.

## Why bother

Most of what you would want to ask a model about is already sitting in a table, a
Parquet file, or an object store. Today the usual route is to pull it out, write a
script around an API, parse what comes back, and put it somewhere else. That is a
lot of moving parts for what is really a projection.

SQL is the one query language everybody already has, and DuckDB reads the formats
the data already lives in. So the shortest path from a question to an answer is to
ask it where the data is, rather than moving the data to where the asking happens.
That is the whole idea here: context comes straight from the source.

### The type safety is not the exciting part

Typed output is nice, and it is what you notice first. But on its own I do not find
it compelling. There is still a network call in the middle, so I already need
retries for the small fraction that fails for boring reasons. If a model returned a
value outside my enum, I would handle it in the same place, the same way. And
models keep getting better at that: schema mistakes are a shrinking problem, not a
growing one.

### The exciting part is what the constraint bought

The interesting move is that TypeSafe did not trade anything away for the type
guarantee. They added it, and in doing so got to rebuild the architecture around a
much smaller job. A model that must emit one of five options does not have to
generate a sentence, or a JSON object, or anything it might then have to be checked
against. It has to pick. That is a different shape of problem, and it can be
answered in a different way.

Here is what that looks like from the outside, measured against the live API:

| | Server-side time |
|---|---|
| One question about a row | 49 to 104 ms |
| Three questions about the same row | 53 to 91 ms |

Two things stand out. The first is that it answers in well under a tenth of a
second. The second is stranger and more telling: asking three questions costs
essentially the same as asking one. The work is not proportional to how much you
ask, which is not how generating text behaves.

That is the real innovation, and the reason this extension exists. Not that the
answer is well typed, but that being well typed is what let it get fast enough to
put in the middle of a query over a whole table. A classification you can afford to
run per row changes what you would even attempt in SQL.

*(Latency measured from Amsterdam against `api.typesafe.ai`, model `jev-1.13.0`,
reading the service's own processing time rather than wall clock. End to end I see
about 700 ms per call, nearly all of it network round trip.)*

## Using it

Every function takes the row's text first and a **criteria** literal second. The
criteria does two jobs at once: it tells the model which answers are permitted, and
it decides the SQL type of the column. That is why the two can never drift apart. It
has to be a constant, because a type must be known before the query runs.

```console
D CREATE SECRET (TYPE jev, API_KEY 'sk-...');
```

`ENDPOINT` and `MODEL` are optional. With no secret, a query fails when it is
planned rather than part-way through.

| Call | Criteria you write | Column you get |
|---|---|---|
| `jev_choice(text, MAP{option: meaning})` | what each option means | `ENUM` of those options |
| `jev_score(text, [worst, ..., best])` | an ordered rubric | `DOUBLE` on that scale |
| `jev_noul(text, MAP{'true': …, 'false': …})` | what yes and no mean | `DOUBLE`, the probability of yes |
| `jev_ask(text, {name: criteria, …})` | any mix of the three | `STRUCT`, one field per question |

Those descriptions are not decoration. They are how the model is told what each
option means, so a good one reads like an instruction to a colleague.

### Ask everything at once

Since three questions cost about what one costs, ask them together. `jev_ask` sends
them in a single request and gives you a struct back. Each field takes its type from
the shape you wrote, and choice and score fields carry a `<name>_confidence` beside
them.

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

Mistakes are caught when the query is planned, not on row four hundred thousand: an
empty option map, a duplicate option, a rubric with one level, more than 255
options, or a criteria that is not constant.

### What a query costs

One request per row is the floor, so treat these like a join against a paid service
rather than like `upper()`. Three things soften that, none of which need
configuring:

- Rows in a chunk go out **concurrently**, sixteen at a time.
- Identical requests are **cached** for the life of the process. This matters more
  than it sounds: DuckDB evaluates a function once per place it appears, so the same
  call in `WHERE` and in `SELECT` would otherwise be billed twice per row.
- Rate limits, server errors and dropped connections are **retried** with backoff,
  four attempts. Anything else fails immediately, since retrying a malformed request
  cannot help.

```console
D SELECT * FROM jev_usage();
┌──────────┬────────────┬──────────────┬───────────────┐
│ requests │ cache_hits │ input_tokens │ output_tokens │
│  int64   │   int64    │    int64     │     int64     │
├──────────┼────────────┼──────────────┼───────────────┤
│        3 │          0 │         1163 │           208 │
└──────────┴────────────┴──────────────┴───────────────┘
```

A row that still fails after its retries fails the query. If you would rather lose
the row than the query, `SET jev_on_error = 'null'` puts `NULL` in that cell and
carries on.

## Where this is going

- A **playground** you can open in a browser, with a few tables to poke at, so the
  idea can be tried without an install or a key.
- **Autocomplete while you write**, suggesting options from the table's own schema,
  so composing one of these queries is a matter of picking rather than typing.
- Publishing to the DuckDB community registry, so installing is one line.

## Licence

MIT
