#include "motors.h"

#include "receiver.h"
#include <util/delay.h>
#include <util/atomic.h>

// 168 CPU tacts for ISR to finish - about 2% error
// TODO: decrease this as much as possible
#define MIN_DIST    21

#define ESC_PERIOD  F_CPU / ESC_RATE
#if ESC_RATE >= 500
#error "ESC rate is too high (protocol restriction)"
#endif
#if ESC_PERIOD >= (1 << 16)
#error "ESC rate is too low (timer width and prescaler restriction)"
#endif

#if M5_USED && M6_USED
#define MOTOR_COUNT 6
#elif M5_USED || M6_USED
#define MOTOR_COUNT 5
#else
#define MOTOR_COUNT 4
#endif

static struct {
    uint16_t offset;
    uint8_t number;
} motors_list[MOTOR_COUNT];
static uint16_t motor_next;

// TODO: implement Servo skip
#if defined(SINGLE_COPTER) \
    || defined(DUAL_COPTER) \
    || defined(TWIN_COPTER) \
    || defined(TRI_COPTER)
uint8_t servo_skip;
uint16_t servo_skip_divider;
#endif

static volatile bool motorReady = true;

void motorsSetup() {
    M1_DIR = OUTPUT;
    M2_DIR = OUTPUT;
    M3_DIR = OUTPUT;
    M4_DIR = OUTPUT;
    M5_DIR = OUTPUT;
    M6_DIR = OUTPUT;
    M1 = 0;
    M2 = 0;
    M3 = 0;
    M4 = 0;
    M5 = 0;
    M6 = 0;

    /*
     * timer0 (8bit) - run at 8MHz, used to control ESC pulses
     * We use 8Mhz instead of 1MHz (1 usec) to avoid alignment jitter.
     */
    TCCR0B = _BV(CS00); /* NOTE: Specified again below with FOC0x bits */

#if defined(SINGLE_COPTER) \
    || defined(DUAL_COPTER) \
    || defined(TWIN_COPTER) \
    || defined(TRI_COPTER)
    /*
     * Calculate the servo rate divider (pulse loop skip count
     * needed to avoid burning analog servos)
     */
    for(servo_skip_divider = 1;;servo_skip_divider++) {
        if(servo_skip_divider * SERVO_RATE >= ESC_RATE) {
            break;
        }
    }
#endif
}

