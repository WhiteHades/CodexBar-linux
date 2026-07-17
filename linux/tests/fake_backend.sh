#!/bin/sh

set -eu

exec cat "$(dirname "$0")/../../fixtures/native/backend-usage.json"
