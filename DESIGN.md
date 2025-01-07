rationale
=========

Before the commit that added this document, Ruby and V8 shared the same system
stack but it's been observed that they don't always co-exist peacefully there.

Symptoms range from unexpected JS stack overflow exceptions and hitting debug
checks in V8, to outright segmentation faults.

To mitigate that, V8 runs on separate threads now.

implementation
==============

Each `MiniRacer::Context` is paired with a native system thread that runs V8.

Multiple Ruby threads can concurrently access the `MiniRacer::Context`.
MiniRacer ensures mutual exclusion. Ruby threads won't trample each other.

Ruby threads communicate with the V8 thread through a mutex-and-condition-variable
protected request/response memory buffer.

The wire format is V8's native (de)serialization format. An encoder/decoder
has been added to MiniRacer.

Requests and (some) responses are prefixed with a single character
that indicates the desired action: `'C'` is `context.call(...)`,
`'E'` is `context.eval(...)`, and so on.

A response from the V8 thread either starts with:

- `'\xff'`, indicating a normal response that should be deserialized as-is

- `'c'`, signaling an in-band request (not a response!) to call a Ruby function
  registered with `context.attach(...)`. In turn, the Ruby thread replies with
  a `'c'` response containing the return value from the Ruby function.

Special care has been taken to ensure Ruby and JS functions can call each other
recursively without deadlocking. The Ruby thread uses a recursive mutex that
excludes other Ruby threads but still allows reentrancy from the same thread.

The exact request and response payloads are documented in the source code but
they are almost universally:

- either a single value (e.g. `true` or `false`), or

- a two or three element array (ex. `[filename, source]` for `context.eval(...)`), or

- for responses, an errback-style `[response, error]` array, where `error`
  is a multi-line string that contains the error message on the first line,
  and, optionally, the stack trace. If not empty, the error string is turned
  into a Ruby exception and raised.

deliberate changes & known bugs
===============================

- `MiniRacer::Platform.set_flags! :single_threaded` still runs everything on
  the same thread but is prone to crashes in Ruby < 3.4.0 due to a Ruby runtime
  bug that clobbers thread-local variables.

- The `Isolate` class is gone. Maintaining a one-to-many relationship between
  isolates and contexts in a multi-threaded environment had a bad cost/benefit
  ratio. `Isolate` methods like `isolate.low_memory_notification` have been
  moved to `Context`, ex., `context.low_memory_notification`.

- The `marshal_stack_depth` argument is still accepted but ignored; it's no
  longer necessary.

- The `ensure_gc_after_idle` argument is a no-op in `:single_threaded` mode.

- The `timeout` argument no longer interrupts long-running Ruby code. Killing
  or interrupting a Ruby thread executing arbitrary code is fraught with peril.

- Returning an invalid JS `Date` object (think `new Date(NaN)`) now raises a
  `RangeError` instead of silently returning a bogus `Time` object.

- Not all JS objects map 1-to-1 to Ruby objects. Typed arrays and arraybuffers
  are currently mapped to `Encoding::ASCII_8BIT`strings as the closest Ruby
  equivalent to a byte buffer.

- Not all JS objects are serializable/cloneable. Where possible, such objects
  are substituted with a cloneable representation, else a `MiniRacer::RuntimeError`
  is raised.

  Promises, argument objects, map and set iterators, etc., are substituted,
  either with an empty object (promises, argument objects), or by turning them
  into arrays (map/set iterators.)

  Function objects are substituted with a marker so they can be represented
  as `MiniRacer::JavaScriptFunction` objects on the Ruby side.

  SharedArrayBuffers are not cloneable by design but aren't really usable in
  `mini_racer` in the first place (no way to share them between isolates.)
