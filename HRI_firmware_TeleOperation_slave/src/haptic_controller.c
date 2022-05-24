/*
 * Copyright (C) 2021 EPFL-REHAssist (Rehabilitation and Assistive Robotics Group).
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

#include "haptic_controller.h"
#include "communication.h"
#include "drivers/adc.h"
#include "drivers/incr_encoder.h"
#include "drivers/hall.h"
#include "drivers/callback_timers.h"
#include "lib/utils.h"
#include "torque_regulator.h"
#include "drivers/ext_uart.h"

#define DEFAULT_HAPTIC_CONTROLLER_PERIOD 350 // Default control loop period [us].
#define START_BYTE 0x4D //header of bits sent
#define CUT_OFF 20000.0


volatile uint32_t  hapt_timestamp; // Time base of the controller, also used to timestamp the samples sent by streaming [us].
volatile float32_t hapt_hallVoltage; // Hall sensor output voltage [V].
volatile float32_t hapt_encoderPaddleAngle; // Paddle angle measured by the incremental encoder [deg].
volatile float32_t hapt_motorTorque; // Motor torque [N.m].

volatile uint8_t slave_bits;	//to check bits received by the slave
volatile uint32_t bytes_read = 0;
volatile float32_t temp_float32 = 0.0f;

//PID gains
volatile float32_t Kp = 0.001;
volatile float32_t Ki = 0.005;
volatile float32_t Kd = 0.0005;

//
volatile float32_t position = 0.0f;
volatile float32_t speed = 0.0;

volatile bool pid_enable = false;	// regulator flag to be turned on/off from GUI

volatile float32_t gui_variable = 45.0f;

float32_t lowPass(float32_t curr, float32_t prev, float32_t dt);
float32_t PID(float32_t position_error, float32_t position_error_prev, float32_t dt);

void hapt_Update(void);

/**
  * @brief Initializes the haptic controller.
  */
void hapt_Init(void)
{
	exuart_Init(115200);
    hapt_timestamp = 0;
    hapt_motorTorque = 0.0f;

    // Make the timers call the update function periodically.
    cbt_SetHapticControllerTimer(hapt_Update, DEFAULT_HAPTIC_CONTROLLER_PERIOD);

    // Share some variables with the computer.
    comm_monitorUint32Func("timestep [us]", cbt_GetHapticControllerPeriod,
                           cbt_SetHapticControllerPeriod);
    comm_monitorFloat("motor_torque [N.m]", (float32_t*)&hapt_motorTorque, READWRITE);
    comm_monitorFloat("encoder_paddle_pos [deg]", (float32_t*)&hapt_encoderPaddleAngle, READONLY);
    comm_monitorFloat("hall_voltage [V]", (float32_t*)&hapt_hallVoltage, READONLY);

    comm_monitorFloat("gui_var", (float32_t*)&gui_variable, READONLY);
    comm_monitorFloat("position", (float32_t*)&position, READONLY);
    //comm_monitorFloat("speed", (float32_t*)&speed, READONLY);
    //comm_monitorUint8("bits received by slave", (uint8_t*)&slave_bits, READONLY);
    //--------------PID controller--------------
    comm_monitorFloat("Kp", (float32_t*)&Kp, READWRITE);
    comm_monitorFloat("Ki", (float32_t*)&Ki, READWRITE);
    comm_monitorFloat("Kd", (float32_t*)&Kd, READWRITE);
    //------------------------------------------
    comm_monitorBool("enable PID", (bool*)&pid_enable, READWRITE);
}

/**
  * @brief Updates the haptic controller state.
  */
void hapt_Update()
{
	static uint32_t temp_int32 = 0;
	//static uint32_t second_div = 0;

	static float32_t position_prev = 0.0f;
	static float32_t hapt_encoderPaddleAngle_prev = 0.0f;

	void *temp_point = NULL;


    float32_t motorShaftAngle; // [deg].

    // Compute the dt (uncomment if you need it).
    float32_t dt = ((float32_t)cbt_GetHapticControllerPeriod()) / 1000000.0f; // [s].

    // Increment the timestamp.
    hapt_timestamp += cbt_GetHapticControllerPeriod();
    
    // Get the Hall sensor voltage.
    hapt_hallVoltage = hall_GetVoltage();

    // Get the encoder position.
    motorShaftAngle = enc_GetPosition();
    hapt_encoderPaddleAngle = motorShaftAngle / REDUCTION_RATIO;



   /* for(int i= 0; i < 1000; i++){
  	   if(exuart_ReceivedBytesCount() >= 4) break;
     }*/


    /*while(exuart_ReceivedBytesCount() > 9){
    	slave_bits = exuart_GetByte(); //discard
    }*/

    if(exuart_ReceivedBytesCount() >= 5){
    	//should keep reading the bytes until you see header
	   while(exuart_ReceivedBytesCount() >= 5){ //if any bits received
			slave_bits = exuart_GetByte();
			if(slave_bits == START_BYTE){ //check the header byte
				break;
			}
	   }
		if(exuart_ReceivedBytesCount() >= 4){ //if number of bytes received is >= 4 bytes, then keep receiving

			slave_bits = exuart_GetByte();
			temp_int32 = slave_bits;

			slave_bits = exuart_GetByte();
			temp_int32 |= slave_bits << 8;

			slave_bits = exuart_GetByte();
			temp_int32 |= slave_bits << 16;

			slave_bits = exuart_GetByte();
			temp_int32 |= slave_bits << 24;

			temp_point = &temp_int32;
			temp_float32 = *(float32_t *) temp_point;
			bytes_read = temp_int32;
			if(1 || temp_float32 < 45 && temp_float32 > -45){
				gui_variable = temp_float32;
			}
		}
	}

    position = lowPass(gui_variable, position_prev, dt);
    //speed = (position - position_prev) / dt;
    //speed = lowPass(speed, speed_prev, dt);


    hapt_encoderPaddleAngle = lowPass(hapt_encoderPaddleAngle, hapt_encoderPaddleAngle_prev, dt);
    if(pid_enable){
    	float32_t error = position - hapt_encoderPaddleAngle;
    	static float32_t error_prev = 0.0f;
    	hapt_motorTorque = PID(error, error_prev, dt);
		error_prev = error;
    }
    else{
    	hapt_motorTorque = 0.0f;
    }
    // Compute the motor torque, and apply it.

    torq_SetTorque(hapt_motorTorque);

    hapt_encoderPaddleAngle_prev = hapt_encoderPaddleAngle;
    //speed_prev = speed;
    position_prev = position;

}


float32_t lowPass(float32_t curr, float32_t prev, float32_t dt){

	float32_t tau = 1 / (2 * M_PI * CUT_OFF);
	float32_t alpha = dt / (dt + tau);

	return curr*alpha + prev*(1-alpha);
}

float32_t PID(float32_t position_error, float32_t position_error_prev, float32_t dt){
	static float32_t error_sum = 0.0f;
	error_sum += position_error*dt;

	return Kp*position_error + Kd*(position_error-position_error_prev)/dt + Ki*error_sum;
}
