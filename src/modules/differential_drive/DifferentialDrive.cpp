/****************************************************************************
 *
 *   Copyright (c) 2023 PX4 Development Team. All rights reserved.
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

#include "DifferentialDrive.hpp"

using namespace time_literals;
using namespace matrix;

DifferentialDrive::DifferentialDrive() :
	ModuleParams(nullptr),
	ScheduledWorkItem(MODULE_NAME, px4::wq_configurations::rate_ctrl)
{
	updateParams();
	pid_init(&_angular_velocity_pid, PID_MODE_DERIVATIV_NONE, 0.001f);
	pid_init(&_speed_pid, PID_MODE_DERIVATIV_NONE, 0.001f);
}

bool DifferentialDrive::init()
{
	ScheduleOnInterval(10_ms); // 100 Hz
	return true;
}

void DifferentialDrive::updateParams()
{
	ModuleParams::updateParams();

	pid_set_parameters(&_angular_velocity_pid,
			   _param_rdd_p_gain_angular_velocity.get(),  // Proportional gain
			   _param_rdd_i_gain_angular_velocity.get(),  // Integral gain
			   0,  // Derivative gain
			   20,  // Integral limit
			   200);  // Output limit

	pid_set_parameters(&_speed_pid,
			   _param_rdd_p_gain_speed.get(),  // Proportional gain
			   _param_rdd_i_gain_speed.get(),  // Integral gain
			   0,  // Derivative gain
			   2,  // Integral limit
			   200);  // Output limit

	_max_speed = _param_rdd_max_wheel_speed.get() * _param_rdd_wheel_radius.get();
	_max_angular_velocity = _max_speed / (_param_rdd_wheel_base.get() / 2.f);

	_differential_drive_kinematics.setWheelBase(_param_rdd_wheel_base.get());
	_differential_drive_kinematics.setMaxSpeed(_max_speed);
	_differential_guidance_controller.setMaxSpeed(_max_speed);
	_differential_drive_kinematics.setMaxAngularVelocity(_max_angular_velocity);
	_differential_guidance_controller.setMaxAngularVelocity(_max_angular_velocity);
}

void DifferentialDrive::Run()
{
	if (should_exit()) {
		ScheduleClear();
		exit_and_cleanup();
	}

	hrt_abstime now = hrt_absolute_time();
	const double dt = static_cast<double>(math::min((now - _time_stamp_last), kTimeoutUs)) / 1e6;
	_time_stamp_last = now;

	if (_parameter_update_sub.updated()) {
		parameter_update_s parameter_update;
		_parameter_update_sub.copy(&parameter_update);

		updateParams();
	}

	if (_vehicle_control_mode_sub.updated()) {
		vehicle_control_mode_s vehicle_control_mode{};

		if (_vehicle_control_mode_sub.copy(&vehicle_control_mode)) {
			_armed = vehicle_control_mode.flag_armed;
			_manual_driving = vehicle_control_mode.flag_control_manual_enabled;
			_mission_driving = vehicle_control_mode.flag_control_auto_enabled;
		}
	}

	if (_vehicle_attitude_sub.updated()) {
		vehicle_attitude_s vehicle_attitude{};

		if (_vehicle_attitude_sub.copy(&vehicle_attitude)) {
			_vehicle_attitude_quaternion = Quatf(vehicle_attitude.q);
			_vehicle_yaw = matrix::Eulerf(_vehicle_attitude_quaternion).psi();
		}
	}

	if (_vehicle_angular_velocity_sub.updated()) {
		_vehicle_angular_velocity_sub.copy(&_vehicle_angular_velocity);
	}

	if (_vehicle_local_position_sub.updated()) {
		vehicle_local_position_s vehicle_local_position{};

		if (_vehicle_local_position_sub.copy(&vehicle_local_position)) {
			Vector3f velocity_in_local_frame(vehicle_local_position.vx, vehicle_local_position.vy, vehicle_local_position.vz);
			_velocity_in_body_frame = _vehicle_attitude_quaternion.rotateVectorInverse(velocity_in_local_frame);
		}
	}

	if (_manual_driving) {
		// Manual mode
		// directly produce setpoints from the manual control setpoint (joystick)
		if (_manual_control_setpoint_sub.updated()) {
			manual_control_setpoint_s manual_control_setpoint{};

			if (_manual_control_setpoint_sub.copy(&manual_control_setpoint)) {
				_differential_drive_setpoint.timestamp = now;
				_differential_drive_setpoint.speed = manual_control_setpoint.throttle * _param_rdd_speed_scale.get() * _max_speed;
				_differential_drive_setpoint.yaw_rate = manual_control_setpoint.roll * _param_rdd_ang_velocity_scale.get() *
									_max_angular_velocity;
				_differential_drive_control_output_pub.publish(_differential_drive_setpoint);
			}
		}

	} else if (_mission_driving) {
		// Mission mode
		// directly receive setpoints from the guidance library
		matrix::Vector2f guidance_output =
			_differential_guidance_controller.computeGuidance(
				_vehicle_yaw,
				_vehicle_angular_velocity.xyz[2],
				dt
			);

		_differential_drive_setpoint.timestamp = now;
		_differential_drive_setpoint.speed = guidance_output(0);
		_differential_drive_setpoint.yaw_rate = guidance_output(1);
		_differential_drive_setpoint_pub.publish(_differential_drive_setpoint);
	}

	// check if the topic is updated and update the setpoint
	if (_differential_drive_control_output_sub.updated()) {
		_differential_drive_control_output_sub.copy(&_differential_drive_setpoint);

		_speed_pid_output = 0;
		_angular_velocity_pid_output = 0;
	}

	if (_differential_drive_setpoint_sub.updated()) {
		_differential_drive_setpoint_sub.copy(&_differential_drive_setpoint);

		_speed_pid_output = pid_calculate(&_speed_pid, _differential_drive_setpoint.speed, _velocity_in_body_frame(0), 0, dt);
		_angular_velocity_pid_output = pid_calculate(&_angular_velocity_pid, _differential_drive_setpoint.yaw_rate,
					       _vehicle_angular_velocity.xyz[2], 0, dt);
	}

	// get the normalized wheel speeds from the inverse kinematics class (DifferentialDriveKinematics)
	Vector2f wheel_speeds = _differential_drive_kinematics.computeInverseKinematics(
					_differential_drive_setpoint.speed + _speed_pid_output,
					_differential_drive_setpoint.yaw_rate + _angular_velocity_pid_output);

	// Check if max_angular_wheel_speed is zero
	const bool setpoint_timeout = (_differential_drive_setpoint.timestamp + 100_ms) < now;
	const bool valid_max_speed = _param_rdd_speed_scale.get() > FLT_EPSILON;

	if (!_armed || setpoint_timeout || !valid_max_speed) {
		wheel_speeds = {}; // stop
	}

	wheel_speeds = matrix::constrain(wheel_speeds, -1.f, 1.f);

	actuator_motors_s actuator_motors{};
	actuator_motors.reversible_flags = _param_r_rev.get(); // should be 3 see rc.rover_differential_defaults
	wheel_speeds.copyTo(actuator_motors.control);
	actuator_motors.timestamp = now;
	_actuator_motors_pub.publish(actuator_motors);
}

int DifferentialDrive::task_spawn(int argc, char *argv[])
{
	DifferentialDrive *instance = new DifferentialDrive();

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init()) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int DifferentialDrive::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int DifferentialDrive::print_usage(const char *reason)
{
	if (reason) {
		PX4_ERR("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
Rover Differential Drive controller.
)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("differential_drive", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();
	return 0;
}

extern "C" __EXPORT int differential_drive_main(int argc, char *argv[])
{
	return DifferentialDrive::main(argc, argv);
}