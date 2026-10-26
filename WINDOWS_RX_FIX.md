# Windows empty-read notification fix

Branch: `fix/windows-rx-empty-wakeup`

Upstream base: `sigrokproject/libserialport`
`21b3dfe5f68c205be4086469335fd2fc2ce11ed2`.

An empty read after a completed RX notification must check whether the wait
needs restarting. The old empty-read fast path left the manual-reset event
signaled, allowing a caller waiting on the event to spin indefinitely.
The fix retains that fast path only while the event is unsignaled; the existing
restart code continues to own the OVERLAPPED operation and its lifetime.

## Build in MSYS2 UCRT64

Install MSYS2 `make`, `autoconf`, `automake`, and `libtool`, and use the UCRT64
GCC toolchain (validated with GCC 16.1.0). Keep the checkout on a Windows drive.

```sh
export PATH=/ucrt64/bin:/usr/bin:$PATH
./autogen.sh
mkdir -p build-ucrt64
cd build-ucrt64
../configure --host=x86_64-w64-mingw32 --enable-shared --disable-static \
    CC=/ucrt64/bin/gcc CFLAGS='-O2 -g'
make -j4 check
```

Artifacts are `.libs/libserialport-0.dll`, `.libs/libserialport.dll.a`, and
the source-tree `libserialport.h`. The DLL uses UCRT rather than MSVCRT.
All 79 public exports match the previously vendored Windows DLL.

The upstream base has stale version macros in `libserialport.h` (0.1.1), while
`configure.ac` reports 0.1.2. This causes pre-existing macro redefinition
warnings; this focused RX fix does not change those public headers.

## Validation

`make check` runs the existing timing test and `test_win32_rx_wait`.
The new test compiles the actual read/rearm implementation with deterministic
Win32 I/O completion mocks and a real manual-reset Windows event. It covers
empty completed notifications, pending waits, later data reception, synchronous
completion followed by an empty read, partial reads, and error propagation.
It fails on the upstream base and passes with this fix.

These tests do not replace a real serial-driver receive/stop/resume smoke test.
