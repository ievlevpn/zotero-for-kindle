project('zotero-kindle', 'c', 'cpp',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['cpp_std=c++20', 'warning_level=2', 'buildtype=debugoptimized'])

backend = get_option('platform')

add_project_arguments('-DZK_BACKEND="' + backend + '"', language : 'cpp')
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

core_lib = static_library('zk-core',
  'src/core/log.cpp',
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
  dependencies : [net_dep, sqlite_dep])
store_dep = declare_dependency(link_with : store_lib, dependencies : [net_dep, sqlite_dep])

zotero_lib = static_library('zk-zotero',
  'src/zotero/parse.cpp',
  'src/zotero/api.cpp',
  include_directories : [inc, include_directories('third_party', is_system : true)],
  dependencies : [store_dep])
zotero_dep = declare_dependency(link_with : zotero_lib, dependencies : [store_dep])

# Device services shared by the app and its tests: lipc, keep-awake, storage watch, paths.
services_lib = static_library('zk-services',
  'src/platform/power.cpp',
  'src/platform/keysym.cpp',
  'src/platform/keyboard.cpp',
  'src/app/paths.cpp',
  backend == 'kindle' ? 'src/platform/kindle/kindle_lipc.cpp' : 'src/platform/desktop/null_lipc.cpp',
  include_directories : inc,
  dependencies : [core_dep])
services_dep = declare_dependency(link_with : services_lib, dependencies : [core_dep])

sync_lib = static_library('zk-sync',
  'src/sync/sync_engine.cpp',
  'src/sync/webdav.cpp',
  'src/sync/config.cpp',
  'src/sync/sync_service.cpp',
  'third_party/pugixml/pugixml.cpp',
  include_directories : [inc, include_directories('third_party', 'third_party/pugixml', is_system : true)],
  dependencies : [zotero_dep, miniz_dep, md5_dep, services_dep])
sync_dep = declare_dependency(link_with : sync_lib, dependencies : [zotero_dep, miniz_dep, md5_dep, services_dep])


cairo_dep = dependency('cairo')
pango_deps = [dependency('pangocairo'), dependency('pangoft2'), dependency('fontconfig')]

ui_incs = [inc]
if backend == 'kindle'
  # kindle-sdk ships no fontconfig headers; the firmware has fontconfig 2.8.
  ui_incs += include_directories('third_party/fontconfig-2.8-headers', is_system : true)
endif

ui_lib = static_library('zk-ui',
  'src/ui/gesture.cpp',
  'src/ui/canvas.cpp',
  'src/ui/text.cpp',
  'src/ui/paint.cpp',
  'src/ui/widgets.cpp',
  'src/ui/pager.cpp',
  'src/ui/text_field.cpp',
  include_directories : ui_incs,
  dependencies : [core_dep, cairo_dep, pango_deps])

ui_dep = declare_dependency(link_with : ui_lib, dependencies : [core_dep, cairo_dep, pango_deps])

fs = import('fs')
cxx = meson.get_compiler('cpp')
deps_prefix = meson.project_source_root() / 'build-deps' / backend / 'prefix'
docs_lib = static_library('zk-docs', 'src/docs/engine_factory.cpp', 'src/docs/shade.cpp', 'src/docs/render_worker.cpp',
  include_directories : inc, dependencies : [core_dep, cxx.find_library('dl', required : false)])
docs_dep = declare_dependency(link_with : docs_lib, dependencies : [core_dep, cxx.find_library('dl', required : false)])

screens_lib = static_library('zk-screens',
  'src/screens/home.cpp',
  'src/screens/sync_status.cpp',
  'src/screens/item_list.cpp',
  'src/screens/library.cpp',
  'src/screens/collections.cpp',
  'src/screens/search.cpp',
  'src/screens/open_flow.cpp',
  'src/screens/book.cpp',
  'src/screens/notes.cpp',
  'src/screens/setup.cpp',
  'src/screens/settings.cpp',
  'src/screens/reader.cpp',
  'src/screens/reader_menus.cpp',
  'src/screens/contents.cpp',
  'src/screens/book_notes.cpp',
  'src/app/settings_store.cpp',
  include_directories : ui_incs + [include_directories('third_party', is_system : true)],
  dependencies : [ui_dep, store_dep, sync_dep, docs_dep])

screens_dep = declare_dependency(link_with : screens_lib, dependencies : [ui_dep, store_dep, sync_dep, docs_dep])

app_lib = static_library('zk-app',
  'src/app/app.cpp',
  'src/app/startup.cpp',
  'src/app/lifecycle.cpp',
  include_directories : ui_incs,
  dependencies : [screens_dep, services_dep, sync_dep])
app_dep = declare_dependency(link_with : app_lib, dependencies : [screens_dep, services_dep, sync_dep])

if backend == 'kindle'
  fbink_lib = meson.get_compiler('cpp').find_library('fbink',
    dirs : meson.project_source_root() / 'build-deps/kindle/prefix/lib', static : true)
  platform_lib = static_library('zk-platform', 'src/platform/kindle/x11_display.cpp', 'src/platform/kindle/x_touch.cpp',
    'src/platform/kindle/kindle_keyboard.cpp',
    include_directories : ui_incs + [include_directories('build-deps/kindle/prefix/include', is_system : true)],
    dependencies : [ui_dep, services_dep, dependency('x11'), fbink_lib])
  platform_dep = declare_dependency(link_with : platform_lib, dependencies : [ui_dep, services_dep, dependency('x11'), fbink_lib])
else
  platform_lib = static_library('zk-platform', 'src/platform/desktop/sdl_display.cpp',
    'src/platform/desktop/sdl_keyboard.cpp',
    include_directories : ui_incs, dependencies : [ui_dep, services_dep, dependency('sdl2')])
  platform_dep = declare_dependency(link_with : platform_lib, dependencies : [ui_dep, services_dep, dependency('sdl2')])
endif

# ---- Readers (Plan 4): each engine is a shared module loaded at runtime ---------------------
# crengine and MuPDF bring their own static freetype/harfbuzz; inside a module linked
# -Bsymbolic with a version script exporting only the factory, they never meet the firmware's
# copies that pango uses (spike README). The app links neither engine.
module_link_args = []
if host_machine.system() == 'darwin'
  module_link_args += ['-Wl,-exported_symbol,_zk_*_engine_create']
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
    'third_party/pugixml/pugixml.cpp',
    'src/docs/shade.cpp',
    name_prefix : 'lib', name_suffix : 'so',
    include_directories : [inc, include_directories('third_party', 'third_party/pugixml', is_system : true)],
    cpp_args : ['-fvisibility=hidden'],
    dependencies : [engine_crengine_dep],
    link_args : module_link_args,
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
  dependencies : [sync_dep],
  install : true)

if get_option('tests')
  subdir('tests')
endif

if get_option('spikes')
  subdir('spikes')
endif
