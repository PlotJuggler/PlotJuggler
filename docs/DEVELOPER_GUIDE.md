# Developer guide

Where to start when changing PlotJuggler 4, and what a pull request has to pass.

| To | Read |
|---|---|
| Build, run and test on Linux, Windows or macOS | [BUILDING.md](./BUILDING.md) |
| Find the module that owns a piece of code, and the conventions a change follows | the root [CLAUDE.md](../CLAUDE.md), then that module's `CLAUDE.md` and `docs/` |
| Run the suite under ASan, TSan or MSan | [SANITIZERS.md](./SANITIZERS.md) |
| Build and serve the browser app | [WASM_DEPLOYMENT.md](./WASM_DEPLOYMENT.md) |
| Produce an installer or package | [packaging/README.md](../packaging/README.md) |
| Find any other design or guide | [docs/README.md](./README.md) |

Code is formatted and checked by `pre-commit` (clang-format, forbidden patterns, file hygiene;
[hooks](../scripts/pre-commit/README.md)): run `pre-commit install` once, and CI runs the same hooks.

## Continuous integration

A pull request merges only when every check that ran on it is green. Draft PRs run nothing until they are marked
ready.

**On every pull request:**

| Workflow | What it checks | Skipped when the PR touches only |
|---|---|---|
| Linux CI | Build, every test, QRhi display tests (OpenGL 3.3 floor, Vulkan), floating docks across pixel ratios | docs, Markdown, packaging |
| Windows CI | Build, every test, QRhi display tests on D3D11 (WARP) | docs, Markdown, packaging |
| WASM CI | Browser build and its smoke specs in Chromium; the full Chromium suite when the PR touches the browser build | docs, Markdown, `.claude/` |
| pre-commit | Formatting, forbidden patterns, file hygiene | never skipped |

**Only on pull requests that touch their area:**

- **macOS CI** (Metal QRhi tests and the unit tests): toolchain pins, the build system, vendored code, or the widget
  and render code the QRhi tests cover. Start it by hand for any other Metal-sensitive change:
  `gh workflow run "macOS CI" --ref <branch>`.
- **Pixi CI** (conda-forge build and tests, Linux and Windows): `pixi.toml` and `recipes/`.
- **Nix**, **conda** and **macOS dmg** packaging: their files under `packaging/`.

**Nightly on `main`** (UTC; a workflow is skipped when `main` has not moved since its last scheduled run):

| Time | Workflow | What it adds to the PR checks |
|---|---|---|
| 01:00 | macOS CI | Metal QRhi tests and unit tests, plus the QRhi tests on OpenGL (Apple's software renderer) |
| 02:00 | Pixi CI | The conda-forge build on Linux and Windows |
| 03:00 | Sanitizers | The whole suite under ASan, TSan and MSan |
| 04:00 | WASM CI | The full Chromium suite and the Firefox suite, on GitHub-hosted runners |
| 05:17 | WASM plugins pin | Nothing: it opens the PR that bumps the browser build's official plugins pin, and runs even when `main` has not moved ([WASM_DEPLOYMENT](./WASM_DEPLOYMENT.md#built-in-plugins)) |

A failed nightly opens (or comments on) a `Nightly failure: <workflow>` issue; the next green run closes it.

The badges at the top of the [README](../README.md) show the latest run on `main`: the push after each merge for Linux and Windows, the latest
nightly or manual run for macOS, WASM and Pixi.
