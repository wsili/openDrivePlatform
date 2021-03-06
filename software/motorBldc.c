/**************************************************
 * This file is Public Domain
 *
 * This file is distributed in the hope that it will
 *	be useful, but WITHOUT ANY WARRANTY; without
 *	even the implied warranty of MERCHANTABILITY
 *	or FITNESS FOR A PARTICULAR PURPOSE.
*************************************************/
#include "misc.h"
#include "motorBldc.h"
#include "mpwm.h"
#include "gpio.h"
#include "adc.h"
#include "milliSecTimer.h"

#define NULL	0

/* Global variables */
typedef struct
{
	volatile uint8_t state;
	volatile int8_t sector;
	volatile uint16_t dutyCycle;
	volatile uint32_t startTimeAbs;
	volatile uint32_t startCommutationTimeAbs;
	volatile uint32_t lockUntilTimeAbs;
	volatile uint16_t phaseA, phaseB, phaseC;
	volatile uint16_t *dormantPhasePtr;
	_BLDC_motorDirection direction;

	_BLDC_sensor sensor;

	uint8_t hallToSector[8];
} _bldc_motor;

typedef struct{
	bool direction;
	uint16_t dutyCycle;
}_bldc_motor_command;

volatile _bldc_motor BLDC_motor;
_bldc_motor_command BLDC_command;

// Used internally to motor.c, "private"
void BLDC_commutate(void);
void BLDC_initPositionSensors(void);
void BLDC_determineSector(void);
void BLDC_adcInterrupt(void);

/* This is a complete table that lists all of the possible translations
 * from hall sensor inputs to sectors. */
const uint8_t hallToSector[12][8] = {	{6,1,3,2,5,0,4,6},
										{6,0,2,1,4,5,3,6},
										{6,5,1,0,3,4,2,6},
										{6,4,0,5,2,3,1,6},
										{6,3,5,4,1,2,0,6},
										{6,2,4,3,0,1,5,6},
										{6,4,2,3,0,5,1,6},
										{6,3,1,2,5,4,0,6},
										{6,2,0,1,4,3,5,6},
										{6,1,5,0,3,2,4,6},
										{6,0,4,5,2,1,3,6},
										{6,5,3,4,1,0,2,6}};

/* This specifies which line is to be used from the hallToSector table */
uint8_t hallTableUtilized = 0;

/***************************************************************
 * Function:	void BLDC_initMotor(void)
 *
 * Purpose:		This function is called by higher-level software in order
 * 					to initialize the motor in preparation for operation.
 *
 * Parameters:	none
 *
 * Returns:		none
 *
 * Globals affected:	none
 **************************************************************/
void
BLDC_initMotor(void)
{
	MPWM_initMotorPwm();
	MPWM_setMotorPwmFreq(16000);

	BLDC_stopMotor();
	BLDC_commandDirection(BLDC_POS);

	BLDC_initPositionSensors();

	// Assign the ADC1 Interrupt to the BLDC_adcInterrupt() function
	//	and enable the interrupt.  Whenever an ADC1 interrupt occurs,
	//	it will execute the BLDC_adcInterrupt() code from this file.
	ADC_initAdc1Interrupt(&BLDC_adcInterrupt);

	return;
} // END BLDC_initMotor()

/***************************************************************
 * Function:	void BLDC_initPositionSensors(void)
 *
 * Purpose:		This function will identify the type of sensors utilized (if any
 * 				and will take any action necessary to initialize the sensor(s)
 *
 * Parameters:	none
 *
 * Returns:		none
 *
 * Globals affected:	none
 **************************************************************/
void
BLDC_initPositionSensors(void)
{
	unsigned int hallValue = 0;

	/* Hall sensors to inputs */
	GPIO_pinSetup(GPIO_PORT_B, 0, GPIO_FLOATING_INPUT);
	GPIO_pinSetup(GPIO_PORT_B, 1, GPIO_FLOATING_INPUT);
	GPIO_pinSetup(GPIO_PORT_B, 2, GPIO_FLOATING_INPUT);

	/* Read the current hall sensor values */
	hallValue += (uint16_t)GPIO_readInput(GPIO_PORT_B, 0);
	hallValue += (uint16_t)GPIO_readInput(GPIO_PORT_B, 1) << 1;
	hallValue += (uint16_t)GPIO_readInput(GPIO_PORT_B, 2) << 2;

	/* If the hall sensor values is valid, then
	 * hall sensors are utilized for sensors */
	if((hallValue != 0) && (hallValue != 7))
	{
		BLDC_motor.sensor = BLDC_HALL;

		// Load the table to be used from program memory
		for(uint8_t i = 0; i < 8; i++)
		{
			BLDC_motor.hallToSector[i] = hallToSector[hallTableUtilized][i];
		}
	}

	return;
} // END BLDC_initPositionSensors

/***************************************************************
 * Function:	void BLDC_startMotor(void)
 *
 * Purpose:		This function is called by higher-level software
 * 					when motor rotation should begin
 *
 * Parameters:	none
 *
 * Returns:		none
 *
 * Globals affected:	BLDC_motor.sector, BLDC_motor.state
 **************************************************************/
