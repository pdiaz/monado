// Copyright 2020-2021, N Madsen.
// Copyright 2020-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Driver interface for Bluetooth based WMR motion controllers.
 * Note: Only tested with HP Reverb (G1) controllers that are manually
 * paired to a non hmd-integrated, generic BT usb adapter.
 * @author Nis Madsen <nima_zero_one@protonmail.com>
 * @ingroup drv_wmr
 */


#include "os/os_threading.h"
#include "math/m_imu_3dof.h"
#include "util/u_logging.h"
#include "xrt/xrt_device.h"

#include "wmr_controller_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * A Bluetooth connected WMR Controller device, representing just a single controller.
 *
 * @ingroup drv_wmr
 * @implements xrt_device
 */
struct wmr_bt_controller
{
	struct xrt_device base;

	struct os_hid_device *controller_hid;
	struct os_thread_helper controller_thread;
	struct os_mutex lock;

	struct wmr_controller_message controller_message;

	struct m_imu_3dof fusion;

	struct
	{
		struct xrt_vec3 acc;
		struct xrt_vec3 gyro;
	} last;

	struct xrt_quat rot_filtered;

	enum u_logging_level ll;

	uint32_t last_ticks;
};


struct xrt_device *
wmr_bt_controller_create(struct os_hid_device *controller_hid,
                         enum xrt_device_type controller_type,
                         enum u_logging_level ll);


#ifdef __cplusplus
}
#endif