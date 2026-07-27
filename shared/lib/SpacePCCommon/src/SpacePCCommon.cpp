#include "SpacePCCommon.h"

namespace spacepc {

void printBootStatus(const char* projectId, const char* version) {
  Serial.printf(
    "{\"platform\":\"SpacePC\",\"project\":\"%s\",\"version\":\"%s\",\"status\":\"scaffold\"}\n",
    projectId,
    version
  );
}

}