void
BLDC_startMotor(void)
{
	// Only allow this routine to execute if
	//	the motor is in the STOPPED state
	if(BLDC_motor.state == BLDC_STOPPED)
	{
		BLDC_motor.sector = 0;
		BLDC_motor.state = BLDC_STARTING;

		BLDC_motor.startTimeAbs = MSTMR_getMilliSeconds();

		BLDC_motor.dutyCycle = BLDC_MIN_DUTY_CYCLE;
		BLDC_motor.direction = BLDC_command.direction;

		BLDC_determineSector();
		BLDC_commutate();
	}

	return;
} // END BLDC_startMotor()

/***************************************************************
 * Function:	void BLDC_stopMotor(void)
 *
 * Purpose:		This function is called by higher-level software
 * 					when motor rotation should cease
 *
 * Parameters:	none
 *
 * Returns:		none
 *
 * Globals affected:	BLDC_motor.sector, BLDC_motor.state
 **************************************************************/
void
BLDC_stopMotor(void)
{
	// Place each phase in the DORMANT state
	MPWM_setPhaseDutyCycle(MPWM_PH_A, MPWM_DORMANT, 0);
	MPWM_setPhaseDutyCycle(MPWM_PH_B, MPWM_DORMANT, 0);
	MPWM_setPhaseDutyCycle(MPWM_PH_C, MPWM_DORMANT, 0);

	// Place the motor in the STOPPED state
	BLDC_motor.state = BLDC_STOPPED;

	return;
} // END BLDC_stopMotor()

/***************************************************************
 * Function:	void BLDC_determineSector(void)
 *
 * Purpose:		This function will return the motor sector
 *
 * Parameters:	none
 *
 * Returns:		none
 *
 * Globals affected:	BLDC_motor.sensorState, BLDC_motor.sector
 **************************************************************/
void
BLDC_determineSector(void)
{
	uint8_t sector;

	switch(BLDC_motor.sensor)
	{
		case BLDC_SENSORLESS:
		{
			break;
		}

		case BLDC_HALL:
		{
			unsigned int hallValue = 0;

			hallValue += (uint16_t)GPIO_readInput(GPIO_PORT_B, 0);
			hallValue += (uint16_t)GPIO_readInput(GPIO_PORT_B, 1) << 1;
			hallValue += (uint16_t)GPIO_readInput(GPIO_PORT_B, 2) << 2;

			/* Uses a lookup table to determine the current
			 * sector based on the current hall value */
			BLDC_motor.sector = sector = BLDC_motor.hallToSector[hallValue];

			break;
		}

		default:
		{
			break;
		}
	}

	return;
} // END BLDC_determineSector

/***************************************************************
 * Function:	void BLDC_commandDutyCycle(unsigned int dutyCycle);
 *
 * Purpose:		This function is called by higher-level software
 * 					to modify the duty cycle.
 *
 * Parameters:	uint16_t dutyCycle		This is the fixed-point representation
 * 										of the motor duty cycle.  0%-100% is scaled
 * 										to 0-65535
 *
 * Returns:		none
 *
 * Globals affected:	BLDC_command.dutyCycle
 **************************************************************/
void
BLDC_commandDutyCycle(uint16_t dutyCycle)
{
	BLDC_command.dutyCycle = dutyCycle;
	return;
}

/***************************************************************
 * Function:	void BLDC_commandDirection(uint8_t direction);
 *
 * Purpose:		This function is called by higher-level software
 * 					to modify the motor direction.
 *
 * Parameters:	bool direction	This determines the direction of the motor.  Valid
 * 								values are BLDC_POS and BLDC_NEG.
 *
 * Returns:		none
 *
 * Globals affected:	BLDC_motor.direction
 **************************************************************/
void
BLDC_commandDirection(bool direction)
{
	BLDC_command.direction = direction;
	return;
}

/***************************************************************
 * Function:	void BLDC_commutate(void)
 *
 * Purpose:		This function is called when the motor phase state
 * 					should be stepped to the next phase state based
 * 					on the current rotor location (sector)
 *
 * Parameters:	none
 *
 * Returns:		none
 *
 * Globals affected:	BLDC_motor.sector
 **************************************************************/
