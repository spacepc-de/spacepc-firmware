// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AppConfig.h"

class ConfigStore {
 public:
  bool load(AppConfig& config);
  bool save(const AppConfig& config);
  void clear();
};

