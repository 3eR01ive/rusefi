#include "pch.h"

#include "biquad.h"
#include "thread_controller.h"
#include "knock_logic.h"
#include "software_knock.h"

#if EFI_SOFTWARE_KNOCK

#include "knock_config.h"
#include "ch.hpp"

#ifdef KNOCK_SPECTROGRAM
#include "development/knock_spectrogram.h"
#include "fft/fft.hpp"

#define SIZE 256
class SpectrogramData {
public:
	float filteredBuffer[SIZE];
	complex_type fftBuffer[SIZE];
	float frequencies[SIZE/2];
	float amplitudes[SIZE/2];
};

static SpectrogramData* spectrogramData = nullptr;
static BigBufferHandle buffer;
#endif //KNOCK_SPECTROGRAM

// static NO_CACHE float sin_data[SIZE];
// static volatile bool sin_data_is_gen = false;

//static NO_CACHE float values[2000];
static NO_CACHE adcsample_t sampleBuffer[2000];
static int8_t currentCylinderNumber = 0;
static efitick_t lastKnockSampleTime = 0;
static Biquad knockFilter;

static volatile bool knockIsSampling = false;
static volatile bool knockNeedsProcess = false;
static volatile size_t sampleCount = 0;

chibios_rt::BinarySemaphore knockSem(/* taken =*/ true);

static void completionCallback(ADCDriver* adcp) {
	if (adcp->state == ADC_COMPLETE) {
		knockNeedsProcess = true;

		// Notify the processing thread that it's time to process this sample
		chSysLockFromISR();
		knockSem.signalI();
		chSysUnlockFromISR();
	}
}

static void errorCallback(ADCDriver*, adcerror_t) {
}

static const uint32_t smpr1 =
	ADC_SMPR1_SMP_AN10(KNOCK_SAMPLE_TIME) |
	ADC_SMPR1_SMP_AN11(KNOCK_SAMPLE_TIME) |
	ADC_SMPR1_SMP_AN12(KNOCK_SAMPLE_TIME) |
	ADC_SMPR1_SMP_AN13(KNOCK_SAMPLE_TIME) |
	ADC_SMPR1_SMP_AN14(KNOCK_SAMPLE_TIME) |
	ADC_SMPR1_SMP_AN15(KNOCK_SAMPLE_TIME);

