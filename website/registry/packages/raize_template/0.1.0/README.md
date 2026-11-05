# RaizeTemplate

String templates for the raize web framework: HTML/text rendering with escaping and sections.

Package `raize_template`, part of [RaizeWeb](https://github.com/Lex-Dellimere/RaizeWeb), the raize
umbrella. Most apps import the whole framework with `import "raize/raize";`; use this package
directly when you only need this layer:

```mettle
import "raize_template/template";
```

Depends on: raize_core .

## Build

This repository is a git submodule of RaizeWeb at `libs/template`. Until the Mettle registry carries the raize
packages, they are path dependencies on each other inside that checkout, so build from there:

```bash
git clone --recursive https://github.com/Lex-Dellimere/RaizeWeb.git raize_web
cd raize_web/libs/template && mbuild check
```

The tests for every raize package run from RaizeWeb (`mbuild integration`); see its README for
the few repositories it expects next to `raize_web`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
