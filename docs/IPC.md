# IPC interfaces

The services the runtime emulates answer the game through IPC (`hle/kernel/ipc.c`). Each service interface is
declared once, in a `.swipc` file beside the service's C file, and `tools/swipc/swipc.c` generates the code that turns
a request into a call of a typed handler and the handler's outputs into the reply. The services' C files hold only
the handlers. This replaced one `switch (c->cmd)` per interface that read every argument by hand at an offset
(`in32(c, 8)`) and wrote every output in order (`out64(c, x)`), which is where layout mistakes hid (#155).

## The language

It is SwIPC's (github.com/reswitched/SwIPC, ISC), with the conventions below.

```
# comments start with # (// and /* */ work too)
type nn::account::Uid = bytes<0x10, 8>;             # bytes<size[, alignment]>
type nn::applet::AppletResourceUserId = u64;
type nn::account::profile::ProfileBase = struct {   # natural alignment, like the SDK's struct
    nn::account::Uid uid;
    u64 last_edit_timestamp;
    bytes<0x20> nickname;
};

interface nn::account::IAccountServiceForApplication is acc:u0, acc:u1, acc:su {
    [0] GetUserCount() -> i32 count;
    [2] ListAllUsers(array<nn::account::Uid, 0xa> users);
    [5] GetProfile(nn::account::Uid uid) -> object<nn::account::profile::IProfile> profile;
    @stub [6] GetProfileDigest(nn::account::Uid uid) -> nn::account::ProfileDigest digest;
    [51] TrySelectUserWithoutInteraction(b8 is_network_service_account_required) -> nn::account::Uid uid;
}
```

- Names are the SDK's (Switchbrew, libnx, the SDK's symbols). `is` lists the service names the interface is served
  as (sm's names are 8 bytes at most). Every argument and output has a name, but `pid` and `aruid` may go without. A
  struct is written in a `type` definition.
- Types: `u8` to `u64`, `i8` to `i64` (or `s8` to `s64`), `b8`, `f32`, `f64`, `bytes<N[, A]>`, `T[N]`, `struct {...}`,
  and names given with `type`. A type is defined before it is used. Types more than one file uses are in
  `hle/services/types.swipc`.
- `pid` is the client's process id (ClientProcessId): an 8 byte slot in the raw data whose value the kernel gives (0
  when the client sent none). It is one slot: SwIPC's `(u64, pid)` is written `pid` here. `aruid` is
  ClientAppletResourceUserId: the same slot, whose raw value the handler gets.
- Buffers are inputs whatever their direction: `buffer<T, attributes[, size]>` and `array<T, attributes>`. The
  attributes are the numeric BufferAttribute: In 1, Out 2, MapAlias 4, Pointer 8, FixedSize 0x10, AutoSelect 0x20,
  NonSecure 0x40, NonDevice 0x80. So 5 is an A buffer, 6 B, 9 X, 0xa C, 0x19 and 0x1a fixed-size X and C, 0x21 and
  0x22 AutoSelect, 0x46 a NonSecure B.
- `object<I>` is an interface object (I is declared in some .swipc file, with no commands if none is served),
  `handle<copy, kind>` and `handle<move, kind>` a handle.
- `@stub` before a command: the generated code answers it with success and zeros, and there is no handler. A command
  that gives an object or a handle can't be a stub. `@fallback(function)` before an interface names the C function
  that answers the commands it doesn't declare: `quiet_ok` (success, silent), `ipc_unknown` (sf's UnknownCommandId,
  what the console answers), or one of the service's own (bsd answers EOPNOTSUPP). Without it `stub_ok` answers
  success and logs the command. `@version(...)` and `@undocumented` are accepted and ignored.

## The layout

The generated code lays a command out as the SDK's service framework does (Atmosphère's
`sf_impl_command_serialization.hpp` and libnx's `cmif.h` describe the same layout; Atmosphère is GPL-2.0-only and
only read):

- The raw arguments are stable-sorted by alignment, ascending, and each goes at the next multiple of its alignment.
  `(u32 a, u64 b, u8 c)` puts c at 0, a at 4 and b at 8. The outputs are placed the same way, and the reply's raw size
  is rounded up to 4.
- Each buffer's attributes give its descriptor list: A, B, X or C, or A and X (B and C) for AutoSelect. Each list is
  numbered in declaration order, and an AutoSelect buffer takes the next index in both of its lists. The server uses
  its mapped half when that has an address.
- Copy and move handles in come in declaration order, each kind numbered on its own. Out objects become move handles
  (sessions) ahead of the declared move handles on a plain session, and object ids on a domain.
- A failed request answers no data, handles or objects: the generated code closes the handles and drops the objects a
  failing handler made.

## The C side

For `hle/services/acc.swipc` the build writes `hle/services/acc.swipc.h` and `.c` under
`build/<preset>/src/nx/snx_hle-swipc/`, from the list in `src/nx/CMakeLists.txt` (`snx_swipc` in
`cmake/swipc.cmake`). The header has the types, with their padding spelled out and their size and field offsets
checked by the compiler, and for each interface `I` (its name with `::` as `_`):

```c
int nn_account_IAccountServiceForApplication_GetProfile(SvcObj* self, const nn_account_Uid* uid, SvcObj** profile);
int nn_account_IAccountServiceForApplication_dispatch(SvcObj* self, IpcCtx* c);
SvcObj* nn_account_IAccountServiceForApplication_new(size_t data_size);
void nn_account_IAccountServiceForApplication_register(SvcCtor ctor); /* when it has service names */
```

The service's C file includes the header and defines one handler per command that isn't a `@stub`. The compiler
checks each definition against its prototype, and a missing handler is a link error. A handler gets the object, then
the arguments in declaration order, then a pointer for each output, which starts zeroed:

| Argument | Input | Output |
| --- | --- | --- |
| scalar, `pid`, `aruid` | the value | `T*` |
| struct, named bytes or array | `const T*` | `T*` |
| `bytes<N>`, `T[N]` written in place | `const T*` to the first element | `T*` |
| buffer | `IpcBuf` (address, size; empty when the client sent none) | |
| object | `SvcObj*`, a reference the handler takes its own of to keep it | `SvcObj**`, a new object |
| handle | the client's handle value | `u32*`, a handle for the reply |

It returns the result: 0, or an `RC(module, description)`. `I_new` makes an object whose dispatcher is the generated
one. An object that needs a lock around every command, or a check before each one, has a dispatcher of its own that
calls `I_dispatch` (fs.c's files and directories). `I_register` registers a constructor under the interface's
service names (services.c). Names that need constructors of their own register by hand: ldr:ro and ro:1 are one
interface with two kinds of session, and bsd:s is the system's bsd:u.

The generator is strict C90 with no dependencies, so a cross build builds it a second time for the machine doing the
build (`cmake/swipc.cmake`, with `SNX_HOST_C_COMPILER`). Its output is C90 as well:
`tests/swipc_dispatch.c` drives a dispatcher generated from `tests/swipc/test.swipc` through the layout rules, the
same output is compiled with `-std=c90 -pedantic-errors`, and `tests/swipc/bad/` holds definitions it has to refuse.
The generator also refuses a command whose reply would not fit the 0x100 byte message with its headers, handles and
domain object ids (0xd0 bytes of raw data at most). The order of out objects among the move handles, and domain ids,
are `ipc.c`'s, which the test replaces.
A request's raw data ends where its data words do: an argument past it reads as zeros.
