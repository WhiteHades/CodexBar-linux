glib_dep = dependency('glib-2.0', version: '>=2.78')
gio_dep = dependency('gio-2.0', version: '>=2.78')
json_dep = dependency('json-c', version: '>=0.17')
ncurses_dep = dependency('ncursesw', version: '>=6.4')
curl_dep = dependency('libcurl', version: '>=8.0')
math_dep = meson.get_compiler('c').find_library('m')
source_includes = include_directories('src')
version_config = configuration_data()
version_config.set_quoted('CODEXBAR_LINUX_VERSION', meson.project_version())
configure_file(output: 'version.h', configuration: version_config)

core_sources = files(
  'src/backend.c',
  'src/cli_config.c',
  'src/cli_usage.c',
  'src/config.c',
  'src/codex.c',
  'src/http.c',
  'src/kimi.c',
  'src/model.c',
  'src/openrouter.c',
  'src/provider_registry.c',
  'src/render.c',
  'src/simple_providers.c',
  'src/tui_actions.c',
)

core_deps = [glib_dep, gio_dep, json_dep, curl_dep, math_dep]

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

config_cli_test = find_program('tests/test_config_cli.sh')
test('codexbar-linux-config-cli', config_cli_test, args: [codexbar_linux])

usage_cli_test = find_program('tests/test_usage_cli.sh')
test('codexbar-linux-usage-cli', usage_cli_test, args: [codexbar_linux, files('tests/fake_backend.sh')])

tui_actions_test = executable(
  'codexbar-linux-tui-actions-tests',
  files('src/provider_registry.c', 'src/tui_actions.c', 'tests/test_tui_actions.c'),
  include_directories: source_includes,
  dependencies: [glib_dep, gio_dep],
)
test('codexbar-linux-tui-actions', tui_actions_test)
