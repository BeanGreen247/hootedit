project('hootedit', 'cpp',
  version: '0.1.0',
  license: 'MIT',
  default_options: ['cpp_std=gnu++20', 'buildtype=release', 'b_ndebug=true', 'warning_level=2'])

cxx = meson.get_compiler('cpp')
args = ['-fno-rtti', '-fno-exceptions', '-fvisibility=hidden', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-array-bounds', '-Wno-missing-field-initializers']
# Same GCC tuning Lestrix measured as +5.4% on the engine, only when the compiler accepts it.
foreach f : ['-freorder-blocks-algorithm=simple', '-fvect-cost-model=unlimited']
  if cxx.has_argument(f)
    args += f
  endif
endforeach
add_project_arguments(args, language: 'cpp')

deps = [dependency('sdl2'), dependency('freetype2'), dependency('fontconfig'), dependency('threads')]

engine = files('src/engine/atlas.cpp', 'src/engine/font.cpp', 'src/engine/gl.cpp', 'src/engine/render.cpp')
# Vendored from Lestrix 0289c8e: VT emulator, pty, terminal core and view, themes, fleetwm follow, shared helpers.
lestrix = files('src/lestrix/vt.cpp', 'src/lestrix/lz.cpp', 'src/lestrix/pty.cpp', 'src/lestrix/util.cpp',
                'src/lestrix/ui/tcore.cpp', 'src/lestrix/ui/termview.cpp', 'src/lestrix/ui/workpool.cpp', 'src/lestrix/ui/fleetwm.cpp', 'src/lestrix/ui/uitheme.cpp')
inc = include_directories('src/engine')
# Lestrix itself is built with exceptions on (fleetwm.cpp uses try/catch); Hootedit's own code is built without.
lestrix_lib = static_library('lestrix_vendored', lestrix, include_directories: inc, cpp_args: ['-fexceptions'], dependencies: deps)
hootedit_exe = executable('hootedit', files('src/main.cpp', 'src/buffer.cpp', 'src/doc.cpp', 'src/syntax.cpp', 'src/icons.cpp', 'src/tree.cpp', 'src/git.cpp', 'src/askpass.cpp', 'src/remote.cpp', 'src/diff.cpp', 'src/merge.cpp', 'src/regex.cpp', 'src/fuzzy.cpp', 'src/search.cpp', 'src/vim.cpp', 'src/outline.cpp', 'src/benchcore.cpp', 'src/lint.cpp') + engine,
           include_directories: inc, link_with: lestrix_lib, dependencies: deps + [cxx.find_library('util', required: false)], install: true)

install_data('data/hootedit.png', install_dir: get_option('datadir') / 'icons' / 'hicolor' / '256x256' / 'apps')

test_buffer = executable('test_buffer', files('tests/test_buffer.cpp', 'src/buffer.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('buffer differential', test_buffer, args: ['--gen'])

test_syntax = executable('test_syntax', files('tests/test_syntax.cpp', 'src/syntax.cpp'))
test('syntax classes', test_syntax)

test_doc = executable('test_doc', files('tests/test_doc.cpp', 'src/doc.cpp', 'src/buffer.cpp', 'src/syntax.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('doc undo and selection', test_doc)

test_regex = executable('test_regex', files('tests/test_regex.cpp', 'src/regex.cpp'), cpp_args: ['-fexceptions'],
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('regex vs std::regex', test_regex, timeout: 300)

test_diff = executable('test_diff', files('tests/test_diff.cpp', 'src/diff.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('line diff minimal and replayable', test_diff)

test_merge = executable('test_merge', files('tests/test_merge.cpp', 'src/merge.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('merge parse and result', test_merge)

test_fuzzy = executable('test_fuzzy', files('tests/test_fuzzy.cpp', 'src/fuzzy.cpp'))
test('fuzzy ranking', test_fuzzy)

bench_search = executable('bench_search', files('tests/bench_search.cpp', 'src/search.cpp', 'src/regex.cpp', 'src/tree.cpp'), dependencies: [dependency('threads')])

test_vim = executable('test_vim', files('tests/test_vim.cpp', 'src/vim.cpp', 'src/doc.cpp', 'src/buffer.cpp', 'src/syntax.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('vim engine', test_vim)

test_outline = executable('test_outline', files('tests/test_outline.cpp', 'src/outline.cpp', 'src/doc.cpp', 'src/buffer.cpp', 'src/syntax.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('outline scan', test_outline)

test_tree = executable('test_tree', files('tests/test_tree.cpp', 'src/tree.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('explorer tree', test_tree)

bash = find_program('bash', required: false)
if bash.found()
  test('ui scenarios', bash, args: [files('tests/ui_test.sh'), hootedit_exe], suite: 'ui', timeout: 180)
endif

test_lint = executable('test_lint', files('tests/test_lint.cpp', 'src/lint.cpp', 'src/syntax.cpp'),
  override_options: ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug'])
test('lint rules', test_lint)
