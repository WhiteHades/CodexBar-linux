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
  'src/codebuff.c',
  'src/config.c',
  'src/codex.c',
  'src/http.c',
  'src/kimi.c',
  'src/model.c',
  'src/openrouter.c',
  'src/process.c',
  'src/provider_registry.c',
  'src/proxy_providers.c',
  'src/render.c',
  'src/simple_providers.c',
  'src/tui_actions.c',
)

core_deps = [glib_dep, gio_dep, json_dep, curl_dep, math_dep]

process_supervisor = executable(
  'codexbar-process-supervisor',
  files('src/process_supervisor.c'),
  install: true,
)

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

normalized_test_env = environment()
normalized_test_env.set('LC_ALL', 'C')
normalized_test_env.set('TZ', 'UTC')
test('codexbar-linux', test_executable, env: normalized_test_env, depends: process_supervisor)

serialization_test = executable(
  'codexbar-linux-usage-serialization-tests',
  files('src/model.c', 'src/render.c', 'tests/test_usage_serialization.c'),
  include_directories: source_includes,
  dependencies: [glib_dep, json_dep, math_dep],
)
serialization_fixture = files('../fixtures/native/usage-serialization-v1.json')
test(
  'codexbar-linux-usage-serialization',
  serialization_test,
  args: serialization_fixture,
  env: normalized_test_env,
)

waybar_test = find_program('tests/test_waybar.sh')
test(
  'codexbar-linux-waybar',
  waybar_test,
  args: [codexbar_linux, files('tests/fake_backend.sh'), files('../fixtures/native/config-openrouter-missing.json')],
  depends: process_supervisor,
)

config_cli_test = find_program('tests/test_config_cli.sh')
test('codexbar-linux-config-cli', config_cli_test, args: [codexbar_linux], depends: process_supervisor)

usage_cli_test = find_program('tests/test_usage_cli.sh')
test(
  'codexbar-linux-usage-cli',
  usage_cli_test,
  args: [codexbar_linux, files('tests/fake_backend.sh'), meson.project_version()],
  depends: process_supervisor,
)

tui_actions_test = executable(
  'codexbar-linux-tui-actions-tests',
  files('src/provider_registry.c', 'src/tui_actions.c', 'tests/test_tui_actions.c'),
  include_directories: source_includes,
  dependencies: [glib_dep, gio_dep],
)
test('codexbar-linux-tui-actions', tui_actions_test)

process_test = executable(
  'codexbar-linux-process-tests',
  files('src/process.c', 'tests/test_process.c'),
  include_directories: source_includes,
  dependencies: [glib_dep, gio_dep],
)
test('codexbar-linux-process', process_test, env: normalized_test_env, depends: process_supervisor)
