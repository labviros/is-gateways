#ifndef __IS_DRIVER_PTGREY_HPP__
#define __IS_DRIVER_PTGREY_HPP__

#include <flycapture/FlyCapture2.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <is/logger.hpp>
#include <is/msgs/camera.hpp>
#include <is/msgs/common.hpp>
#include <mutex>
#include <numeric>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <string>

namespace is {
namespace driver {

using namespace std::chrono_literals;
using namespace is::msg::camera;
using namespace is::msg::common;
using namespace FlyCapture2;

struct ErrorLogger {
  ErrorLogger() {}

  ErrorLogger(Error error) {
    if (error != PGRERROR_OK) {
      is::logger()->warn("[Ptgrey][{}][{}] {}", error.GetFilename(), error.GetLine(),
                         error.GetDescription());
    }
  }
};

struct PtGrey {
  std::mutex mutex;

  GigECamera camera;
  PGRGuid* handle;

  ImageType image_type;
  PixelFormat pixel_format;
  int cv_type;
  Timestamp last_timestamp;
  cv::Mat last_frame;

  ErrorLogger error;

  enum State { STOPPED, CAPTURING };
  State state;

  PtGrey(std::string const& ip) : handle(new PGRGuid()) {
    BusManager bus;
    bus.GetCameraFromIPAddress(make_ip_address(ip), handle);
    camera.Connect(handle);
    set_packet_delay(6000);
    set_packet_size(1400);
    set_image_type(image_type::rgb);
    is::msg::common::SamplingRate rate;
    rate.rate = 1.0;
    set_sample_rate(rate);
    is::msg::camera::Resolution resolution;
    resolution.width = 1288;
    resolution.height = 728;
    set_resolution(resolution);
    set_brightness(1.367);
    Exposure exposure;
    exposure.auto_mode = true;
    set_exposure(exposure);
    Shutter shutter;
    shutter.auto_mode = true;
    set_shutter(shutter);
    Gain gain;
    gain.auto_mode = true;
    set_gain(gain);
    WhiteBalance white_balance;
    white_balance.auto_mode = true;
    set_white_balance(white_balance);
    camera.StartCapture();
    state = CAPTURING;
  }

  ~PtGrey() {
    camera.Disconnect();
    if (handle != nullptr) {
      delete handle;
    }
  }

  void start_capture() {
    std::lock_guard<std::mutex> lock(mutex);
    if (state == STOPPED) {
      if (!camera.IsConnected()) {
        camera.Connect(handle);
      }
      camera.StartCapture();
      state = CAPTURING;
      is::log::info("Starting capture");
    }
    camera.StartCapture();
  }

  void stop_capture() {
    std::lock_guard<std::mutex> lock(mutex);
    if (state == CAPTURING) {
      is::log::info("Stopping capture");
      camera.StopCapture();
      state = STOPPED;
    }
  }

  void set_sample_rate(SamplingRate sample_rate) {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(FRAME_RATE);
    error = camera.GetProperty(&property);

    PropertyInfo info(FRAME_RATE);
    error = camera.GetPropertyInfo(&info);

    float fps;
    if (sample_rate.period) {
      fps = static_cast<float>(1000.0f / *sample_rate.period);
    } else if (sample_rate.rate) {
      fps = static_cast<float>(*sample_rate.rate);
    } else {
      return;
    }

    property.absValue = std::max(info.absMin, std::min(fps, info.absMax));
    property.autoManualMode = false;

    error = camera.SetProperty(&property);
  }

  SamplingRate get_sample_rate() {
    std::lock_guard<std::mutex> lock(mutex);
    Property property(FRAME_RATE);
    error = camera.GetProperty(&property);
    SamplingRate sample_rate;
    sample_rate.rate = static_cast<double>(property.absValue);
    sample_rate.period = static_cast<unsigned int>(1000.0 / property.absValue);
    return sample_rate;
  }

  void set_resolution(Resolution resolution) {
    std::lock_guard<std::mutex> lock(mutex);
    if (resolution.width == 1288 && resolution.height == 728) {
      camera.StopCapture();
      error = camera.SetGigEImagingMode(MODE_0);
      camera.StartCapture();
    } else if (resolution.width == 644 && resolution.height == 364) {
      camera.StopCapture();
      error = camera.SetGigEImagingMode(MODE_1);
      camera.StartCapture();
    }
  }

