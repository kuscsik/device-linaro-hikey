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

static int register_device(const peripheral_io_module_t* dev,
                           const peripheral_registration_cb_t* callbacks) {
  LOG(INFO) << "register_device hikey";
  (void)dev;

  // Register GPIO Pins as per 96Boards spec.
  callbacks->register_gpio_sysfs("GPIO2_0", 488);
  callbacks->register_gpio_sysfs("GPIO2_1", 489);
  callbacks->register_gpio_sysfs("GPIO2_2", 490);
  callbacks->register_gpio_sysfs("GPIO2_3", 491);
  callbacks->register_gpio_sysfs("GPIO2_4", 492);
  callbacks->register_gpio_sysfs("GPIO2_5", 493);
  callbacks->register_gpio_sysfs("GPIO2_6", 494);
  callbacks->register_gpio_sysfs("GPIO2_7", 495);

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