void
BLDC_commutate(void)
{
	// Move to the next step in the 6-step scheme
	if(BLDC_motor.direction == BLDC_POS)
	{
		if(++BLDC_motor.sector > 5)
		{
			BLDC_motor.sector = 0;
		}
	}
	else
	{
		if(--BLDC_motor.sector < 0)
		{
			BLDC_motor.sector = 5;
		}
	}


	// Use lookup tables to determine which phase should be high,
	//	low, and dormant based on the current sector (as defined by
	//	the positive direction).
	//
	//		sector	hiPhase	loPhase	dormantPhase
	//		0		PH_A	PH_B	PH_C
	//		1		PH_A	PH_C	PH_B
	//		2		PH_B	PH_C	PH_A
	//		3		PH_B	PH_A	PH_C
	//		4		PH_C	PH_A	PH_B
	//		5		PH_C	PH_B	PH_A
	const uint8_t hiPhaseTable[] = {MPWM_PH_A, MPWM_PH_A, MPWM_PH_B, MPWM_PH_B, MPWM_PH_C, MPWM_PH_C};
	const uint8_t loPhaseTable[] = {MPWM_PH_B, MPWM_PH_C, MPWM_PH_C, MPWM_PH_A, MPWM_PH_A, MPWM_PH_B};
	const uint8_t dormantPhaseTable[] = {MPWM_PH_C, MPWM_PH_B, MPWM_PH_A, MPWM_PH_C, MPWM_PH_B, MPWM_PH_A};

	// Calculate the high side and low side duty cycles
	uint16_t halfDutyCycle = (BLDC_motor.dutyCycle >> 1);
	uint16_t highSideDutyCycle = 32767 + halfDutyCycle;
	uint16_t lowSideDutyCycle = 32767 - halfDutyCycle;

	// Load each phase with the appropriate duty cycle
	MPWM_setPhaseDutyCycle(dormantPhaseTable[BLDC_motor.sector], MPWM_DORMANT, BLDC_motor.dutyCycle);
	MPWM_setPhaseDutyCycle(hiPhaseTable[BLDC_motor.sector], MPWM_HI_STATE, highSideDutyCycle);
	MPWM_setPhaseDutyCycle(loPhaseTable[BLDC_motor.sector], MPWM_HI_STATE, lowSideDutyCycle);
	//MPWM_setAdcSamplingTime(highSideDutyCycle);


	// Indicate which phase is dormant for later use by the ADC module
	switch(dormantPhaseTable[BLDC_motor.sector])
	{
		case MPWM_PH_A:
		{
			BLDC_motor.dormantPhasePtr = &BLDC_motor.phaseA;
			break;
		}

		case MPWM_PH_B:
		{
			BLDC_motor.dormantPhasePtr = &BLDC_motor.phaseB;
			break;
		}

		case MPWM_PH_C:
		{
			BLDC_motor.dormantPhasePtr = &BLDC_motor.phaseC;
			break;
		}

		default:
		{
			BLDC_motor.dormantPhasePtr = NULL;
			break;
		}
	}

	if(BLDC_motor.state == BLDC_STARTING)
	{
		BLDC_motor.startCommutationTimeAbs = MSTMR_getMilliSeconds();
	}

	return;
} //END BLDC_commutate

/***************************************************************
 * Function:	uint8_t BLDC_getMotorState(void)
 *
 * Purpose:		This function is called by higher-level software
 * 					to retrieve the current motor state.
 *
 * Parameters:	none
 *
 * Returns:		uint8_t BLDC_motor.state
 *
 * Globals affected:	none
 **************************************************************/
uint8_t
BLDC_getMotorState(void)
{
	return BLDC_motor.state;
}

/***************************************************************
 * Function:	uint8_t BLDC_adcInterrupt(void)
 *
 * Purpose:		This function is executed when all of the phase
 * 					ADC's have been sampled and converted.
 *
 * Parameters:	none
 *
 * Returns:		none
 *
 * Globals affected:	none
 **************************************************************/
void
BLDC_adcInterrupt(void)
{
	// retrieve phase adc values
	// TODO: Could probably do this using DMA more efficiently
	BLDC_motor.phaseA = ADC_getVoltage(ADC_PH_A);
	BLDC_motor.phaseB = ADC_getVoltage(ADC_PH_B);
	BLDC_motor.phaseC = ADC_getVoltage(ADC_PH_C);

	uint16_t neutralVoltage = ADC_getVoltage(ADC_V_BUS) >> 1;

	switch(BLDC_motor.state)
	{
		case BLDC_LOCKED:
		{
			// When the locked timer expires, then shift
			//	the motor into the "stopped" state
			if(MSTMR_getMilliSeconds() > BLDC_motor.lockUntilTimeAbs)
			{
				BLDC_motor.state = BLDC_STOPPED;
			}

			break;
		}

		case BLDC_STOPPED:
		{
			break;
		}

		// TODO: verify everything in this case on the hardware
		case BLDC_STARTING:
		{
			// Apply the starting signal to the phases
			//	TODO: make a case for the reverse direction
			if((BLDC_motor.sector & 0b1) == 1)
			{
				if(*BLDC_motor.dormantPhasePtr < neutralVoltage)
				{
					BLDC_commutate();
				}
			}
			else
			{
				if(*BLDC_motor.dormantPhasePtr > neutralVoltage)
				{
					BLDC_commutate();
				}
			}

			// If a few milliseconds have passed without a commutation,
			//	then apply a commutation so that the motor isn't stuck
			//	in one position
			if((BLDC_motor.startCommutationTimeAbs + 25) < MSTMR_getMilliSeconds())
			{
				BLDC_commutate();
			}

			// When the motor has reached a sufficient speed,
			//	then shift the motor into the "running" mode


			break;
		}

		case BLDC_RUNNING:
		{
			// Apply the running signal to the phases

			break;
		}

		default:
		{
			// Should never get here
			break;
		}

	}

	return;
}

