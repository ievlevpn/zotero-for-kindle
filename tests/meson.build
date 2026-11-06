doctest_dep = dependency('doctest')

test_sources = files(
  'test_main.cpp',
  'test_geometry.cpp',
  'test_gesture.cpp',
  'test_canvas.cpp',
  'test_text.cpp',
  'test_home_golden.cpp',
  'test_x_touch.cpp',
  '../src/platform/kindle/x_touch.cpp',
)

zk_tests = executable('zk-tests', test_sources,
  dependencies : [core_dep, ui_dep, screens_dep, doctest_dep])

test('zk-tests', zk_tests, workdir : meson.project_source_root())
