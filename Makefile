SHELL := /bin/bash

MESON ?= meson
BUILD_DIR ?= build
RELEASE_DIR ?= build-release
SANITIZE_DIR ?= build-asan

.PHONY: build check docs-list format lint native-build native-check native-clean native-configure native-install \
	native-release native-run native-sanitize native-test release restart start start-debug start-release stop test \
	test-live test-tty

start:
	./Scripts/compile_and_run.sh

start-debug:
	./Scripts/compile_and_run.sh

start-release:
	./Scripts/package_app.sh release
	pkill -x CodexBar || pkill -f CodexBar.app || true
	cd /Users/steipete/Projects/codexbar && open -n /Users/steipete/Projects/codexbar/CodexBar.app

restart: start

stop:
	pkill -x CodexBar || pkill -f CodexBar.app || true

check lint:
	./Scripts/lint.sh lint

format:
	./Scripts/lint.sh format

docs-list:
	node Scripts/docs-list.mjs

build:
	swift build

test:
	./Scripts/test.sh

test-tty:
	swift test --filter TTYIntegrationTests

test-live:
	LIVE_TEST=1 swift test --filter LiveAccountTests

release:
	./Scripts/package_app.sh release

native-configure:
	@if test -f "$(BUILD_DIR)/build.ninja"; then \
		$(MESON) setup --reconfigure "$(BUILD_DIR)" --buildtype=debug --wrap-mode=nodownload \
			-Db_sanitize= -Db_lundef=true; \
	else \
		$(MESON) setup "$(BUILD_DIR)" --buildtype=debug --wrap-mode=nodownload \
			-Db_sanitize= -Db_lundef=true; \
	fi

native-build: native-configure
	$(MESON) compile -C "$(BUILD_DIR)"

native-test: native-build
	$(MESON) test -C "$(BUILD_DIR)" --no-rebuild --print-errorlogs

native-check: native-test
	git diff HEAD --check

native-release:
	@if test -f "$(RELEASE_DIR)/build.ninja"; then \
		$(MESON) setup --reconfigure "$(RELEASE_DIR)" --buildtype=release --wrap-mode=nodownload \
			-Db_sanitize= -Db_lundef=true; \
	else \
		$(MESON) setup "$(RELEASE_DIR)" --buildtype=release --wrap-mode=nodownload \
			-Db_sanitize= -Db_lundef=true; \
	fi
	$(MESON) compile -C "$(RELEASE_DIR)"
	$(MESON) test -C "$(RELEASE_DIR)" --no-rebuild --print-errorlogs

native-sanitize:
	@if test -f "$(SANITIZE_DIR)/build.ninja"; then \
		$(MESON) setup --reconfigure "$(SANITIZE_DIR)" --buildtype=debug --wrap-mode=nodownload \
			-Db_sanitize=address,undefined -Db_lundef=false; \
	else \
		$(MESON) setup "$(SANITIZE_DIR)" --buildtype=debug --wrap-mode=nodownload \
			-Db_sanitize=address,undefined -Db_lundef=false; \
	fi
	$(MESON) compile -C "$(SANITIZE_DIR)"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(MESON) test -C "$(SANITIZE_DIR)" --no-rebuild --print-errorlogs

native-run: native-build
	"$(BUILD_DIR)/linux/codexbar-linux" tui

native-install: native-release
	$(MESON) install -C "$(RELEASE_DIR)"

native-clean:
	rm -rf "$(BUILD_DIR)" "$(RELEASE_DIR)" "$(SANITIZE_DIR)"
