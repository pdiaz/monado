// Copyright 2022, Collabora, Ltd.
// Copyright 2022 Max Thomas
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to quest_link XRSP protocol.
 * @author Max Thomas <mtinc2@gmail.com>
 * @ingroup drv_quest_link
 */

#include <libusb.h>
#include <stdio.h>

#include "util/u_trace_marker.h"
#include "math/m_api.h"
#include "math/m_vec3.h"

#include "os/os_time.h"

#include "ql_system.h"
#include "ql_hmd.h"
#include "ql_xrsp.h"
#include "ql_xrsp_hostinfo.h"
#include "ql_xrsp_topic.h"
#include "ql_xrsp_types.h"
#include "ql_utils.h"
#include "ql_xrsp_pose.h"
#include "ql_xrsp_ipc.h"
#include "ql_xrsp_hands.h"
#include "ql_xrsp_logging.h"
#include "ql_xrsp_segmented_pkt.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "protos/HostInfo.capnp.h"
#include "protos/Slice.capnp.h"
#include "protos/RuntimeIPC.capnp.h"

static void *
ql_xrsp_read_thread(void *ptr);
static void *
ql_xrsp_write_thread(void *ptr);
static void xrsp_reset_echo(struct ql_xrsp_host *host);
static bool xrsp_read_usb(struct ql_xrsp_host *host);
static void xrsp_send_usb(struct ql_xrsp_host *host, const uint8_t* data, int32_t data_size);
static void xrsp_send_to_topic(struct ql_xrsp_host *host, uint8_t topic, const uint8_t* data, int32_t data_size);
static void xrsp_send_to_topic_raw(struct ql_xrsp_host *host, uint8_t topic, const uint8_t* data, int32_t data_size);

static void xrsp_flush_stream(struct ql_xrsp_host *host, int64_t target_ns);
static void xrsp_send_csd(struct ql_xrsp_host *host, const uint8_t* data, size_t data_len);
static void xrsp_send_idr(struct ql_xrsp_host *host, const uint8_t* data, size_t data_len);
static void xrsp_send_video(struct ql_xrsp_host *host, int slice_idx, int frame_idx, const uint8_t* csd_dat, size_t csd_len,
                            const uint8_t* video_dat, size_t video_len, int blit_y_pos);
int ql_xrsp_usb_init(struct ql_xrsp_host* host, bool do_reset);

static void xrsp_send_ripc_cmd(struct ql_xrsp_host* host, uint32_t cmd_idx, uint32_t client_id, uint32_t unk, const uint8_t* data, int32_t data_size, const uint8_t* extra_data, int32_t extra_data_size);

int ql_xrsp_host_create(struct ql_xrsp_host* host, uint16_t vid, uint16_t pid, int if_num)
{
    int ret;

    *host = (struct ql_xrsp_host){0};
    host->if_num = if_num;
    host->vid = vid;
    host->pid = pid;

    host->num_slices = 1;

    host->ready_to_send_frames = false;
    host->stream_read_idx = 0;
    host->stream_write_idx = 0;
    for (int i = 0; i < 3; i++)
    {
        host->csd_stream[i] = (uint8_t*)malloc(0x1000000);
        host->idr_stream[i] = (uint8_t*)malloc(0x1000000);
        host->csd_stream_len[i] = 0;
        host->idr_stream_len[i] = 0;

        ret = os_mutex_init(&host->stream_mutex[i]);
        if (ret != 0) {
            QUEST_LINK_ERROR("Failed to init usb mutex");
            goto cleanup;
        }
    }
    
    host->frame_idx = 0;
    ret = os_mutex_init(&host->usb_mutex);
    if (ret != 0) {
        QUEST_LINK_ERROR("Failed to init usb mutex");
        goto cleanup;
    }

    ret = os_mutex_init(&host->pose_mutex);
    if (ret != 0) {
        QUEST_LINK_ERROR("Failed to init pose mutex");
        goto cleanup;
    }
    

    //
    // Thread and other state.
    ret = os_thread_helper_init(&host->read_thread);
    if (ret != 0) {
        QUEST_LINK_ERROR("Failed to init packet read processing thread");
        goto cleanup;
    }

    //
    // Thread and other state.
    ret = os_thread_helper_init(&host->write_thread);
    if (ret != 0) {
        QUEST_LINK_ERROR("Failed to init packet write processing thread");
        goto cleanup;
    }

    host->dev = NULL;

    ret = libusb_init(&host->ctx);
    if (ret < 0) {
        QUEST_LINK_ERROR("Failed libusb_init");
        goto cleanup;
    }

    ret = ql_xrsp_usb_init(host, false);
    if (ret != 0) {
        goto cleanup;
    }

    //printf("Endpoints %x %x\n", host->ep_out, host->ep_in);

    host->pairing_state = PAIRINGSTATE_WAIT_FIRST;
    host->start_ns = os_monotonic_get_ns();
    host->paired_ns = os_monotonic_get_ns()*2;
    host->last_read_ns = 0;
    xrsp_reset_echo(host);

    host->send_csd = xrsp_send_csd;
    host->send_idr = xrsp_send_idr;
    host->flush_stream = xrsp_flush_stream;
    host->client_id = 0x4a60dcca;

    // Start the packet reading thread
    ret = os_thread_helper_start(&host->read_thread, ql_xrsp_read_thread, host);
    if (ret != 0) {
        QUEST_LINK_ERROR("Failed to start packet processing thread");
        goto cleanup;
    }

    // Start the packet reading thread
    ret = os_thread_helper_start(&host->write_thread, ql_xrsp_write_thread, host);
    if (ret != 0) {
        QUEST_LINK_ERROR("Failed to start packet processing thread");
        goto cleanup;
    }

    return 0;
cleanup:
    return -1;
}

int ql_xrsp_usb_init(struct ql_xrsp_host* host, bool do_reset)
{
    int ret;
    const struct libusb_interface_descriptor* pIf = NULL;
    struct libusb_config_descriptor *config = NULL;
    libusb_device *usb_dev = NULL;

    QUEST_LINK_INFO("(Re)initializing Quest Link USB device...");

    os_mutex_lock(&host->usb_mutex);

    if (host->dev) {
        libusb_close(host->dev);
    }

    host->usb_speed = LIBUSB_SPEED_LOW;
    host->usb_valid = false;
    host->pairing_state = PAIRINGSTATE_WAIT_FIRST;

    host->dev = libusb_open_device_with_vid_pid(host->ctx, host->vid, host->pid);
    if (host->dev == NULL) {
        QUEST_LINK_ERROR("Failed initial libusb_open_device_with_vid_pid");
        QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(ret));
        goto cleanup;
    }

#if 1
    if (do_reset) {
        printf("Reset?\n");
        ret = libusb_reset_device(host->dev);
        if (ret == LIBUSB_ERROR_NOT_FOUND) {
            // We're reconnecting anyhow.
            QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(ret));
            QUEST_LINK_INFO("Device needs reconnect...");
        }
        else if (ret != LIBUSB_SUCCESS) {
            QUEST_LINK_ERROR("Failed libusb_reset_device");
            QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(ret));
            goto cleanup;
        }
        else {
            libusb_close(host->dev);
        }

        

        printf("Reset done?\n");

        for (int i = 0; i < 10; i++)
        {
            // Re-initialize the device
            host->dev = libusb_open_device_with_vid_pid(host->ctx, host->vid, host->pid);
            if (host->dev) break;

            os_nanosleep(U_TIME_1MS_IN_NS * 500);
        }

        if (host->dev == NULL) {
            QUEST_LINK_ERROR("Failed post-reset libusb_open_device_with_vid_pid");
            QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(ret));
            goto cleanup;
        }
    }
    