  Resolution get_resolution() {
    std::lock_guard<std::mutex> lock(mutex);
    Mode mode;
    camera.GetGigEImagingMode(&mode);
    switch (mode) {
    case MODE_0:
      return {1288, 728};
    case MODE_1:
      return {644, 364};
    default:
      return {0, 0};
    }
  }

  void set_image_type(ImageType image_type) {
    std::lock_guard<std::mutex> lock(mutex);
    GigEImageSettings settings;
    error = camera.GetGigEImageSettings(&settings);

    boost::algorithm::to_lower(image_type.value);
    if (image_type.value == "gray") {
      pixel_format = PIXEL_FORMAT_MONO8;
      cv_type = CV_8UC1;
      settings.pixelFormat = PIXEL_FORMAT_MONO8;
    } else if (image_type.value == "rgb") {
      pixel_format = PIXEL_FORMAT_BGR;
      cv_type = CV_8UC3;
      settings.pixelFormat = PIXEL_FORMAT_RAW8;
      // settings.pixelFormat = PIXEL_FORMAT_RGB8;
    } else {
      return;
    }

    this->image_type = image_type;
    camera.StopCapture();
    error = camera.SetGigEImageSettings(&settings);
    camera.StartCapture();
  }

  ImageType get_image_type() { return image_type; }

  void set_delay(Delay delay) {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(TRIGGER_DELAY);
    error = camera.GetProperty(&property);

    PropertyInfo info(TRIGGER_DELAY);
    error = camera.GetPropertyInfo(&info);

    property.absValue = std::max(
        info.absMin, std::min(static_cast<float>(delay.milliseconds) / 1000.0f, info.absMax));
    property.onOff = true;

    error = camera.SetProperty(&property);
  }

  Delay get_delay() {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(TRIGGER_DELAY);
    error = camera.GetProperty(&property);

    Delay delay;
    delay.milliseconds = static_cast<int64_t>(1000.0 * property.absValue);
    return delay;
  }

  void update() {
    std::lock_guard<std::mutex> lock(mutex);
    if (state == CAPTURING) {
      Image buffer, image;
      error = camera.RetrieveBuffer(&buffer);
      last_timestamp = Timestamp();
      buffer.Convert(pixel_format, &image);
      cv::Mat frame(image.GetRows(), image.GetCols(), cv_type, image.GetData(),
                    image.GetDataSize() / image.GetRows());
      last_frame = frame.clone();
      buffer.ReleaseBuffer();
      image.ReleaseBuffer();
    }
  }

  cv::Mat get_last_frame() {
    std::lock_guard<std::mutex> lock(mutex);
    return last_frame;
  }

  Timestamp get_last_timestamp() {
    std::lock_guard<std::mutex> lock(mutex);
    return last_timestamp;
  }

  void set_brightness(float brightness) {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(BRIGHTNESS);
    error = camera.GetProperty(&property);

    PropertyInfo info(BRIGHTNESS);
    error = camera.GetPropertyInfo(&info);

    property.absValue = std::max(info.absMin, std::min(brightness, info.absMax));
    property.absControl = true;

    error = camera.SetProperty(&property);
  }

  float get_brightness() {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(BRIGHTNESS);
    error = camera.GetProperty(&property);
    return property.absValue;
  }

  void set_exposure(Exposure exposure) {
    std::lock_guard<std::mutex> lock(mutex);
    if (exposure.auto_mode) {
      Property property(AUTO_EXPOSURE);
      error = camera.GetProperty(&property);

      PropertyInfo info(AUTO_EXPOSURE);
      error = camera.GetPropertyInfo(&info);

      if (!exposure.auto_mode.get()) {
        property.absValue = std::max(info.absMin, std::min(exposure.value, info.absMax));
        property.absControl = true;
      }
      property.autoManualMode = exposure.auto_mode.get();

      error = camera.SetProperty(&property);
    }
  }

  Exposure get_exposure() {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(AUTO_EXPOSURE);
    error = camera.GetProperty(&property);

    Exposure exposure;
    exposure.auto_mode = property.autoManualMode;
    exposure.value = property.absValue;
    return exposure;
  }

