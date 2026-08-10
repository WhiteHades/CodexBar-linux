SHELL := /bin/sh

MESON ?= meson
BUILD_DIR ?= .build/debug
RELEASE_DIR ?= .build/release
SANITIZE_DIR ?= .build/sanitize
PREFIX ?= /usr/local
PACKAGE_VERSION := $(shell sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" meson.build)
AUR_VERSION ?= $(PACKAGE_VERSION)

.PHONY: all build check check-packaging clean configure install package package-appimage package-aur package-deb \
	package-rpm package-source release run sanitize test verify-tree

all: build

configure:
	@if test -f "$(BUILD_DIR)/build.ninja"; then \
		$(MESON) setup --reconfigure "$(BUILD_DIR)" --buildtype=debug --wrap-mode=nodownload; \
	else \
		$(MESON) setup "$(BUILD_DIR)" --buildtype=debug --wrap-mode=nodownload; \
	fi

build: configure
	$(MESON) compile -C "$(BUILD_DIR)"

test: build
	$(MESON) test -C "$(BUILD_DIR)" --no-rebuild --print-errorlogs

check:
	./Scripts/check.sh

sanitize:
	@if test -f "$(SANITIZE_DIR)/build.ninja"; then \
		$(MESON) setup --reconfigure "$(SANITIZE_DIR)" --buildtype=debug --wrap-mode=nodownload \
			-Db_sanitize=address,undefined -Db_lundef=false; \
	else \
		$(MESON) setup "$(SANITIZE_DIR)" --buildtype=debug --wrap-mode=nodownload \
			-Db_sanitize=address,undefined -Db_lundef=false; \
	fi
	$(MESON) compile -C "$(SANITIZE_DIR)"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(MESON) test -C "$(SANITIZE_DIR)" --no-rebuild --print-errorlogs

release:
	@if test -f "$(RELEASE_DIR)/build.ninja"; then \
		$(MESON) setup --reconfigure "$(RELEASE_DIR)" --buildtype=release --wrap-mode=nodownload; \
	else \
		$(MESON) setup "$(RELEASE_DIR)" --buildtype=release --wrap-mode=nodownload; \
	fi
	$(MESON) compile -C "$(RELEASE_DIR)"
	$(MESON) test -C "$(RELEASE_DIR)" --no-rebuild --print-errorlogs

run: build
	"$(BUILD_DIR)/linux/codexbar-linux" tui

install: release
	$(MESON) install -C "$(RELEASE_DIR)" --destdir "$(DESTDIR)"

package: check-packaging
	./Scripts/package.sh

package-source: check-packaging
	./Scripts/source-archive.sh

package-deb: check-packaging
	./Scripts/package-deb.sh

package-rpm: check-packaging
	./Scripts/package-rpm.sh

package-appimage: check-packaging
	./Scripts/package-appimage.sh

package-aur:
	./Scripts/prepare-aur.sh "$(AUR_VERSION)"

check-packaging:
	./Scripts/check-packaging.sh

verify-tree:
	./Scripts/verify-tree.sh

clean:
	@printf '%s\n' 'Remove .build/ manually when a clean rebuild is required.'
