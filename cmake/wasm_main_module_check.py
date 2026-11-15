#!/usr/bin/env python3
# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0
"""Usage: wasm_main_module_check.py <emscripten root> <main.wasm> <main.js> <side.wasm>...

Fails, naming them, when the main module imports a symbol a side module it is linked against exports: that symbol
then resolves only once the module is dlopen()ed, so a call before that aborts. A side module exports every symbol
it does not hide, and the link that names it resolves through it whatever the main module's own libraries would
have provided (libc++ and SDK code, for one).

Also fails when a GOT.func import of an Emscripten main module has no signature in its JS. Those are the
JS functions whose address the wasm takes (JS exception handling's invoke_* takes the address of any call that can
throw); the startup linker wraps each with addFunction(f, f.sig), and a missing one aborts the app before its first
frame. emcc infers the signature of an EM_JS or JS library function only when it is also called directly, so a
--js-library function needs its own __sig.
"""
import re
import sys


def has_signature(javascript, function, depth=0):
  """Whether main.js sets `function.sig`, directly or on the function an alias `var _a=_b;` names."""
  if re.search(r'(?<![\w$])_?' + re.escape(function) + r'\.sig\s*=', javascript):
    return True
  alias = re.search(r'var _' + re.escape(function) + r'\s*=\s*_([\w$]+)\s*;', javascript)
  return bool(alias) and depth < 4 and has_signature(javascript, alias.group(1), depth + 1)


def check(emscripten_root, main_wasm, main_js, *side_wasms):
  sys.path.insert(0, emscripten_root)
  from tools import webassembly  # noqa: PLC0415 (only importable once the path is set)
  with webassembly.Module(main_wasm) as module:
    exported = {e.name for e in module.get_exports()}
    imports = module.get_imports()
  got = {i.field for i in imports if i.module == 'GOT.func'} - exported
  for side_wasm in side_wasms:
    with webassembly.Module(side_wasm) as side:
      side_exports = {e.name for e in side.get_exports()}
    early = sorted({i.field for i in imports} & side_exports)
    if early:
      sys.exit(f'{main_wasm} imports {len(early)} symbol(s) {side_wasm} exports, unresolved until it is dlopen()ed: '
               f'{", ".join(early[:20])}')
  with open(main_js, encoding='utf-8') as js:
    javascript = js.read()
  unsigned = sorted(f for f in got if not has_signature(javascript, f))
  if unsigned:
    sys.exit(f'{main_js} has no signature for {", ".join(unsigned)}: give each JS library function a __sig')
  print(f'{main_js} has a signature for each of the {len(got)} JS functions {main_wasm} takes the address of')


if __name__ == '__main__':
  if len(sys.argv) < 4:
    sys.exit(__doc__)
  check(*sys.argv[1:])