  void set_shutter(Shutter shutter) {
    std::lock_guard<std::mutex> lock(mutex);
    if (shutter.auto_mode) {
      Property property(SHUTTER);
      error = camera.GetProperty(&property);

      PropertyInfo info(SHUTTER);
      error = camera.GetPropertyInfo(&info);

      if (!shutter.auto_mode.get()) {
        if (shutter.ms) {
          property.absValue = std::max(info.absMin, std::min(shutter.ms.get(), info.absMax));
        } else if (shutter.percent) {
          property.absValue = std::max(0.0, std::min(shutter.percent.get() / 100.0, 1.0)) *
                              (info.absMax - info.absMin);
        }
        property.absControl = true;
      }
      property.autoManualMode = shutter.auto_mode.get();

      error = camera.SetProperty(&property);
    }
  }

  Shutter get_shutter() {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(SHUTTER);
    error = camera.GetProperty(&property);

    PropertyInfo info(SHUTTER);
    error = camera.GetPropertyInfo(&info);

    Shutter shutter;
    shutter.auto_mode = property.autoManualMode;
    shutter.ms = property.absValue;
    shutter.percent = (100.0 * property.absValue) / (info.absMax - info.absMin);
    return shutter;
  }

  void set_gain(Gain gain) {
    std::lock_guard<std::mutex> lock(mutex);
    if (gain.auto_mode) {
      Property property(GAIN);
      error = camera.GetProperty(&property);

      PropertyInfo info(GAIN);
      error = camera.GetPropertyInfo(&info);

      if (!gain.auto_mode.get()) {
        if (gain.db) {
          property.absValue = std::max(info.absMin, std::min(gain.db.get(), info.absMax));
        } else if (gain.percent) {
          property.absValue = std::max(0.0, std::min(gain.percent.get() / 100.0, 1.0)) *
                              (info.absMax - info.absMin);
        }
        property.absControl = true;
      }
      property.autoManualMode = gain.auto_mode.get();

      error = camera.SetProperty(&property);
    }
  }

  Gain get_gain() {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(GAIN);
    error = camera.GetProperty(&property);

    PropertyInfo info(GAIN);
    error = camera.GetPropertyInfo(&info);

    Gain gain;
    gain.auto_mode = property.autoManualMode;
    gain.db = property.absValue;
    gain.percent = (100.0 * property.absValue) / (info.absMax - info.absMin);
    return gain;
  }

  void set_white_balance(WhiteBalance white_balance) {
    std::lock_guard<std::mutex> lock(mutex);
    if (white_balance.auto_mode) {
      Property property(WHITE_BALANCE);
      error = camera.GetProperty(&property);

      PropertyInfo info(WHITE_BALANCE);
      error = camera.GetPropertyInfo(&info);

      if (!white_balance.auto_mode.get()) {
        if (white_balance.red) {
          property.valueA = std::max(info.min, std::min(white_balance.red.get(), info.max));
        }
        if (white_balance.blue) {
          property.valueB = std::max(info.min, std::min(white_balance.blue.get(), info.max));
        }
      }
      property.autoManualMode = white_balance.auto_mode.get();

      error = camera.SetProperty(&property);
    }
  }

  WhiteBalance get_white_balance() {
    std::lock_guard<std::mutex> lock(mutex);

    Property property(WHITE_BALANCE);
    error = camera.GetProperty(&property);

    WhiteBalance white_balance;
    white_balance.auto_mode = property.autoManualMode;
    white_balance.red = property.valueA;
    white_balance.blue = property.valueB;
    return white_balance;
  }

 private:
  void set_property(unsigned int value, GigEPropertyType type) {
    GigEProperty property;
    property.propType = type;
    error = camera.GetGigEProperty(&property);
    property.value = std::max(property.min, std::min(value, property.max));
    error = camera.SetGigEProperty(&property);
  }

  void set_packet_delay(unsigned int delay) { set_property(delay, PACKET_DELAY); }

  void set_packet_size(unsigned int size) { set_property(size, PACKET_SIZE); }

  IPAddress make_ip_address(std::string const& ip) {
    std::vector<std::string> fields;
    boost::split(fields, ip, boost::is_any_of("."), boost::token_compress_on);
    auto reducer = [i = 8 * (fields.size() - 1)](auto total, auto current) mutable {
      total += (std::stoi(current) << i);
      i -= 8;
      return total;
    };
    IPAddress address = std::accumulate(std::begin(fields), std::end(fields), 0, reducer);
    return address;
  }
};

}  // ::driver
}  // ::is

#endif  // __IS_DRIVER_PTGREY_HPP__