void motorOutputPPM(struct MT_STATE_S *state){

    // Ensure all values are in their ranges
    state->m1out = MIN(1000, state->m1out);
    state->m1out = MAX(0, state->m1out);
    state->m2out = MIN(1000, state->m2out);
    state->m2out = MAX(0, state->m2out);
    state->m3out = MIN(1000, state->m3out);
    state->m3out = MAX(0, state->m3out);
    state->m4out = MIN(1000, state->m4out);
    state->m4out = MAX(0, state->m4out);
#if M5_USED
    state->m5out = MIN(1000, state->m5out);
    state->m5out = MAX(0, state->m5out);
#endif
#if M6_USED
    state->m6out = MIN(1000, state->m6out);
    state->m6out = MAX(0, state->m6out);
#endif


    // Temporary array used for sorting
    uint16_t motors[MOTOR_COUNT];
    motors[0] = state->m1out;
    motors[1] = state->m2out;
    motors[2] = state->m3out;
    motors[3] = state->m4out;
#if M5_USED
    motors[4] = state->m5out;
#endif
#if M6_USED
    motors[5] = state->m6out;
#endif


    // Handles indexes of motors array for sorting purposes
    uint8_t sort_result[MOTOR_COUNT] = {0, 1, 2, 3,
#if MOTOR_COUNT == 5
            5
#endif
#if MOTOR_COUNT == 6
            5, 6
#endif
    };


    /* Selection sort (ascending).
     *  We need to provide sorted list for the ISR */
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        uint8_t max = i;
        for (uint8_t j = i + 1; j < MOTOR_COUNT; j++) {
            if (motors[sort_result[j]] > motors[sort_result[max]]) {
                max = j;
            }
        }
        if (i != max) {
            uint8_t tmp = sort_result[i];
            sort_result[i] = sort_result[max];
            sort_result[max] = tmp;
        }
    }


    /* Wait previous output to finish. Next calculations update variables used
     *  in ISR so we can't perform those before ready-wait loop*/
    while(!motorReady);
    motorReady = false;


    /* We will use all motor_list contents in ISR indexing it reverse direction.
     *  So put the last possible index here */
    motor_next = MOTOR_COUNT - 1;

    /* Loop trough sorted motors array (motors[sort_result[i]]) in reverse
     *  direction copying values and doing some magic */
    for (int8_t i = MOTOR_COUNT - 1; i >= 0; i--) {
        motors_list[i].number = sort_result[i];
        if (i != (MOTOR_COUNT - 1) &&
                (motors[sort_result[i]] - motors[sort_result[i + 1]]) < MIN_DIST) {
            /* Due to ISR latency we can't output values which are closer than
             *   MIN_DIST to each other. In this case we are putting zero to
             *   offset indicating that current motor should have same output
             *   as previous one */
            // TODO: implement values averaging to mitigate error a bit
            motors_list[i].offset = 0;
        } else {
            /* Normal case - difference is high enough to setup Compare event
             *  and not hit it before ISR ends. Add 1000 offset as required by
             *  protocol and multiply all by 8 as we have 8MHz clock */
            motors_list[i].offset = 1000 * 8 + motors[sort_result[i]] * 8;
        }
    }


    // Start outputting signal
    M1 = 1; M2 = 1; M3 = 1; M4 = 1; M5 = 1; M6 = 1;

    /* Remember current counter value to base compare further compare events on
     *  it */
    uint16_t curr_cnt;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        /* We should disable interrupts here so no one will be able to corrupt
         *  TEMP register used for atomic 16-bit access */
        curr_cnt = TCNT1;
    }

    /* Calculate initial value for our main compare register.
     *  This should be offset for motor with lowest value and that in turn
     *  resides in the last member of the motor_list because we have sorted it.
     */
    uint16_t pulse_delay = curr_cnt + (motors_list[MOTOR_COUNT - 1].offset);

    /* Second available compare register will be used to add some pause in
     *  pulses generation to ensure that we have pulses rate not higher than
     *  ESC_RATE */
    uint16_t pause_delay = curr_cnt + ESC_PERIOD;

    /* Loop through motors list adding required offset.
     *  Skip last member as we have already computed and used that few lines
     *  before. Also skip members with magic applied - they will use previous
     *  motors offset */
    for(uint8_t i = 0; i < MOTOR_COUNT - 1; i++) {
        if (motors_list[i].offset != 0) {
            motors_list[i].offset += curr_cnt;
        }
    }

    // Clear interrupt flags (just for case as a good practice)
    TIFR1 = _BV(OCF1A) | _BV(OCF1B);

    /* We are splitting atomic access section in two pieces to give a chance
     *  for some other modules interrupt to strike between that two parts.
     *  It's not critical for this code but could be could decrease overall
     *  interrupt latency */
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        /* We should disable interrupts here so no one will be able to corrupt
         *  TEMP register used for atomic 16-bit access */
        OCR1A = pulse_delay;
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        /* We should disable interrupts here so no one will be able to corrupt
         *  TEMP register used for atomic 16-bit access */
        OCR1B = pause_delay;
    }
    // Enable interrupts processing
    TIMSK1 |= _BV(OCIE1A) | _BV(OCIE1B);
}

ISR(TIMER1_COMPA_vect) {
    /* Loop through motor list (from highest index set in motorOutputPPM to
     * zero) */
    for (;; motor_next--) {
        // Decide which line should go down now
        switch (motors_list[motor_next].number) {
        case 0:
            M1 = 0;
            break;
        case 1:
            M2 = 0;
            break;
        case 2:
            M3 = 0;
// Mirror M3 to M5 if it's not used
#if !M5_USED
            M5 = 0;
#endif
            break;
        case 3:
            M4 = 0;
// Mirror M4 to M6 if it's not used
#if !M6_USED
            M6 = 0;
#endif
            break;
#if M5_USED
        case 4:
            M5 = 0;
            break;
#endif
#if M5_USED
        case 5:
            M6 = 0;
            break;
#endif
        }
        if (motor_next == 0) {
            /* Whew, we are done here - disable further interrupts and get some
             *  rest */
            TIMSK1 &= ~_BV(OCIE1A);
            break;
        }
        // Check for magic
        if (motors_list[motor_next - 1].offset != 0) {
            /* Normal case - next line will be release in some time later.
             *  Setup event for that time and exit. */
            OCR1A = motors_list[motor_next - 1].offset;
            // Need to decrement manually as we are breaking the loop
            motor_next--;
            break;
        }
        // Otherwise go and release next line
    }
}

