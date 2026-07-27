#include <Arduino.h>
#include <SpacePCCommon.h>

void setup() {
  Serial.begin(115200);
  spacepc::printBootStatus(SPACEPC_PROJECT_ID, SPACEPC_FIRMWARE_VERSION);
}

void loop() {
  delay(1000);
}
