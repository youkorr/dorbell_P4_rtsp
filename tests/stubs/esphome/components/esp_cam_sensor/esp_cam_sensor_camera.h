#pragma once
#include <cstdint>
namespace esphome { namespace esp_cam_sensor {
struct SimpleBufferElement { uint8_t *data; bool allocated; uint32_t index; };
class MipiDSICamComponent {
 public:
  bool start_streaming();
  bool capture_frame();
  SimpleBufferElement *acquire_buffer();
  void release_buffer(SimpleBufferElement *e);
  bool get_current_rgb_frame(SimpleBufferElement **out, uint8_t **data, int *w, int *h);
  uint16_t get_image_width() const;
  uint16_t get_image_height() const;
};
} }
