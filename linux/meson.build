glib_dep = dependency('glib-2.0', version: '>=2.78')
gio_dep = dependency('gio-2.0', version: '>=2.78')
json_dep = dependency('json-c', version: '>=0.17')
ncurses_dep = dependency('ncursesw', version: '>=6.4')
curl_dep = dependency('libcurl', version: '>=8.0')
source_includes = include_directories('src')
version_config = configuration_data()
version_config.set_quoted('CODEXBAR_LINUX_VERSION', meson.project_version())
configure_file(output: 'version.h', configuration: version_config)

core_sources = files(
  'src/backend.c',
  'src/config.c',
  'src/http.c',
  'src/model.c',
  'src/openrouter.c',
  'src/render.c',
)

core_deps = [glib_dep, gio_dep, json_dep, curl_dep]

codexbar_linux = executable(
  'codexbar-linux',
  core_sources + files('src/main.c', 'src/tui.c'),
  include_directories: source_includes,
  dependencies: core_deps + [ncurses_dep],
  install: true,
)

test_executable = executable(
  'codexbar-linux-tests',
  core_sources + files('tests/test_model.c'),
  include_directories: source_includes,
  dependencies: core_deps,
)

test('codexbar-linux', test_executable)

waybar_test = find_program('tests/test_waybar.sh')
test(
  'codexbar-linux-waybar',
  waybar_test,
  args: [codexbar_linux, files('tests/fake_backend.sh'), files('tests/config-openrouter-missing.json')],
)
