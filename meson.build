project('hootedit', 'cpp',
  version: '0.2.0',
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
hootedit_exe = executable('hootedit', files('src/main.cpp', 'src/buffer.cpp', 'src/doc.cpp', 'src/syntax.cpp', 'src/icons.cpp', 'src/tree.cpp', 'src/git.cpp', 'src/askpass.cpp', 'src/remote.cpp', 'src/diff.cpp', 'src/merge.cpp', 'src/regex.cpp', 'src/fuzzy.cpp', 'src/search.cpp', 'src/vim.cpp', 'src/outline.cpp', 'src/benchcore.cpp', 'src/lint.cpp', 'src/lint_k8s.cpp', 'src/lint_formats.cpp') + engine,
           include_directories: inc, link_with: lestrix_lib, dependencies: deps + [cxx.find_library('util', required: false)], install: true)

install_data('data/hootedit.png', install_dir: get_option('datadir') / 'icons' / 'hicolor' / '256x256' / 'apps')

# ---- tests: GoogleTest, one binary for the unit tests under ASan and UBSan, sliced into areas, plus a speed gate and the scripted UI test ----
# groups (the same names in fleetwm and Lestrix): unit, differential and fuzz (inside the unit binary), perf, ui, static; see docs/testing.md
gtest_dep = dependency('gtest', main: true, required: true, fallback: ['gtest', 'gtest_main_dep'])
san = ['b_sanitize=address,undefined', 'b_ndebug=false', 'buildtype=debug']

unit_sources = files(
  'tests/test_buffer.cpp', 'tests/test_doc.cpp', 'tests/test_syntax.cpp', 'tests/test_fuzzy.cpp', 'tests/test_diff.cpp', 'tests/test_merge.cpp', 'tests/test_regex.cpp',
  'tests/test_vim.cpp', 'tests/test_outline.cpp', 'tests/test_tree.cpp', 'tests/test_lint.cpp',
  'src/buffer.cpp', 'src/doc.cpp', 'src/syntax.cpp', 'src/regex.cpp', 'src/fuzzy.cpp', 'src/diff.cpp', 'src/merge.cpp', 'src/vim.cpp', 'src/outline.cpp', 'src/tree.cpp',
  'src/lint.cpp', 'src/lint_k8s.cpp', 'src/lint_formats.cpp')
unit_tests = executable('hootedit-unit-tests', unit_sources, dependencies: [gtest_dep], cpp_args: ['-fexceptions'], override_options: san)
test('hootedit-unit-tests', unit_tests, protocol: 'gtest', timeout: 300)

# one meson test per area, so `meson test lint` runs just that slice (the same slices scripts/test.sh offers)
test_areas = {
  'buffer': 'Buffer.*', 'doc': 'Doc.*', 'syntax': 'Syntax.*', 'fuzzy': 'Fuzzy.*', 'diff': 'Diff.*', 'merge': 'Merge.*', 'regex': 'Regex.*',
  'vim': 'Vim.*', 'outline': 'Outline.*', 'tree': 'Tree.*', 'lint': 'Lint.*',
}
foreach area, filter : test_areas
  test(area, unit_tests, args: ['--gtest_filter=' + filter], suite: 'areas', protocol: 'gtest', timeout: 300)
endforeach

perf_gate = executable('hootedit-perf-gate', files('tests/perf_gate.cpp', 'src/buffer.cpp', 'src/doc.cpp', 'src/syntax.cpp', 'src/regex.cpp', 'src/fuzzy.cpp', 'src/lint.cpp', 'src/lint_k8s.cpp', 'src/lint_formats.cpp'),
  dependencies: [gtest_dep], cpp_args: ['-fexceptions'])
test('perf-gate', perf_gate, protocol: 'gtest', suite: 'perf', timeout: 120, is_parallel: false)

bench_search = executable('bench_search', files('tests/bench_search.cpp', 'src/search.cpp', 'src/regex.cpp', 'src/tree.cpp'), dependencies: [dependency('threads')])

bash = find_program('bash', required: false)
if bash.found()
  test('ui scenarios', bash, args: [files('tests/ui_test.sh'), hootedit_exe], suite: 'ui', timeout: 180)
endif


