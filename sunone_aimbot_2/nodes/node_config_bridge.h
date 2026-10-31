#pragma once
#include "nodes/node_graph.h"
class Config;

namespace NodeGraph
{
// Import old global tuning exactly once. Version 3 graphs own their settings.
void migrateLegacySettings(Workspace& workspace, const Config& config, bool seedDefaults = false);
}
