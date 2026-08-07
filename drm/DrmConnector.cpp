/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <drm/drm_mode.h>
#define LOG_TAG "drmhwc"

#include "DrmConnector.h"

#include <cutils/properties.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <sstream>

#include "DrmDevice.h"
#include "compositor/DisplayInfo.h"
#include "utils/log.h"

#ifndef DRM_MODE_CONNECTOR_SPI
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DRM_MODE_CONNECTOR_SPI 19
#endif

#ifndef DRM_MODE_CONNECTOR_USB
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DRM_MODE_CONNECTOR_USB 20
#endif

namespace android {

constexpr size_t kTypesCount = 21;

auto DrmConnector::GetConnectorProperty(const char *prop_name,
                                        DrmProperty *property,
                                        bool is_optional) -> bool {
  auto err = drm_->GetProperty(GetId(), DRM_MODE_OBJECT_CONNECTOR, prop_name,
                               property);
  if (err == 0)
    return true;

  if (is_optional) {
    ALOGV("Could not get optional %s property from connector %d", prop_name,
          GetId());
  } else {
    ALOGE("Could not get %s property from connector %d", prop_name, GetId());
  }
  return false;
}

auto DrmConnector::CreateInstance(DrmDevice &dev, uint32_t connector_id,
                                  uint32_t index)
    -> std::unique_ptr<DrmConnector> {
  auto conn = MakeDrmModeConnectorUnique(*dev.GetFd(), connector_id);
  if (!conn) {
    ALOGE("Failed to get connector %d", connector_id);
    return {};
  }

  auto c = std::unique_ptr<DrmConnector>(
      new DrmConnector(std::move(conn), &dev, index));

  if (!c->Init()) {
    ALOGE("Failed to initialize connector %d", connector_id);
    return {};
  }

  return c;
}

auto DrmConnector::Init()-> bool {
  if (!GetConnectorProperty("DPMS", &dpms_property_) ||
      !GetConnectorProperty("CRTC_ID", &crtc_id_property_)) {
    return false;
  }

  UpdateEdidProperty();

  if (IsWriteback() &&
      (!GetConnectorProperty("WRITEBACK_PIXEL_FORMATS",
                             &writeback_pixel_formats_) ||
       !GetConnectorProperty("WRITEBACK_FB_ID", &writeback_fb_id_) ||
       !GetConnectorProperty("WRITEBACK_OUT_FENCE_PTR",
                             &writeback_out_fence_))) {
    return false;
  }

  if (GetConnectorProperty("Colorspace", &colorspace_property_,
                           /*is_optional=*/true)) {
    colorspace_property_.AddEnumToMap("Default", Colorspace::kDefault,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("SMPTE_170M_YCC", Colorspace::kSmpte170MYcc,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("BT709_YCC", Colorspace::kBt709Ycc,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("XVYCC_601", Colorspace::kXvycc601,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("XVYCC_709", Colorspace::kXvycc709,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("SYCC_601", Colorspace::kSycc601,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("opYCC_601", Colorspace::kOpycc601,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("opRGB", Colorspace::kOprgb,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("BT2020_CYCC", Colorspace::kBt2020Cycc,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("BT2020_RGB", Colorspace::kBt2020Rgb,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("BT2020_YCC", Colorspace::kBt2020Ycc,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("DCI-P3_RGB_D65", Colorspace::kDciP3RgbD65,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("DCI-P3_RGB_Theater", Colorspace::kDciP3RgbTheater,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("RGB_WIDE_FIXED", Colorspace::kRgbWideFixed,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("RGB_WIDE_FLOAT", Colorspace::kRgbWideFloat,
                                      colorspace_enum_map_);
    colorspace_property_.AddEnumToMap("BT601_YCC", Colorspace::kBt601Ycc,
                                      colorspace_enum_map_);
  }

  GetConnectorProperty("content type", &content_type_property_,
                       /*is_optional=*/true);

  if (GetConnectorProperty("panel orientation", &panel_orientation_,
                           /*is_optional=*/true)) {
    panel_orientation_
        .AddEnumToMapReverse("Normal",
                             PanelOrientation::kModePanelOrientationNormal,
                             panel_orientation_enum_map_);
    panel_orientation_
        .AddEnumToMapReverse("Upside Down",
                             PanelOrientation::kModePanelOrientationBottomUp,
                             panel_orientation_enum_map_);
    panel_orientation_
        .AddEnumToMapReverse("Left Side Up",
                             PanelOrientation::kModePanelOrientationLeftUp,
                             panel_orientation_enum_map_);
    panel_orientation_
        .AddEnumToMapReverse("Right Side Up",
                             PanelOrientation::kModePanelOrientationRightUp,
                             panel_orientation_enum_map_);
  }

  return true;
}

int DrmConnector::UpdateEdidProperty() {
  return GetConnectorProperty("EDID", &edid_property_, /*is_optional=*/true)
             ? 0
             : -EINVAL;
}

auto DrmConnector::GetEdidBlob() -> DrmModePropertyBlobUnique {
  auto ret = UpdateEdidProperty();
  if (ret != 0) {
    return {};
  }

  auto blob_id = GetEdidProperty().GetValue();
  if (!blob_id) {
    return {};
  }

  return MakeDrmModePropertyBlobUnique(*drm_->GetFd(), *blob_id);
}

bool DrmConnector::IsInternal() const {
  auto type = connector_->connector_type;
  return type == DRM_MODE_CONNECTOR_Unknown ||
         type == DRM_MODE_CONNECTOR_LVDS || type == DRM_MODE_CONNECTOR_eDP ||
         type == DRM_MODE_CONNECTOR_DSI || type == DRM_MODE_CONNECTOR_VIRTUAL ||
         type == DRM_MODE_CONNECTOR_DPI || type == DRM_MODE_CONNECTOR_SPI;
}

bool DrmConnector::IsExternal() const {
  auto type = connector_->connector_type;
  return type == DRM_MODE_CONNECTOR_HDMIA ||
         type == DRM_MODE_CONNECTOR_DisplayPort ||
         type == DRM_MODE_CONNECTOR_DVID || type == DRM_MODE_CONNECTOR_DVII ||
         type == DRM_MODE_CONNECTOR_VGA || type == DRM_MODE_CONNECTOR_USB;
}

bool DrmConnector::IsWriteback() const {
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
  return connector_->connector_type == DRM_MODE_CONNECTOR_WRITEBACK;
#else
  return false;
#endif
}

bool DrmConnector::IsValid() const {
  return IsInternal() || IsExternal() || IsWriteback();
}

std::string DrmConnector::GetName() const {
  constexpr std::array<const char *, kTypesCount> kNames =
      {"None",      "VGA",  "DVI-I",     "DVI-D",   "DVI-A", "Composite",
       "SVIDEO",    "LVDS", "Component", "DIN",     "DP",    "HDMI-A",
       "HDMI-B",    "TV",   "eDP",       "Virtual", "DSI",   "DPI",
       "Writeback", "SPI",  "USB"};

  if (connector_->connector_type < kTypesCount) {
    std::ostringstream name_buf;
    name_buf << kNames[connector_->connector_type] << "-"
             << connector_->connector_type_id;
    return name_buf.str();
  }

  ALOGE("Unknown type in connector %d, could not make his name", GetId());
  return "None";
}

namespace {

/* Parse one EDID 18-byte detailed timing descriptor into a KMS mode. */
bool ParseEdidDetailedTiming(const uint8_t *d, drmModeModeInfo *out) {
  const uint16_t pixel_clock_10khz = static_cast<uint16_t>(d[0] | (d[1] << 8));
  if (pixel_clock_10khz == 0) {
    return false;
  }

  const uint16_t hactive = static_cast<uint16_t>(d[2] | ((d[4] & 0xf0) << 4));
  const uint16_t hblank = static_cast<uint16_t>(d[3] | ((d[4] & 0x0f) << 8));
  const uint16_t vactive = static_cast<uint16_t>(d[5] | ((d[7] & 0xf0) << 4));
  const uint16_t vblank = static_cast<uint16_t>(d[6] | ((d[7] & 0x0f) << 8));
  const uint16_t hsync_off = static_cast<uint16_t>(d[8] |
                                                   ((d[11] & 0xc0) << 2));
  const uint16_t hsync_width = static_cast<uint16_t>(d[9] |
                                                    ((d[11] & 0x30) << 4));
  const uint16_t vsync_off = static_cast<uint16_t>(((d[10] >> 4) & 0x0f) |
                                                  ((d[11] & 0x0c) << 2));
  const uint16_t vsync_width = static_cast<uint16_t>((d[10] & 0x0f) |
                                                    ((d[11] & 0x03) << 4));

  if (hactive == 0 || vactive == 0 || hblank == 0 || vblank == 0) {
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->clock = pixel_clock_10khz * 10;
  out->hdisplay = hactive;
  out->hsync_start = hactive + hsync_off;
  out->hsync_end = hactive + hsync_off + hsync_width;
  out->htotal = hactive + hblank;
  out->vdisplay = vactive;
  out->vsync_start = vactive + vsync_off;
  out->vsync_end = vactive + vsync_off + vsync_width;
  out->vtotal = vactive + vblank;
  if (out->htotal > 0 && out->vtotal > 0) {
    out->vrefresh = static_cast<uint32_t>(
        (static_cast<uint64_t>(out->clock) * 1000) /
        (static_cast<uint64_t>(out->htotal) * out->vtotal));
  }
  const uint8_t features = d[17];
  if (features & 0x80) {
    out->flags |= DRM_MODE_FLAG_INTERLACE;
  }
  if ((features & 0x18) == 0x18) {
    out->flags |= (features & 0x04) ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
    out->flags |= (features & 0x02) ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
  }
  out->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
  snprintf(out->name, DRM_DISPLAY_MODE_LEN, "%dx%d", hactive, vactive);
  return true;
}

}  // namespace

int DrmConnector::UpdateModes() {
  char value[PROPERTY_VALUE_MAX];
  uint32_t xres = 0, yres = 0, rate = 0;
  if (property_get("debug.drm.mode.force", value, NULL)) {
    // parse <xres>x<yres>[@<refreshrate>]
    if (sscanf(value, "%dx%d@%d", &xres, &yres, &rate) != 3) {
      rate = 0;
      if (sscanf(value, "%dx%d", &xres, &yres) != 2) {
        xres = yres = 0;
      }
    }
    ALOGI_IF(xres && yres, "force mode to %dx%d@%dHz", xres, yres, rate);
  }

  auto conn = MakeDrmModeConnectorUnique(*drm_->GetFd(), GetId());
  if (!conn) {
    ALOGE("Failed to get connector %d", GetId());
    return -ENODEV;
  }
  connector_ = std::move(conn);

  modes_.clear();
  for (int i = 0; i < connector_->count_modes; ++i) {
    bool exists = false;
    for (const DrmMode &mode : modes_) {
      if (mode == connector_->modes[i]) {
        exists = true;
        break;
      }
    }

    if (!exists) {
      DrmMode m(&connector_->modes[i]);
      if (xres && yres) {
        if (m.GetRawMode().hdisplay != xres || m.GetRawMode().vdisplay != yres ||
              (rate && uint32_t(m.GetVRefresh()) != rate))
          continue;
      }
      modes_.emplace_back(&connector_->modes[i]);
      ALOGD("add new mode %dx%d@%.1f for connector %d", m.GetRawMode().hdisplay, m.GetRawMode().vdisplay, m.GetVRefresh(), GetId());
    }
  }

  /* Intel xe + USB-C/DP often exposes EDID while count_modes stays 0. Without a
   * userspace mode, our hotplug defer path never attaches the display. */
  if (IsConnected() && IsExternal() && modes_.empty()) {
    auto edid_blob = GetEdidBlob();
    if (edid_blob && edid_blob->length >= 128) {
      const auto *edid = static_cast<const uint8_t *>(edid_blob->data);
      constexpr int kDtdOffsets[] = {54, 72, 90, 108};
      for (int off : kDtdOffsets) {
        drmModeModeInfo mi{};
        if (!ParseEdidDetailedTiming(edid + off, &mi)) {
          continue;
        }
        if (xres && yres) {
          if (mi.hdisplay != xres || mi.vdisplay != yres ||
              (rate && mi.vrefresh != rate)) {
            continue;
          }
        }
        modes_.emplace_back(&mi);
        ALOGW("Synthesized EDID mode %dx%d@%u for %s (KMS reported 0 modes)",
              mi.hdisplay, mi.vdisplay, mi.vrefresh, GetName().c_str());
        /* Prefer the first detailed timing (EDID preferred mode). */
        break;
      }
    }
  }

  if (IsConnected() && IsExternal() && modes_.empty()) {
    ALOGW("Connector %s is connected but KMS reported 0 modes",
          GetName().c_str());
  }

  return 0;
}

bool DrmConnector::IsLinkStatusGood() {
  if (GetConnectorProperty("link-status", &link_status_property_, false)) {
    auto link_status_property_value = link_status_property_.GetValue();
    if (link_status_property_value &&
        (link_status_property_value == DRM_MODE_LINK_STATUS_BAD))
      return false;
  }

  return true;
}

std::optional<PanelOrientation> DrmConnector::GetPanelOrientation() {
  if (!panel_orientation_.GetValue().has_value()) {
    ALOGW("No panel orientation property available.");
    return {};
  }

  /* The value_or(0) satisfies the compiler warning. However,
   * panel_orientation_.GetValue() is guaranteed to have a value since we check
   * has_value() and return early otherwise.
   */
  uint64_t panel_orientation_value = panel_orientation_.GetValue().value_or(0);

  if (panel_orientation_enum_map_.count(panel_orientation_value) == 1) {
    return panel_orientation_enum_map_[panel_orientation_value];
  }

  ALOGE("Unknown panel orientation: panel_orientation = %" PRIu64,
        panel_orientation_value);
  return {};
}

}  // namespace android
