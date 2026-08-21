#include "score_addon_openzen.hpp"

#include <score/plugins/FactorySetup.hpp>

#include <OpenZen/ProtocolFactory.hpp>

score_addon_openzen::score_addon_openzen() = default;
score_addon_openzen::~score_addon_openzen() = default;

std::vector<score::InterfaceBase*> score_addon_openzen::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext, FW<Device::ProtocolFactory, OpenZen::ProtocolFactory>>(
      ctx, key);
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_openzen)