static const uint32_t smpr2 =
	ADC_SMPR2_SMP_AN0(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN1(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN2(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN3(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN4(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN5(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN6(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN7(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN8(KNOCK_SAMPLE_TIME) |
	ADC_SMPR2_SMP_AN9(KNOCK_SAMPLE_TIME);

static const ADCConversionGroup adcConvGroupCh1 = {
	.circular = FALSE,
	.num_channels = 1,
	.end_cb = &completionCallback,
	.error_cb = &errorCallback,
	.cr1 = 0,
	.cr2 = ADC_CR2_SWSTART,
	// sample times for channels 10...18
	.smpr1 = smpr1,
	// sample times for channels 0...9
	.smpr2 = smpr2,

	.htr = 0,
	.ltr = 0,

	.sqr1 = 0,
	.sqr2 = 0,
	.sqr3 = ADC_SQR3_SQ1_N(KNOCK_ADC_CH1)
};

// Not all boards have a second channel - configure it if it exists
#if KNOCK_HAS_CH2
static const ADCConversionGroup adcConvGroupCh2 = {
	.circular = FALSE,
   	.num_channels = 1,
   	.end_cb = &completionCallback,
   	.error_cb = &errorCallback,
   	.cr1 = 0,
   	.cr2 = ADC_CR2_SWSTART,
   	// sample times for channels 10...18
   	.smpr1 = smpr1,
  	// sample times for channels 0...9
   	.smpr2 = smpr2,

    .htr = 0,
    .ltr = 0,

    .sqr1 = 0,
    .sqr2 = 0,
    .sqr3 = ADC_SQR3_SQ1_N(KNOCK_ADC_CH2)
};
#endif // KNOCK_HAS_CH2

static const ADCConversionGroup* getConversionGroup(uint8_t channelIdx) {
#if KNOCK_HAS_CH2
	if (channelIdx == 1) {
		return &adcConvGroupCh2;
	}
#else
	(void)channelIdx;
#endif // KNOCK_HAS_CH2

	return &adcConvGroupCh1;
}

// static float* gen_data(int size, float* x) {

//     const double dt = 1.0/KNOCK_SAMPLE_RATE;

// 	int rand_max = 400000000;
// 	int random_adder = 1 + rand() / ((rand_max + 1) / 300);

// 	int f1 = 8125 + random_adder;
// 	int f2 = 15350 + random_adder;

// 	for (std::size_t i = 0; i < size; ++i)
// 	{
// 		x[i] = 32*std::sin(2*M_PI*f1*i*dt) + 8*std::sin(2*M_PI*f2*i*dt + 0.75*M_PI);
// 	}

//     return x;
// }

void onStartKnockSampling(uint8_t cylinderNumber, float samplingSeconds, uint8_t channelIdx) {
	if (!engineConfiguration->enableSoftwareKnock) {
		return;
	}

	// Cancel if ADC isn't ready
	if (!((KNOCK_ADC.state == ADC_READY) ||
			(KNOCK_ADC.state == ADC_COMPLETE) ||
			(KNOCK_ADC.state == ADC_ERROR))) {
		return;
	}

	// If there's pending processing, skip this event
	if (knockNeedsProcess) {
		return;
	}

	// Convert sampling time to number of samples
	constexpr int sampleRate = KNOCK_SAMPLE_RATE;
	sampleCount = 0xFFFFFFFE & static_cast<size_t>(clampF(100, samplingSeconds * sampleRate, efi::size(sampleBuffer)));

	// Select the appropriate conversion group - it will differ depending on which sensor this cylinder should listen on
	auto conversionGroup = getConversionGroup(channelIdx);

	// Stash the current cylinder's number so we can store the result appropriately
	currentCylinderNumber = cylinderNumber;

	adcStartConversionI(&KNOCK_ADC, conversionGroup, sampleBuffer, sampleCount);
	lastKnockSampleTime = getTimeNowNt();
}

class KnockThread : public ThreadController<UTILITY_THREAD_STACK_SIZE> {
public:
	KnockThread() : ThreadController("knock", PRIO_KNOCK_PROCESS) {}
	void ThreadTask() override;
};

static KnockThread kt;

void initSoftwareKnock() {
	if (engineConfiguration->enableSoftwareKnock) {
		knockFilter.configureBandpass(KNOCK_SAMPLE_RATE, 1000 * engineConfiguration->knockBandCustom, 3);
		adcStart(&KNOCK_ADC, nullptr);

#ifdef KNOCK_SPECTROGRAM
		initKnockSpectrogram();
#endif

  // fun fact: we do not offer any ADC channel flexibility like we have for many other kinds of inputs
		efiSetPadMode("knock ch1", KNOCK_PIN_CH1, PAL_MODE_INPUT_ANALOG);
#if KNOCK_HAS_CH2
		efiSetPadMode("knock ch2", KNOCK_PIN_CH2, PAL_MODE_INPUT_ANALOG);
#endif
		kt.start();
	}
}

static void processLastKnockEvent() {
	if (!knockNeedsProcess) {
		return;
	}

	float sumSq = 0;

	// todo: reduce magic constants. engineConfiguration->adcVcc?
	constexpr float ratio = 3.3f / 4095.0f;

	size_t localCount = sampleCount;

	// Prepare the steady state at vcc/2 so that there isn't a step
	// when samples begin
	// todo: reduce magic constants. engineConfiguration->adcVcc?
	knockFilter.cookSteadyState(3.3f / 2);

	// Compute the sum of squares
	for (size_t i = 0; i < localCount; i++) {
		float volts = ratio * sampleBuffer[i];

		float filtered = knockFilter.filter(volts);
		if (i == localCount - 1 && engineConfiguration->debugMode == DBG_KNOCK) {
			engine->outputChannels.debugFloatField1 = volts;
			engine->outputChannels.debugFloatField2 = filtered;
		}

		// if(i < SIZE && engineConfiguration->enableKnockSpectrogram) {
		// 	spectrogramData->filteredBuffer[i] = volts;
		// }

		//values[i] = volts;

		sumSq += filtered * filtered;
	}

	//gen_data(SIZE, sin_data);

	// take a local copy
	auto lastKnockTime = lastKnockSampleTime;

	// We're done with inspecting the buffer, another sample can be taken
	knockNeedsProcess = false;

	float mainFreq = 0.f;

	//knockSpectorgramAddLine(mainFreq, values, 2000);

	#ifdef KNOCK_SPECTROGRAM
	if(engineConfiguration->enableKnockSpectrogram) {
		constexpr float ratio = 3.3f / 4095.0f;

		//fft::fft(sampleBuffer, spectrogramData->fftBuffer, SIZE);
		fft::fft_adc_sample(ratio, sampleBuffer, spectrogramData->fftBuffer, SIZE);
		fft::fft_freq(spectrogramData->frequencies, SIZE, KNOCK_SAMPLE_RATE); //samples per sec 218750
		fft::fft_amp(spectrogramData->fftBuffer, spectrogramData->amplitudes, SIZE);
		fft::fft_db(spectrogramData->amplitudes, SIZE);

		float mainFreq = fft::get_main_freq(spectrogramData->amplitudes, spectrogramData->frequencies, SIZE / 2);

		knockSpectorgramAddLine(mainFreq, spectrogramData->amplitudes, 60); // [60] to 25207.5kHz for optimize data size

		//engineConfiguration->knockBandCustom = mainFreq / 1000; // need save max amplitude of all
	}
	#endif

	// mean of squares (not yet root)
	float meanSquares = sumSq / localCount;

	// RMS
	float db = 10 * log10(meanSquares);

	// clamp to reasonable range
	db = clampF(-100, db, 100);

	engine->module<KnockController>()->onKnockSenseCompleted(currentCylinderNumber, db, mainFreq, lastKnockTime);
}

void KnockThread::ThreadTask() {
	while (1) {
		knockSem.wait();

		ScopePerf perf(PE::SoftwareKnockProcess);
		processLastKnockEvent();
	}
}

#ifdef KNOCK_SPECTROGRAM
void knockSpectrogramEnable() {
	chibios_rt::CriticalSectionLocker csl;

	buffer = getBigBuffer(BigBufferUser::KnockSpectrogram);
	if (!buffer) {
		return;
	}

	spectrogramData = buffer.get<SpectrogramData>();

	engineConfiguration->enableKnockSpectrogram = true;
}

void knockSpectrogramDisable() {
	chibios_rt::CriticalSectionLocker csl;
	// we're done with the buffer - let somebody else have it
	buffer = {};
	spectrogramData = nullptr;
	engineConfiguration->enableKnockSpectrogram = false;
}

const BigBufferHandle& knockSpectrogramGetBuffer() {
	return buffer;
}
#endif

#endif // EFI_SOFTWARE_KNOCK
