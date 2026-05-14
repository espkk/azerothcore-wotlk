# Metric System Refactor

This note explains why the old metric pipeline was painful to extend and why
the new shape is a better base for QuestDB, Grafana, and future producer-side
optimization work.

## Why The Old Metric System Was Bad

The old system mixed too many responsibilities in `Metric` itself:

- The public metric API, queueing, InfluxDB line formatting, TCP/HTTP transport,
  backend configuration, and retry/error handling all lived in one class.
- Adding QuestDB meant either making `Metric` even larger or leaking backend
  selection details into the public interface.
- Backend selection by runtime string was the wrong abstraction for our use
  case. The server is built with a known metric backend, so the choice should be
  compile-time configuration and implementation detail.

The producer side was also too expensive:

- Every metric allocated a `MetricData` object.
- Records carried dynamic `std::string` and `std::vector` state even for common
  cases like one numeric value and one static tag.
- The queue was effectively unbounded, so a slow or broken backend could let
  metrics consume unbounded memory.
- Values were formatted before enqueue, but the record still kept a dynamic
  shape that was hard to reason about and hard to cap.

The data model was too Influx-shaped and too loose:

- A metric was mostly a category plus either `value` or event `title/text`.
- Events encouraged arbitrary message strings, which are awkward for QuestDB and
  Grafana because they are hard to filter, aggregate, and join cleanly.
- Tags were just string pairs, making it easy to accidentally put high-cardinality
  data such as account ids, player names, or session ids into tag/SYMBOL columns.
- Field keys and tag keys were not strongly represented, so misuse was easy and
  only visible after data hit the database.

The formatter was not a good shared foundation:

- Influx formatting helpers were public-ish implementation baggage on `Metric`.
- Escaping was incomplete and spread through the old implementation.
- QuestDB and InfluxDB both accept line protocol, but the code did not expose a
  backend-neutral record model that both senders could serialize from.

## What The New Design Improves

The new pipeline is centered on `MetricRecord`:

- A record has a table, name, timestamp, tags, and fields.
- Tags and fields have explicit types.
- `MetricSymbol` is the named type we use for metric names, tag names, field
  names, and table names. It is currently `std::string_view`, and can become a
  stricter static-symbol type later.
- Metrics, perf timings, and events are separate concepts instead of all being
  squeezed through category/title/text.

The producer side is bounded and more predictable:

- Producers reserve a slot in a bounded MPSC queue.
- If the queue is full, the sample is dropped and counted instead of allocating
  more memory forever.
- Common records are stored inline in the queue slot.
- Tag and field counts have compile-time caps. If a call site needs more, it has
  to make that explicit by increasing the limit.
- String tag/field values are copied into capped static strings so individual
  records cannot grow without bound.

Backend details are now private:

- `Metric.h` no longer exposes a backend factory or backend name string.
- `Metric.cpp` chooses the backend from compile definitions.
- InfluxDB and QuestDB are both implementations of `IMetricBackend`.
- HTTP transport lives in a small shared helper instead of inside `Metric`.
- Realm is remembered by the backend at initialization and added during
  serialization, rather than being passed through every producer call.

Line protocol is centralized:

- Escaping and value formatting live in `MetricLineProtocol`.
- QuestDB and InfluxDB serialize the same `MetricRecord` model.
- Numeric fields remain numeric fields, string fields remain string fields, and
  tags are clearly treated as line protocol tags / QuestDB SYMBOL values.

The macro surface is harder to misuse:

- `METRIC_VALUE` is for a single metric value.
- `METRIC_VALUES` is for a metric record with multiple fields.
- `METRIC_TIMER` and detailed timers write to the perf table through the same
  record model.
- `METRIC_EVENT` records a named event in an explicit event table.
- `METRIC_EVENT_VALUES` records an event with structured fields, not a random
  log message string.

This makes the QuestDB/Grafana model cleaner:

- Low-cardinality dimensions belong in tags/SYMBOL columns.
- High-cardinality or descriptive values belong in fields.
- Events can be queried alongside perf and metric samples by timestamp.
- Event tables can be chosen deliberately, without creating one table per metric
  name.
- Field names should stay stable per table so we do not accidentally create a
  wide, chaotic schema.

## Current Tradeoffs

This refactor is a better base, not the end state:

- Any non-static string field/tag value must be copied into queue-owned storage
  because the producer returns before the backend serializes the record. The
  current design makes that copy bounded and allocation-free by using static
  buffers.
- Large dynamic strings are capped instead of fully preserved.
- The queue drops samples under pressure by design.
- `MetricMaxTags` and `MetricMaxFields` are intentionally small until real call
  sites prove they need more.
- We still need real-world load testing to decide whether the bounded MPSC queue
  is enough or whether thread-local sharding is worth the complexity.

The important part is that the new system makes these tradeoffs explicit. The
old system hid cost and schema problems inside dynamic allocation, loose strings,
and Influx-specific formatting.
