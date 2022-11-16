// Copyright 2022, Guillaume Meunier
// Copyright 2022, Patrick Nicolas
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "wivrn_packets.h"
#include "wivrn_sockets.h"
#include <optional>
#include <poll.h>

using namespace xrt::drivers::wivrn;

class wivrn_connection
{
	typed_socket<TCP, from_headset::control_packets, to_headset::control_packets> control;
	typed_socket<UDP, from_headset::stream_packets, to_headset::stream_packets> stream;

public:
	wivrn_connection(TCP &&tcp, in6_addr address);
	wivrn_connection(const wivrn_connection &) = delete;
	wivrn_connection &
	operator=(const wivrn_connection &) = delete;

	void
	send_control(const to_headset::control_packets &packet);
	void
	send_stream(const to_headset::stream_packets &packet);

	std::optional<from_headset::stream_packets>
	poll_stream(int timeout);
	std::optional<from_headset::control_packets>
	poll_control(int timeout);

	template <typename T>
	int
	poll(T &&visitor, int timeout)
	{
		pollfd fds[2] = {};
		fds[0].events = POLLIN;
		fds[0].fd = stream.get_fd();
		fds[1].events = POLLIN;
		fds[1].fd = control.get_fd();

		int r = ::poll(fds, std::size(fds), timeout);
		if (r < 0)
			throw std::system_error(errno, std::system_category());
		if (r > 0 && (fds[0].revents & POLLIN)) {
			std::visit(std::forward<T>(visitor), stream.receive());
		}
		if (r > 0 && (fds[1].revents & POLLIN)) {
			std::visit(std::forward<T>(visitor), control.receive());
		}
		return r;
	}
};
