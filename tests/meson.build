doctest_dep = dependency('doctest')

test_sources = files(
  'test_main.cpp',
  'test_geometry.cpp',
  'test_gesture.cpp',
  'test_canvas.cpp',
  'test_text.cpp',
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
  'test_annotation_sync.cpp',
  'test_selection_api.cpp',
  'test_epub_select.cpp',
  'test_pdf_select.cpp',
  'test_annotation_bar.cpp',
  'test_annotation_edit.cpp',
  'test_notes_conflicts.cpp',
  'test_push_triggers.cpp',
  'test_review_fixes5.cpp',
  'test_stall_watchdog.cpp',
  'test_device_requests.cpp',
  'test_metrics.cpp',
  'test_layout_scribe.cpp',
  'test_keyboard_bounds.cpp',
  'test_review_fixes6.cpp',
  'test_item_icons.cpp',
  'test_landscape.cpp',
  'test_webdav.cpp',
  'test_config.cpp',
  'test_power.cpp',
  'test_keysym.cpp',
  'test_library_screens.cpp',
  'test_widgets.cpp',
  'test_app_shell.cpp',
  'test_sync_service.cpp',
  'test_home.cpp',
  'test_library_screens_ui.cpp',
  'test_search_screen.cpp',
  'test_open_flow.cpp',
  'test_setup_settings.cpp',
  'test_lifecycle.cpp',
  'test_review_fixes.cpp',
  'test_engine_isolation.cpp',
  'test_document_types.cpp',
  'test_epub_engine.cpp',
  'test_pdf_engine.cpp',
  'test_render_worker.cpp',
  'test_reader.cpp',
  'test_reader_menus.cpp',
  'test_display_settings.cpp',
  'test_reader_highlights.cpp',
  'test_book_search.cpp',
  'test_reader_real.cpp',
  'test_review_fixes4.cpp',
  '../src/platform/kindle/x_touch.cpp',
)

# EPUB internals (CFI) are tested directly on the desktop, where crengine shares Homebrew's
# freetype/harfbuzz with pango.
if backend == 'desktop' and is_variable('engine_crengine_dep')
  test_sources += files('test_cfi.cpp', 'test_cfi_gen.cpp', '../src/docs/epub/cfi.cpp')
  test_extra_deps = [engine_crengine_dep, declare_dependency(include_directories : include_directories('../third_party/pugixml', is_system : true))]
else
  test_extra_deps = []
endif

zk_tests = executable('zk-tests', test_sources,
  dependencies : [core_dep, ui_dep, screens_dep, doctest_dep, sqlite_dep, miniz_dep, md5_dep, net_dep, store_dep, zotero_dep, sync_dep, services_dep, app_dep, docs_dep] + test_extra_deps)

test('zk-tests', zk_tests, workdir : meson.project_source_root(),
  env : ['ZK_LIB_DIR=' + meson.project_build_root()])
