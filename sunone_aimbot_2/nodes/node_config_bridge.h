#pragma once
#include "nodes/node_graph.h"
class Config;

namespace NodeGraph
{
// Import old global tuning exactly once. Version 3 graphs own their settings.
void migrateLegacySettings(Workspace& workspace, const Config& config, bool seedDefaults = false);
// Startup fills only empty, unwired local model inputs. Runtime still reads
// the selected path from the graph, including any connected override.
bool initializeMissingModels(Workspace& workspace,const std::string& preferredModel,const std::string& nativeBackend);
bool importLegacyCalibrations(Workspace& workspace,const Config& config);
bool adjustActiveDefaults(Workspace& workspace,int offsetDirection,int recoilDirection,bool head);
}
