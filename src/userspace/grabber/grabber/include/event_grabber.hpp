#pragma once

#include "apple_hid_usage_tables.hpp"
#include "constants.hpp"
#include "hid_report.hpp"
#include "human_interface_device.hpp"
#include "iokit_user_client.hpp"
#include "iokit_utility.hpp"
#include "local_datagram_client.hpp"
#include "logger.hpp"
#include "userspace_defs.h"
#include "virtual_hid_manager_user_client_method.hpp"

class event_grabber final {
public:
  event_grabber(void) : iokit_user_client_(logger::get_logger(), "org_pqrs_driver_VirtualHIDManager", kIOHIDServerConnectType),
                        console_user_client_(constants::get_console_user_socket_file_path()) {
    iokit_user_client_.start();

    manager_ = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager_) {
      return;
    }

    auto device_matching_dictionaries = iokit_utility::create_device_matching_dictionaries({
        std::make_pair(kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard),
        // std::make_pair(kHIDPage_Consumer, kHIDUsage_Csmr_ConsumerControl),
        // std::make_pair(kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse),
    });
    if (device_matching_dictionaries) {
      IOHIDManagerSetDeviceMatchingMultiple(manager_, device_matching_dictionaries);
      CFRelease(device_matching_dictionaries);

      IOHIDManagerRegisterDeviceMatchingCallback(manager_, device_matching_callback, this);
      IOHIDManagerRegisterDeviceRemovalCallback(manager_, device_removal_callback, this);

      IOHIDManagerScheduleWithRunLoop(manager_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }
  }

  ~event_grabber(void) {
    if (manager_) {
      IOHIDManagerUnscheduleFromRunLoop(manager_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
      CFRelease(manager_);
      manager_ = nullptr;
    }
  }

private:
  static void device_matching_callback(void* _Nullable context, IOReturn result, void* _Nullable sender, IOHIDDeviceRef _Nonnull device) {
    if (result != kIOReturnSuccess) {
      return;
    }

    auto self = static_cast<event_grabber*>(context);
    if (!self) {
      return;
    }

    if (!device) {
      return;
    }

    (self->hids_)[device] = std::make_unique<human_interface_device>(device);
    auto& dev = (self->hids_)[device];

    std::cout << "matching: " << std::endl
              << "  vendor_id:0x" << std::hex << dev->get_vendor_id() << std::endl
              << "  product_id:0x" << std::hex << dev->get_product_id() << std::endl
              << "  location_id:0x" << std::hex << dev->get_location_id() << std::endl
              << "  serial_number:" << dev->get_serial_number_string() << std::endl
              << "  manufacturer:" << dev->get_manufacturer() << std::endl
              << "  product:" << dev->get_product() << std::endl
              << "  transport:" << dev->get_transport() << std::endl;

    if (dev->get_serial_number_string() == "org.pqrs.driver.VirtualHIDKeyboard") {
      return;
    }

    //if (dev->get_manufacturer() != "pqrs.org") {
    if (dev->get_manufacturer() == "Apple Inc.") {
      dev->grab(queue_value_available_callback, self);
    }
  }

  static void device_removal_callback(void* _Nullable context, IOReturn result, void* _Nullable sender, IOHIDDeviceRef _Nonnull device) {
    if (result != kIOReturnSuccess) {
      return;
    }

    auto self = static_cast<event_grabber*>(context);
    if (!self) {
      return;
    }

    if (!device) {
      return;
    }

    auto it = (self->hids_).find(device);
    if (it != (self->hids_).end()) {
      auto& dev = it->second;
      if (dev) {
        std::cout << "removal vendor_id:0x" << std::hex << dev->get_vendor_id() << " product_id:0x" << std::hex << dev->get_product_id() << std::endl;
        (self->hids_).erase(it);
      }
    }
  }

  static void queue_value_available_callback(void* _Nullable context, IOReturn result, void* _Nullable sender) {
    auto self = static_cast<event_grabber*>(context);
    auto queue = static_cast<IOHIDQueueRef>(sender);

    while (true) {
      auto value = IOHIDQueueCopyNextValueWithTimeout(queue, 0.);
      if (!value) {
        break;
      }

      auto element = IOHIDValueGetElement(value);
      if (element) {
        auto usage_page = IOHIDElementGetUsagePage(element);
        auto usage = IOHIDElementGetUsage(element);

        std::cout << "element" << std::endl
                  << "  usage_page:0x" << std::hex << usage_page << std::endl
                  << "  usage:0x" << std::hex << usage << std::endl
                  << "  type:" << IOHIDElementGetType(element) << std::endl
                  << "  length:" << IOHIDValueGetLength(value) << std::endl
                  << "  integer_value:" << IOHIDValueGetIntegerValue(value) << std::endl;

        bool pressed = IOHIDValueGetIntegerValue(value);

        switch (usage_page) {
        case kHIDPage_KeyboardOrKeypad: {
          if (self) {
            self->handle_keyboard_event(usage, pressed);
          }
          break;
        }

        case kHIDPage_AppleVendorTopCase:
          if (usage == kHIDUsage_AV_TopCase_KeyboardFn) {
            IOOptionBits flags = 0;
            if (pressed) {
              flags |= NX_SECONDARYFNMASK;
            }

            std::vector<uint8_t> buffer;
            buffer.resize(1 + sizeof(flags));
            buffer[0] = KRBN_OP_TYPE_POST_MODIFIER_FLAGS;
            memcpy(&(buffer[1]), &flags, sizeof(flags));
            self->console_user_client_.send_to(buffer);
          }
          break;

        default:
          break;
        }
      }

      CFRelease(value);
    }
  }

  void handle_keyboard_event(uint32_t usage, bool pressed) {
    // ----------------------------------------
    // modify usage
    if (usage == kHIDUsage_KeyboardCapsLock) {
      usage = kHIDUsage_KeyboardDeleteOrBackspace;
    }

    // ----------------------------------------
    if (pressed) {
      pressing_key_usages_.push_back(usage);
    } else {
      pressing_key_usages_.remove(usage);
    }

    // ----------------------------------------
    hid_report::keyboard_input report;

    while (pressing_key_usages_.size() > sizeof(report.keys)) {
      pressing_key_usages_.pop_front();
    }

    int i = 0;
    for (const auto& u : pressing_key_usages_) {
      report.keys[i] = u;
      ++i;
    }

    auto kr = iokit_user_client_.call_struct_method(static_cast<uint32_t>(virtual_hid_manager_user_client_method::keyboard_input_report),
                                                    static_cast<const void*>(&report), sizeof(report),
                                                    nullptr, 0);
    if (kr != KERN_SUCCESS) {
      std::cerr << "failed to sent report: 0x" << std::hex << kr << std::dec << std::endl;
    }
  }

  iokit_user_client iokit_user_client_;
  IOHIDManagerRef _Nullable manager_;
  std::unordered_map<IOHIDDeviceRef, std::unique_ptr<human_interface_device>> hids_;
  std::list<uint32_t> pressing_key_usages_;

  local_datagram_client console_user_client_;
};
