project('zotero-kindle', 'c', 'cpp',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['cpp_std=c++20', 'warning_level=2', 'buildtype=debugoptimized'])

backend = get_option('platform')

add_project_arguments('-DZK_BACKEND="' + backend + '"', language : 'cpp')
if backend == 'kindle'
  add_project_arguments('-DZK_KINDLE=1', language : 'cpp')
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
  include_directories : ui_incs,
  dependencies : [core_dep, cairo_dep, pango_deps])

ui_dep = declare_dependency(link_with : ui_lib, dependencies : [core_dep, cairo_dep, pango_deps])

screens_lib = static_library('zk-screens',
  'src/screens/home_demo.cpp',
  include_directories : ui_incs,
  dependencies : [ui_dep])

screens_dep = declare_dependency(link_with : screens_lib, dependencies : [ui_dep])

if backend == 'kindle'
  fbink_lib = meson.get_compiler('cpp').find_library('fbink',
    dirs : meson.project_source_root() / 'build-deps/kindle/prefix/lib', static : true)
  platform_lib = static_library('zk-platform', 'src/platform/kindle/x11_display.cpp', 'src/platform/kindle/x_touch.cpp',
    include_directories : ui_incs + [include_directories('build-deps/kindle/prefix/include', is_system : true)],
    dependencies : [ui_dep, dependency('x11'), fbink_lib])
  platform_dep = declare_dependency(link_with : platform_lib, dependencies : [ui_dep, dependency('x11'), fbink_lib])
else
  platform_lib = static_library('zk-platform', 'src/platform/desktop/sdl_display.cpp',
    include_directories : ui_incs, dependencies : [ui_dep, dependency('sdl2')])
  platform_dep = declare_dependency(link_with : platform_lib, dependencies : [ui_dep, dependency('sdl2')])
endif

executable('zotero', 'src/app/main.cpp',
  dependencies : [core_dep, ui_dep, screens_dep, platform_dep],
  install : true)

if get_option('tests')
  subdir('tests')
endif

if get_option('spikes')
  subdir('spikes')
endif
