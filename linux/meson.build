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
  'src/cli_cache.c',
  'src/cli_cards.c',
  'src/cli_config.c',
  'src/cli_cost.c',
  'src/cli_diagnose.c',
  'src/cli_sessions.c',
  'src/cli_serve.c',
  'src/cli_usage.c',
  'src/claude.c',
  'src/codebuff.c',
  'src/config.c',
  'src/copilot.c',
  'src/codex.c',
  'src/cost.c',
  'src/diagnose.c',
  'src/desktop.c',
  'src/http.c',
  'src/jetbrains.c',
  'src/kilo.c',
  'src/kimi.c',
  'src/model.c',
  'src/openai_api.c',
  'src/openrouter.c',
  'src/process.c',
  'src/provider_registry.c',
  'src/proxy_providers.c',
  'src/render.c',
  'src/sessions.c',
  'src/serve.c',
  'src/simple_providers.c',
  'src/tui_actions.c',
  'src/zai.c',
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

install_data(
  'data/com.steipete.codexbar.desktop',
  install_dir: get_option('datadir') / 'applications',
)
install_data(
  'data/codexbar-status.desktop',
  install_dir: get_option('sysconfdir') / 'xdg' / 'autostart',
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

tui_launch_test = find_program('tests/test_tui_launch.sh')
test(
  'codexbar-linux-tui-launch',
  tui_launch_test,
  args: [codexbar_linux],
  depends: process_supervisor,
)

status_item_test = find_program('tests/test_status_item.sh')
test(
  'codexbar-linux-status-item',
  status_item_test,
  args: [
    codexbar_linux,
    find_program('tests/fake_backend.sh'),
    files('data/com.steipete.codexbar.desktop'),
    files('data/codexbar-status.desktop'),
  ],
  depends: process_supervisor,
  timeout: 30,
)

config_cli_test = find_program('tests/test_config_cli.sh')
test('codexbar-linux-config-cli', config_cli_test, args: [codexbar_linux], depends: process_supervisor)

cards_cli_test = find_program('tests/test_cards_cli.sh')
test(
  'codexbar-linux-cards-cli',
  cards_cli_test,
  args: [codexbar_linux, find_program('tests/fake_backend.sh')],
  depends: process_supervisor,
)

cost_cli_test = find_program('tests/test_cost_cli.sh')
test('codexbar-linux-cost-cli', cost_cli_test, args: [codexbar_linux], depends: process_supervisor)

sessions_cli_test = find_program('tests/test_sessions_cli.sh')
test('codexbar-linux-sessions-cli', sessions_cli_test, args: [codexbar_linux], depends: process_supervisor)

cache_cli_test = find_program('tests/test_cache_cli.sh')
test('codexbar-linux-cache-cli', cache_cli_test, args: [codexbar_linux])

claude_test = executable(
  'codexbar-linux-claude-tests',
  files('tests/test_claude.c', 'src/claude.c', 'src/http.c', 'src/model.c', 'src/process.c'),
  include_directories: source_includes,
  dependencies: core_deps,
)
test('codexbar-linux-claude', claude_test)

claude_cli_test = find_program('tests/test_claude_cli.sh')
test('codexbar-linux-claude-cli', claude_cli_test, args: [codexbar_linux], depends: process_supervisor)

copilot_test = executable(
  'codexbar-linux-copilot-tests',
  files('tests/test_copilot.c', 'src/copilot.c', 'src/http.c', 'src/model.c'),
  include_directories: source_includes,
  dependencies: core_deps,
)
test('codexbar-linux-copilot', copilot_test)

copilot_cli_test = find_program('tests/test_copilot_cli.sh')
test('codexbar-linux-copilot-cli', copilot_cli_test, args: [codexbar_linux])

zai_test = executable(
  'codexbar-linux-zai-tests',
  files('tests/test_zai.c', 'src/zai.c', 'src/http.c', 'src/model.c'),
  include_directories: source_includes,
  dependencies: core_deps,
)
test('codexbar-linux-zai', zai_test)

zai_cli_test = find_program('tests/test_zai_cli.sh')
test('codexbar-linux-zai-cli', zai_cli_test, args: [codexbar_linux])

openai_api_test = executable(
  'codexbar-linux-openai-api-tests',
  files('tests/test_openai_api.c', 'src/openai_api.c', 'src/http.c', 'src/model.c'),
  include_directories: source_includes,
  dependencies: core_deps,
)
test('codexbar-linux-openai-api', openai_api_test)

openai_api_cli_test = find_program('tests/test_openai_api_cli.sh')
test('codexbar-linux-openai-api-cli', openai_api_cli_test, args: [codexbar_linux])

kilo_test = executable(
  'codexbar-linux-kilo-tests',
  files('tests/test_kilo.c', 'src/kilo.c', 'src/http.c', 'src/model.c'),
  include_directories: source_includes,
  dependencies: core_deps,
)
test('codexbar-linux-kilo', kilo_test)

diagnose_cli_test = find_program('tests/test_diagnose_cli.sh')
test(
  'codexbar-linux-diagnose-cli',
  diagnose_cli_test,
  args: [codexbar_linux, find_program('tests/fake_backend.sh')],
  depends: process_supervisor,
)

serve_cli_test = find_program('tests/test_serve_cli.sh')
test(
  'codexbar-linux-serve-cli',
  serve_cli_test,
  args: [codexbar_linux, find_program('tests/fake_backend.sh')],
  depends: process_supervisor,
  timeout: 30,
)

usage_cli_test = find_program('tests/test_usage_cli.sh')
test(
  'codexbar-linux-usage-cli',
  usage_cli_test,
  args: [
    codexbar_linux,
    files('tests/fake_backend.sh'),
    meson.project_version(),
    files('../fixtures/native/jetbrains/quota-full.xml'),
  ],
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

jetbrains_test = executable(
  'codexbar-linux-jetbrains-tests',
  files('src/jetbrains.c', 'src/model.c', 'tests/test_jetbrains.c'),
  include_directories: source_includes,
  dependencies: [glib_dep, gio_dep, json_dep, math_dep],
)
test(
  'codexbar-linux-jetbrains',
  jetbrains_test,
  args: [meson.project_source_root() / 'fixtures/native/jetbrains'],
  env: normalized_test_env,
)