#endif

    ret = libusb_claim_interface(host->dev, host->if_num);
    if (ret < 0) {
        QUEST_LINK_ERROR("Failed libusb_claim_interface");
        QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(ret));

        // Reset, there's probably something weird.
        libusb_reset_device(host->dev);
        goto cleanup;
    }

    //libusb_set_interface_alt_setting(host->dev, host->if_num, 1);

    usb_dev = libusb_get_device(host->dev);
    ret = libusb_get_active_config_descriptor(usb_dev, &config);
    if (ret < 0 || !config) {
        QUEST_LINK_ERROR("Failed libusb_get_active_config_descriptor");
        QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(ret));
        goto cleanup;
    }

    
    for (uint8_t i = 0; i < config->bNumInterfaces; i++) {
        if (pIf) break;
        const struct libusb_interface* pIfAlts = &config->interface[i];
        for (uint8_t j = 0; j < pIfAlts->num_altsetting; j++) {
            const struct libusb_interface_descriptor* pIfIt = &pIfAlts->altsetting[j];
            if (pIfIt->bInterfaceNumber == host->if_num) {
                pIf = pIfIt;
                break;
            }
        }
    }

    host->ep_out = 0;
    host->ep_in = 0;
    for (uint8_t i = 0; i < pIf->bNumEndpoints; i++) {
        const struct libusb_endpoint_descriptor* pEp = &pIf->endpoint[i];

        if (!host->ep_out && !(pEp->bEndpointAddress & LIBUSB_ENDPOINT_IN)) {
            host->ep_out = pEp->bEndpointAddress;
        }
        else if (!host->ep_in && (pEp->bEndpointAddress & LIBUSB_ENDPOINT_IN)) {
            host->ep_in = pEp->bEndpointAddress;
        }
    }

    host->usb_slow_cable = false;
    host->usb_speed = libusb_get_device_speed(usb_dev);
    switch (host->usb_speed)
    {
    case LIBUSB_SPEED_LOW:
        host->usb_slow_cable = true;
        QUEST_LINK_ERROR("Headset is operating at 1.5Mbit/s");
        break;
    case LIBUSB_SPEED_FULL:
        host->usb_slow_cable = true;
        QUEST_LINK_ERROR("Headset is operating at 12Mbit/s");
        break;
    case LIBUSB_SPEED_HIGH:
        host->usb_slow_cable = true;
        QUEST_LINK_ERROR("Headset is operating at 480Mbit/s");
        break;
    case LIBUSB_SPEED_SUPER:
        QUEST_LINK_INFO("Headset is operating at 5000Mbit/s");
        break;
    case LIBUSB_SPEED_SUPER_PLUS:
        QUEST_LINK_INFO("Headset is operating at 10000Mbit/s");
        break;
    default:
        host->usb_slow_cable = true;
        QUEST_LINK_ERROR("libusb_get_device_speed returned unknown value!");
        break;
    }

    libusb_clear_halt(host->dev, host->ep_in);
    libusb_clear_halt(host->dev, host->ep_out);
    libusb_clear_halt(host->dev, host->ep_in);
    libusb_clear_halt(host->dev, host->ep_out);

    host->usb_valid = true;

    os_mutex_unlock(&host->usb_mutex);
    return 0;

cleanup:
    os_mutex_unlock(&host->usb_mutex);
    return -1;
}

void ql_xrsp_host_destroy(struct ql_xrsp_host* host)
{
    libusb_release_interface(host->dev, host->if_num);
    libusb_close(host->dev);

    os_mutex_destroy(&host->pose_mutex);
    os_mutex_destroy(&host->usb_mutex);
    for (int i = 0; i < 3; i++)
    {
        os_mutex_destroy(&host->stream_mutex[i]);
    }
}

static void xrsp_flush_stream(struct ql_xrsp_host *host, int64_t target_ns)
{
    if (!host->ready_to_send_frames) return;

    /*while (host->needs_flush) {
        os_nanosleep(U_TIME_1MS_IN_NS / 2);
    }*/
    //printf("Flush 1? %zx %zx\n", host->csd_stream_len[host->stream_write_idx], host->idr_stream_len[host->stream_write_idx]);

    bool wait = false;
    int stream_write_idx = host->stream_write_idx;
    os_mutex_lock(&host->stream_mutex[stream_write_idx]);

    if (host->csd_stream_len[stream_write_idx] || host->idr_stream_len[stream_write_idx]) {
        host->stream_write_idx++;
        if (host->stream_write_idx >= 3)
            host->stream_write_idx = 0;
        //printf("Write %x -> %x\n", stream_write_idx, host->stream_write_idx);
        host->needs_flush[stream_write_idx] = true;
        wait = true;
        host->stream_started_ns[stream_write_idx] = target_ns;

#if 0
        static int64_t last_ns = 0;
        int64_t delta = host->stream_started_ns[stream_write_idx] - last_ns;
        printf("%zx -> %ffps\n", delta, 1000000000.0 / (double)delta);

        last_ns = target_ns;
#endif
    }
    os_mutex_unlock(&host->stream_mutex[stream_write_idx]);

    os_mutex_lock(&host->stream_mutex[host->stream_write_idx]);
    host->csd_stream_len[host->stream_write_idx] = 0;
    host->idr_stream_len[host->stream_write_idx] = 0;
    //host->stream_started_ns[host->stream_write_idx] = target_ns; // TODO actually check the render time?
    host->needs_flush[host->stream_write_idx] = false;
    os_mutex_unlock(&host->stream_mutex[host->stream_write_idx]);

    /*while (wait && host->needs_flush) {
        os_nanosleep(U_TIME_1MS_IN_NS / 2);
    }*/
    //printf("Flush 2? %zx %zx\n", host->csd_stream_len, host->idr_stream_len);
    //printf("Flush!\n");
}

static void xrsp_send_csd(struct ql_xrsp_host *host, const uint8_t* data, size_t data_len)
{
    //printf("CSD\n");
    //if (!host->ready_to_send_frames) return;
    os_mutex_lock(&host->stream_mutex[host->stream_write_idx]);
    //bool success = xrsp_read_usb(host); 
    
    //printf("CSD: %x into %x\n", data_len, host->csd_stream_len);
    //hex_dump(data, data_len);

    if (host->csd_stream_len[host->stream_write_idx] + data_len < 0x1000000) {
        memcpy(host->csd_stream[host->stream_write_idx] + host->csd_stream_len[host->stream_write_idx], data, data_len);
        host->csd_stream_len[host->stream_write_idx] += data_len;
    }

    os_mutex_unlock(&host->stream_mutex[host->stream_write_idx]);
}

static void xrsp_send_idr(struct ql_xrsp_host *host, const uint8_t* data, size_t data_len)
{
    //printf("IDR\n");
    //if (!host->ready_to_send_frames) return;
    os_mutex_lock(&host->stream_mutex[host->stream_write_idx]);
    //printf("IDR: %x into %x\n", data_len, host->idr_stream_len);
    //hex_dump(data, data_len);

    if (host->idr_stream_len[host->stream_write_idx] + data_len < 0x1000000) {
        memcpy(host->idr_stream[host->stream_write_idx] + host->idr_stream_len[host->stream_write_idx], data, data_len);
        host->idr_stream_len[host->stream_write_idx] += data_len;
    }
    
    os_mutex_unlock(&host->stream_mutex[host->stream_write_idx]);
}

static void xrsp_send_usb(struct ql_xrsp_host *host, const uint8_t* data, int32_t data_size)
{
    //printf("Send to usb:\n");
    //hex_dump(data, data_size);

    if (!host->usb_valid) return;

    int sent_len = 0;
    int r = libusb_bulk_transfer(host->dev, host->ep_out, (uint8_t*)data, data_size, &sent_len, 1000);
    if (r != 0 || !sent_len) {
        QUEST_LINK_ERROR("Failed to send %x bytes", sent_len);
        QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(r));

        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_TIMEOUT) {
           host->usb_valid = false;
           host->pairing_state = PAIRINGSTATE_WAIT_FIRST;
        }
    }
    else {
        //printf("Sent %x bytes\n", sent_len);
    }
}

static void xrsp_send_to_topic_capnp_wrapped(struct ql_xrsp_host *host, uint8_t topic, uint32_t idx, const uint8_t* data, int32_t data_size)
{
    uint32_t preamble[2] = {idx, static_cast<uint32_t>(data_size) >> 3};
    xrsp_send_to_topic(host, topic, (uint8_t*)preamble, sizeof(uint32_t) * 2);
    xrsp_send_to_topic(host, topic, data, data_size);
}    

