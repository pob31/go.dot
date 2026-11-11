# Handoff: four fixes found in the OSCQuery Automation Plugin that apply to go.dot

*Written 2026-11-11 by a Claude session working in the OSCQuery Automation Plugin
(`../OSCQuery-Automation-plugin`), for the author and for whichever Claude session picks this up in go.dot.
go.dot was read at `1c8aaa0a`; nothing in go.dot was changed, built or run. Every measurement below was made
in the plugin, whose `lib/oscquery/` holds copies of go.dot's mDNS, OSC and HTTP code taken at that same
commit.*

## In one paragraph

Two of the four matter. **First: on macOS and Linux, go.dot can be killed outright when a TCP peer hangs up.**
Writing with `juce::StreamingSocket::write` to a connection the other end has closed raises `SIGPIPE`, and
go.dot does not handle that signal, so the process ends at once, with no crash report and nothing logged. Five
places in go.dot write that way, the MQTT broker among them. The fix is one line in `main()`. **Second:
go.dot's mDNS responder answers every question at once, to the whole group.** In the plugin, that let a
validator loading the plugin in about two hundred processes send 11,600 multicast datagrams in a second, which
took the author's phone hotspot down twice. go.dot runs as one process, so it cannot flood like that, but it
breaks RFC 6762's pacing rules in the same way, and the paced version is ready to copy. The other two are
small: a browser that is restarted keeps what it heard before (latent: go.dot never restarts it), and the HTTP
GET ignores `Content-Length`.

| # | Issue | For go.dot | Fix |
|---|---|---|---|
| 1 | A write to a closed TCP connection ends the process (`SIGPIPE`) | **High**: a show stops dead | One line in `main()` |
| 2 | The mDNS responder answers without pacing | Medium: RFC compliance, busy networks | Copy one header and five call sites |
| 3 | `Discovery::start` keeps an earlier run's records | Latent: go.dot starts discovery once | A few lines |
| 4 | The HTTP GET reads until the server closes | Low: only servers that ignore `Connection: close` | Optional |

## 1. A write to a closed TCP connection ends the process

### What happens

On macOS and Linux, `send()` on a TCP socket whose peer has closed raises `SIGPIPE`. Unless the process has
arranged otherwise, `SIGPIPE` terminates it. `juce::StreamingSocket::write` is a plain `send()` with no flag
(`ThirdParty/JUCE/modules/juce_core/network/juce_Socket.cpp:532`), and JUCE sets no socket option against
it. JUCE installs a `SIGPIPE` handler only in `juce_NamedPipe_posix.cpp`, which go.dot does not use, and
go.dot's own handlers cover `SIGINT` and `SIGTERM` only (`src/wfg/engine/Console.cpp:2658-2659`). Windows has
no such signal and is not affected.

### Where go.dot is exposed

| File | What is on the other end |
|---|---|
| `src/wfg/engine/mqtt/Broker.cpp:428` | The broker writing to a connected client. A sensor, phone or controller that hangs up while messages are queued for it. |
| `src/wfg/engine/serial/TcpLink.cpp:66` | A TCP serial link. The device rebooting. |
| `src/wfg/engine/net/TlsStream.cpp:101` | Every TLS connection built on `TlsStream`. |
| `src/wfg/engine/oscquery/OscQueryClient.cpp:116` | The HTTP GET to an OSCQuery server. The server restarting between the connection and the write. |
| `src/wfg/engine/process/PatchEditor.cpp:546` | Asking a local Pd. Pd quitting between the connection and the write. |

Not exposed, as far as a search shows: the OSCQuery server and every websocket, which run on `juce_simpleweb`
over asio, and asio sets `SO_NOSIGPIPE` or `MSG_NOSIGNAL` itself; UDP; and `video/PipedChild`, which only
reads from the child's pipes.

### Measured in the plugin

The plugin's copy of `OscQueryClient::get` (`lib/oscquery/oscquery/net/HttpGet.cpp`) had the same write. On
the Mac mini, the plugin's test program vanished once in about a dozen full runs, in tests that restart their
mock device: no summary, no error, no macOS crash report. On the Linux NUC, a test that writes to a peer that
has closed, using `juce::StreamingSocket::write`, ends the test program with **exit status 141 (128 + 13,
`SIGPIPE`) and prints nothing**. With the fix, the same test passes three runs out of three. This was not
reproduced in go.dot itself; the mechanism is the same JUCE call.

### Fix for go.dot

go.dot owns its process, so the simplest complete fix is to ignore the signal once, at the top of `main()`
in `src/wfg/app/Main.cpp`:

```cpp
#include <csignal>

int main (int argc, char** argv)
{
   #if ! JUCE_WINDOWS
    std::signal (SIGPIPE, SIG_IGN);     // a write to a peer that has gone fails with EPIPE instead of ending go.dot
   #endif

    return wfg::runConsole (argc, argv, wfg::client::factory());
}
```

That covers all five sites, any added later, and third-party code. A failed write then returns -1, and every
site above already treats that as failure: the broker breaks out of its client loop, `TcpLink` fails,
`TlsStream` returns `MBEDTLS_ERR_SSL_INTERNAL_ERROR`, the GET returns an error, and `askPd` returns false.

One side effect to check: with `SIGPIPE` ignored, a `wfg` command whose output is piped into something that
quits early (`wfg … | head`) no longer dies quietly. Its writes to `std::cout` fail and it carries on. A
command that prints a lot to a pipe should stop when `std::cout` goes bad.

