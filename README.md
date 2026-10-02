# corTools

Command line tools built on the Cor-Libs - generic: the functests use them, and so can anything else.

| tool | what it is |
|---|---|
| `corRequest` | a cor:// client: one request, like `curl -i` (`--curl` prints what corTest's `corCurl` prints), parallel requests (`--paths`), or load, like `wrk` (`-c`, `--duration`) |
| `corTestClient` | the other end of a test: a notification receiver (HTTP, HTTPS, MQTT), a mock context source (`--delay`, `--status`, programmed replies), and a host for the broker's bridge plugins - the peer a bridge talks to |

## Building

A sibling of the Cor-Libs, like every repo of the stack: `-I..` and `../<lib>/lib<lib>.a`. `corLibs`
builds it after every library and installs the tools into `corLibs/bin`, beside `corTest`:

    make -C ../corLibs di

or on its own, once the libraries are built:

    make di                          # bin/corRequest, bin/corTestClient
    make COR_HTTP_SERVER=builtin di  # against a corRest built with the builtin HTTP server

Needs libmicrohttpd (or the builtin server's corHttp), OpenSSL and libmosquitto.

## History

Both came from coraine (`test/funcTests/corRequest`, `test/funcTests/ftClient`), with their history;
`corTestClient` was called `ftClient` there.
