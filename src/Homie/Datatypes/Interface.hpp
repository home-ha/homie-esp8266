#pragma once

#include "../Limits.hpp"
#include "./Callbacks.hpp"
#include "../../HomieNode.h"
#include "../../HomieEvent.h"

namespace HomieInternals {
  struct Interface {
    /***** User configurable data *****/
    char brand[MAX_BRAND_LENGTH];

    struct Firmware {
      char name[MAX_FIRMWARE_NAME_LENGTH];
      char version[MAX_FIRMWARE_VERSION_LENGTH];
    } firmware;

    struct LED {
      bool enable;
      uint8_t pin;
      byte on;
    } led;

    struct Reset {
      bool enable;
      bool able;
      uint8_t triggerPin;
      byte triggerState;
      uint16_t triggerTime;
      ResetFunction userFunction;
    } reset;

    HomieNode* registeredNodes[MAX_REGISTERED_NODES_COUNT];
    unsigned char registeredNodesCount;

    GlobalInputHandler globalInputHandler;
    OperationFunction setupFunction;
    OperationFunction loopFunction;
    EventHandler eventHandler;

    /***** Runtime data *****/
    bool readyToOperate;
  };
}
