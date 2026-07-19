#pragma once

#include <json-c/json.h>

#include "config.h"
#include "provider_registry.h"

json_object *codexbar_diagnose_provider(const CodexBarProviderDescriptor *descriptor,
                                        const CodexBarProviderConfig *config);
