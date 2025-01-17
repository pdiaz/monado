// Copyright 2022, Collabora, Ltd.
// Copyright 2022 Max Thomas
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  quest_link XRSP Pose topic packets
 * @author Max Thomas <mtinc2@gmail.com>
 * @ingroup drv_quest_link
 */

#pragma once

#include <stdlib.h>

#include "ql_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void ql_xrsp_handle_pose(struct ql_xrsp_segpkt* segpkt, struct ql_xrsp_host* host);

#ifdef __cplusplus
}
#endif