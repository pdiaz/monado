// Copyright 2022, Collabora, Ltd.
// Copyright 2022, Max Thomas
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  All types, structs and enums associated with Quest Link and XRSP
 * @author Max Thomas <mtinc2@gmail.com>
 * @ingroup drv_quest_link
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "os/os_threading.h"
#include "util/u_logging.h"
#include "util/u_distortion_mesh.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_frame.h"
#include "xrt/xrt_frameserver.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_tracking.h"
#include "math/m_filter_one_euro.h"

typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;
struct ql_hmd;
struct ql_controller;
struct ql_hands;
struct ql_camera;

typedef struct ql_xrsp_host ql_xrsp_host;
typedef struct ql_xrsp_segpkt ql_xrsp_segpkt;
typedef struct ql_xrsp_ipc_segpkt ql_xrsp_ipc_segpkt;

typedef void (*ql_xrsp_segpkt_handler_t)(struct ql_xrsp_segpkt* segpkt, struct ql_xrsp_host* host);
typedef void (*ql_xrsp_ipc_segpkt_handler_t)(struct ql_xrsp_ipc_segpkt* segpkt, struct ql_xrsp_host* host);

typedef struct ql_xrsp_segpkt
{
    int state;
    int type_idx;
    int reading_idx;

    int num_segs;
    uint8_t* segs[3];
    size_t segs_valid[3];
    size_t segs_expected[3];
    size_t segs_max[3];

    ql_xrsp_segpkt_handler_t handler;
} ql_xrsp_segpkt;

typedef struct ql_xrsp_ipc_segpkt
{
    int state;
    int type_idx;
    int reading_idx;

    int num_segs;
    uint8_t* segs[2];
    size_t segs_valid[2];
    size_t segs_expected[2];
    size_t segs_max[2];

    uint32_t cmd_id;
    uint32_t next_size;
    uint32_t client_id;
    uint32_t unk;

    ql_xrsp_ipc_segpkt_handler_t handler;
} ql_xrsp_ipc_segpkt;

typedef struct ql_xrsp_hostinfo_capnp_payload
{
    uint32_t unk_8;
    uint32_t len_u64s;
} ql_xrsp_hostinfo_capnp_payload;

typedef struct ql_xrsp_echo_payload
{
    int64_t org;
    int64_t recv;
    int64_t xmt;
    int64_t offset;
} ql_xrsp_echo_payload;

typedef struct ql_xrsp_hostinfo_pkt
{
    uint8_t* payload;
    uint32_t payload_size;

    uint8_t message_type;
    uint16_t result;
    uint32_t stream_size;

    uint32_t unk_4;

    int64_t recv_ns;
} ql_xrsp_hostinfo_pkt;

typedef struct ql_xrsp_topic_pkt
{
    bool has_alignment_padding;
    bool packet_version_is_internal;
    uint8_t packet_version_number;
    uint8_t topic;

    uint16_t num_words;
    uint16_t sequence_num;

    uint8_t* payload;
    uint32_t payload_size;
    uint32_t payload_valid;
    uint32_t remainder_offs;
    int32_t missing_bytes;

    int64_t recv_ns;
} ql_xrsp_topic_pkt;

typedef struct ql_xrsp_host ql_xrsp_host;

#define MAX_TRACKED_DEVICES 2

#define QL_MESH_NONE (1)
#define QL_MESH_FOVEATED (1002)

#define QL_SWAPCHAIN_DEPTH (3)
#define QL_NUM_SLICES (1)
#define QL_IDX_SLICE(_slice_idx, _frame_idx) ((_slice_idx*QL_SWAPCHAIN_DEPTH)+_frame_idx)

