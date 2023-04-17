/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022-2023 Intel Corporation
 */

#ifndef __INTEL_DISPLAY_DRIVER_H__
#define __INTEL_DISPLAY_DRIVER_H__

#include <linux/types.h>

struct drm_i915_private;
struct pci_dev;

bool intel_display_driver_probe_defer(struct pci_dev *pdev);
void intel_modeset_init_hw(struct drm_i915_private *i915);
int intel_modeset_init_noirq(struct drm_i915_private *i915);
int intel_modeset_init_nogem(struct drm_i915_private *i915);
int intel_modeset_init(struct drm_i915_private *i915);
void intel_display_driver_register(struct drm_i915_private *i915);
void intel_modeset_driver_remove(struct drm_i915_private *i915);
void intel_modeset_driver_remove_noirq(struct drm_i915_private *i915);
void intel_modeset_driver_remove_nogem(struct drm_i915_private *i915);
void intel_display_driver_unregister(struct drm_i915_private *i915);

#endif /* __INTEL_DISPLAY_DRIVER_H__ */

