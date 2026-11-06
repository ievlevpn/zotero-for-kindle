project('zotero-kindle', 'cpp',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['cpp_std=c++20', 'warning_level=2', 'buildtype=debugoptimized'])

backend = get_option('platform')

add_project_arguments('-DZK_BACKEND="' + backend + '"', language : 'cpp')
if backend == 'kindle'
  add_project_arguments('-DZK_KINDLE=1', language : 'cpp')
  add_project_link_arguments('-static-libstdc++', '-static-libgcc', language : 'cpp')
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

ui_lib = static_library('zk-ui',
  'src/ui/gesture.cpp',
  'src/ui/canvas.cpp',
  'src/ui/text.cpp',
  include_directories : inc,
  dependencies : [core_dep, cairo_dep, pango_deps])

ui_dep = declare_dependency(link_with : ui_lib, dependencies : [core_dep, cairo_dep, pango_deps])

executable('zotero', 'src/app/main.cpp',
  dependencies : [core_dep, ui_dep],
  install : true)

if get_option('tests')
  subdir('tests')
endif

if get_option('spikes')
  subdir('spikes')
endif
