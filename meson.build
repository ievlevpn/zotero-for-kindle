project('zotero-kindle', 'c', 'cpp',
  version : '0.16.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['cpp_std=c++20', 'warning_level=2', 'buildtype=debugoptimized'])

backend = get_option('platform')

add_project_arguments('-DZK_BACKEND="' + backend + '"', '-DZK_VERSION="' + meson.project_version() + '"',
  language : 'cpp')
if backend == 'kindle'
  add_project_arguments('-DZK_KINDLE=1', language : 'cpp')
  # GCC's notes about the GCC 7.1 ARM parameter-passing change; harmless with static libstdc++.
  add_project_arguments('-Wno-psabi', language : 'cpp')
  # Static C++ runtime. --exclude-libs only keeps static-archive symbols out of the export
  # table; it does NOT keep crengine on its own freetype/harfbuzz when the app also links the
  # firmware's (via pango). Plan 4 must isolate crengine (own .so with -Bsymbolic, or ld -r +
  # localized symbols) and test pango + crengine in one process. See research/spikes/README.md.
  add_project_link_arguments('-static-libstdc++', '-static-libgcc', '-Wl,--exclude-libs,ALL', language : 'cpp')
else
  add_project_arguments('-DZK_DESKTOP=1', language : 'cpp')
endif

inc = include_directories('src')

# The isolation checks' file-open guard (src/core/open_guard.h): in builds with the tests only,
# never in the Kindle's (built with -Dtests=false).
if get_option('tests')
  add_project_arguments('-DZK_OPEN_GUARD=1', language : 'cpp')
endif

core_lib = static_library('zk-core',
  'src/core/log.cpp',
  'src/core/wake.cpp',
  'src/core/open_guard.cpp',
  'src/platform/pen_decoder.cpp',
  include_directories : inc)

core_dep = declare_dependency(link_with : core_lib, include_directories : inc)

# Vendored C libraries (third_party/): SQLite with FTS5, miniz (unzip), Openwall MD5.
sqlite_lib = static_library('zk-sqlite', 'third_party/sqlite/sqlite3.c',
  c_args : ['-DSQLITE_ENABLE_FTS5', '-DSQLITE_DQS=0', '-DSQLITE_THREADSAFE=1', '-w'])
sqlite_dep = declare_dependency(link_with : sqlite_lib,
  include_directories : include_directories('third_party/sqlite', is_system : true),
  dependencies : [dependency('threads'), meson.get_compiler('c').find_library('dl', required : false)])

miniz_lib = static_library('zk-miniz', 'third_party/miniz/miniz.c', c_args : ['-w'])
miniz_dep = declare_dependency(link_with : miniz_lib,
  include_directories : include_directories('third_party/miniz', is_system : true))

md5_lib = static_library('zk-md5', 'third_party/md5/md5.c', c_args : ['-w'])
md5_dep = declare_dependency(link_with : md5_lib,
  include_directories : include_directories('third_party/md5', is_system : true))

# QuickJS-ng (plugin runtime): engine sources only, no qjs/qjsc/std/os modules. C11, gnu mode.
quickjs_lib = static_library('zk-quickjs',
  'third_party/quickjs-ng/quickjs.c', 'third_party/quickjs-ng/libregexp.c',
  'third_party/quickjs-ng/libunicode.c', 'third_party/quickjs-ng/dtoa.c',
  c_args : ['-w', '-DQUICKJS_NG_BUILD', '-funsigned-char', '-D_GNU_SOURCE'],
  override_options : ['c_std=gnu11'])
quickjs_dep = declare_dependency(link_with : quickjs_lib,
  include_directories : include_directories('third_party/quickjs-ng', is_system : true),
  dependencies : [dependency('threads'), meson.get_compiler('c').find_library('m', required : false)])

# libcurl: Homebrew/system on the desktop; on the Kindle our static curl 8.11 + mbedTLS 3.6
# (tools/build-net-deps.sh), because the firmware's libcurl/OpenSSL are too old.
if backend == 'kindle'
  kprefix = meson.project_source_root() / 'build-deps/kindle/prefix'
  curl_libs = []
  foreach l : ['curl', 'mbedtls', 'mbedx509', 'mbedcrypto', 'z']
    curl_libs += meson.get_compiler('cpp').find_library(l, dirs : kprefix / 'lib', static : true)
  endforeach
  curl_dep = declare_dependency(dependencies : curl_libs + [dependency('threads')],
    include_directories : include_directories('build-deps/kindle/prefix/include', is_system : true))
else
  curl_dep = dependency('libcurl')
endif

net_lib = static_library('zk-net',
  'src/net/curl_http.cpp',
  'src/net/write_guard.cpp',
  'src/net/errors.cpp',
  include_directories : [inc, include_directories('third_party', is_system : true)],
  dependencies : [core_dep, curl_dep])
net_dep = declare_dependency(link_with : net_lib, dependencies : [core_dep, curl_dep],
  include_directories : include_directories('third_party', is_system : true))

store_lib = static_library('zk-store',
  'src/store/db.cpp',
  'src/store/library.cpp',
  include_directories : [inc, include_directories('third_party', is_system : true)],
  dependencies : [net_dep, sqlite_dep, dependency('glib-2.0')])  # glib: Unicode folding for sort keys
store_dep = declare_dependency(link_with : store_lib, dependencies : [net_dep, sqlite_dep, dependency('glib-2.0')])

zotero_lib = static_library('zk-zotero',
  'src/zotero/parse.cpp',
  'src/zotero/api.cpp',
  'src/zotero/rate_limit.cpp',
  include_directories : [inc, include_directories('third_party', is_system : true)],
  dependencies : [store_dep])
zotero_dep = declare_dependency(link_with : zotero_lib, dependencies : [store_dep])

# What plugins hear (events, the Item/Annotation shapes): used by the sync engine, the screens and
# the plugin runtime alike, so it stands apart from the runtime.
plugin_events_lib = static_library('zk-plugin-events',
  'src/plugins/events.cpp',
  'src/plugins/identifiers.cpp',
  include_directories : [inc, include_directories('third_party', is_system : true)],
  dependencies : [store_dep])
plugin_events_dep = declare_dependency(link_with : plugin_events_lib, dependencies : [store_dep],
  include_directories : include_directories('third_party', is_system : true))

# Device services shared by the app and its tests: lipc, keep-awake, storage watch, paths.
services_lib = static_library('zk-services',
  'src/platform/power.cpp',
  'src/platform/keysym.cpp',
  'src/platform/keyboard.cpp',
  'src/platform/device_profile.cpp',
  'src/app/paths.cpp',
  backend == 'kindle' ? 'src/platform/kindle/kindle_lipc.cpp' : 'src/platform/desktop/null_lipc.cpp',
  include_directories : inc,
  dependencies : [core_dep])
services_dep = declare_dependency(link_with : services_lib, dependencies : [core_dep])

# Monocypher: signatures (updates) and PIN hashing (accounts).
monocypher_lib = static_library('zk-monocypher', 'third_party/monocypher/monocypher.c',
  'third_party/monocypher/monocypher-ed25519.c', c_args : ['-w'])
sync_lib = static_library('zk-sync',
  'src/sync/sync_engine.cpp', 'src/sync/libraries.cpp',
  'src/sync/files_sweep.cpp',
  'src/sync/webdav.cpp', 'src/sync/zotero_storage.cpp', 'src/sync/snapshot_zip.cpp',
  'src/sync/config.cpp',
  'src/app/accounts.cpp', 'src/app/pin.cpp',
  'src/sync/sync_service.cpp',
  'third_party/pugixml/pugixml.cpp',
  include_directories : [inc, include_directories('third_party', 'third_party/pugixml', 'third_party/monocypher', is_system : true)],
  dependencies : [zotero_dep, miniz_dep, md5_dep, services_dep, plugin_events_dep],
  link_with : monocypher_lib)
sync_dep = declare_dependency(link_with : [sync_lib, monocypher_lib], dependencies : [zotero_dep, miniz_dep, md5_dep, services_dep, plugin_events_dep])

# Updates (Plan 7): versions, signatures (Monocypher), checking, downloading, installing.
update_lib = static_library('zk-update',
  'src/update/version.cpp', 'src/update/signature.cpp', 'src/update/manifest.cpp', 'src/update/release_check.cpp',
  'src/update/download.cpp', 'src/update/install.cpp', 'src/update/updater.cpp',
  include_directories : [inc, include_directories('third_party/monocypher', 'third_party', is_system : true)],
  dependencies : [net_dep, sqlite_dep, miniz_dep],
  link_with : monocypher_lib)
update_dep = declare_dependency(link_with : [update_lib, monocypher_lib], include_directories : inc,
  dependencies : [net_dep, sqlite_dep, miniz_dep])


cairo_dep = dependency('cairo')
pango_deps = [dependency('pangocairo'), dependency('pangoft2'), dependency('fontconfig')]

# MicroTeX (MIT): on-device LaTeX math typesetting with built-in Computer Modern/AMS/Euler metrics.
microtex_src = [
  'third_party/microtex/src/latex.cpp',
  'third_party/microtex/src/render.cpp',
  'third_party/microtex/src/atom/atom_basic.cpp',
  'third_party/microtex/src/atom/atom_char.cpp',
  'third_party/microtex/src/atom/atom_impl.cpp',
  'third_party/microtex/src/atom/atom_matrix.cpp',
  'third_party/microtex/src/atom/atom_row.cpp',
  'third_party/microtex/src/atom/atom_space.cpp',
  'third_party/microtex/src/atom/colors_def.cpp',
  'third_party/microtex/src/atom/unit_conversion.cpp',
  'third_party/microtex/src/box/box.cpp',
  'third_party/microtex/src/box/box_factory.cpp',
  'third_party/microtex/src/box/box_group.cpp',
  'third_party/microtex/src/box/box_single.cpp',
  'third_party/microtex/src/core/core.cpp',
  'third_party/microtex/src/core/formula.cpp',
  'third_party/microtex/src/core/formula_def.cpp',
  'third_party/microtex/src/core/glue.cpp',
  'third_party/microtex/src/core/localized_num.cpp',
  'third_party/microtex/src/core/macro.cpp',
  'third_party/microtex/src/core/macro_def.cpp',
  'third_party/microtex/src/core/macro_impl.cpp',
  'third_party/microtex/src/core/parser.cpp',
  'third_party/microtex/src/fonts/alphabet.cpp',
  'third_party/microtex/src/fonts/font_basic.cpp',
  'third_party/microtex/src/fonts/font_info.cpp',
  'third_party/microtex/src/fonts/fonts.cpp',
  'third_party/microtex/src/res/builtin/formula_mappings.res.cpp',
  'third_party/microtex/src/res/builtin/symbol_mapping.res.cpp',
  'third_party/microtex/src/res/builtin/tex_param.res.cpp',
  'third_party/microtex/src/res/builtin/tex_symbols.res.cpp',
  'third_party/microtex/src/res/font/bi10.def.cpp',
  'third_party/microtex/src/res/font/bx10.def.cpp',
  'third_party/microtex/src/res/font/cmbsy10.def.cpp',
  'third_party/microtex/src/res/font/cmbx10.def.cpp',
  'third_party/microtex/src/res/font/cmbxti10.def.cpp',
  'third_party/microtex/src/res/font/cmex10.def.cpp',
  'third_party/microtex/src/res/font/cmmi10.def.cpp',
  'third_party/microtex/src/res/font/cmmi10_unchanged.def.cpp',
  'third_party/microtex/src/res/font/cmmib10.def.cpp',
  'third_party/microtex/src/res/font/cmmib10_unchanged.def.cpp',
  'third_party/microtex/src/res/font/cmr10.def.cpp',
  'third_party/microtex/src/res/font/cmss10.def.cpp',
  'third_party/microtex/src/res/font/cmssbx10.def.cpp',
  'third_party/microtex/src/res/font/cmssi10.def.cpp',
  'third_party/microtex/src/res/font/cmsy10.def.cpp',
  'third_party/microtex/src/res/font/cmti10.def.cpp',
  'third_party/microtex/src/res/font/cmti10_unchanged.def.cpp',
  'third_party/microtex/src/res/font/cmtt10.def.cpp',
  'third_party/microtex/src/res/font/dsrom10.def.cpp',
  'third_party/microtex/src/res/font/eufb10.def.cpp',
  'third_party/microtex/src/res/font/eufm10.def.cpp',
  'third_party/microtex/src/res/font/i10.def.cpp',
  'third_party/microtex/src/res/font/moustache.def.cpp',
  'third_party/microtex/src/res/font/msam10.def.cpp',
  'third_party/microtex/src/res/font/msbm10.def.cpp',
  'third_party/microtex/src/res/font/r10.def.cpp',
  'third_party/microtex/src/res/font/r10_unchanged.def.cpp',
  'third_party/microtex/src/res/font/rsfs10.def.cpp',
  'third_party/microtex/src/res/font/sb10.def.cpp',
  'third_party/microtex/src/res/font/sbi10.def.cpp',
  'third_party/microtex/src/res/font/si10.def.cpp',
  'third_party/microtex/src/res/font/special.def.cpp',
  'third_party/microtex/src/res/font/ss10.def.cpp',
  'third_party/microtex/src/res/font/stmary10.def.cpp',
  'third_party/microtex/src/res/font/tt10.def.cpp',
  'third_party/microtex/src/res/parser/parser_stub.cpp',
  'third_party/microtex/src/res/reg/builtin_font_reg.cpp',
  'third_party/microtex/src/res/reg/builtin_syms_reg.cpp',
  'third_party/microtex/src/res/sym/amsfonts.def.cpp',
  'third_party/microtex/src/res/sym/amssymb.def.cpp',
  'third_party/microtex/src/res/sym/base.def.cpp',
  'third_party/microtex/src/res/sym/stmaryrd.def.cpp',
  'third_party/microtex/src/res/sym/symspecial.def.cpp',
  'third_party/microtex/src/utils/string_utils.cpp',
  'third_party/microtex/src/utils/utf.cpp',
  'third_party/microtex/src/utils/utils.cpp',
]
microtex_inc = include_directories('third_party/microtex/src', is_system : true)
microtex_lib = static_library('zk-microtex', microtex_src,
  include_directories : microtex_inc,
  cpp_args : ['-w'],
  override_options : ['cpp_std=c++17'])
microtex_dep = declare_dependency(link_with : microtex_lib, include_directories : microtex_inc)

ui_incs = [inc, include_directories('third_party', is_system : true)]  # nlohmann/json.hpp
if backend == 'kindle'
  # kindle-sdk ships no fontconfig headers; the firmware has fontconfig 2.8.
  ui_incs += include_directories('third_party/fontconfig-2.8-headers', is_system : true)
endif

ui_lib = static_library('zk-ui',
  'src/ui/gesture.cpp',
  'src/ui/canvas.cpp',
  'src/platform/scaled_display.cpp',
  'src/platform/color.cpp',
  'src/ui/metrics.cpp',
  'src/ui/item_icons.cpp',
  'src/ui/text.cpp',
  'src/ui/math_cairo.cpp',
  'src/ui/math.cpp',
  'src/ui/paint.cpp',
  'src/ui/widgets.cpp',
  'src/ui/pager.cpp',
  'src/ui/text_field.cpp',
  'src/docs/fonts.cpp',
  include_directories : ui_incs,
  dependencies : [core_dep, cairo_dep, pango_deps, microtex_dep])

ui_dep = declare_dependency(link_with : ui_lib, dependencies : [core_dep, cairo_dep, pango_deps, microtex_dep])

fs = import('fs')
cxx = meson.get_compiler('cpp')
deps_prefix = meson.project_source_root() / 'build-deps' / backend / 'prefix'
docs_lib = static_library('zk-docs', 'src/docs/engine_factory.cpp', 'src/docs/shade.cpp', 'src/docs/render_worker.cpp',
  'src/docs/annotation_text.cpp', 'src/plugins/fulltext.cpp',
  include_directories : inc, dependencies : [core_dep, cxx.find_library('dl', required : false)])
docs_dep = declare_dependency(link_with : docs_lib, dependencies : [core_dep, dependency('threads'), cxx.find_library('dl', required : false)])

# Plugin runtime (JavaScript): grows file by file as the plugin tasks land.
plugins_lib = static_library('zk-plugins',
  'src/plugins/js.cpp',
  'src/plugins/manifest.cpp',
  'src/plugins/state.cpp',
  'src/plugins/safe_mode.cpp',
  'src/plugins/plugin_log.cpp',
  'src/plugins/host.cpp',
  'src/plugins/bindings_core.cpp',
  'src/plugins/bindings_storage.cpp',
  'src/plugins/bindings_fetch.cpp',
  'src/plugins/bindings_library.cpp',
  'src/plugins/bindings_zotero.cpp',
  'src/plugins/bindings_entry.cpp',
  'src/plugins/bindings_ui.cpp',
  'src/plugins/bindings_reader.cpp',
  'src/plugins/plugin_http.cpp',
  'src/plugins/ui_tree.cpp',
  include_directories : [inc, include_directories('third_party', is_system : true)],
  dependencies : [core_dep, store_dep, docs_dep, quickjs_dep, plugin_events_dep, ui_dep])  # ui: zk.display reads the metrics
plugins_dep = declare_dependency(link_with : plugins_lib, dependencies : [core_dep, store_dep, docs_dep, quickjs_dep, plugin_events_dep, ui_dep],
  include_directories : include_directories('third_party', is_system : true))

# Plugin catalog (part 3): the signed registry, the release check and the installer.
catalog_lib = static_library('zk-catalog', 'src/catalog/registry.cpp', 'src/catalog/check.cpp', 'src/catalog/install.cpp',
  include_directories : [inc, include_directories('third_party', is_system : true)],
  dependencies : [update_dep, plugins_dep])
catalog_dep = declare_dependency(link_with : catalog_lib, dependencies : [update_dep, plugins_dep])

screens_lib = static_library('zk-screens',
  'src/screens/home.cpp',
  'src/screens/annotation_bar.cpp',
  'src/screens/note_popup.cpp',
  'src/screens/collection_search.cpp',
  'src/screens/read_tag.cpp',
  'src/screens/group_libraries.cpp',
  'src/screens/sync_status.cpp',
  'src/screens/item_list.cpp',
  'src/screens/library.cpp',
  'src/screens/library_sheets.cpp',
  'src/screens/collections.cpp',
  'src/screens/search.cpp',
  'src/screens/open_flow.cpp',
  'src/screens/file_choice.cpp',
  'src/screens/book.cpp',
  'src/screens/notes.cpp',
  'src/screens/setup.cpp',
  'src/screens/account_error.cpp',
  'src/screens/account_sheet.cpp',
  'src/screens/accounts.cpp',
  'src/screens/pin_pad.cpp',
  'src/screens/remove_account.cpp',
  'src/screens/switching.cpp',
  'src/screens/settings.cpp',
  'src/screens/settings_home.cpp',
  'src/screens/update_screen.cpp',
  'src/update/update_notice.cpp',
  'src/screens/item_sheet.cpp',
  'src/screens/item_details.cpp',
  'src/screens/goto_page.cpp',
  'src/screens/reader.cpp',
  'src/screens/reader_menus.cpp',
  'src/screens/contents.cpp',
  'src/screens/book_notes.cpp',
  'src/screens/book_search.cpp',
  'src/screens/plugin_confirm.cpp',
  'src/screens/plugins.cpp',
  'src/screens/plugin_page.cpp',
  'src/screens/item_source.cpp',
  'src/screens/plugin_blocks.cpp',
  'src/screens/plugin_diff.cpp',
  'src/screens/plugin_body.cpp',
  'src/screens/plugin_screen.cpp',
  'src/screens/preview.cpp',
  'src/screens/preview_screen.cpp',
  'src/screens/plugin_result.cpp',
  'src/screens/plugin_launcher.cpp',
  'src/screens/catalog_list.cpp',
  'src/screens/catalog_page.cpp',
  'src/app/catalog_service.cpp',
  'src/app/settings_store.cpp', 'src/app/device_settings.cpp',
  include_directories : ui_incs + [include_directories('third_party', is_system : true)],
  dependencies : [ui_dep, store_dep, sync_dep, docs_dep, update_dep, plugins_dep, catalog_dep])

screens_dep = declare_dependency(link_with : screens_lib, dependencies : [ui_dep, store_dep, sync_dep, docs_dep, update_dep, plugins_dep, catalog_dep])

app_lib = static_library('zk-app',
  'src/app/stall_watchdog.cpp', 'src/app/app.cpp', 'src/app/color_test.cpp',
  'src/app/startup.cpp', 'src/app/migrate_accounts.cpp',
  'src/app/lifecycle.cpp',
  'src/app/plugin_events.cpp',
  'src/app/home_tree.cpp',
  'src/app/plugin_background.cpp',
  'src/app/plugin_watch.cpp',
  'src/app/previews.cpp',
  'src/app/session.cpp',
  'src/app/account_switch.cpp',
  'src/app/account_ui.cpp',
  include_directories : ui_incs,
  dependencies : [screens_dep, services_dep, sync_dep, plugins_dep])
app_dep = declare_dependency(link_with : app_lib, dependencies : [screens_dep, services_dep, sync_dep, plugins_dep])

if backend == 'kindle'
  fbink_lib = meson.get_compiler('cpp').find_library('fbink',
    dirs : meson.project_source_root() / 'build-deps/kindle/prefix/lib', static : true)
  platform_lib = static_library('zk-platform', 'src/platform/kindle/x11_display.cpp', 'src/platform/kindle/x_touch.cpp',
    'src/platform/kindle/kindle_keyboard.cpp', 'src/platform/kindle/evdev_pen.cpp', 'src/platform/kindle/wet_ink.cpp',
    include_directories : ui_incs + [include_directories('build-deps/kindle/prefix/include', is_system : true)],
    dependencies : [ui_dep, services_dep, dependency('x11'), dependency('xext'), fbink_lib])
  platform_dep = declare_dependency(link_with : platform_lib, dependencies : [ui_dep, services_dep, dependency('x11'), dependency('xext'), fbink_lib])
else
  platform_lib = static_library('zk-platform', 'src/platform/desktop/sdl_display.cpp',
    'src/platform/desktop/sdl_keyboard.cpp',
    include_directories : ui_incs, dependencies : [ui_dep, services_dep, dependency('sdl2')])
  platform_dep = declare_dependency(link_with : platform_lib, dependencies : [ui_dep, services_dep, dependency('sdl2')])
endif

# ---- Readers (Plan 4): each engine is a shared module loaded at runtime ---------------------
# crengine and MuPDF bring their own static freetype/harfbuzz; inside a module linked
# -Bsymbolic with a version script exporting only the factory (and the EPUB module's cache
# close), they never meet the firmware's
# copies that pango uses (spike README). The app links neither engine.
module_link_args = []
epub_link_args = []  # (the version script exports it on Linux)
if host_machine.system() == 'darwin'
  module_link_args += ['-Wl,-exported_symbol,_zk_*_engine_create']
  epub_link_args = ['-Wl,-exported_symbol,_zk_epub_doc_cache_close']  # the account session closes crengine's cache
else
  module_link_args += ['-Wl,-Bsymbolic', '-Wl,--version-script=' + meson.project_source_root() / 'src/docs/zk-engine.map']
endif

if fs.exists(deps_prefix / 'lib' / 'libcrengine.a')
  cre_libs = [
    cxx.find_library('crengine', dirs : deps_prefix / 'lib', static : true),
    cxx.find_library('unibreak', dirs : deps_prefix / 'lib', static : true),
  ]
  cre_link_args = []
  if backend == 'kindle'
    foreach l : ['harfbuzz', 'freetype', 'fribidi', 'utf8proc', 'zstd', 'png16', 'jpeg', 'xxhash', 'z']
      cre_libs += cxx.find_library(l, dirs : deps_prefix / 'lib', static : true)
    endforeach
    cre_link_args = ['-lpthread', '-ldl']
  else
    foreach p : ['freetype2', 'harfbuzz', 'fribidi', 'libutf8proc', 'libzstd', 'libpng', 'libjpeg', 'zlib', 'libxxhash']
      cre_libs += dependency(p)
    endforeach
  endif
  engine_crengine_dep = declare_dependency(
    include_directories : include_directories(
      'build-deps' / backend / 'prefix/include',
      'build-deps' / backend / 'prefix/include/crengine',
      'build-deps' / backend / 'prefix/include/harfbuzz',
      'build-deps' / backend / 'prefix/include/freetype2', is_system : true),
    dependencies : cre_libs,
    link_args : cre_link_args)
  shared_module('zk-epub',
    'src/docs/epub/epub_module.cpp',
    'src/docs/epub/epub_engine.cpp',
    'src/docs/epub/cfi.cpp',
    'src/docs/annotation_text.cpp',
    'third_party/pugixml/pugixml.cpp',
    'src/docs/shade.cpp',
    name_prefix : 'lib', name_suffix : 'so',
    include_directories : [inc, include_directories('third_party', 'third_party/pugixml', is_system : true)],
    cpp_args : ['-fvisibility=hidden'],
    dependencies : [engine_crengine_dep],
    link_args : module_link_args + epub_link_args,
    install : true, install_dir : 'lib')
endif

if fs.exists(deps_prefix / 'lib' / 'libmupdf.a')
  mupdf_libs = [cxx.find_library('mupdf', dirs : deps_prefix / 'lib', static : true),
                cxx.find_library('mupdf-third', dirs : deps_prefix / 'lib', static : true)]
  if backend == 'desktop'
    mupdf_libs += dependency('zlib')  # MuPDF built with the system zlib on macOS
  endif
  shared_module('zk-pdf',
    'src/docs/pdf/pdf_module.cpp',
    'src/docs/pdf/pdf_engine.cpp',
    'src/docs/annotation_text.cpp',
    'src/docs/shade.cpp',
    name_prefix : 'lib', name_suffix : 'so',
    include_directories : [inc, include_directories('third_party', is_system : true),
                           include_directories('build-deps' / backend / 'prefix/include', is_system : true)],
    cpp_args : ['-fvisibility=hidden'],
    dependencies : mupdf_libs,
    link_args : module_link_args + (backend == 'kindle' ? ['-lm', '-lpthread'] : []),
    install : true, install_dir : 'lib')
endif

executable('zotero', 'src/app/main.cpp',
  dependencies : [app_dep, platform_dep],
  install : true)

executable('zk-sync', 'src/app/zk_sync.cpp',
  include_directories : include_directories('third_party', is_system : true),
  dependencies : [sync_dep, docs_dep],
  install : true)

executable('zk-sign', 'tools/zk_sign.cpp',
  dependencies : [update_dep],
  build_by_default : false)

# The README's demo library (public-domain books only): tools/screenshots.sh.
executable('zk-demo-library', 'tools/demo_library.cpp',
  include_directories : include_directories('third_party', is_system : true),
  dependencies : [store_dep],
  build_by_default : false)

if get_option('tests')
  subdir('tests')
endif

if get_option('spikes')
  subdir('spikes')
endif
