/**
 * @file	knock_spectrogramm.cpp
 *
 * @date Feb 20, 2023
 * @author Alexey Ershov, (c) 2012-2023
 */

#include "pch.h"

#include "knock_spectrogramm.h"

//#if EFI_SENSOR_CHART
#include "status_loop.h"

#if EFI_TEXT_LOGGING
static char LOGGING_BUFFER[264] CCM_OPTIONAL;
static Logging scLogging("knock_spectrogramm", LOGGING_BUFFER, sizeof(LOGGING_BUFFER));
#endif /* EFI_TEXT_LOGGING */

static int initialized = false;


void knockSpectorgrammAddLine(float* data, size_t size) {
#if EFI_TEXT_LOGGING
	if (!initialized) {
		return; // this is possible because of initialization sequence
	}

	if (scLogging.remainingSize() > size) {
		scLogging.reset();
		scLogging.appendPrintf(PROTOCOL_ANALOG_CHART LOG_DELIMITER);
		for(size_t i = 0; i < size; ++i) {
			scLogging.appendFloat(data[i], 4);
			scLogging.appendPrintf(LOG_DELIMITER);
		}
	}

#endif /* EFI_TEXT_LOGGING */
}

void initKnockSpectrogramm(void) {
#if EFI_SIMULATOR
	printf("initKnockSpectorgramm\n");
#endif

	initialized = true;
}

void publishKnockSpectrogrammIfFull() {

	scLogging.terminate();
	scheduleLogging(&scLogging);
}