typedef struct ql_xrsp_host
{
    struct ql_system* sys;

    // Packet processing threads
    struct os_thread_helper read_thread;
    struct os_thread_helper write_thread;

    libusb_context *ctx;
    libusb_device_handle *dev;

    bool usb_valid;
    bool usb_slow_cable;
    int usb_speed;
    int if_num;
    uint16_t vid;
    uint16_t pid;
    uint8_t ep_out;
    uint8_t ep_in;

    uint32_t client_id;
    uint32_t session_idx;

    // Parsing state
    bool have_working_pkt;
    ql_xrsp_topic_pkt working_pkt;

    uint16_t increment;
    int pairing_state;
    int64_t start_ns;

    // Echo state
    int echo_idx;
    int64_t ns_offset;
    int64_t ns_offset_from_target;

    int64_t echo_req_sent_ns;
    int64_t echo_req_recv_ns;
    int64_t echo_resp_sent_ns;
    int64_t echo_resp_recv_ns;
    int64_t last_xmt;

    int num_slices;
    int64_t frame_sent_ns;
    int64_t paired_ns;
    int64_t last_read_ns;
    int64_t add_test;
    int64_t last_echo_sent_ns;

    struct os_mutex usb_mutex;
    struct os_mutex pose_mutex;

    bool ready_to_send_frames;
    bool sent_first_frame;
    int frame_idx;

    struct os_mutex stream_mutex[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    bool needs_flush[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int stream_write_idx;
    int stream_read_idx;

    uint8_t* csd_stream[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    uint8_t* idr_stream[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];

    size_t csd_stream_len[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    size_t idr_stream_len[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int64_t stream_started_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    struct xrt_pose stream_poses[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int64_t stream_pose_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];

    int64_t encode_started_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int64_t encode_done_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int64_t encode_duration_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int64_t tx_started_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int64_t tx_done_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];
    int64_t tx_duration_ns[QL_SWAPCHAIN_DEPTH*QL_NUM_SLICES];

    struct ql_xrsp_segpkt pose_ctx;
    struct ql_xrsp_ipc_segpkt ipc_ctx;

    bool runtime_connected;
    bool bodyapi_connected;
    bool eyetrack_connected;
    bool shell_connected;
    bool sent_mesh;
    bool is_inactive;

    void (*start_encode)(struct ql_xrsp_host* host,  int64_t target_ns, int index, int slice_idx);
    void (*send_csd)(struct ql_xrsp_host* host, const uint8_t* data, size_t len, int index, int slice_idx);
    void (*send_idr)(struct ql_xrsp_host* host, const uint8_t* data, size_t len, int index, int slice_idx);
    void (*flush_stream)(struct ql_xrsp_host* host, int64_t target_ns, int index, int slice_idx);
} ql_xrsp_host;

struct ql_controller
{
    struct xrt_device base;

    struct xrt_pose pose;
    struct xrt_vec3 center;

    struct xrt_vec3 vel;
    struct xrt_vec3 acc;
    struct xrt_vec3 angvel;
    struct xrt_vec3 angacc;

    struct xrt_vec3 pose_add;

    int64_t pose_ns;
    double created_ns;

    uint8_t features;
    uint8_t battery;
    uint32_t feat_2;

    uint32_t buttons;
    uint32_t capacitance;
    float joystick_x;
    float joystick_y;
    float grip_z;
    float trigger_z;
    float stylus_pressure;

    float last_simple_haptic;

    struct os_mutex pose_mutex;

    struct ql_system *sys;
};

typedef struct ovr_pose_f
{
    struct xrt_quat orient;
    struct xrt_vec3 pos;
} ovr_pose_f;

typedef struct ovr_capsule
{
    uint32_t idx;
    struct xrt_vec3 pos1;
    struct xrt_vec3 pos2;
} ovr_capsule;

enum ovr_hand_joint
{
    OVR_HAND_JOINT_WRIST = 0,
    OVR_HAND_JOINT_FOREARM = 1,

    OVR_HAND_JOINT_THUMB_TRAPEZIUM = 2, // extra
    OVR_HAND_JOINT_THUMB_METACARPAL = 3,
    OVR_HAND_JOINT_THUMB_PROXIMAL = 4,
    OVR_HAND_JOINT_THUMB_DISTAL = 5,

    // missing: OVR_HAND_JOINT_INDEX_METACARPAL
    OVR_HAND_JOINT_INDEX_PROXIMAL = 6,
    OVR_HAND_JOINT_INDEX_INTERMEDIATE = 7,
    OVR_HAND_JOINT_INDEX_DISTAL = 8,

    // missing: OVR_HAND_JOINT_MIDDLE_METACARPAL
    OVR_HAND_JOINT_MIDDLE_PROXIMAL = 9,
    OVR_HAND_JOINT_MIDDLE_INTERMEDIATE = 10,
    OVR_HAND_JOINT_MIDDLE_DISTAL = 11,
    
    // missing: OVR_HAND_JOINT_RING_METACARPAL
    OVR_HAND_JOINT_RING_PROXIMAL = 12,
    OVR_HAND_JOINT_RING_INTERMEDIATE = 13,
    OVR_HAND_JOINT_RING_DISTAL = 14,
    
    OVR_HAND_JOINT_LITTLE_METACARPAL = 15,
    OVR_HAND_JOINT_LITTLE_PROXIMAL = 16,
    OVR_HAND_JOINT_LITTLE_INTERMEDIATE = 17,
    OVR_HAND_JOINT_LITTLE_DISTAL = 18,
    
    OVR_HAND_JOINT_THUMB_TIP = 19,
    OVR_HAND_JOINT_INDEX_TIP = 20,
    OVR_HAND_JOINT_MIDDLE_TIP = 21,
    OVR_HAND_JOINT_RING_TIP = 22,
    OVR_HAND_JOINT_LITTLE_TIP = 23,

    OVR_HAND_JOINT_MAX_ENUM = 0x7FFFFFFF
};

// Physical buttons
enum ovr_touch_btn
{
    OVR_TOUCH_BTN_A       = 0x00000001,
    OVR_TOUCH_BTN_B       = 0x00000002,
    OVR_TOUCH_BTN_STICK_R = 0x00000004,
    OVR_TOUCH_BTN_8       = 0x00000008,
    OVR_TOUCH_BTN_10      = 0x00000010,
    OVR_TOUCH_BTN_20      = 0x00000020,
    OVR_TOUCH_BTN_40      = 0x00000040,
    OVR_TOUCH_BTN_80      = 0x00000080,

    OVR_TOUCH_BTN_X       = 0x00000100,
    OVR_TOUCH_BTN_Y       = 0x00000200,
    OVR_TOUCH_BTN_STICK_L = 0x00000400,
    OVR_TOUCH_BTN_800     = 0x00000800,
    OVR_TOUCH_BTN_1000    = 0x00001000,
    OVR_TOUCH_BTN_2000    = 0x00002000,
    OVR_TOUCH_BTN_4000    = 0x00004000,
    OVR_TOUCH_BTN_8000    = 0x00008000,

    OVR_TOUCH_BTN_10000    = 0x00010000,
    OVR_TOUCH_BTN_20000    = 0x00020000,
    OVR_TOUCH_BTN_40000    = 0x00040000,
    OVR_TOUCH_BTN_80000    = 0x00080000,

    OVR_TOUCH_BTN_100000   = 0x00100000,
    OVR_TOUCH_BTN_200000   = 0x00200000,
    OVR_TOUCH_BTN_400000   = 0x00400000,
    OVR_TOUCH_BTN_800000   = 0x00800000,

    OVR_TOUCH_BTN_SYSTEM   = 0x01000000,
    OVR_TOUCH_BTN_2000000  = 0x02000000,
    OVR_TOUCH_BTN_4000000  = 0x04000000,
    OVR_TOUCH_BTN_8000000  = 0x08000000,

    OVR_TOUCH_BTN_10000000 = 0x10000000,
    OVR_TOUCH_BTN_20000000 = 0x20000000,
    OVR_TOUCH_BTN_MENU     = 0x40000000,
    OVR_TOUCH_BTN_STICKS   = 0x80000000,
};

// Capacitive sensors
enum ovr_touch_cap
{
    OVR_TOUCH_CAP_A_X       = 0x00000001,
    OVR_TOUCH_CAP_B_Y       = 0x00000002,
    OVR_TOUCH_CAP_STICK     = 0x00000004,
    OVR_TOUCH_CAP_TRIGGER   = 0x00000008,
    OVR_TOUCH_CAP_THUMB_NEAR = 0x00000010,
    OVR_TOUCH_CAP_POINTING  = 0x00000020,
    OVR_TOUCH_CAP_TOUCHPAD  = 0x00000040,
    OVR_TOUCH_CAP_80        = 0x00000080,

    OVR_TOUCH_CAP_100       = 0x00000100,
    OVR_TOUCH_CAP_200       = 0x00000200,
    OVR_TOUCH_CAP_400       = 0x00000400,
    OVR_TOUCH_CAP_800       = 0x00000800,
    OVR_TOUCH_CAP_1000      = 0x00001000,
    OVR_TOUCH_CAP_2000      = 0x00002000,
    OVR_TOUCH_CAP_4000      = 0x00004000,
    OVR_TOUCH_CAP_8000      = 0x00008000,

    OVR_TOUCH_CAP_10000     = 0x00010000,
    OVR_TOUCH_CAP_20000     = 0x00020000,
    OVR_TOUCH_CAP_40000     = 0x00040000,
    OVR_TOUCH_CAP_80000     = 0x00080000,

    OVR_TOUCH_CAP_100000    = 0x00100000,
    OVR_TOUCH_CAP_200000    = 0x00200000,
    OVR_TOUCH_CAP_400000    = 0x00400000,
    OVR_TOUCH_CAP_800000    = 0x00800000,

    OVR_TOUCH_CAP_1000000   = 0x01000000,
    OVR_TOUCH_CAP_2000000   = 0x02000000,
    OVR_TOUCH_CAP_4000000   = 0x04000000,
    OVR_TOUCH_CAP_8000000   = 0x08000000,

    OVR_TOUCH_CAP_10000000  = 0x10000000,
    OVR_TOUCH_CAP_20000000  = 0x20000000,
    OVR_TOUCH_CAP_40000000  = 0x40000000,
    OVR_TOUCH_CAP_80000000  = 0x80000000,
};

enum ovr_haptic_target
{
    OVR_HAPTIC_LEFT = 1,
    OVR_HAPTIC_RIGHT = 2,
    OVR_HAPTIC_GAMEPAD = 3,
};
typedef int ovr_haptic_target_t;

enum ovr_haptic_type
{
    OVR_HAPTIC_SIMPLE = 0,
    OVR_HAPTIC_BUFFERED = 1,
};

// Quest Pro Left: 00036100, 0035f00?
// Quest Pro Right: 00035e01, 0035c01?
enum ovr_touch_feature
{
    OVR_TOUCH_FEAT_RIGHT = 0x00000001,
};

enum ovr_face_expression
{
    OVR_EXPRESSION_BROW_LOWERER_L         = 0,
    OVR_EXPRESSION_BROW_LOWERER_R         = 1,
    OVR_EXPRESSION_CHEEK_PUFF_L           = 2,
    OVR_EXPRESSION_CHEEK_PUFF_R           = 3,
    OVR_EXPRESSION_CHEEK_RAISER_L         = 4,
    OVR_EXPRESSION_CHEEK_RAISER_R         = 5,
    OVR_EXPRESSION_CHEEK_SUCK_L           = 6,
    OVR_EXPRESSION_CHEEK_SUCK_R           = 7,
    OVR_EXPRESSION_CHIN_RAISER_B          = 8,
    OVR_EXPRESSION_CHIN_RAISER_T          = 9,
    OVR_EXPRESSION_DIMPLER_L              = 10,
    OVR_EXPRESSION_DIMPLER_R              = 11,
    OVR_EXPRESSION_EYES_CLOSED_L          = 12,
    OVR_EXPRESSION_EYES_CLOSED_R          = 13,
    OVR_EXPRESSION_EYES_LOOK_DOWN_L       = 14,
    OVR_EXPRESSION_EYES_LOOK_DOWN_R       = 15,
    OVR_EXPRESSION_EYES_LOOK_LEFT_L       = 16,
    OVR_EXPRESSION_EYES_LOOK_LEFT_R       = 17,
    OVR_EXPRESSION_EYES_LOOK_RIGHT_L      = 18,
    OVR_EXPRESSION_EYES_LOOK_RIGHT_R      = 19,
    OVR_EXPRESSION_EYES_LOOK_UP_L         = 20,
    OVR_EXPRESSION_EYES_LOOK_UP_R         = 21,
    OVR_EXPRESSION_INNER_BROW_RAISER_L    = 22,
    OVR_EXPRESSION_INNER_BROW_RAISER_R    = 23,
    OVR_EXPRESSION_JAW_DROP               = 24,
    OVR_EXPRESSION_JAW_SIDEWAYS_LEFT      = 25,
    OVR_EXPRESSION_JAW_SIDEWAYS_RIGHT     = 26,
    OVR_EXPRESSION_JAW_THRUST             = 27,
    OVR_EXPRESSION_LID_TIGHTENER_L        = 28,
    OVR_EXPRESSION_LID_TIGHTENER_R        = 29,
    OVR_EXPRESSION_LIP_CORNER_DEPRESSOR_L = 30,
    OVR_EXPRESSION_LIP_CORNER_DEPRESSOR_R = 31,
    OVR_EXPRESSION_LIP_CORNER_PULLER_L    = 32,
    OVR_EXPRESSION_LIP_CORNER_PULLER_R    = 33,
    OVR_EXPRESSION_LIP_FUNNELER_LB        = 34,
    OVR_EXPRESSION_LIP_FUNNELER_LT        = 35,
    OVR_EXPRESSION_LIP_FUNNELER_RB        = 36,
    OVR_EXPRESSION_LIP_FUNNELER_RT        = 37,
    OVR_EXPRESSION_LIP_PRESSOR_L          = 38,
    OVR_EXPRESSION_LIP_PRESSOR_R          = 39,
    OVR_EXPRESSION_LIP_PUCKER_L           = 40,
    OVR_EXPRESSION_LIP_PUCKER_R           = 41,
    OVR_EXPRESSION_LIP_STRETCHER_L        = 42,
    OVR_EXPRESSION_LIP_STRETCHER_R        = 43,
    OVR_EXPRESSION_LIP_SUCK_LB            = 44,
    OVR_EXPRESSION_LIP_SUCK_LT            = 45,
    OVR_EXPRESSION_LIP_SUCK_RB            = 46,
    OVR_EXPRESSION_LIP_SUCK_RT            = 47,
    OVR_EXPRESSION_LIP_TIGHTENER_L        = 48,
    OVR_EXPRESSION_LIP_TIGHTENER_R        = 49,
    OVR_EXPRESSION_LIPS_TOWARD            = 50,
    OVR_EXPRESSION_LOWER_LIP_DEPRESSOR_L  = 51,
    OVR_EXPRESSION_LOWER_LIP_DEPRESSOR_R  = 52,
    OVR_EXPRESSION_MOUTH_LEFT             = 53,
    OVR_EXPRESSION_MOUTH_RIGHT            = 54,
    OVR_EXPRESSION_NOSE_WRINKLER_L        = 55,
    OVR_EXPRESSION_NOSE_WRINKLER_R        = 56,
    OVR_EXPRESSION_OUTER_BROW_RAISER_L    = 57,
    OVR_EXPRESSION_OUTER_BROW_RAISER_R    = 58,
    OVR_EXPRESSION_UPPER_LID_RAISER_L     = 59,
    OVR_EXPRESSION_UPPER_LID_RAISER_R     = 60,
    OVR_EXPRESSION_UPPER_LIP_RAISER_L     = 61,
    OVR_EXPRESSION_UPPER_LIP_RAISER_R     = 62,
    OVR_EXPRESSION_MAX                    = 63,
};

struct ql_hands
{
    struct xrt_device base;

    struct xrt_pose poses[2];

    struct ovr_pose_f bones_last[24*2];
    struct ovr_pose_f bones_last_raw[24*2];
    int16_t bone_parent_idx[24*2];

    int64_t pose_ns;
    double created_ns;

    struct ql_system *sys;
};

struct ql_foveation_parameter_item
{
    double center;
    double scale;
    double a;
    double b;
};

struct ql_foveation_parameter
{
    struct ql_foveation_parameter_item x;
    struct ql_foveation_parameter_item y;
};

struct ql_hmd
{
    struct xrt_device base;

    struct xrt_pose pose;
    struct xrt_vec3 center;

    struct xrt_vec3 vel;
    struct xrt_vec3 acc;
    struct xrt_vec3 angvel;
    struct xrt_vec3 angacc;

    struct m_filter_euro_quat eye_l_oe;
    struct m_filter_euro_quat eye_r_oe;

    int64_t pose_ns;
    double created_ns;

    struct ql_system *sys;

    /* Pose tracker provided by the system */
    struct ql_tracker *tracker;

    struct ql_foveation_parameter foveation_parameters[2];

    int32_t encode_width;
    int32_t encode_height;
    float fps;

    int quest_vtx_count;
    int quest_index_count;
    float* quest_vertices;
    int16_t* quest_indices;

    /* Temporary distortion values for mesh calc */
    struct u_panotools_values distortion_vals[2];
    float ipd_meters;
    float fov_angle_left;
    int device_type;
};

typedef struct ql_system
{
    struct xrt_tracking_origin base;
    struct xrt_reference ref;

    ql_xrsp_host xrsp_host;

    /* Device lock protects device access */
    struct os_mutex dev_mutex;

    /* HMD device */
    struct ql_hmd *hmd;

    /* Controller devices */
    struct ql_controller *controllers[MAX_TRACKED_DEVICES];

    struct ql_hands *hands;
} ql_system;

void ql_xrsp_host_init();

#ifdef __cplusplus
}
#endif
