// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

#include "AppConfig.h"

class ProvisioningPortal {
 public:
  bool connectAndConfigure(
      AppConfig& config, bool forcePortal,
      std::function<void(const String&)> onPortalStarted = nullptr);
};