typedef struct ripc_capnp
{
    uint64_t data_info;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint64_t end_info;
} ripc_capnp;

static void xrsp_send_ripc_cmd(struct ql_xrsp_host* host, uint32_t cmd_idx, uint32_t client_id, uint32_t unk, const uint8_t* data, int32_t data_size, const uint8_t* extra_data, int32_t extra_data_size)
{

    ::capnp::MallocMessageBuilder message;
    PayloadRuntimeIPC::Builder msg = message.initRoot<PayloadRuntimeIPC>();

    msg.setCmdId(cmd_idx);
    msg.setNextSize(data_size);
    msg.setClientId(client_id);
    msg.setUnk(unk);
    if (extra_data && extra_data_size) {
        msg.setData(kj::arrayPtr(extra_data, extra_data_size));
    }

    kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> out = message.getSegmentsForOutput();

    uint8_t* packed_data = (uint8_t*)out[0].begin();
    size_t packed_data_size = out[0].size()*sizeof(uint64_t);

    xrsp_send_to_topic_capnp_wrapped(host, TOPIC_RUNTIME_IPC, 0, packed_data, packed_data_size);
    xrsp_send_to_topic(host, TOPIC_RUNTIME_IPC, data, data_size);
}

static void xrsp_send_to_topic(struct ql_xrsp_host *host, uint8_t topic, const uint8_t* data, int32_t data_size)
{
    //printf("Send to topic %s\n", xrsp_topic_str(topic));
    //hex_dump(data, data_size);

    os_mutex_lock(&host->usb_mutex);

    if (!host) return;
    if (data_size <= 0) return;

    int32_t idx = 0;
    int32_t to_send = data_size;
    while (true)
    {
        if (idx >= to_send) break;

        int32_t amt = 0xFFF8;
        if (idx+amt >= to_send) {
            amt = to_send - idx;
        }
        xrsp_send_to_topic_raw(host, topic, data + idx, amt);

        idx += amt;
    }
    os_mutex_unlock(&host->usb_mutex);
}

static void xrsp_send_to_topic_raw(struct ql_xrsp_host *host, uint8_t topic, const uint8_t* data, int32_t data_size)
{
    //printf("Send to topic raw %s\n", xrsp_topic_str(topic));
    //hex_dump(data, data_size);

    if (!host) return;
    //if (data_size <= 0) return;

    int32_t real_len = data_size;
    int32_t align_up_bytes = (((4+data_size) >> 2) << 2) - data_size;
    if (align_up_bytes == 4) {
        align_up_bytes = 0;
    }

    //printf("align up %x, %x, %x\n", align_up_bytes, data_size, real_len);

    uint8_t* msg = (uint8_t*)malloc(data_size + align_up_bytes + sizeof(xrsp_topic_header) + 0x400);
    uint8_t* msg_payload = msg + sizeof(xrsp_topic_header);
    int32_t msg_size = data_size + align_up_bytes + sizeof(xrsp_topic_header);

    // Sometimes we can end up with 0x4 bytes leftover, so we have to pad a bit extra
    int32_t to_fill_check = 0x400 - ((msg_size + 0x400) & 0x3FF);
    if (to_fill_check >= 0 && to_fill_check < 8) {
        align_up_bytes += to_fill_check;

        msg_size = data_size + align_up_bytes + sizeof(xrsp_topic_header);
    }

    xrsp_topic_header* header = (xrsp_topic_header*)msg;
    header->version_maybe = 0;
    header->has_alignment_padding = align_up_bytes ? 1 : 0;
    header->packet_version_is_internal = 1;
    header->packet_version_number = 0;
    header->topic = topic;
    header->unk_14_15 = 0;

    header->num_words = ((data_size + align_up_bytes) >> 2) + 1;
    header->sequence_num = host->increment;
    header->pad = 0;

    memcpy(msg_payload, data, data_size);

    if (align_up_bytes)
    {
        if (align_up_bytes > 1)
        {
            memset(msg_payload+data_size, 0xDE, align_up_bytes-1);
        }
        msg_payload[data_size + align_up_bytes - 1] = align_up_bytes;
    }

    uint8_t* msg_end = msg + msg_size;
    memset(msg_end, 0, 0x400);

    int32_t to_fill = 0x400 - ((msg_size + 0x400) & 0x3FF) - 8;
    int32_t final_size = (msg_size + 8 + to_fill);
    //printf("final_size=%x, to_fill=%x, msg_size=%x\n", final_size, to_fill, msg_size);
    if (to_fill < 0x3f8 && to_fill >= 0 && final_size <= 0x10000) {
        xrsp_topic_header* fill_header = (xrsp_topic_header*)msg_end;
        fill_header->version_maybe = 0;
        fill_header->has_alignment_padding = 0;
        fill_header->packet_version_is_internal = 1;
        fill_header->packet_version_number = 0;
        fill_header->topic = 0;
        fill_header->unk_14_15 = 0;

        fill_header->num_words = (to_fill >> 2) + 1;
        fill_header->sequence_num = host->increment;
        fill_header->pad = 0;
        msg_size += to_fill + sizeof(xrsp_topic_header);
    }

    xrsp_send_usb(host, msg, msg_size);
    host->increment += 1;

    free(msg);
}

static void xrsp_reset_echo(struct ql_xrsp_host *host)
{
    host->echo_idx = 1;
    host->ns_offset = 0;
    host->ns_offset_from_target = 0;
    host->last_xmt = 0;

    host->echo_req_sent_ns = 0; // client ns
    host->echo_req_recv_ns = 0; // server ns
    host->echo_resp_sent_ns = 0; // server ns
    host->echo_resp_recv_ns = 0; // server ns

    host->frame_sent_ns = 0;

    host->gotten_ipcs = 0;

    ql_xrsp_segpkt_init(&host->pose_ctx, host, 1, ql_xrsp_handle_pose);
    //ql_xrsp_segpkt_init(&host->ipc_ctx, host, 1, ql_xrsp_handle_ipc);

    if (!host->sys) return;

    struct ql_hmd* hmd = host->sys->hmd;
    if (hmd) {
        hmd->pose_ns = os_monotonic_get_ns();
    }
}
       
int64_t xrsp_target_ts_ns(struct ql_xrsp_host *host)
{
    //return (os_monotonic_get_ns()) + host->ns_offset;
    return (os_monotonic_get_ns()) - host->ns_offset_from_target;
}

int64_t xrsp_ts_ns(struct ql_xrsp_host *host)
{
    return os_monotonic_get_ns();
}

static void xrsp_send_ping(struct ql_xrsp_host *host)
{
    if (xrsp_ts_ns(host) - host->echo_req_sent_ns < 16000000) // 16ms
    {
        return;
    }

    host->echo_req_sent_ns = xrsp_ts_ns(host);

/*
request_echo_ping = HostInfoPkt.craft_echo(self, result=ECHO_PING, echo_id=self.echo_idx, org=0, recv=0, xmt=self., offset=self.).to_bytes()
#hex_dump(request_echo_ping)
self.send_to_topic(TOPIC_HOSTINFO_ADV, request_echo_ping)
*/

    //printf("Ping sent: xmt=%zx offs=%zx\n", host->echo_req_sent_ns, host->ns_offset);

    int32_t request_echo_ping_len = 0;
    uint8_t* request_echo_ping = ql_xrsp_craft_echo(ECHO_PING, host->echo_idx, 0, 0, host->echo_req_sent_ns, host->ns_offset, &request_echo_ping_len);

    xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, request_echo_ping, request_echo_ping_len);
    free(request_echo_ping);

    host->echo_idx += 1;
}
//uint8_t* ql_xrsp_craft_capnp(uint8_t message_type, uint16_t result, uint32_t unk_4, const uint8_t* payload, size_t payload_size, int32_t* out_len);

static void xrsp_init_session(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    //xrsp_read_usb(host);
    const uint8_t response_ok_payload[] = {0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int32_t response_ok_len = 0;
    uint8_t* response_ok = ql_xrsp_craft_capnp(BUILTIN_OK, 0x2C8, 1, response_ok_payload, sizeof(response_ok_payload), &response_ok_len);

    printf("OK send\n");

    xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, response_ok, response_ok_len);
    free(response_ok);

    //xrsp_read_usb(host); // old
}

