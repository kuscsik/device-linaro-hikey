/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <base/logging.h>
#include <hardware/hardware.h>
#include <hardware/peripheral_io.h>

static int register_device(const peripheral_io_module_t* /*dev*/,
                           const peripheral_registration_cb_t* callbacks) {
  LOG(INFO) << "register_device hikey";

  // Register GPIO Pins as per 96Boards spec:
  // Low Speed Expanstion Connector: pins 23 - 34
  callbacks->register_gpio_sysfs("A", 488); // gpio_2_0
  callbacks->register_gpio_sysfs("B", 489); // gpio_2_1
  callbacks->register_gpio_sysfs("C", 490); // gpio_2_2
  callbacks->register_gpio_sysfs("D", 491); // gpio_2_3
  callbacks->register_gpio_sysfs("E", 492); // gpio_2_4
  callbacks->register_gpio_sysfs("F", 415); // gpio_12_7
  callbacks->register_gpio_sysfs("G", 463); // gpio_6_7
  callbacks->register_gpio_sysfs("H", 495); // gpio_2_7
  callbacks->register_gpio_sysfs("I", 426); // gpio_10_2
  callbacks->register_gpio_sysfs("J", 433); // gpio_9_1
  callbacks->register_gpio_sysfs("K", 427); // gpio_10_3
  callbacks->register_gpio_sysfs("L", 434); // gpio_9_2

  // Register the SPI bus
  callbacks->register_spi_dev_bus("SPI0", 0, 0);
  // Register the I2C bus
  callbacks->register_i2c_dev_bus("I2C0", 0);
  callbacks->register_i2c_dev_bus("I2C1", 1);

  // Register sysfs leds
  callbacks->register_led_sysfs("LED1", "user_led1");
  callbacks->register_led_sysfs("LED2", "user_led2");
  callbacks->register_led_sysfs("LED3", "user_led3");
  callbacks->register_led_sysfs("LED4", "user_led4");
  callbacks->register_led_sysfs("LEDWIFI", "wifi_active");
  callbacks->register_led_sysfs("LEDBT", "bt_active");

  return 0;
}

static struct hw_module_methods_t hal_module_methods = {};

peripheral_io_module_t HAL_MODULE_INFO_SYM = {
    .common =
        {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = 0,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = PERIPHERAL_IO_HARDWARE_MODULE_ID,
            .name = "periperal IO HAL",
            .author = "The Android Open Source Project",
            .methods = &hal_module_methods,
        },
    .register_devices = register_device,
};
