project('zotero-kindle', 'cpp',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['cpp_std=c++20', 'warning_level=2', 'buildtype=debugoptimized'])

backend = get_option('platform')

add_project_arguments('-DZK_BACKEND="' + backend + '"', language : 'cpp')
if backend == 'kindle'
  add_project_arguments('-DZK_KINDLE=1', language : 'cpp')
  # Static C++ runtime; static third-party libs (freetype, harfbuzz for crengine) stay private
  # so they don't override the firmware's copies that pango uses.
  add_project_link_arguments('-static-libstdc++', '-static-libgcc', '-Wl,--exclude-libs,ALL', language : 'cpp')
else
  add_project_arguments('-DZK_DESKTOP=1', language : 'cpp')
endif

inc = include_directories('src')

core_lib = static_library('zk-core',
  'src/core/log.cpp',
  include_directories : inc)

core_dep = declare_dependency(link_with : core_lib, include_directories : inc)

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

executable('zotero', 'src/app/main.cpp',
  dependencies : [core_dep, ui_dep],
  install : true)

if get_option('tests')
  subdir('tests')
endif

if get_option('spikes')
  subdir('spikes')
endif
