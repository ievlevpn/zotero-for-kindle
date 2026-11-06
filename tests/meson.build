doctest_dep = dependency('doctest')

test_sources = files(
  'test_main.cpp',
  'test_geometry.cpp',
  'test_gesture.cpp',
  'test_canvas.cpp',
  'test_text.cpp',
  'test_home_golden.cpp',
  'test_x_touch.cpp',
  'test_expose_policy.cpp',
  'test_vendored.cpp',
  'test_http_fake.cpp',
  'test_write_guard.cpp',
  'test_errors.cpp',
  'test_library.cpp',
  'test_parse.cpp',
  'test_api.cpp',
  'test_sync_engine.cpp',
  'test_webdav.cpp',
  'test_config.cpp',
  '../src/platform/kindle/x_touch.cpp',
)

zk_tests = executable('zk-tests', test_sources,
  dependencies : [core_dep, ui_dep, screens_dep, doctest_dep, sqlite_dep, miniz_dep, md5_dep, net_dep, store_dep, zotero_dep, sync_dep])

test('zk-tests', zk_tests, workdir : meson.project_source_root())
