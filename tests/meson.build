doctest_dep = dependency('doctest')

test_sources = files(
  'test_main.cpp',
  'test_geometry.cpp',
)

zk_tests = executable('zk-tests', test_sources,
  dependencies : [core_dep, doctest_dep])

test('zk-tests', zk_tests, workdir : meson.project_source_root())