The plugin could not use this fix, because a plugin does not own its host's signal handlers. It added
`lib/oscquery/oscquery/net/WriteNoSignal.h` instead: a write that never raises the signal (`MSG_NOSIGNAL` on
Linux, `SO_NOSIGPIPE` on macOS). Using it in go.dot's `OscQueryClient.cpp` as well is optional; it would keep
the two copies of that file alike. That is plugin pull request #9, commit `c3e7ba9` on its branch
`fix/sigpipe`, with its tests in `lib/oscquery/tests/WriteNoSignalTests.cpp`.

### To test

Copy the first case of `lib/oscquery/tests/WriteNoSignalTests.cpp`: connect to a listener of the test's own,
close the accepted end, write until the write fails. Without the fix the test program exits with 141 on
Linux and nothing more is printed; with it, the write returns -1 and the case passes.

## 2. The mDNS responder answers without pacing

### What happens

`Discovery::answer` (`src/wfg/engine/oscquery/Discovery.cpp:395-488`) sends every multicast answer to the
group at once, on every interface. It breaks three of RFC 6762's rules:

- **§6:** a multicast answer should wait a random 20 to 120 ms.
- **§7.4:** an answer another responder has just given should not be repeated.
- **§6:** no record should be multicast more than once a second per interface.

There is also no overall limit on what one browser sends to the group (`sendToGroup`, `ask`).

### Measured in the plugin

clap-validator loads a plugin in about two hundred processes within a second or two. With the plugin's server
on, each one announced itself and answered every other's question. One machine sent **11,600 multicast
datagrams in about a second**, and its pings to the author's phone hotspot lost half their packets. That was
on two evenings, 2026-11-09 and 2026-11-10. The plugin's write-up is `docs/bench/2026-11-10-mdns-flood.md`.

### For go.dot

go.dot is one process per machine, so it cannot flood the way the plugin did. But it answers every question
it hears, from every browser on the network, at once and on every interface. A rig with many browsers
multiplies that: several go.dots, a DAW full of plugin instances, a validator. And it is a conformance point
for the RFC.

### Fix

The plugin's fix is plugin pull request #4, commit `bd4e5d2` (merged as `e100521`):

- **`lib/oscquery/oscquery/mdns/AnswerPacer.h`.** Header-only and pure: no socket and no clock of its own.
  It holds the delay, the duplicate suppression, one second's rest per record, and a budget of ten datagrams a
  second with a burst of twenty. Its tests are `lib/oscquery/tests/AnswerPacerTests.cpp`, including two
  hundred responders asked at once, who answer once between them.
- **Five changes in `Discovery.cpp`.**
  - `pacerKey` names one record.
  - `sendToGroup` and `ask` spend the budget.
  - `announce` marks its records as said.
  - `answer` queues its multicast answer through the pacer instead of sending it.
  - The run loop sends what is due and never sleeps past the next due answer, and `heard` marks records seen in
    other responders' answers on port 5353.

The plugin also added a setting to turn DNS-SD off; go.dot already has `discoveryOff`.

`AnswerPacer.h` was written in the plugin for this fix, and its header names Pierre-Olivier Boulant and
Julien Bayle, by the plugin's rule. Under which header it enters go.dot is the author's call.

## 3. A restarted browser keeps what it heard before

`Discovery::start` clears `interfaces` (`Discovery.cpp:185`) but not `instances`, `hosts` or the published
list. A `Discovery` that is stopped and started again, with other options for instance, goes on listing
records from its earlier run for their whole time to live (75 minutes for a PTR), and can no longer hear them
withdrawn. In the plugin, the finder went on probing a dozen devices long gone, and a new one waited behind
them for more than twenty seconds.

In go.dot today this does not happen. Discovery is started once, by `wfg serve` (`Console.cpp:6767`), and
stopped at shutdown (`Console.cpp:10596`). The fix is a few lines in `start()`: clear `instances`, `hosts`
and `list`, and bump `stamp`. It keeps the two copies alike and removes the trap for whoever first restarts
discovery. In the plugin it is part of commit `2c8c36f`.

## 4. The HTTP GET reads until the server closes

`OscQueryClient::get` sends `Connection: close` and reads until `read` returns 0. It never uses
`Content-Length`. A server that answers with `Content-Length` and then keeps the connection open makes every
request time out. Under HTTP/1.1 (RFC 9112 §9.6), such a server is at fault once the client has said
`close`, but such servers exist. This was found by Julien Bayle's retest of the plugin (his item 2.9). It is
not fixed in the plugin either. Reading `Content-Length`, or the end of a chunked body, would make the client
robust to them. Low priority.

## Where the plugin's copies now differ from go.dot

| Plugin file | Copied from go.dot | Changed since |
|---|---|---|
| `lib/oscquery/oscquery/mdns/Discovery.{h,cpp}` | `src/wfg/engine/oscquery/Discovery.{h,cpp}` | Answers paced (§2); a fresh start (§3). Both noted in the files' origin headers. |
| `lib/oscquery/oscquery/mdns/AnswerPacer.h` | (new) | §2 |
| `lib/oscquery/oscquery/net/HttpGet.cpp` | `OscQueryClient::get` | The no-signal write (§1), once plugin PR #9 is merged |
| `lib/oscquery/oscquery/mdns/Mdns.*`, `MulticastJoin.*`, `osc/*`, `UdpEndpoint`, `SocketBuffers` | the same names | Unchanged apart from namespaces |

If go.dot takes these fixes, the plugin's origin notes can say the two are aligned again.

## Not checked

- **go.dot's own OSCQuery client behaviour**, for instance whether a mount follows a server that comes back
  with another `OSC_PORT`. The plugin's equivalents (Julien Bayle's items 2.4 and 2.7) were in the plugin's
  own code, not in anything copied from go.dot, so they say nothing about go.dot.
- **Any of the above run in go.dot.** The fixes are described from reading go.dot's source and from what was
  measured in the plugin.
