# RaizeSession

Sessions for the raize web framework: cookie sessions, rotation, CSRF protection and flash messages.

Package `raize_session`, part of [RaizeWeb](https://github.com/Lex-Dellimere/RaizeWeb), the raize
umbrella. Most apps import the whole framework with `import "raize/raize";`; use this package
directly when you only need this layer:

```mettle
import "raize_session/raize_session";
```

Depends on: raize_app, raize_core and crypto ([Cryptex](https://github.com/Lex-Dellimere/Cryptex)).

Two layers live here:

- `ses/`: the framework-agnostic session core (`ses_` prefix): hashed opaque tokens, idle and
  absolute expiry, rotation, data, CSRF, flash, signed values and cookie strings over a
  pluggable `SesStore`. It works without raize: `import "raize_session/ses/session";`.
  See [ses/README.md](ses/README.md).
- the root files: the raize binding (`rz_` prefix): `rz_use_session`, `rz_session_*`,
  `rz_csrf_*`, `rz_flash_*` and `rz_conn_session_get` for Signals.

## Build

This repository is a git submodule of RaizeWeb at `libs/session`. Until the Mettle registry carries the raize
packages, they are path dependencies on each other inside that checkout, so build from there:

```bash
git clone --recursive https://github.com/Lex-Dellimere/RaizeWeb.git raize_web
cd raize_web/libs/session && mbuild check
```

The session core has its own suites (`mbuild test-programs`, debug and release); the raize
binding is tested from RaizeWeb (`mbuild integration`); see its README for
the few repositories it expects next to `raize_web`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