ISR(TIMER1_COMPB_vect) {
    /* This happens when pause required to care ESC_RATE is finished.
     *  Indicate readiness for the next cycle and disable further interrupts */
    TIMSK1 &= ~_BV(OCIE1B);
    motorReady = true;
}

void motorsIdentify() {
    LED = 0;
    int8_t motor = 0;
    uint16_t delay = 0;
    uint16_t time = TCNT2;
    bool escInit = true; // Wait until the ESCs have initialized
    struct MT_STATE_S motors;

    while (true) {
        delay += (uint8_t) (TCNT2 - time);
        time = TCNT2;

        if (escInit) {
            if (delay > 23437) { // 3.00 second delay (3.00 / .000128 = 23437.5)
                escInit = false;
                delay = 0;
            }
        } else if (LED) {
            if (delay > 1171) { // 0.15 second delay (0.15 / .000128 = 1171.8)
                if (++motor > 6) {
                    motor = 0;
                }
                delay = 0;
                LED = !LED;
            }
        } else {
            if (delay > 7812) { // 1.00 second delay (1.00 / .000128 = 7812.5)
                delay = 0;
                LED = !LED;
            }
        }

        motors.m1out = 0;
        motors.m2out = 0;
        motors.m3out = 0;
        motors.m4out = 0;
#if M5_USED
        motors.m5out = 0;
#endif
#if M6_USED
        motors.m6out = 0;
#endif

        if (LED) {
            if (motor == 1) {
                motors.m1out = MOTOR_LOWEST_VALUE;
            }
            if (motor == 2) {
                motors.m2out = MOTOR_LOWEST_VALUE;
            }
            if (motor == 3) {
                motors.m3out = MOTOR_LOWEST_VALUE;
            }
            if (motor == 4) {
                motors.m4out = MOTOR_LOWEST_VALUE;
            }
#if M5_USED
            if (motor == 5) {
                motors.m5out = MOTOR_LOWEST_VALUE;
            }
#endif
#if M6_USED
            if (motor == 6) {
                motors.m6out = MOTOR_LOWEST_VALUE;
            }
#endif
        }

        motorOutputPPM(&motors);
    }
}

void motorsThrottleCalibration() {
    struct MT_STATE_S motors;
    struct RX_STATE_S rxState;

    // flash LED 3 times
    for (uint8_t i = 0; i < 3; i++) {
        LED = 1;
        _delay_ms(25);
        LED = 0;
        _delay_ms(25);
    }

    while (true) {
        receiverGetChannels(&rxState);
#ifdef SINGLE_COPTER
        motors.m1out = rxState.collective;
        motors.m2out = 1400; // Center: 140
        motors.m3out = 1400;
        motors.m4out = 1400;
        motors.m5out = 1400;
#elif defined(DUAL_COPTER)
        motors.m1out = rxState.collective;
        motors.m2out = rxState.collective;
        motors.m3out = 500; // Center: 50
        motors.m4out = 500;
#elif defined(TWIN_COPTER)
        motors.m1out = rxState.collective;
        motors.m2out = rxState.collective;
        motors.m3out = 500; // Center: 50
        motors.m4out = 500;
        motors.m5out = 500;
        motors.m6out = 500; // Center: 50, Reverse
#elif defined(TRI_COPTER)
        motors.m1out = rxState.collective;
        motors.m2out = rxState.collective;
        motors.m3out = rxState.collective;
        motors.m4out = 500 + rxState.yaw * 2; // Center: 50
#elif defined(QUAD_COPTER) || defined(QUAD_X_COPTER) || defined(Y4_COPTER)
        motors.m1out = rxState.collective;
        motors.m2out = rxState.collective;
        motors.m3out = rxState.collective;
        motors.m4out = rxState.collective;
#elif defined(HEX_COPTER) ||  defined(Y6_COPTER)
        motors.m1out = rxState.collective;
        motors.m2out = rxState.collective;
        motors.m3out = rxState.collective;
        motors.m4out = rxState.collective;
        motors.m5out = rxState.collective;
        motors.m6out = rxState.collective;
#else
#error No Copter configuration defined !!!!
#endif
        // this regulates rate at which we output signals
        motorOutputPPM(&motors);
    }
}