static void xrsp_send_codegen_1(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    const uint8_t response_codegen_payload[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int32_t response_codegen_len = 0;
    uint8_t* response_codegen = ql_xrsp_craft_capnp(BUILTIN_CODE_GENERATION, 0xC8, 1, response_codegen_payload, sizeof(response_codegen_payload), &response_codegen_len);

    printf("Codegen send\n");

    xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, response_codegen, response_codegen_len);
    free(response_codegen);

    //xrsp_read_usb(host); // old
}

static void xrsp_send_pairing_1(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    const uint8_t response_pairing_payload[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    int32_t response_pairing_len = 0;
    uint8_t* response_pairing = ql_xrsp_craft_capnp(BUILTIN_PAIRING, 0xC8, 1, response_pairing_payload, sizeof(response_pairing_payload), &response_pairing_len);

    printf("Pairing send\n");

    xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, response_pairing, response_pairing_len);
    free(response_pairing);

    //xrsp_read_usb(host); // old
}

static void xrsp_trigger_bye(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    const uint8_t request_video_idk[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    xrsp_send_to_topic_capnp_wrapped(host, TOPIC_VIDEO, 0, request_video_idk, sizeof(request_video_idk));
}

static void xrsp_finish_pairing_1(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    const uint8_t request_video_idk[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    printf("Echo send\n");
    xrsp_send_ping(host);

    printf("Video idk cmd send\n");
    xrsp_send_to_topic_capnp_wrapped(host, TOPIC_VIDEO, 0, request_video_idk, sizeof(request_video_idk));

    printf("Waiting for user to accept...\n");
}

static void xrsp_init_session_2(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    xrsp_reset_echo(host);
    xrsp_read_usb(host);

    struct ql_hmd* hmd = host->sys->hmd;

    uint8_t fps = (uint8_t)hmd->fps;
    const uint8_t response_ok_2_payload[] = {0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x03, 0x00, 0x01, 0x00, 0x1F, 0x00, 0x00, 0x00, (uint8_t)(host->num_slices & 0xFF), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, fps, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x55, 0x53, 0x42, 0x33, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00};
    int32_t response_ok_2_len = 0;
    uint8_t* response_ok_2 = ql_xrsp_craft_capnp(BUILTIN_OK, 0x2C8, 1, response_ok_2_payload, sizeof(response_ok_2_payload), &response_ok_2_len);

    printf("Done?\n");

    printf("OK send #2\n");
    xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, response_ok_2, response_ok_2_len);
    free(response_ok_2);

    printf("OK read #2\n");
}

static void xrsp_send_codegen_2(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    const uint8_t response_codegen_payload[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int32_t response_codegen_len = 0;
    uint8_t* response_codegen = ql_xrsp_craft_capnp(BUILTIN_CODE_GENERATION, 0xC8, 1, response_codegen_payload, sizeof(response_codegen_payload), &response_codegen_len);

    printf("Codegen send #2\n");
    xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, response_codegen, response_codegen_len);
    free(response_codegen);

    printf("Codegen read #2\n");

    //xrsp_read_usb(host); // old

/*
request_codegen_2_payload = bytes([])
request_codegen_2 = HostInfoPkt.craft_capnp(self, BUILTIN_CODE_GENERATION, result=0xC8, unk_4=1, payload=request_codegen_2_payload).to_bytes()

print ("Codegen send #2")
self.send_to_topic(TOPIC_HOSTINFO_ADV, request_codegen_2)

print ("Codegen read #2")
self.old_read_xrsp()
*/
}

static void xrsp_send_pairing_2(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    const uint8_t response_pairing_payload[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int32_t response_pairing_len = 0;
    uint8_t* response_pairing = ql_xrsp_craft_capnp(BUILTIN_PAIRING, 0xC8, 1, response_pairing_payload, sizeof(response_pairing_payload), &response_pairing_len);

    printf("Pairing send #2\n");
    xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, response_pairing, response_pairing_len);
    free(response_pairing);

    printf("Pairing read #2\n");

    //xrsp_read_usb(host); // old

/*
request_pairing_2_payload = bytes([])
request_pairing_2 = HostInfoPkt.craft_capnp(self, BUILTIN_PAIRING, result=0xC8, unk_4=1, payload=request_pairing_2_payload).to_bytes()

print ("Pairing send #2")
self.send_to_topic(TOPIC_HOSTINFO_ADV, request_pairing_2)

print ("Pairing read #2")
self.old_read_xrsp()
*/
}

typedef struct cmd_pkt_idk
{
    uint64_t a;
    uint32_t cmd_idx;

    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
} cmd_pkt_idk;

typedef struct audio_pkt_idk
{
    uint32_t a;
    uint32_t b;
    
    uint16_t c;
    uint16_t d;

    uint32_t e;
    uint32_t f;
    uint32_t g;
} audio_pkt_idk;

typedef struct body_pkt_idk
{
    uint32_t a;
    uint32_t b;
} body_pkt_idk;

static void xrsp_finish_pairing_2(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    struct audio_pkt_idk send_audiocontrol_idk = {0, 2, 1, 1, 0, 0, 0};
        
    struct cmd_pkt_idk send_cmd_chemx_toggle = {0x0005EC94E91B9D4F, COMMAND_TOGGLE_CHEMX, 0, 0, 0, 0, 0};
    struct cmd_pkt_idk send_cmd_asw_toggle = {0x0005EC94E91B9D83, COMMAND_TOGGLE_ASW, 0, 0, 0, 0, 0};
    struct cmd_pkt_idk send_cmd_asw_disable = {0x0005EC94E91B9D83, COMMAND_TOGGLE_ASW, 0, 1, 0, 0, 0};
    struct cmd_pkt_idk send_cmd_dropframestate_toggle = {0x0005EC94E91B9D83, COMMAND_DROP_FRAMES_STATE, 0, 0, 0, 0, 0};
    struct cmd_pkt_idk send_cmd_camerastream = {0x0005EC94E91B9D83, COMMAND_ENABLE_CAMERA_STREAM, 0, 0, 0, 0, 0};

    struct audio_pkt_idk send_cmd_body = {0, 2, 2, 1, 0, 0, 0};
    struct audio_pkt_idk send_cmd_hands = {0, 2, 1, 1, 0, 0, 0};
    struct body_pkt_idk send_idk_body = {0,0};


    printf("Echo send\n");
    xrsp_send_ping(host);

    printf("Audio Control cmd send\n");
    xrsp_send_to_topic_capnp_wrapped(host, TOPIC_AUDIO_CONTROL, 0, (uint8_t*)&send_audiocontrol_idk, sizeof(send_audiocontrol_idk));

    //print ("1A read")
    //self.old_read_xrsp()

    //print ("1 send")
    //response_echo_pong = HostInfoPkt.craft_echo(self, result=ECHO_PONG, echo_id=1, org=0x000011148017ea57, recv=0x00000074c12277bc, xmt=0x00000074c122daf4, offset=0).to_bytes()
    //self.send_to_topic(TOPIC_HOSTINFO_ADV, response_echo_pong)

    //print ("2 sends")
    xrsp_send_to_topic(host, TOPIC_COMMAND, (uint8_t*)&send_cmd_chemx_toggle, sizeof(send_cmd_chemx_toggle));
    xrsp_send_to_topic(host, TOPIC_COMMAND, (uint8_t*)&send_cmd_asw_toggle, sizeof(send_cmd_asw_toggle));
    //xrsp_send_to_topic(host, TOPIC_COMMAND, (uint8_t*)&send_cmd_asw_disable, sizeof(send_cmd_asw_disable));
    xrsp_send_to_topic(host, TOPIC_COMMAND, (uint8_t*)&send_cmd_dropframestate_toggle, sizeof(send_cmd_dropframestate_toggle));
    //xrsp_send_to_topic(host, TOPIC_COMMAND, &send_cmd_camerastream, sizeof(send_cmd_camerastream));

    //self.send_to_topic(TOPIC_COMMAND, send_cmd_chemx_toggle)
    //self.send_to_topic(TOPIC_COMMAND, send_cmd_asw_toggle)
    //self.send_to_topic(TOPIC_COMMAND, send_cmd_dropframestate_toggle)
    //self.send_to_topic(TOPIC_COMMAND, send_cmd_camerastream)

    xrsp_send_to_topic_capnp_wrapped(host, TOPIC_INPUT_CONTROL, 0, (uint8_t*)&send_cmd_hands, sizeof(send_cmd_hands));
    //xrsp_send_to_topic_capnp_wrapped(host, TOPIC_INPUT_CONTROL, 0, (uint8_t*)&send_cmd_body, sizeof(send_cmd_body));
    //xrsp_send_to_topic(host, TOPIC_BODY, (uint8_t*)&send_idk_body, sizeof(send_idk_body));
    //send_idk_body.a = 1;
    //xrsp_send_to_topic(host, TOPIC_BODY, (uint8_t*)&send_idk_body, sizeof(send_idk_body));
    //send_idk_body.a = 2;
    //xrsp_send_to_topic(host, TOPIC_BODY, (uint8_t*)&send_idk_body, sizeof(send_idk_body));

    // Packages?
    // com.oculus.systemdriver
    // com.facebook.spatial_persistence_service
    // com.oculus.bodyapiservice
    // com.oculus.qplservice
    // com.oculus.presence
    // com.oculus.os.dialoghost


    const uint8_t ripc_test3[] = {0x1B, 0x00, 0x00, 0x00, 0x28, 0x9F, 0x04, 0xC1, 0x17, 0x00, 0x00, 0x00, 0x63, 0x6F, 0x6D, 0x2E, 0x6F, 0x63, 0x75, 0x6C, 0x75, 0x73, 0x2E, 0x73, 0x79, 0x73, 0x74, 0x65, 0x6D, 0x64, 0x72, 0x69, 0x76, 0x65, 0x72, 0x30, 0x00, 0x00, 0x00, 0x00, 0xDE, 0xFE, 0x74, 0x2C, 0x00, 0x00, 0x00, 0x63, 0x6F, 0x6D, 0x2E, 0x6F, 0x63, 0x75, 0x6C, 0x75, 0x73, 0x2E, 0x76, 0x72, 0x72, 0x75, 0x6E, 0x74, 0x69, 0x6D, 0x65, 0x73, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x2E, 0x56, 0x72, 0x52, 0x75, 0x6E, 0x74, 0x69, 0x6D, 0x65, 0x53, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x00, 0x00, 0x00, 0x00};
    const uint8_t ripc_test4[] = {0x1B, 0x00, 0x00, 0x00, 0x28, 0x9F, 0x04, 0xC1, 0x17, 0x00, 0x00, 0x00, 0x63, 0x6F, 0x6D, 0x2E, 0x6F, 0x63, 0x75, 0x6C, 0x75, 0x73, 0x2E, 0x73, 0x79, 0x73, 0x74, 0x65, 0x6D, 0x64, 0x72, 0x69, 0x76, 0x65, 0x72, 0x1F, 0x00, 0x00, 0x00, 0xFB, 0x18, 0x63, 0x6A, 0x1B, 0x00, 0x00, 0x00, 0x63, 0x6F, 0x6D, 0x2E, 0x6F, 0x63, 0x75, 0x6C, 0x75, 0x73, 0x2E, 0x76, 0x72, 0x72, 0x75, 0x6E, 0x74, 0x69, 0x6D, 0x65, 0x73, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x18, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x77, 0x98, 0x14, 0x00, 0x00, 0x00, 0x52, 0x75, 0x6E, 0x74, 0x69, 0x6D, 0x65, 0x53, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x53, 0x65, 0x72, 0x76, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00};

    xrsp_send_ripc_cmd(host, RIPC_MSG_ENSURE_SERVICE_STARTED, host->client_id, 0x3, (uint8_t*)&ripc_test3, sizeof(ripc_test3), NULL, 0);
    xrsp_send_ripc_cmd(host, RIPC_MSG_CONNECT_TO_REMOTE_SERVER, 0xF0000004, 0x4, (uint8_t*)&ripc_test4, sizeof(ripc_test4), NULL, 0);

}

static void xrsp_handle_echo(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    ql_xrsp_echo_payload* payload = (ql_xrsp_echo_payload*)pkt->payload;
    if ((pkt->result & 1) == 1) // PONG
    {
        host->echo_req_recv_ns = payload->recv; // server recv ns
        host->echo_resp_sent_ns = payload->xmt; // server tx ns
        host->echo_resp_recv_ns = pkt->recv_ns; // client rx ns
        host->echo_req_sent_ns = xrsp_ts_ns(host);

        host->ns_offset = ((host->echo_req_recv_ns-host->echo_req_sent_ns) + (host->echo_resp_sent_ns-pkt->recv_ns)) >> 1; // 2
        //self.ns_offset = self.ns_offset & 0xFFFFFFFFFFFFFFFF

        //printf("Ping offs: %x", self.ns_offset);

        //printf("Pong get: org=%zx recv=%zx xmt=%zx offs=%zx\n", payload->org, payload->recv, payload->xmt, payload->offset);

        /*if (payload->offset) {
            host->ns_offset_from_target = payload->offset;
        }*/

        if (host->pairing_state == PAIRINGSTATE_PAIRED) {
            xrsp_send_ping(host);
        }
    }
    else //PING
    {
        //printf ("Ping! %x\n", self.ns_offset);
        host->last_xmt = payload->xmt;

        if (payload->offset) {
            host->ns_offset_from_target = payload->offset;
        }

        //printf("Ping get: org=%zx recv=%zx xmt=%zx offs=%zx\n", payload->org, payload->recv, payload->xmt, payload->offset);

        int32_t request_echo_ping_len = 0;
        int64_t send_xmt = xrsp_ts_ns(host);
        uint8_t* request_echo_ping = ql_xrsp_craft_echo(ECHO_PONG, pkt->unk_4, host->last_xmt, pkt->recv_ns, send_xmt, host->ns_offset, &request_echo_ping_len);

        //printf("Pong sent: org=%zx recv=%zx xmt=%zx offs=%zx\n", host->last_xmt, pkt->recv_ns, send_xmt, host->ns_offset);

        xrsp_send_to_topic(host, TOPIC_HOSTINFO_ADV, request_echo_ping, request_echo_ping_len);
        free(request_echo_ping);
    }  
}

static void xrsp_handle_invite(struct ql_xrsp_host *host, struct ql_xrsp_hostinfo_pkt* pkt)
{
    ql_xrsp_hostinfo_capnp_payload* payload = (ql_xrsp_hostinfo_capnp_payload*)pkt->payload;
    uint8_t* capnp_data = pkt->payload + sizeof(*payload);

    //hex_dump(capnp_data, payload->len_u64s * 8);

    //size_t num_words = (pkt->stream_size - 8) >> 3;

    try
    {
        kj::ArrayPtr<const capnp::word> dataptr[1] = {kj::arrayPtr((capnp::word*)capnp_data, payload->len_u64s)};
        capnp::SegmentArrayMessageReader message(kj::arrayPtr(dataptr, 1));

        PayloadHostInfo::Reader info = message.getRoot<PayloadHostInfo>();

        HeadsetConfig::Reader config = info.getConfig();
        HeadsetDescription::Reader description = config.getDescription();
        HeadsetLens::Reader lensLeft = description.getLeftLens();
        HeadsetLens::Reader lensRight = description.getRightLens();

        struct ql_hmd* hmd = host->sys->hmd;

        // TODO mutex

        os_mutex_lock(&host->pose_mutex);
        hmd->device_type = description.getDeviceType();

        if (hmd->device_type == DEVICE_TYPE_QUEST_2) {
            hmd->fps = 120;
        }
        else if (hmd->device_type == DEVICE_TYPE_QUEST_PRO) {
            hmd->fps = 90;
        }
        else {
            hmd->fps = 72;
        }

        float scale = 1.0;
        if (host->usb_slow_cable) {
            scale = 0.5;
            if (hmd->device_type == DEVICE_TYPE_QUEST_2) {
                hmd->fps = 90;
            }
        }

        QUEST_LINK_INFO("HMD FPS is %f, scale is %f", hmd->fps, scale);

        ql_hmd_set_per_eye_resolution(hmd, (int)((float)description.getResolutionWidth() * scale), (int)((float)description.getResolutionHeight() * scale), /*description.getRefreshRateHz()*/ hmd->fps);

        // Quest 2:
        // 58mm (0.057928182) angle_left -> -52deg
        // 65mm (0.065298356) angle_left -> -49deg
        // 68mm (0.068259589) angle_left -> -43deg

        // Pull FOV information
        hmd->base.hmd->distortion.fov[0].angle_up = lensLeft.getAngleUp() * M_PI / 180;
        hmd->base.hmd->distortion.fov[0].angle_down = -lensLeft.getAngleDown() * M_PI / 180;
        hmd->base.hmd->distortion.fov[0].angle_left = -lensLeft.getAngleLeft() * M_PI / 180;
        hmd->base.hmd->distortion.fov[0].angle_right = lensLeft.getAngleRight() * M_PI / 180;
        
        hmd->base.hmd->distortion.fov[1].angle_up = lensRight.getAngleUp() * M_PI / 180;
        hmd->base.hmd->distortion.fov[1].angle_down = -lensRight.getAngleDown() * M_PI / 180;
        hmd->base.hmd->distortion.fov[1].angle_left = -lensRight.getAngleLeft() * M_PI / 180;
        hmd->base.hmd->distortion.fov[1].angle_right = lensRight.getAngleRight() * M_PI / 180;

        hmd->fov_angle_left = lensLeft.getAngleLeft();
        os_mutex_unlock(&host->pose_mutex);
    }
    catch(...) {

    }
}

static void xrsp_handle_hostinfo_adv(struct ql_xrsp_host *host)
{
    int ret;
    struct ql_xrsp_topic_pkt* pkt = &host->working_pkt;

    struct ql_xrsp_hostinfo_pkt hostinfo;
    ret = ql_xrsp_hostinfo_pkt_create(&hostinfo, pkt, host);
    if (ret < 0) {
        // TODO
    }

    //hex_dump(pkt->payload, pkt->payload_valid);

    if (hostinfo.message_type == BUILTIN_ECHO) {
        xrsp_handle_echo(host, &hostinfo);
        return;
    }

    // Pull lens and distortion info
    if (hostinfo.message_type == BUILTIN_INVITE) {
        xrsp_handle_invite(host, &hostinfo);
    }

    if (host->pairing_state == PAIRINGSTATE_WAIT_FIRST)
    {
        if (hostinfo.message_type == BUILTIN_INVITE) {
            xrsp_init_session(host, &hostinfo);
        }
        else if (hostinfo.message_type == BUILTIN_ACK) {
            xrsp_send_codegen_1(host, &hostinfo);
        }
        else if (hostinfo.message_type == BUILTIN_CODE_GENERATION_ACK) {
            xrsp_send_pairing_1(host, &hostinfo);
        }
        else if (hostinfo.message_type == BUILTIN_PAIRING_ACK) {
            xrsp_finish_pairing_1(host, &hostinfo);

            host->pairing_state = PAIRINGSTATE_WAIT_SECOND;
        }
    }
    else if (host->pairing_state == PAIRINGSTATE_WAIT_SECOND || host->pairing_state == PAIRINGSTATE_PAIRING)
    {
        if (hostinfo.message_type == BUILTIN_INVITE) {
            host->pairing_state = PAIRINGSTATE_PAIRING;
            xrsp_init_session_2(host, &hostinfo);
        } 
        else if (hostinfo.message_type == BUILTIN_ACK) {
            xrsp_send_codegen_2(host, &hostinfo);
        }
        else if (hostinfo.message_type == BUILTIN_CODE_GENERATION_ACK) {
            xrsp_send_pairing_2(host, &hostinfo);
        }
        else if (hostinfo.message_type == BUILTIN_PAIRING_ACK) {
            xrsp_finish_pairing_2(host, &hostinfo);

            host->pairing_state = PAIRINGSTATE_PAIRED;

            host->paired_ns = xrsp_ts_ns(host);
        } 
    }   
}

static void xrsp_handle_pkt(struct ql_xrsp_host *host)
{
    struct ql_xrsp_topic_pkt* pkt = &host->working_pkt;

    ql_xrsp_topic_pkt_dump(pkt);

    if (pkt->topic == TOPIC_HOSTINFO_ADV)
    {
        xrsp_handle_hostinfo_adv(host);
    }
    else if (pkt->topic == TOPIC_POSE)
    {
        ql_xrsp_segpkt_consume(&host->pose_ctx, host, pkt);
    }
    else if (pkt->topic == TOPIC_HANDS)
    {
        ql_xrsp_handle_hands(host, pkt);
    }
    else if (pkt->topic == TOPIC_SKELETON)
    {
        ql_xrsp_handle_skeleton(host, pkt);
    }
    else if (pkt->topic == TOPIC_BODY)
    {
        ql_xrsp_handle_body(host, pkt);
    }
    else if (pkt->topic == TOPIC_LOGGING)
    {
        ql_xrsp_handle_logging(host, pkt);
    }
    else if (pkt->topic == TOPIC_RUNTIME_IPC)
    {
        if (host->gotten_ipcs == 3) {
            const uint8_t ripc_test4[] = {0x1B, 0x00, 0x00, 0x00, 0x28, 0x9F, 0x04, 0xC1, 0x17, 0x00, 0x00, 0x00, 0x63, 0x6F, 0x6D, 0x2E, 0x6F, 0x63, 0x75, 0x6C, 0x75, 0x73, 0x2E, 0x73, 0x79, 0x73, 0x74, 0x65, 0x6D, 0x64, 0x72, 0x69, 0x76, 0x65, 0x72, 0x1F, 0x00, 0x00, 0x00, 0xFB, 0x18, 0x63, 0x6A, 0x1B, 0x00, 0x00, 0x00, 0x63, 0x6F, 0x6D, 0x2E, 0x6F, 0x63, 0x75, 0x6C, 0x75, 0x73, 0x2E, 0x76, 0x72, 0x72, 0x75, 0x6E, 0x74, 0x69, 0x6D, 0x65, 0x73, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x18, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x77, 0x98, 0x14, 0x00, 0x00, 0x00, 0x52, 0x75, 0x6E, 0x74, 0x69, 0x6D, 0x65, 0x53, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x53, 0x65, 0x72, 0x76, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00};
            xrsp_send_ripc_cmd(host, RIPC_MSG_CONNECT_TO_REMOTE_SERVER, host->client_id, 0x4, (uint8_t*)&ripc_test4, sizeof(ripc_test4), NULL, 0);
        }
        else if (host->gotten_ipcs < 20) {
            //hash_djb2("SendSwitchToHandsNotif") ^ hash_djb2("Void") ^ hash_djb2("bool")
            uint8_t ripc_SendSwitchToHandsNotif[] = {0x02, 0x00, 0xF4, 0xAC, 0x64, 0xB1, 0x00, 0x01};
            
            uint8_t ripc_SystemButtonEventRequest[] = {0x02, 0x00, 0x37, 0xCB, 0x58, 0xF1, 0x00, 0x01};
            uint8_t ripc_EnableEyeTrackingForPCLink[] = {0x02, 0x00, 0x8C, 0xDE, 0x80, 0xD0, 0x00, 0x01};
            uint8_t ripc_EnableFaceTrackingForPCLink[] = {0x02, 0x00, 0x50, 0x41, 0x9C, 0xB7, 0x00, 0x01};
            //uint8_t ripc_test5[] = {0x02, 0x00, 0x0A, 0xC3, 0x0C, 0x68, 0x00, 0x01};
            //uint8_t ripc_test5[] = {0x02, 0x00, 0xBE, 0x3B, 0xD8, 0x3E, 0x00, 0x01};
            //uint8_t ripc_test5[] = {0x02, 0x00, 0xF1, 0x58, 0xCB, 0x37, 0x00, 0x01};
            uint8_t ripc_extra[] = {0x01, 0x00, 0x00, 0x00, 0xe5, 0x62, 0xb7, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00}; // 0x1, 0x38b762e4, 0x0

            xrsp_send_ripc_cmd(host, RIPC_MSG_RPC, host->client_id, 4, (uint8_t*)&ripc_EnableEyeTrackingForPCLink, sizeof(ripc_EnableEyeTrackingForPCLink), ripc_extra, sizeof(ripc_extra)); 
            xrsp_send_ripc_cmd(host, RIPC_MSG_RPC, host->client_id, 4, (uint8_t*)&ripc_EnableFaceTrackingForPCLink, sizeof(ripc_EnableFaceTrackingForPCLink), ripc_extra, sizeof(ripc_extra));

        }
        host->gotten_ipcs++;
    }

    if ((pkt->topic == TOPIC_POSE || pkt->topic == TOPIC_LOGGING) && host->pairing_state != PAIRINGSTATE_PAIRED)
    {
        xrsp_trigger_bye(host, NULL);
    }
}

static bool xrsp_read_usb(struct ql_xrsp_host *host)
{
    int ret;

    if (!host->usb_valid) return false;

    //os_mutex_lock(&host->usb_mutex);
    while (true)
    {
        unsigned char data[0x400];
        int32_t data_consumed = 0;

        int read_len = 0;
        int r = libusb_bulk_transfer(host->dev, host->ep_in, data, sizeof(data), &read_len, 1);
        if (r != 0 || !read_len) {
            //printf("asdf %d %x\n", r, read_len);

            if (r != LIBUSB_ERROR_TIMEOUT) {
                QUEST_LINK_ERROR("libusb error: %s", libusb_strerror(r));
            }

            if (r == LIBUSB_ERROR_NO_DEVICE) {
                ql_xrsp_usb_init(host, true);
            }
            break;
        }

        host->last_read_ns = xrsp_ts_ns(host);

        //printf("Read %x bytes\n", read_len);
        //hex_dump(data, read_len);

        if (!host->have_working_pkt) {
            //printf("Create pkt\n");
            ret = ql_xrsp_topic_pkt_create(&host->working_pkt, data, read_len, host->last_read_ns);
            if (ret < 0) {
                // TODO
                data_consumed += 0x8;
                host->have_working_pkt = false;
            }
            else {
                data_consumed += ret;
                host->have_working_pkt = true;
            }
        }
        else if (host->working_pkt.missing_bytes == 0) {
            //printf("Handle pkt\n");
            try {
                xrsp_handle_pkt(host);
            }
            catch(...) {
                QUEST_LINK_ERROR("Exception while parsing packet...");
            }
            

            printf("Is remaining data possible?\n");

            int32_t remaining_data = read_len - data_consumed;
            /*
            remains = self.working_pkt.remainder_bytes()
            if len(remains) > 0 and len(remains) < 8:
                self.working_pkt = None
                print("Weird remainder!")
                hex_dump(remains)
            elif len(remains) > 0:
                self.working_pkt = TopicPkt(self, remains)
                self.working_pkt.add_missing_bytes(b)
            else:
                self.working_pkt = TopicPkt(self, b)
            */
        }
        else {
            //printf("Append pkt\n");
            ret = ql_xrsp_topic_pkt_append(&host->working_pkt, data, read_len);
            if (ret < 0) {
                // TODO
                data_consumed += 0x8;
                host->have_working_pkt = false;
            }
            else {
                data_consumed += ret;
            }
        }

        while (host->have_working_pkt) {
            //printf("Handle pkt 2 loop %x\n", read_len - data_consumed);
            if (host->working_pkt.missing_bytes == 0) {
                //printf("Handle pkt 2 %x\n", read_len - data_consumed);
                xrsp_handle_pkt(host);
                ql_xrsp_topic_pkt_destroy(&host->working_pkt);
                host->have_working_pkt = false;
            }

            int32_t remaining_data = read_len - data_consumed;
            if (remaining_data <= 0) {
                break;
            }

            if (remaining_data > 0 && remaining_data < 8) {
                //printf("Weird remainder! data_consumed: %x\n", data_consumed);
                hex_dump(&data[data_consumed], read_len-data_consumed);
                ql_xrsp_topic_pkt_destroy(&host->working_pkt);
                host->have_working_pkt = false;
            }
            else if (remaining_data > 0) {
                //printf("Create pkt 2 %x\n", read_len - data_consumed);
                ret = ql_xrsp_topic_pkt_create(&host->working_pkt, &data[data_consumed], remaining_data, host->last_read_ns);
                if (ret < 0) {
                    // TODO
                    data_consumed += 0x8;
                    host->have_working_pkt = false;
                }
                else {
                    data_consumed += ret;
                    host->have_working_pkt = true;
                }
                //printf("Create pkt 2 end %x\n", read_len - data_consumed);
            }
        }
    }
    //os_mutex_unlock(&host->usb_mutex);
    
    return true;
}

static void xrsp_send_video(struct ql_xrsp_host *host, int slice_idx, int frame_idx, int64_t frame_started_ns, const uint8_t* csd_dat, size_t csd_len,
                            const uint8_t* video_dat, size_t video_len, int blit_y_pos)
{
    ::capnp::MallocMessageBuilder message;
    PayloadSlice::Builder msg = message.initRoot<PayloadSlice>();

    struct ql_hmd* hmd = host->sys->hmd;

    struct xrt_space_relation out_head_relation;
    U_ZERO(&out_head_relation);

    //xrt_device_get_tracked_pose(&hmd->base, XRT_INPUT_GENERIC_HEAD_POSE, frame_started_ns, &out_head_relation);

    os_mutex_lock(&host->pose_mutex);
    int bits = 0;
    if (csd_len > 0)
        bits |= 1;
    if (slice_idx == host->num_slices-1)
        bits |= 2;

    out_head_relation.pose = hmd->last_req_poses[0];

    msg.setFrameIdx(frame_idx);
    msg.setUnk0p1(0);
    msg.setRectifyMeshId(1);

    // TODO mutex

    //TODO: we need some way to know the pose as it was when the frame was rendered,
    // so that the Quest can handle timewarp for us.
    msg.setPoseQuatX(out_head_relation.pose.orientation.x);
    msg.setPoseQuatY(out_head_relation.pose.orientation.y);
    msg.setPoseQuatZ(out_head_relation.pose.orientation.z);
    msg.setPoseQuatW(out_head_relation.pose.orientation.w);
    msg.setPoseX(out_head_relation.pose.position.x);
    msg.setPoseY(out_head_relation.pose.position.y);
    msg.setPoseZ(out_head_relation.pose.position.z);

    /*msg.setPoseQuatX(0.0);
    msg.setPoseQuatY(0.0);
    msg.setPoseQuatZ(0.0);
    msg.setPoseQuatW(1.0);
    msg.setPoseX(0.0);
    msg.setPoseY(0.0);
    msg.setPoseZ(0.0);*/

    msg.setTimestamp05(xrsp_target_ts_ns(host)+41540173); // Deadline //18278312488115 // xrsp_ts_ns(host)
    msg.setSliceNum(slice_idx);
    msg.setUnk6p1(bits); 
    msg.setUnk6p2(0);
    msg.setUnk6p3(0);
    msg.setBlitYPos(blit_y_pos);
    msg.setCropBlocks((hmd->encode_height/16) / host->num_slices); // 24 for slice count 5
    
    msg.setUnk8p1(0);
    msg.setTimestamp09(xrsp_target_ts_ns(host));//18787833654115
    msg.setUnkA(29502900);
    msg.setTimestamp0B(xrsp_target_ts_ns(host)+28713475);////18278296859411
    msg.setTimestamp0C(xrsp_target_ts_ns(host)+23714318);////18278292486840
    msg.setTimestamp0D(xrsp_target_ts_ns(host)+9415134);//18787848654114
    //printf("%x\n", host->ns_offset);

    msg.getQuat1().setX(0);
    msg.getQuat1().setY(0);
    msg.getQuat1().setZ(0);
    msg.getQuat1().setW(0);

    msg.getQuat1().setX(0);
    msg.getQuat2().setY(0);
    msg.getQuat2().setZ(0);
    msg.getQuat2().setW(0);

    msg.setCsdSize(csd_len);
    msg.setVideoSize(video_len);
    os_mutex_unlock(&host->pose_mutex);

    kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> out = message.getSegmentsForOutput();

    uint8_t* packed_data = (uint8_t*)out[0].begin();
    size_t packed_data_size = out[0].size()*sizeof(uint64_t);

    //hex_dump(packed_data, packed_data_size);

    //printf("adsf %zx %zx\n", csd_len, video_len);

    //printf("Send capnp\n");
    xrsp_send_to_topic_capnp_wrapped(host, TOPIC_SLICE_0+slice_idx, 0, packed_data, packed_data_size);
    //printf("Send csd %x\n", csd_len);
    if (csd_len)
        xrsp_send_to_topic(host, TOPIC_SLICE_0+slice_idx, csd_dat, csd_len);
    //printf("Send vid %x\n", video_len);
    if (video_len)
        xrsp_send_to_topic(host, TOPIC_SLICE_0+slice_idx, video_dat, video_len);
    //printf("done\n");

    //hex_dump((uint8_t*)out[0].begin(), );
    //printf("%x\n", out[0].size());

    static int idk = 0;
    if (idk >= 20) return;

    idk++;
    //hash_djb2("SendSwitchToHandsNotif") ^ hash_djb2("Void") ^ hash_djb2("bool")
    uint8_t ripc_SendSwitchToHandsNotif[] = {0x02, 0x00, 0xF4, 0xAC, 0x64, 0xB1, 0x00, 0x01};
    
    uint8_t ripc_SystemButtonEventRequest[] = {0x02, 0x00, 0x37, 0xCB, 0x58, 0xF1, 0x00, 0x01};
    uint8_t ripc_EnableEyeTrackingForPCLink[] = {0x02, 0x00, 0x8C, 0xDE, 0x80, 0xD0, 0x00, 0x01};
    uint8_t ripc_EnableFaceTrackingForPCLink[] = {0x02, 0x00, 0x50, 0x41, 0x9C, 0xB7, 0x00, 0x01};
    //uint8_t ripc_test5[] = {0x02, 0x00, 0x0A, 0xC3, 0x0C, 0x68, 0x00, 0x01};
    //uint8_t ripc_test5[] = {0x02, 0x00, 0xBE, 0x3B, 0xD8, 0x3E, 0x00, 0x01};
    //uint8_t ripc_test5[] = {0x02, 0x00, 0xF1, 0x58, 0xCB, 0x37, 0x00, 0x01};
    uint8_t ripc_extra[] = {0x01, 0x00, 0x00, 0x00, 0xe5, 0x62, 0xb7, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00}; // 0x1, 0x38b762e4, 0x0

    xrsp_send_ripc_cmd(host, RIPC_MSG_RPC, host->client_id, 4, (uint8_t*)&ripc_EnableEyeTrackingForPCLink, sizeof(ripc_EnableEyeTrackingForPCLink), ripc_extra, sizeof(ripc_extra)); 
    xrsp_send_ripc_cmd(host, RIPC_MSG_RPC, host->client_id, 4, (uint8_t*)&ripc_EnableFaceTrackingForPCLink, sizeof(ripc_EnableFaceTrackingForPCLink), ripc_extra, sizeof(ripc_extra));
    xrsp_send_ripc_cmd(host, RIPC_MSG_RPC, host->client_id, 4, (uint8_t*)&ripc_SendSwitchToHandsNotif, sizeof(ripc_SendSwitchToHandsNotif), ripc_extra, sizeof(ripc_extra));
    xrsp_send_ripc_cmd(host, RIPC_MSG_RPC, host->client_id, 4, (uint8_t*)&ripc_SystemButtonEventRequest, sizeof(ripc_SystemButtonEventRequest), ripc_extra, sizeof(ripc_extra));
}

static void *
ql_xrsp_read_thread(void *ptr)
{
    DRV_TRACE_MARKER();

    struct ql_xrsp_host *host = (struct ql_xrsp_host *)ptr;

    os_thread_helper_lock(&host->read_thread);
    while (os_thread_helper_is_running_locked(&host->read_thread)) {
        os_thread_helper_unlock(&host->read_thread);

        //printf("%x %x %x\n", xrsp_ts_ns(host) - host->last_read_ns > 1000000000, host->pairing_state == PAIRINGSTATE_WAIT_FIRST, host->usb_valid);
        if (xrsp_ts_ns(host) - host->last_read_ns > 1000000000 && host->pairing_state == PAIRINGSTATE_WAIT_FIRST && !host->usb_valid)
        {
            ql_xrsp_usb_init(host, false);
            host->last_read_ns = xrsp_ts_ns(host);
        }

        //printf(".\n");
        //os_mutex_lock(&host->usb_mutex);
        bool success = xrsp_read_usb(host); 
        

        if (success) {
            
        }
        //os_mutex_unlock(&host->usb_mutex);
        

        os_thread_helper_lock(&host->read_thread);

        //if (!success) {
        //    break;
        //}

        if (os_thread_helper_is_running_locked(&host->read_thread)) {
            os_nanosleep(U_TIME_1MS_IN_NS / 2);
        }
    }
    os_thread_helper_unlock(&host->read_thread);

    QUEST_LINK_DEBUG("Exiting packet reading thread");

    return NULL;
}

static void *
ql_xrsp_write_thread(void *ptr)
{
    DRV_TRACE_MARKER();

    struct ql_xrsp_host *host = (struct ql_xrsp_host *)ptr;

    os_thread_helper_lock(&host->write_thread);
    while (os_thread_helper_is_running_locked(&host->write_thread)) {
        os_thread_helper_unlock(&host->write_thread);

        os_mutex_lock(&host->stream_mutex[host->stream_read_idx]);

        if (host->needs_flush[host->stream_read_idx] && (host->csd_stream_len[host->stream_read_idx] || host->idr_stream_len[host->stream_read_idx])) { //  && (xrsp_ts_ns(host) - host->frame_sent_ns) >= 13890000
            //printf("Flush: %zx %zx\n", host->csd_stream_len, host->idr_stream_len);
            //if (host->csd_stream_len == 0x27)
            xrsp_send_video(host, 0, host->frame_idx, host->stream_started_ns[host->stream_read_idx], (const uint8_t*)host->csd_stream[host->stream_read_idx], host->csd_stream_len[host->stream_read_idx], (const uint8_t*)host->idr_stream[host->stream_read_idx], host->idr_stream_len[host->stream_read_idx], 0);
            //printf("%zu\n", xrsp_ts_ns(host) - host->frame_sent_ns);
            host->frame_sent_ns = xrsp_ts_ns(host);

            host->frame_idx++;
            host->csd_stream_len[host->stream_read_idx] = 0;
            host->idr_stream_len[host->stream_read_idx] = 0;
            host->needs_flush[host->stream_read_idx] = false;

            host->stream_read_idx++;
            if (host->stream_read_idx >= 3)
                host->stream_read_idx = 0;
            //printf("Flush: %x\n", host->stream_read_idx);
        }

        os_mutex_unlock(&host->stream_mutex[host->stream_read_idx]);

        //printf("%zx\n", xrsp_ts_ns(host) - host->paired_ns);
        if (xrsp_ts_ns(host) - host->paired_ns > 5000000000 && host->pairing_state == PAIRINGSTATE_PAIRED) // && xrsp_ts_ns(host) - host->frame_sent_ns >= 16000000
        {
            host->ready_to_send_frames = true;
        }

        if (xrsp_ts_ns(host) - host->last_read_ns > 1000000000 && host->pairing_state == PAIRINGSTATE_WAIT_FIRST && host->usb_valid)
        {
            xrsp_trigger_bye(host, NULL);
            host->last_read_ns = xrsp_ts_ns(host);
        }
        
        os_thread_helper_lock(&host->write_thread);

        //if (!success) {
        //    break;
        //}

        if (os_thread_helper_is_running_locked(&host->write_thread)) {
            os_nanosleep(U_TIME_1MS_IN_NS);
        }
    }
    os_thread_helper_unlock(&host->write_thread);

    QUEST_LINK_DEBUG("Exiting packet writing thread");

    return NULL;
}
