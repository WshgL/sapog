/****************************************************************************
 *
 *   Copyright (C) 2014 PX4 Development Team. All rights reserved.
 *   Author: Pavel Kirienko <pavel.kirienko@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "esc_controller.hpp"
#include <uavcan/equipment/esc/RawCommand.hpp>
#include <uavcan/equipment/esc/RPMCommand.hpp>
#include <uavcan/equipment/esc/Status.hpp>
#include <uavcan/equipment/esc/RPMFeedback.hpp>
#include <zubax_chibios/os.hpp>
#include <motor/motor.h>
#include <temperature_sensor.hpp>

namespace uavcan_node
{
namespace
{

uavcan::Publisher<uavcan::equipment::esc::Status>* pub_status;
uavcan::Publisher<uavcan::equipment::esc::RPMFeedback>* pub_rpm_fb;

unsigned publish_rate_ms;
unsigned self_index;
unsigned command_ttl_ms;
float max_dc_to_start;

os::config::Param<unsigned> param_pub_rate_ms("pub_rate_ms",     40,      1,   100); // default: 25hz
os::config::Param<unsigned> param_esc_index("esc_index",           0,      0,    15);
os::config::Param<unsigned> param_cmd_ttl_ms("cmd_ttl_ms",       200,    100,  5000);
os::config::Param<float> param_cmd_start_dc("cmd_start_dc",      1.0,   0.01,   1.0);


void cb_raw_command(const uavcan::ReceivedDataStructure<uavcan::equipment::esc::RawCommand>& msg)
{
	if (msg.cmd.size() <= self_index) {
		motor_stop();
		return;
	}

	const float scaled_dc =
		msg.cmd[self_index] / float(uavcan::equipment::esc::RawCommand::FieldTypes::cmd::RawValueType::max());

	const bool idle = motor_is_idle();
	const bool accept = (!idle) || (idle && (scaled_dc <= max_dc_to_start));

	if (accept && (scaled_dc > 0)) {
		motor_set_duty_cycle(scaled_dc, command_ttl_ms);
	} else {
		motor_stop();
	}
}

void cb_rpm_command(const uavcan::ReceivedDataStructure<uavcan::equipment::esc::RPMCommand>& msg)
{
	if (msg.rpm.size() <= self_index) {
		motor_stop();
		return;
	}

	const int rpm = msg.rpm[self_index];

	if (rpm > 0) {
		motor_set_rpm(rpm, command_ttl_ms);
	} else {
		motor_stop();
	}
}

void cb_esc(const uavcan::TimerEvent& event)
{
	// Publish RPMFeedback
	uavcan::equipment::esc::RPMFeedback msg_rpm_fb;

	msg_rpm_fb.esc_index = self_index;
	msg_rpm_fb.rpm = motor_get_rpm();

	pub_rpm_fb->broadcast(msg_rpm_fb);

	// Publish Status
	uavcan::equipment::esc::Status msg_status;

	msg_status.esc_index = self_index;
	// msg_status.rpm = motor_get_rpm();
	motor_get_input_voltage_current(&msg_status.voltage, &msg_status.current);
	msg_status.power_rating_pct = static_cast<unsigned>(motor_get_duty_cycle() * 100 + 0.5F);
	msg_status.error_count = motor_get_zc_failures_since_start();

	msg_status.temperature = float(temperature_sensor::get_temperature_K());
	if (msg_status.temperature < 0) {
		msg_status.temperature = std::numeric_limits<float>::quiet_NaN();
	}

	static uavcan::MonotonicTime prev_pub_ts;
	if ((event.scheduled_time - prev_pub_ts).toMSec() >= 990) { // 1hz
		prev_pub_ts = event.scheduled_time;
		pub_status->broadcast(msg_status);
	}
}

}

int init_esc_controller(uavcan::INode& node)
{
	static uavcan::Subscriber<uavcan::equipment::esc::RawCommand> sub_raw_command(node);
	static uavcan::Subscriber<uavcan::equipment::esc::RPMCommand> sub_rpm_command(node);
	static uavcan::Timer timer_esc(node);

	publish_rate_ms = param_pub_rate_ms.get();
	self_index = param_esc_index.get();
	command_ttl_ms = param_cmd_ttl_ms.get();
	max_dc_to_start = param_cmd_start_dc.get();

	int res = 0;

	res = sub_raw_command.start(cb_raw_command);
	if (res != 0) {
		return res;
	}

	res = sub_rpm_command.start(cb_rpm_command);
	if (res != 0) {
		return res;
	}

	pub_status = new uavcan::Publisher<uavcan::equipment::esc::Status>(node);
	res = pub_status->init();
	if (res != 0) {
		return res;
	}

	pub_rpm_fb = new uavcan::Publisher<uavcan::equipment::esc::RPMFeedback>(node);
	res = pub_rpm_fb->init();
	if (res != 0) {
		return res;
	}

	timer_esc.setCallback(&cb_esc);
	timer_esc.startPeriodic(uavcan::MonotonicDuration::fromMSec(publish_rate_ms));

	return 0;
}

}
